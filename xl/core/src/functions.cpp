#include <cdsmodel/xl/functions.hpp>

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/cdsone.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/defaulted.hpp>
#include <cdsmodel/ir_zero_curve.hpp>
#include <cdsmodel/xl/arguments.hpp>
#include <cdsmodel/xl/calendars.hpp>

#include <algorithm>
#include <array>

namespace cdsmodel::xl {
namespace {

constexpr std::string_view kCategory = "CDS Model";

// ------------------------------------------------------- single-cell readers
//
// Small, and each names its own argument in any error. These functions take up
// to twelve cells, so "expected a number" without a field name leaves a user
// comparing twelve cells against a help string.

[[nodiscard]] Expected<Date> read_single_date(const XlValue &cell, std::string_view what) {
  const Expected<double> serial = cell.as_double();
  if (!serial) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + ": expected a date, got " + cell.describe());
  }
  if (*serial != static_cast<double>(static_cast<long>(*serial))) {
    return fail(ErrorKind::bad_argument, std::string(what) +
                                             ": a date must be a whole number of days, got " +
                                             detail::num(*serial));
  }
  // -25569 converts an Excel serial to days since the Unix epoch, which is
  // what `Date` counts. Pinned by a non-circular test.
  return Date{} + std::chrono::days{static_cast<long>(*serial) - 25569};
}

[[nodiscard]] Expected<DayCount> read_day_count(const XlValue &cell, std::string_view what) {
  const Expected<std::string> text = cell.as_string();
  if (!text) {
    return fail(ErrorKind::bad_argument, std::string(what) + ": " + text.error().message);
  }
  const Expected<DayCount> parsed = parse_day_count(*text);
  if (!parsed) {
    return fail(parsed.error().kind,
                std::string(what) + " \"" + *text + "\": " + parsed.error().message);
  }
  return parsed;
}

[[nodiscard]] Expected<BadDayConv> read_bad_day(const XlValue &cell, std::string_view what) {
  const Expected<std::string> text = cell.as_string();
  if (!text) {
    return fail(ErrorKind::bad_argument, std::string(what) + ": " + text.error().message);
  }
  if (text->size() != 1) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + " \"" + *text + "\": expected one of N, F, P, M");
  }
  const Expected<BadDayConv> parsed = bad_day_conv_from_upstream(text->front());
  if (!parsed) {
    return fail(parsed.error().kind,
                std::string(what) + " \"" + *text + "\": " + parsed.error().message);
  }
  return parsed;
}

/// A payment frequency, which upstream states as an interval like `6M` and the
/// library holds as a count per year. Both spellings are accepted because a
/// sheet written against upstream will have the interval form.
[[nodiscard]] Expected<long> read_frequency(const XlValue &cell, std::string_view what) {
  if (const Expected<double> number = cell.as_double(); number) {
    if (*number < 1 || *number > 12) {
      return fail(ErrorKind::bad_argument, std::string(what) +
                                               ": a frequency must be 1 to 12, "
                                               "got " +
                                               detail::num(*number));
    }
    return static_cast<long>(*number);
  }
  const Expected<std::string> text = cell.as_string();
  if (!text) {
    return fail(ErrorKind::bad_argument, std::string(what) +
                                             ": expected a frequency or an interval, got " +
                                             cell.describe());
  }
  const Expected<DateInterval> interval = parse_date_interval(*text);
  if (!interval) {
    return fail(interval.error().kind,
                std::string(what) + " \"" + *text + "\": " + interval.error().message);
  }
  if (interval->unit != PeriodUnit::Months || interval->periods <= 0 ||
      12 % interval->periods != 0) {
    return fail(ErrorKind::unsupported,
                std::string(what) + " \"" + *text +
                    "\": a swap frequency must be a whole number of months dividing 12");
  }
  return 12 / interval->periods;
}

[[nodiscard]] Expected<double> read_recovery(const XlValue &cell) {
  const Expected<double> value = cell.as_double();
  if (!value) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate: expected a number, got " + cell.describe());
  }
  if (!(*value >= 0.0 && *value <= 1.0)) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate " + detail::num(*value) + " is outside 0 to 1");
  }
  return value;
}

[[nodiscard]] Expected<std::string> read_name(const XlValue &cell) {
  const Expected<std::string> text = cell.as_string();
  if (!text) {
    return fail(ErrorKind::bad_argument, "name: " + text.error().message);
  }
  if (text->empty()) {
    return fail(ErrorKind::bad_argument, "name: a stored object needs a name to be "
                                         "referenced by");
  }
  return text;
}

[[nodiscard]] Expected<ZeroCurve> read_curve_argument(const XlValue &cell, CallContext &ctx,
                                                      std::string_view what) {
  const Expected<std::string> handle = cell.as_string();
  if (!handle) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + ": expected a curve handle, got " + cell.describe());
  }
  const Expected<ZeroCurve> curve = ctx.curves.resolve(*handle);
  if (!curve) {
    return fail(curve.error().kind, std::string(what) + ": " + curve.error().message);
  }
  return curve;
}

/// A `Holidays` cell: a calendar handle, or NONE / blank for weekends only.
[[nodiscard]] Expected<Calendar> read_calendar_argument(const XlValue &cell, CallContext &ctx) {
  if (cell.is_missing()) {
    return Calendar::none();
  }
  const Expected<std::string> text = cell.as_string();
  if (!text) {
    return fail(ErrorKind::bad_argument, "Holidays: " + text.error().message);
  }
  if (text->empty() || *text == "NONE" || *text == "None" || *text == "none") {
    return Calendar::none();
  }
  const Expected<Calendar> named = ctx.calendars.resolve(*text);
  if (!named) {
    return fail(named.error().kind, "Holidays: " + named.error().message);
  }
  return named;
}

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
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
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
  const Expected<Conventions> conventions = Conventions::read_block(args[2], ctx.calendars);
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

