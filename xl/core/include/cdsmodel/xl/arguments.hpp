#pragma once

/// The shape of a CDS pricing call, and the two ways a sheet can express it.
///
/// This is the design that keeps the signatures honest. Upstream's `CdsPrice`
/// takes 16 positional arguments and `UpfrontFlat` takes 17; across 8 priced
/// functions there are 111 arguments covering ~38 distinct concepts, because
/// the same four groups are flattened into every formula. The groups are:
///
///     Valuation     when we are pricing, and for what settlement   (3)
///     TradeTerms    what was traded                                (3)
///     Conventions   how the market computes it                     (6)
///     MarketData    what the market says today                     (3)
///
/// **Both registered forms read into these same four structs.** The 16-argument
/// form reads them from positional cells; the grouped form reads them from
/// labelled ranges. Neither can drift from the other, because after the read
/// there is only one code path -- which is what makes "both forms agree"
/// structural rather than something a test has to keep catching.

#include <cdsmodel/cds.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/xl/handles.hpp>
#include <cdsmodel/xl/labelled_range.hpp>
#include <cdsmodel/xl/value.hpp>

#include <span>
#include <vector>

namespace cdsmodel::xl {

/// When the valuation happens. Three dates that upstream keeps apart for good
/// reasons -- see the notes on `cds_price`.
struct Valuation {
  Date today;
  Date cash_settle;
  Date step_in;

  static constexpr std::array<LabelSpec, 3> kLabels{
      {{"Today", true}, {"Cash Settle Date", true}, {"Step In Date", true}}};

  /// Three consecutive cells, in upstream's order.
  [[nodiscard]] static Expected<Valuation> read_positional(std::span<const XlValue> cells);
  [[nodiscard]] static Expected<Valuation> read_block(const XlValue &block);
};

/// What was traded. The part that genuinely varies per row of a sheet.
struct TradeTerms {
  Date start;
  Date end;
  double coupon_rate = 0.0;

  static constexpr std::array<LabelSpec, 3> kLabels{
      {{"Start Date", true}, {"End Date", true}, {"Coupon Rate", true}}};

  [[nodiscard]] static Expected<TradeTerms> read_positional(std::span<const XlValue> cells);
  [[nodiscard]] static Expected<TradeTerms> read_block(const XlValue &block);
};

/// How the market computes it. Six values a desk sets once and upstream makes
/// you retype into every formula: `Holidays` appears in 8 of 8 priced
/// functions, and four of the others in 7 of 8.
struct Conventions {
  bool pay_accrual_on_default = true;
  DateInterval coupon_interval{3, PeriodUnit::Months};
  StubMethod stub{};
  DayCount day_count = DayCount::Act360;
  BadDayConv bad_day = BadDayConv::None;
  std::vector<Calendar> holidays{Calendar::none()};

  static constexpr std::array<LabelSpec, 6> kLabels{{{"Pay Accrual On Default", true},
                                                     {"Coupon Interval", true},
                                                     {"Stub Type", true},
                                                     {"Payment DCC", true},
                                                     {"Bad Day Convention", true},
                                                     {"Holidays", false}}};

  [[nodiscard]] static Expected<Conventions> read_positional(std::span<const XlValue> cells);
  [[nodiscard]] static Expected<Conventions> read_block(const XlValue &block);
};

/// What the market says today. The curves arrive as handles, because a cell
/// cannot hold a curve.
struct MarketData {
  ZeroCurve discount;
  ZeroCurve spread;
  double recovery = 0.0;

  static constexpr std::array<LabelSpec, 3> kLabels{
      {{"Discount Curve", true}, {"Spread Curve", true}, {"Recovery Rate", true}}};

  [[nodiscard]] static Expected<MarketData> read_positional(std::span<const XlValue> cells,
                                                            const CurveRegistry &curves);
  [[nodiscard]] static Expected<MarketData> read_block(const XlValue &block,
                                                       const CurveRegistry &curves);
};

/// Assembles a `cdsmodel::Cds` from the trade and the conventions. The library
/// type still carries both, which is why this function exists rather than the
/// split living in `cdsmodel` itself -- that is a library API change and its
/// own artifact. The grouping is real at this layer first.
[[nodiscard]] Cds make_cds(const TradeTerms &trade, const Conventions &conventions);

} // namespace cdsmodel::xl
