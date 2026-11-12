#pragma once

// D23: the Python layer mirrors `xl/core`'s argument grouping but defines its
// own types. `cdsmodel::xl`'s blocks include `xl/value.hpp` and
// `xl/handles.hpp`, so binding them would make Python depend on the Excel
// layer -- which the layering in CLAUDE.md forbids. Python sits *beside*
// `xl/core`, not above it: both depend only on `cdsmodel`.
//
// The grouping is deliberately identical, so one mental model serves both
// surfaces. A field added to one must be added to the other by hand.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <vector>

namespace cdsmodel::py {

/// What was traded. Mirrors `cdsmodel::xl::TradeTerms`.
struct TradeTerms {
  Date start;
  Date end;
  double coupon_rate = 0.0;
};

/// When the valuation happens. Mirrors `cdsmodel::xl::Valuation`.
///
/// Three dates upstream keeps apart for good reasons, and three adjacent
/// parameters of the same type -- which is why they are keyword-only.
struct Valuation {
  Date today;
  Date cash_settle;
  Date step_in;
};

/// How the market computes it. Mirrors `cdsmodel::xl::Conventions`.
///
/// Six fields, all defaulted: the one named exemption to the five-parameter
/// cap (D11a). They are the values a desk sets once, which is why upstream
/// made you retype them into all eight priced functions.
struct Conventions {
  bool pay_accrual_on_default = true;
  DateInterval coupon_interval{3, PeriodUnit::Months};
  StubMethod stub{};
  DayCount day_count = DayCount::Act360;
  BadDayConv bad_day = BadDayConv::None;
  std::vector<Calendar> holidays{Calendar::none()};
};

/// What the market says today. Mirrors `cdsmodel::xl::MarketData`.
struct MarketData {
  ZeroCurve discount;
  ZeroCurve spread;
  double recovery = 0.0;
};

/// The same, for the functions quoted on a flat number rather than a curve.
///
/// D11b: `UpfrontFlat` and `ParSpreadFlat` take a flat par spread or an
/// upfront charge instead of a spread *curve*, so the XLL leaves `Discount
/// Curve`, the quote and `Recovery Rate` loose -- seven arguments. Grouping
/// them as `MarketData` is grouped brings both to five. `quote` is named for
/// its role rather than its meaning because the two functions are inverses:
/// it is a par spread for one and an upfront charge for the other.
struct FlatMarket {
  ZeroCurve discount;
  double quote = 0.0;
  double recovery = 0.0;
};

} // namespace cdsmodel::py