// --------------------------------------------------------------- calendars
//
// Three ways in, one handle out. The range form is primary (D5): the sheet
// shows what the formula means, it travels with the workbook, and it cannot be
// missing at recalculation time. The file form is interop, because desks share
// NYC.dat centrally.

/// `LoadHolidays(path, name)` -- upstream's signature, unchanged.
Expected<XlValue> dispatch_load_holidays(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 2) {
    return fail(ErrorKind::bad_argument,
                "LoadHolidays takes 2 arguments, got " + detail::num(args.size()));
  }
  const Expected<std::string> path = args[0].as_string();
  if (!path) {
    return fail(ErrorKind::bad_argument, "Filename: " + path.error().message);
  }
  const Expected<std::string> name = args[1].as_string();
  if (!name) {
    return fail(ErrorKind::bad_argument, "Name: " + name.error().message);
  }
  if (name->empty()) {
    return fail(ErrorKind::bad_argument, "Name: a calendar needs a name to be referenced by");
  }
  const Expected<Calendar> calendar = read_calendar_file(*path);
  if (!calendar) {
    return unexpected(calendar.error());
  }
  return XlValue{ctx.calendars.store(*name, *calendar)};
}

/// `CDS.Calendar(dates, weekend, name)` -- from cells. No path, no IO.
Expected<XlValue> dispatch_calendar_from_range(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 3) {
    return fail(ErrorKind::bad_argument,
                "CDS.Calendar takes 3 arguments, got " + detail::num(args.size()));
  }
  const Expected<std::string> name = args[2].as_string();
  if (!name || name->empty()) {
    return fail(ErrorKind::bad_argument, "Name: a calendar needs a name");
  }
  const Expected<Calendar> calendar = calendar_from_range(args[0], args[1]);
  if (!calendar) {
    return unexpected(calendar.error());
  }
  return XlValue{ctx.calendars.store(*name, *calendar)};
}

/// `CDS.CalendarFromFile(path, name)` -- the grouped-form equivalent, which
/// differs from LoadHolidays only in taking its arguments in the order a
/// reader expects. Kept separate rather than aliased so the help text can say
/// which is upstream's.
Expected<XlValue> dispatch_calendar_from_file(std::span<const XlValue> args, CallContext &ctx) {
  return dispatch_load_holidays(args, ctx);
}

/// `CDS.CalendarDates(handle)` -- spills a calendar's dates into cells.
Expected<XlValue> dispatch_calendar_dates(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 1) {
    return fail(ErrorKind::bad_argument,
                "CDS.CalendarDates takes 1 argument, got " + detail::num(args.size()));
  }
  const Expected<std::string> handle = args[0].as_string();
  if (!handle) {
    return fail(ErrorKind::bad_argument, "Calendar: expected a handle, got " + args[0].describe());
  }
  const Expected<Calendar> calendar = ctx.calendars.resolve(*handle);
  if (!calendar) {
    return unexpected(calendar.error());
  }
  return calendar_dates_grid(*calendar);
}

// ------------------------------------------------------------- curve building
//
// These are what produce the handles everything else consumes. Each builds a
// ZeroCurve from cells and stores it, so a sheet's curves live in named cells
// instead of in a process-wide cache keyed by string, which is what upstream
// had and what UPSTREAM-37's neighbour made dangerous.

