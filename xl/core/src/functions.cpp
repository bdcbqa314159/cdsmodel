#include <cdsmodel/xl/functions.hpp>

#include <cdsmodel/xl/arguments.hpp>

#include <algorithm>
#include <array>

namespace cdsmodel::xl {
namespace {

constexpr std::string_view kCategory = "CDS Model";

// ------------------------------------------------------- CdsPrice, two forms
//
// Both read into Valuation / TradeTerms / Conventions / MarketData and then
// run the SAME three lines. That is the design: the two signatures are two
// readers over one shape, so they cannot disagree about anything except how
// the sheet spelled the inputs.

[[nodiscard]] Expected<XlValue> price_from(const Valuation &v, const TradeTerms &t,
                                           const Conventions &c, const MarketData &m, bool clean) {
  const Cds cds = make_cds(t, c);
  const Expected<double> price = cds_price(cds, v.today, v.cash_settle, v.step_in, c.holidays,
                                           m.discount, m.spread, m.recovery, clean);
  if (!price) {
    return unexpected(price.error());
  }
  return XlValue{*price};
}

/// The original signature, unchanged, so an existing workbook keeps working.
/// 16 arguments in upstream's order; see the parameter table below.
Expected<XlValue> dispatch_cds_price_legacy(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 16) {
    return fail(ErrorKind::bad_argument,
                "CdsPrice takes 16 arguments, got " + detail::num(args.size()));
  }
  const Expected<Valuation> valuation = Valuation::read_positional(args.subspan(0, 3));
  if (!valuation) {
    return unexpected(valuation.error());
  }
  const Expected<TradeTerms> trade = TradeTerms::read_positional(args.subspan(3, 3));
  if (!trade) {
    return unexpected(trade.error());
  }
  // Upstream's order interleaves: coupon rate sits at 6, then the conventions,
  // then the curves. The subspans below are that order, not a tidier one.
  const std::array<XlValue, 6> convention_cells{args[6], args[7],  args[8],
                                                args[9], args[10], args[11]};
  const Expected<Conventions> conventions = Conventions::read_positional(convention_cells);
  if (!conventions) {
    return unexpected(conventions.error());
  }
  const std::array<XlValue, 3> market_cells{args[12], args[13], args[14]};
  const Expected<MarketData> market = MarketData::read_positional(market_cells, ctx.curves);
  if (!market) {
    return unexpected(market.error());
  }
  const Expected<bool> clean = args[15].as_bool();
  if (!clean) {
    return fail(ErrorKind::bad_argument, "Clean Price: " + clean.error().message);
  }
  return price_from(*valuation, *trade, *conventions, *market, *clean);
}

/// The grouped signature: four labelled ranges and a flag. Five arguments
/// where the original has sixteen, and the six convention cells are written
/// once per desk rather than once per formula.
Expected<XlValue> dispatch_cds_price_grouped(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 5) {
    return fail(ErrorKind::bad_argument,
                "CDS.Price takes 5 arguments, got " + detail::num(args.size()));
  }
  const Expected<Valuation> valuation = Valuation::read_block(args[0]);
  if (!valuation) {
    return unexpected(valuation.error());
  }
  const Expected<TradeTerms> trade = TradeTerms::read_block(args[1]);
  if (!trade) {
    return unexpected(trade.error());
  }
  const Expected<Conventions> conventions = Conventions::read_block(args[2]);
  if (!conventions) {
    return unexpected(conventions.error());
  }
  const Expected<MarketData> market = MarketData::read_block(args[3], ctx.curves);
  if (!market) {
    return unexpected(market.error());
  }
  const Expected<bool> clean = args[4].as_bool();
  if (!clean) {
    return fail(ErrorKind::bad_argument, "Clean Price: " + clean.error().message);
  }
  return price_from(*valuation, *trade, *conventions, *market, *clean);
}

// ------------------------------------------------------------ the table

constexpr std::array<ParamSpec, 16> kCdsPriceLegacyParams{
    {{"Today", "Risk starts at the end of today"},
     {"CashSettle Date", "Date for which the PV is calculated"},
     {"StepIn Date", "Date when step-in begins"},
     {"Start Date", "Date when protection begins"},
     {"End Date", "Date when protection ends (end of day)"},
     {"Coupon Rate", "Fixed coupon rate for the fee leg"},
     {"Pay Acc On Default", "Should accrued interest be paid on default?"},
     {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
     {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
     {"Payment DCC", "Day count convention for coupon payment, e.g. ACT/360"},
     {"Bad Day Convention", "N, F, P or M"},
     {"Holidays", "Calendar used when adjusting coupon dates"},
     {"Discount Curve", "Handle from IRZeroCurveBuild"},
     {"Spread Curve", "Handle from CleanSpreadCurveBuild"},
     {"Recovery Rate", "Assumed recovery, 0 to 1"},
     {"Clean Price", "TRUE for clean, FALSE for dirty"}}};

constexpr std::array<ParamSpec, 5> kCdsPriceGroupedParams{
    {{"Valuation", "Two-column range: Today, Cash Settle Date, Step In Date"},
     {"Trade", "Two-column range: Start Date, End Date, Coupon Rate"},
     {"Conventions", "Two-column range: Pay Accrual On Default, Coupon Interval, "
                     "Stub Type, Payment DCC, Bad Day Convention, Holidays"},
     {"Market", "Two-column range: Discount Curve, Spread Curve, Recovery Rate"},
     {"Clean Price", "TRUE for clean, FALSE for dirty"}}};

constexpr std::array<FunctionSpec, 2> kFunctions{
    {{"CdsPrice", "QQQQQQQQQQQQQQQQQ", kCdsPriceLegacyParams,
      "Prices a CDS. The original 16-argument form, kept so existing "
      "workbooks keep working; CDS.Price is the same calculation with the "
      "arguments grouped.",
      kCategory, &dispatch_cds_price_legacy},
     {"CDS.Price", "QQQQQQ", kCdsPriceGroupedParams,
      "Prices a CDS from four labelled ranges. Identical to CdsPrice, with "
      "the six convention values written once instead of per formula.",
      kCategory, &dispatch_cds_price_grouped}}};

} // namespace

std::span<const FunctionSpec> all_functions() { return kFunctions; }

const FunctionSpec *find_function(std::string_view name) {
  const auto found = std::find_if(kFunctions.begin(), kFunctions.end(),
                                  [&](const FunctionSpec &f) { return f.name == name; });
  return found == kFunctions.end() ? nullptr : &*found;
}

XlValue invoke(const FunctionSpec &spec, std::span<const XlValue> args, CallContext &context) {
  const Expected<XlValue> result = spec.dispatch(args, context);
  if (!result) {
    return to_cell(result.error());
  }
  return *result;
}

XlValue to_cell(const Error &error) {
  // The kind is prefixed so a cell says what KIND of problem it is before the
  // detail, which is what makes a column of these skimmable.
  return XlValue{std::string(to_string(error.kind)) + ": " + error.message};
}

} // namespace cdsmodel::xl