/// `IRZeroCurveMake(base, dates, rates, basis, dcc, name)` -- a curve stated
/// directly, point by point. Upstream's `JpmcdsMakeTCurve` behind it, minus
/// the TDate-0 terminator that silently truncates a point list.
Expected<XlValue> dispatch_ir_zero_curve_make(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 6) {
    return fail(ErrorKind::bad_argument,
                "IRZeroCurveMake takes 6 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> base = read_single_date(args[0], "Base Date");
  if (!base) {
    return unexpected(base.error());
  }
  const Expected<std::vector<Date>> dates = read_dates(args[1], "Dates");
  if (!dates) {
    return unexpected(dates.error());
  }
  const Expected<std::vector<double>> rates = read_numbers(args[2], "Rates");
  if (!rates) {
    return unexpected(rates.error());
  }
  if (dates->size() != rates->size()) {
    return fail(ErrorKind::bad_argument, "Dates has " + detail::num(dates->size()) +
                                             " entries and Rates has " +
                                             detail::num(rates->size()) + "; they must match");
  }
  const Expected<double> basis = args[3].as_double();
  if (!basis) {
    return fail(ErrorKind::bad_argument, "Basis: " + basis.error().message);
  }
  const Expected<DayCount> dcc = read_day_count(args[4], "DCC");
  if (!dcc) {
    return unexpected(dcc.error());
  }
  const Expected<std::string> name = read_name(args[5]);
  if (!name) {
    return unexpected(name.error());
  }

  std::vector<RatePoint> points;
  points.reserve(dates->size());
  for (std::size_t i = 0; i < dates->size(); ++i) {
    points.push_back(RatePoint{(*dates)[i], (*rates)[i]});
  }
  const Expected<ZeroCurve> curve = ZeroCurve::create(*base, points, *basis, *dcc);
  if (!curve) {
    return unexpected(curve.error());
  }
  return XlValue{ctx.curves.store(*name, *curve)};
}

/// `IRZeroCurveBuild(value, types, ends, rates, mmdcc, fixedIvl, floatIvl,
///                   fixedDcc, floatDcc, swapBdc, holidays, name)`
/// -- bootstrapped from money-market and swap quotes.
Expected<XlValue> dispatch_ir_zero_curve_build(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 12) {
    return fail(ErrorKind::bad_argument,
                "IRZeroCurveBuild takes 12 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> value_date = read_single_date(args[0], "Value Date");
  if (!value_date) {
    return unexpected(value_date.error());
  }
  const Expected<std::vector<std::string>> types = read_strings(args[1], "Types");
  if (!types) {
    return unexpected(types.error());
  }
  const Expected<std::vector<Date>> ends = read_dates(args[2], "End Dates");
  if (!ends) {
    return unexpected(ends.error());
  }
  const Expected<std::vector<double>> rates = read_numbers(args[3], "Rates");
  if (!rates) {
    return unexpected(rates.error());
  }
  if (types->size() != ends->size() || types->size() != rates->size()) {
    return fail(ErrorKind::bad_argument,
                "Types, End Dates and Rates must be the same length -- got " +
                    detail::num(types->size()) + ", " + detail::num(ends->size()) + " and " +
                    detail::num(rates->size()));
  }

  std::vector<InstrumentQuote> instruments;
  instruments.reserve(types->size());
  for (std::size_t i = 0; i < types->size(); ++i) {
    const std::string &code = (*types)[i];
    if (code.size() != 1) {
      return fail(ErrorKind::bad_argument, "Types row " + detail::num(i + 1) + ": \"" + code +
                                               "\" -- expected M (money market) or S (swap)");
    }
    const Expected<InstrumentKind> kind = instrument_kind_from_code(code.front());
    if (!kind) {
      return fail(kind.error().kind, "Types row " + detail::num(i + 1) + ": \"" + code + "\" -- " +
                                         kind.error().message);
    }
    instruments.push_back(InstrumentQuote{*kind, (*ends)[i], (*rates)[i]});
  }

  const Expected<DayCount> mm_dcc = read_day_count(args[4], "MMDCC");
  if (!mm_dcc) {
    return unexpected(mm_dcc.error());
  }

  SwapConventions conventions;
  const Expected<long> fixed_freq = read_frequency(args[5], "Fixed IVL");
  if (!fixed_freq) {
    return unexpected(fixed_freq.error());
  }
  conventions.fixed_frequency = *fixed_freq;
  const Expected<long> float_freq = read_frequency(args[6], "Float IVL");
  if (!float_freq) {
    return unexpected(float_freq.error());
  }
  conventions.float_frequency = *float_freq;
  const Expected<DayCount> fixed_dcc = read_day_count(args[7], "Fixed DCC");
  if (!fixed_dcc) {
    return unexpected(fixed_dcc.error());
  }
  conventions.fixed_day_count = *fixed_dcc;
  const Expected<DayCount> float_dcc = read_day_count(args[8], "Float DCC");
  if (!float_dcc) {
    return unexpected(float_dcc.error());
  }
  conventions.float_day_count = *float_dcc;
  const Expected<BadDayConv> bdc = read_bad_day(args[9], "Swap BDC");
  if (!bdc) {
    return unexpected(bdc.error());
  }
  conventions.bad_day = *bdc;

  const Expected<Calendar> calendar = read_calendar_argument(args[10], ctx);
  if (!calendar) {
    return unexpected(calendar.error());
  }
  const Expected<std::string> name = read_name(args[11]);
  if (!name) {
    return unexpected(name.error());
  }

  const Expected<ZeroCurve> curve =
      build_ir_zero_curve(*value_date, instruments, *mm_dcc, conventions, *calendar);
  if (!curve) {
    return unexpected(curve.error());
  }
  return XlValue{ctx.curves.store(*name, *curve)};
}

/// `DiscountFactor(curve, date)` -- the only read-only curve function that
/// returns a single number.
Expected<XlValue> dispatch_discount_factor(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 2) {
    return fail(ErrorKind::bad_argument,
                "DiscountFactor takes 2 arguments, got " + detail::num(args.size()));
  }
  const Expected<ZeroCurve> curve = read_curve_argument(args[0], ctx, "Curve");
  if (!curve) {
    return unexpected(curve.error());
  }
  const Expected<Date> date = read_single_date(args[1], "Date");
  if (!date) {
    return unexpected(date.error());
  }
  // `zero_price` returns a plain double because ZeroCurve::create refuses
  // every curve that could produce a NaN -- see UPSTREAM-2.
  return XlValue{curve->zero_price(*date)};
}

/// `DatesAndRates(curve)` -- the curve's points, as two columns.
Expected<XlValue> dispatch_dates_and_rates(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 1) {
    return fail(ErrorKind::bad_argument,
                "DatesAndRates takes 1 argument, got " + detail::num(args.size()));
  }
  const Expected<ZeroCurve> curve = read_curve_argument(args[0], ctx, "Curve");
  if (!curve) {
    return unexpected(curve.error());
  }
  return rate_points_grid(curve->points());
}

/// `CleanSpreadCurveBuild(today, start, stepin, cashSettle, ends, coupons,
///                         includeFlags, payAcc, interval, stub, dcc, bdc,
///                         holidays, discount, recovery, name)`
///
/// The credit curve, bootstrapped from benchmark CDS quotes. Sixteen arguments
/// in upstream's order; the conventions among them are the same six that
/// `CdsPrice` takes, which is the repetition artifact #4's grouped forms exist
/// to remove -- a grouped `CDS.SpreadCurve` arrives with the other grouped
/// forms rather than being invented here on its own.
Expected<XlValue> dispatch_clean_spread_curve_build(std::span<const XlValue> args,
                                                    CallContext &ctx) {
  if (args.size() != 16) {
    return fail(ErrorKind::bad_argument,
                "CleanSpreadCurveBuild takes 16 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> today = read_single_date(args[0], "Today");
  if (!today) {
    return unexpected(today.error());
  }
  const Expected<Date> start = read_single_date(args[1], "Start Date");
  if (!start) {
    return unexpected(start.error());
  }
  const Expected<Date> stepin = read_single_date(args[2], "Stepin Date");
  if (!stepin) {
    return unexpected(stepin.error());
  }
  const Expected<Date> cash_settle = read_single_date(args[3], "CashSettle Date");
  if (!cash_settle) {
    return unexpected(cash_settle.error());
  }

  const Expected<std::vector<Date>> ends = read_dates(args[4], "End Dates");
  if (!ends) {
    return unexpected(ends.error());
  }
  const Expected<std::vector<double>> coupons = read_numbers(args[5], "Coupon Rates");
  if (!coupons) {
    return unexpected(coupons.error());
  }
  if (ends->size() != coupons->size()) {
    return fail(ErrorKind::bad_argument, "End Dates has " + detail::num(ends->size()) +
                                             " entries and Coupon Rates has " +
                                             detail::num(coupons->size()) + "; they must match");
  }

  // Include Flags is optional: upstream documents NULL as "include all", and
  // a blank cell is this interface's NULL.
  std::vector<bool> include(ends->size(), true);
  if (!args[6].is_missing()) {
    const Expected<XlGrid> flags = args[6].as_grid();
    if (!flags) {
      return fail(ErrorKind::bad_argument,
                  "Include Flags: expected a range, got " + args[6].describe());
    }
    std::vector<bool> read;
    for (const XlValue &cell : flags->cells) {
      if (cell.is_missing()) {
        continue;
      }
      const Expected<bool> flag = cell.as_bool();
      if (!flag) {
        return fail(ErrorKind::bad_argument, "Include Flags row " + detail::num(read.size() + 1) +
                                                 ": " + flag.error().message);
      }
      read.push_back(*flag);
    }
    if (!read.empty()) {
      if (read.size() != ends->size()) {
        return fail(ErrorKind::bad_argument,
                    "Include Flags has " + detail::num(read.size()) +
                        " entries and End Dates has " + detail::num(ends->size()) +
                        "; leave Include Flags blank to include every benchmark");
      }
      include = read;
    }
  }

  const std::array<XlValue, 6> convention_cells{args[7],  args[8],  args[9],
                                                args[10], args[11], args[12]};
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
  if (!conventions) {
    return unexpected(conventions.error());
  }
  const Expected<ZeroCurve> discount = read_curve_argument(args[13], ctx, "Discount Curve");
  if (!discount) {
    return unexpected(discount.error());
  }
  const Expected<double> recovery = args[14].as_double();
  if (!recovery) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate: expected a number, got " + args[14].describe());
  }
  if (!(*recovery >= 0.0 && *recovery <= 1.0)) {
    return fail(ErrorKind::bad_argument,
                "Recovery Rate " + detail::num(*recovery) + " is outside 0 to 1");
  }
  const Expected<std::string> name = read_name(args[15]);
  if (!name) {
    return unexpected(name.error());
  }

  std::vector<Benchmark> benchmarks;
  benchmarks.reserve(ends->size());
  for (std::size_t i = 0; i < ends->size(); ++i) {
    benchmarks.push_back(Benchmark{(*ends)[i], (*coupons)[i], include[i]});
  }

  // The conventions travel as a `Cds` because that is what the library takes;
  // its start and end are unused by the bootstrap, which reads each
  // benchmark's own dates.
  const Cds convention_carrier = make_cds(TradeTerms{*start, (*ends).back(), 0.0}, *conventions);
  const Expected<ZeroCurve> curve =
      clean_spread_curve(*today, *discount, *start, *stepin, *cash_settle, benchmarks, *recovery,
                         convention_carrier, conventions->holidays);
  if (!curve) {
    return unexpected(curve.error());
  }
  return XlValue{ctx.curves.store(*name, *curve)};
}

// --------------------------------------------------------------- pricing
//
// The last five. Each reads into the same four blocks `CdsPrice` uses, so
// there is nothing new here beyond their argument orders -- which are
// upstream's, and are the reason artifact #4's grouped forms exist.

/// `ParSpreads(today, stepin, start, ends, payAcc, interval, stub, dcc, bdc,
///             holidays, discount, spread, recovery)`
///
/// One par spread per maturity, as a column. Upstream takes a list of end
/// dates and returns a list.
Expected<XlValue> dispatch_par_spreads(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 13) {
    return fail(ErrorKind::bad_argument,
                "ParSpreads takes 13 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> today = read_single_date(args[0], "Today");
  if (!today) {
    return unexpected(today.error());
  }
  const Expected<Date> stepin = read_single_date(args[1], "StepIn Date");
  if (!stepin) {
    return unexpected(stepin.error());
  }
  const Expected<Date> start = read_single_date(args[2], "Start Date");
  if (!start) {
    return unexpected(start.error());
  }
  const Expected<std::vector<Date>> ends = read_dates(args[3], "End Dates");
  if (!ends) {
    return unexpected(ends.error());
  }
  const std::array<XlValue, 6> convention_cells{args[4], args[5], args[6],
                                                args[7], args[8], args[9]};
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
  if (!conventions) {
    return unexpected(conventions.error());
  }
  const Expected<ZeroCurve> discount = read_curve_argument(args[10], ctx, "Discount Curve");
  if (!discount) {
    return unexpected(discount.error());
  }
  const Expected<ZeroCurve> spread = read_curve_argument(args[11], ctx, "Spread Curve");
  if (!spread) {
    return unexpected(spread.error());
  }
  const Expected<double> recovery = read_recovery(args[12]);
  if (!recovery) {
    return unexpected(recovery.error());
  }

  XlGrid out;
  out.rows = ends->size();
  out.columns = 1;
  out.cells.reserve(out.rows);
  for (std::size_t i = 0; i < ends->size(); ++i) {
    const Cds cds = make_cds(TradeTerms{*start, (*ends)[i], 0.0}, *conventions);
    const Expected<double> par =
        cds_par_spread(cds, *today, *stepin, conventions->holidays, *discount, *spread, *recovery);
    // One bad maturity does not lose the whole column: the failing row shows
    // its own message and the rest still price. A column where one cell is
    // wrong and twelve are right is more useful than one error.
    if (!par) {
      out.cells.emplace_back(std::string(to_string(par.error().kind)) + ": " + par.error().message);
    } else {
      out.cells.emplace_back(*par);
    }
  }
  return XlValue{out};
}

/// The seven cells `ParSpreadFlat` and `UpfrontFlat` share, read once.
struct FlatInputs {
  UpfrontDates dates;
  Cds cds;
  std::vector<Calendar> holidays;
  ZeroCurve discount;
  double recovery = 0.0;
  bool clean = true;
};

[[nodiscard]] Expected<FlatInputs> read_flat_inputs(std::span<const XlValue> args,
                                                    CallContext &ctx) {
  const Expected<Date> today = read_single_date(args[0], "Today");
  if (!today) {
    return unexpected(today.error());
  }
  const Expected<Date> cash_settle = read_single_date(args[1], "CashSettle Date");
  if (!cash_settle) {
    return unexpected(cash_settle.error());
  }
  const Expected<Date> benchmark_start = read_single_date(args[2], "Benchmark St Date");
  if (!benchmark_start) {
    return unexpected(benchmark_start.error());
  }
  const Expected<Date> stepin = read_single_date(args[3], "StepIn Date");
  if (!stepin) {
    return unexpected(stepin.error());
  }
  const Expected<Date> start = read_single_date(args[4], "Start Date");
  if (!start) {
    return unexpected(start.error());
  }
  const Expected<Date> end = read_single_date(args[5], "End Date");
  if (!end) {
    return unexpected(end.error());
  }
  const Expected<double> coupon = args[6].as_double();
  if (!coupon) {
    return fail(ErrorKind::bad_argument,
                "Coupon Rate: expected a number, got " + args[6].describe());
  }
  const std::array<XlValue, 6> convention_cells{args[7],  args[8],  args[9],
                                                args[10], args[11], args[12]};
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
  if (!conventions) {
    return unexpected(conventions.error());
  }
  const Expected<ZeroCurve> discount = read_curve_argument(args[13], ctx, "Discount Curve");
  if (!discount) {
    return unexpected(discount.error());
  }
  const Expected<double> recovery = read_recovery(args[15]);
  if (!recovery) {
    return unexpected(recovery.error());
  }
  const Expected<bool> clean = args[16].as_bool();
  if (!clean) {
    return fail(ErrorKind::bad_argument, "Clean Price: " + clean.error().message);
  }

  FlatInputs out{UpfrontDates{*today, *cash_settle, *benchmark_start, *stepin},
                 make_cds(TradeTerms{*start, *end, *coupon}, *conventions),
                 conventions->holidays,
                 *discount,
                 *recovery,
                 *clean};
  return out;
}

/// `UpfrontFlat(..., marketParSpread, recovery, clean)` -- the upfront charge
/// implied by a quoted par spread.
Expected<XlValue> dispatch_upfront_flat(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 17) {
    return fail(ErrorKind::bad_argument,
                "UpfrontFlat takes 17 arguments, got " + detail::num(args.size()));
  }
  const Expected<FlatInputs> in = read_flat_inputs(args, ctx);
  if (!in) {
    return unexpected(in.error());
  }
  const Expected<double> market_spread = args[14].as_double();
  if (!market_spread) {
    return fail(ErrorKind::bad_argument,
                "Market Par Spread: expected a number, got " + args[14].describe());
  }
  const Expected<double> charge = cdsone_upfront_charge(
      in->cds, in->dates, in->holidays, in->discount, *market_spread, in->recovery, in->clean);
  if (!charge) {
    return unexpected(charge.error());
  }
  return XlValue{*charge};
}

/// `ParSpreadFlat(..., upfrontCharge, recovery, clean)` -- the inverse: the
/// flat par spread implied by an upfront charge.
Expected<XlValue> dispatch_par_spread_flat(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 17) {
    return fail(ErrorKind::bad_argument,
                "ParSpreadFlat takes 17 arguments, got " + detail::num(args.size()));
  }
  const Expected<FlatInputs> in = read_flat_inputs(args, ctx);
  if (!in) {
    return unexpected(in.error());
  }
  const Expected<double> upfront = args[14].as_double();
  if (!upfront) {
    return fail(ErrorKind::bad_argument,
                "Upfront Charge: expected a number, got " + args[14].describe());
  }
  const Expected<double> spread = cdsone_spread(in->cds, in->dates, in->holidays, in->discount,
                                                *upfront, in->recovery, in->clean);
  if (!spread) {
    return unexpected(spread.error());
  }
  return XlValue{*spread};
}

/// `DefaultAccrual(trade, edd, effective, maturity, coupon, notional,
///                 interval, stub, dcc, bdc, holidays)`
///
/// Two columns out: the day count and the accrued amount. Upstream returns the
/// day count through an out-parameter as a `double` even though it is always
/// integral; it is a `long` in the library and an integer here.
Expected<XlValue> dispatch_default_accrual(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 11) {
    return fail(ErrorKind::bad_argument,
                "DefaultAccrual takes 11 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> trade = read_single_date(args[0], "Trade Date");
  if (!trade) {
    return unexpected(trade.error());
  }
  const Expected<Date> edd = read_single_date(args[1], "EDD Date");
  if (!edd) {
    return unexpected(edd.error());
  }
  const Expected<Date> effective = read_single_date(args[2], "Effective Date");
  if (!effective) {
    return unexpected(effective.error());
  }
  const Expected<Date> maturity = read_single_date(args[3], "Maturity Date");
  if (!maturity) {
    return unexpected(maturity.error());
  }
  const Expected<double> coupon = args[4].as_double();
  if (!coupon) {
    return fail(ErrorKind::bad_argument,
                "Coupon Rate: expected a number, got " + args[4].describe());
  }
  const Expected<double> notional = args[5].as_double();
  if (!notional) {
    return fail(ErrorKind::bad_argument, "Notional: expected a number, got " + args[5].describe());
  }
  const std::array<XlValue, 6> convention_cells{XlValue{true}, args[6], args[7],
                                                args[8],       args[9], args[10]};
  // Pay-accrual-on-default is not an argument here, so it takes the library's
  // default rather than being invented from a cell that does not exist.
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
  if (!conventions) {
    return unexpected(conventions.error());
  }

  const Cds cds = make_cds(TradeTerms{*effective, *maturity, *coupon}, *conventions);
  const Expected<DefaultAccrual> accrual =
      default_accrual(cds, *trade, *edd, *notional, conventions->holidays);
  if (!accrual) {
    return unexpected(accrual.error());
  }

  XlGrid out;
  out.rows = 1;
  out.columns = 2;
  out.cells.emplace_back(static_cast<double>(accrual->days));
  out.cells.emplace_back(accrual->amount);
  return XlValue{out};
}

/// `FeeLegFlows(start, end, coupon, notional, interval, stub, dcc, bdc,
///              holidays)`
///
/// Three columns: accrual start, accrual end, pay date -- and the amount.
Expected<XlValue> dispatch_fee_leg_flows(std::span<const XlValue> args, CallContext &ctx) {
  if (args.size() != 9) {
    return fail(ErrorKind::bad_argument,
                "FeeLegFlows takes 9 arguments, got " + detail::num(args.size()));
  }
  const Expected<Date> start = read_single_date(args[0], "Start Date");
  if (!start) {
    return unexpected(start.error());
  }
  const Expected<Date> end = read_single_date(args[1], "End Date");
  if (!end) {
    return unexpected(end.error());
  }
  const Expected<double> coupon = args[2].as_double();
  if (!coupon) {
    return fail(ErrorKind::bad_argument,
                "Coupon Rate: expected a number, got " + args[2].describe());
  }
  const Expected<double> notional = args[3].as_double();
  if (!notional) {
    return fail(ErrorKind::bad_argument, "Notional: expected a number, got " + args[3].describe());
  }
  const std::array<XlValue, 6> convention_cells{XlValue{true}, args[4], args[5],
                                                args[6],       args[7], args[8]};
  const Expected<Conventions> conventions =
      Conventions::read_positional(convention_cells, ctx.calendars);
  if (!conventions) {
    return unexpected(conventions.error());
  }

  const Cds cds = make_cds(TradeTerms{*start, *end, *coupon}, *conventions);
  const Expected<std::vector<CashFlow>> flows =
      cds_fee_leg_flows(cds, *notional, conventions->holidays);
  if (!flows) {
    return unexpected(flows.error());
  }

  XlGrid out;
  out.rows = flows->empty() ? 1 : flows->size();
  out.columns = 2;
  out.cells.reserve(out.rows * 2);
  if (flows->empty()) {
    out.cells.emplace_back(std::string("the fee leg has no payments"));
    out.cells.emplace_back(XlValue{});
    return XlValue{out};
  }
  for (const CashFlow &f : *flows) {
    out.cells.emplace_back(to_excel_serial(f.date));
    out.cells.emplace_back(f.amount);
  }
  return XlValue{out};
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

constexpr std::array<ParamSpec, 2> kLoadHolidaysParams{
    {{"Filename", "Path to a holiday file (one YYYYMMDD per line)"},
     {"Name", "Name to reference this calendar by"}}};

constexpr std::array<ParamSpec, 3> kCalendarRangeParams{
    {{"Dates", "A range of holiday dates, in any order"},
     {"Weekend", "STANDARD, NONE, or a comma-separated list of day names"},
     {"Name", "Name to reference this calendar by"}}};

constexpr std::array<ParamSpec, 1> kCalendarDatesParams{
    {{"Calendar", "A handle from LoadHolidays, CDS.Calendar or CDS.CalendarFromFile"}}};

constexpr std::array<ParamSpec, 6> kIRZeroCurveMakeParams{
    {{"Base Date", "The curve's base date; discount factors are 1.0 here"},
     {"Dates", "A range of dates, one per point, strictly increasing"},
     {"Rates", "A range of zero rates, one per date"},
     {"Basis", "Compounding periods per year, or 5000 for continuous"},
     {"DCC", "Day count for the rates, e.g. ACT/365F"},
     {"Name", "Name to reference this curve by"}}};

constexpr std::array<ParamSpec, 12> kIRZeroCurveBuildParams{
    {{"Value Date", "Today; the curve is built forward from here"},
     {"Types", "One per instrument: M for money market, S for swap"},
     {"End Dates", "Maturity of each instrument"},
     {"Rates", "Quoted rate of each instrument"},
     {"MMDCC", "Day count for the money-market instruments"},
     {"Fixed IVL", "Fixed-leg frequency, as a count per year or an interval like 6M"},
     {"Float IVL", "Floating-leg frequency, same form"},
     {"Fixed DCC", "Day count for the fixed leg"},
     {"Float DCC", "Day count for the floating leg"},
     {"Swap BDC", "Bad day convention for the swap dates: N, F, P or M"},
     {"Holidays", "A calendar handle, or NONE for weekends only"},
     {"Name", "Name to reference this curve by"}}};

constexpr std::array<ParamSpec, 16> kCleanSpreadCurveBuildParams{
    {{"Today", "Risk starts at the end of today"},
     {"Start Date", "When protection begins on the benchmarks"},
     {"Stepin Date", "When step-in begins"},
     {"CashSettle Date", "Date the benchmark quotes settle"},
     {"End Dates", "Maturity of each benchmark CDS"},
     {"Coupon Rates", "Quoted spread of each benchmark"},
     {"Include Flags", "TRUE/FALSE per benchmark; leave blank to include all"},
     {"Pay Acc On Default", "Should accrued interest be paid on default?"},
     {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
     {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
     {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
     {"Bad Day Convention", "N, F, P or M"},
     {"Holidays", "A calendar handle, or NONE for weekends only"},
     {"Discount Curve", "Handle from IRZeroCurveBuild or IRZeroCurveMake"},
     {"Recovery Rate", "Assumed recovery, 0 to 1"},
     {"Name", "Name to reference this curve by"}}};

constexpr std::array<ParamSpec, 13> kParSpreadsParams{
    {{"Today", "Risk starts at the end of today"},
     {"StepIn Date", "Date when step-in begins"},
     {"Start Date", "Date when protection begins"},
     {"End Dates", "A range of maturities; one par spread comes back per row"},
     {"Pay Acc On Default", "Should accrued interest be paid on default?"},
     {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
     {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
     {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
     {"Bad Day Convention", "N, F, P or M"},
     {"Holidays", "A calendar handle, or NONE for weekends only"},
     {"Discount Curve", "Handle from IRZeroCurveBuild or IRZeroCurveMake"},
     {"Spread Curve", "Handle from CleanSpreadCurveBuild"},
     {"Recovery Rate", "Assumed recovery, 0 to 1"}}};

constexpr std::array<ParamSpec, 17> kUpfrontFlatParams{
    {{"Today", "Risk starts at the end of today"},
     {"CashSettle Date", "Date for which the PV is calculated"},
     {"Benchmark St Date", "Start date of the synthetic benchmark CDS"},
     {"StepIn Date", "Date when step-in begins"},
     {"Start Date", "Date when protection begins"},
     {"End Date", "Date when protection ends"},
     {"Coupon Rate", "Fixed coupon rate for the fee leg"},
     {"Pay Acc On Default", "Should accrued interest be paid on default?"},
     {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
     {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
     {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
     {"Bad Day Convention", "N, F, P or M"},
     {"Holidays", "A calendar handle, or NONE for weekends only"},
     {"Discount Curve", "Handle from IRZeroCurveBuild or IRZeroCurveMake"},
     {"Market Par Spread", "Quoted par spread to imply the flat curve from"},
     {"Recovery Rate", "Assumed recovery, 0 to 1"},
     {"Clean Price", "TRUE for clean, FALSE for dirty"}}};

constexpr std::array<ParamSpec, 17> kParSpreadFlatParams{
    {{"Today", "Risk starts at the end of today"},
     {"CashSettle Date", "Date for which the PV is calculated"},
     {"Benchmark St Date", "Start date of the synthetic benchmark CDS"},
     {"StepIn Date", "Date when step-in begins"},
     {"Start Date", "Date when protection begins"},
     {"End Date", "Date when protection ends"},
     {"Coupon Rate", "Fixed coupon rate for the fee leg"},
     {"Pay Acc On Default", "Should accrued interest be paid on default?"},
     {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
     {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
     {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
     {"Bad Day Convention", "N, F, P or M"},
     {"Holidays", "A calendar handle, or NONE for weekends only"},
     {"Discount Curve", "Handle from IRZeroCurveBuild or IRZeroCurveMake"},
     {"Upfront Charge", "Present value of the CDS to imply the spread from"},
     {"Recovery Rate", "Assumed recovery, 0 to 1"},
     {"Clean Price", "TRUE for clean, FALSE for dirty"}}};

constexpr std::array<ParamSpec, 11> kDefaultAccrualParams{{
    {"Trade Date", "When the trade was struck"},
    {"EDD Date", "Event determination date"},
    {"Effective Date", "When protection began"},
    {"Maturity Date", "When protection ends"},
    {"Coupon Rate", "Fixed coupon rate for the fee leg"},
    {"Notional", "Trade notional"},
    {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
    {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
    {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
    {"Bad Day Convention", "N, F, P or M"},
    {"Holidays", "A calendar handle, or NONE for weekends only"},
}};

constexpr std::array<ParamSpec, 9> kFeeLegFlowsParams{{
    {"Start Date", "When protection begins"},
    {"End Date", "When protection ends"},
    {"Coupon Rate", "Fixed coupon rate for the fee leg"},
    {"Notional", "Trade notional"},
    {"Coupon Interval", "Interval between coupon payments, e.g. 3M"},
    {"Stub Type", "Where off-cycle coupon dates go, e.g. f/s"},
    {"Payment DCC", "Day count for coupon payment, e.g. ACT/360"},
    {"Bad Day Convention", "N, F, P or M"},
    {"Holidays", "A calendar handle, or NONE for weekends only"},
}};

constexpr std::array<ParamSpec, 2> kDiscountFactorParams{
    {{"Curve", "A curve handle"}, {"Date", "The date to discount to"}}};

constexpr std::array<ParamSpec, 1> kDatesAndRatesParams{{{"Curve", "A curve handle"}}};

constexpr std::array<FunctionSpec, 16> kFunctions{
    {{"CdsPrice", "UUUUUUUUUUUUUUUUU", kCdsPriceLegacyParams,
      "Prices a CDS. The original 16-argument form, kept so existing "
      "workbooks keep working; CDS.Price is the same calculation with the "
      "arguments grouped.",
      kCategory, &dispatch_cds_price_legacy},
     {"CDS.Price", "UUUUUU", kCdsPriceGroupedParams,
      "Prices a CDS from four labelled ranges. Identical to CdsPrice, with "
      "the six convention values written once instead of per formula.",
      kCategory, &dispatch_cds_price_grouped},
     {"LoadHolidays", "UUU", kLoadHolidaysParams,
      "Reads a holiday file and returns a handle. Upstream's signature, "
      "unchanged.",
      kCategory, &dispatch_load_holidays},
     {"CDS.Calendar", "UUUU", kCalendarRangeParams,
      "Builds a calendar from a range of dates in the sheet. No file, no "
      "path, and the holidays are visible where they are used.",
      kCategory, &dispatch_calendar_from_range},
     {"CDS.CalendarFromFile", "UUU", kLoadHolidaysParams,
      "Reads a holiday file and returns a handle. Same as LoadHolidays.", kCategory,
      &dispatch_calendar_from_file},
     {"CDS.CalendarDates", "UU", kCalendarDatesParams,
      "The dates in a calendar, as a column. Use it to pull a holiday file "
      "into a sheet once and drop the file dependency.",
      kCategory, &dispatch_calendar_dates},
     {"IRZeroCurveMake", "UUUUUUU", kIRZeroCurveMakeParams,
      "Builds an interest-rate curve from dates and rates stated directly, "
      "and returns a handle.",
      kCategory, &dispatch_ir_zero_curve_make},
     {"IRZeroCurveBuild", "UUUUUUUUUUUUU", kIRZeroCurveBuildParams,
      "Bootstraps an interest-rate curve from money-market and swap quotes, "
      "and returns a handle.",
      kCategory, &dispatch_ir_zero_curve_build},
     {"DiscountFactor", "UUU", kDiscountFactorParams, "The discount factor from a curve to a date.",
      kCategory, &dispatch_discount_factor},
     {"CleanSpreadCurveBuild", "UUUUUUUUUUUUUUUUU", kCleanSpreadCurveBuildParams,
      "Bootstraps a clean spread (credit) curve from benchmark CDS quotes, "
      "and returns a handle.",
      kCategory, &dispatch_clean_spread_curve_build},
     {"ParSpreads", "UUUUUUUUUUUUUU", kParSpreadsParams,
      "The par spread of a CDS at each of several maturities, as a column.", kCategory,
      &dispatch_par_spreads},
     {"UpfrontFlat", "UUUUUUUUUUUUUUUUUU", kUpfrontFlatParams,
      "The upfront charge implied by a quoted par spread, through a flat "
      "credit curve.",
      kCategory, &dispatch_upfront_flat},
     {"ParSpreadFlat", "UUUUUUUUUUUUUUUUUU", kParSpreadFlatParams,
      "The flat par spread implied by an upfront charge. The inverse of "
      "UpfrontFlat.",
      kCategory, &dispatch_par_spread_flat},
     {"DefaultAccrual", "UUUUUUUUUUUU", kDefaultAccrualParams,
      "Accrued interest owed on a defaulted CDS: the day count and the "
      "amount, as two columns.",
      kCategory, &dispatch_default_accrual},
     {"FeeLegFlows", "UUUUUUUUUU", kFeeLegFlowsParams,
      "The fee leg's cash flows: pay date and amount, one row each.", kCategory,
      &dispatch_fee_leg_flows},
     {"DatesAndRates", "UU", kDatesAndRatesParams,
      "A curve's points, as two columns: dates then rates.", kCategory,
      &dispatch_dates_and_rates}}};

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
