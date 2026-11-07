// TIER 1 -- the rates a curve implies, without an oracle.
//
// Both quantities have a defining property that needs no reference
// implementation. A forward rate must reproduce the discount factors it came
// from; a par coupon must price its own instrument at par. Those are the
// checks that would survive someone rewriting either function, and the ones a
// differential test cannot make -- two implementations can agree on a wrong
// answer, but neither can agree with arithmetic by accident.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/curve_rates.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/curve_rates_cases.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::CashFlow;
using cdsmodel::DateInterval;
using cdsmodel::InterpType;
using cdsmodel::PeriodUnit;
using cdsmodel::RatePoint;
using cdsmodel::StubPosition;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<RatePoint> rising(int count = 8) {
  std::vector<RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(
        RatePoint{*cdsmodel::make_date(2024 + i, 1, 1), 0.02 + 0.003 * static_cast<double>(i)});
  }
  return points;
}

cdsmodel::ZeroCurve curve_at(double basis,
                             cdsmodel::DayCount dcc = cdsmodel::DayCount::Act365Fixed) {
  return *cdsmodel::ZeroCurve::create(d(2024, 1, 1), rising(), basis, dcc);
}

std::vector<cdsmodel::Calendar> weekdays() { return {cdsmodel::Calendar::no_weekends()}; }

// =====================================================================
// The forward rate reproduces the discount factors it came from.
// =====================================================================

TEST(CurveRates, AForwardRateReproducesItsOwnDiscountRatio) {
  // Convert the forward back to a discount factor over the same period: it
  // must equal end/start. That is the definition, and it catches a day count
  // or basis applied inconsistently between the division and the conversion.
  int checked = 0;
  for (const double basis : {1.0, 2.0, 12.0, cdsmodel::kContinuousBasis}) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
      for (const long quote_basis : {0L, 1L, 2L, 4L}) {
        for (const auto &[from, to] :
             {std::pair{d(2025, 1, 1), d(2026, 1, 1)}, std::pair{d(2026, 7, 1), d(2029, 3, 15)}}) {
          const std::optional<double> forward = cdsmodel::forward_from_curve(
              curve, interp, from, to, cdsmodel::DayCount::Act365Fixed, quote_basis);
          ASSERT_TRUE(forward.has_value()) << basis << " " << quote_basis;
          const std::optional<double> back = cdsmodel::rate_to_discount(
              *forward, from, to, cdsmodel::DayCount::Act365Fixed, quote_basis);
          ASSERT_TRUE(back.has_value());

          const std::optional<double> at_start = cdsmodel::discount_date(curve, from, interp);
          const std::optional<double> at_end = cdsmodel::discount_date(curve, to, interp);
          ASSERT_TRUE(at_start.has_value());
          ASSERT_TRUE(at_end.has_value());
          const double ratio = *at_end / *at_start;
          const std::string where =
              "basis " + std::to_string(basis) + " quote " + std::to_string(quote_basis);
          EXPECT_NEAR(*back, ratio, 1.0e-12 * std::max(1.0, std::fabs(ratio))) << where;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 50) << checked;
}

TEST(CurveRates, AForwardOverNoTimeAndTheBackwardOfAForward) {
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  // Start == end: a zero-length period has no rate, and the conversion says so
  // rather than dividing by zero.
  EXPECT_FALSE(cdsmodel::forward_from_curve(curve, InterpType::FlatForwards, d(2026, 1, 1),
                                            d(2026, 1, 1), cdsmodel::DayCount::Act365Fixed, 0)
                   .has_value());
  // A backward forward is legitimate -- upstream's comment says curve
  // construction needs it -- so it must produce a value, not a refusal.
  const std::optional<double> backward =
      cdsmodel::forward_from_curve(curve, InterpType::FlatForwards, d(2029, 1, 1), d(2026, 1, 1),
                                   cdsmodel::DayCount::Act365Fixed, 0);
  EXPECT_TRUE(backward.has_value()) << "a backward forward is a legitimate request";
}

TEST(CurveRates, AForwardOnAFlatCurveIsTheCurveRate) {
  // On a curve that is flat in continuous terms, every forward equals the
  // level. Checked against the level rather than against upstream.
  const std::vector<RatePoint> flat{
      {d(2025, 1, 1), 0.04}, {d(2027, 1, 1), 0.04}, {d(2031, 1, 1), 0.04}};
  const std::optional<cdsmodel::ZeroCurve> curve = cdsmodel::ZeroCurve::create(
      d(2024, 1, 1), flat, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  for (const auto &[from, to] :
       {std::pair{d(2025, 1, 1), d(2026, 1, 1)}, std::pair{d(2026, 1, 1), d(2030, 1, 1)},
        std::pair{d(2025, 6, 1), d(2027, 9, 9)}}) {
    const std::optional<double> forward = cdsmodel::forward_from_curve(
        *curve, InterpType::FlatForwards, from, to, cdsmodel::DayCount::Act365Fixed,
        static_cast<long>(cdsmodel::kContinuousBasis));
    ASSERT_TRUE(forward.has_value());
    EXPECT_NEAR(*forward, 0.04, 1.0e-12)
        << "a flat curve implies a flat forward, at " << cdsmodel::to_tdate(from);
  }
}

// =====================================================================
// The par coupon prices its own instrument at par.
// =====================================================================

TEST(CurveRates, AParCouponPricesItsOwnInstrumentAtPar) {
  // The defining property, and the one worth more than every value comparison
  // in this file: build the instrument with the coupon the function returned,
  // price it off the same curve, and the present value of the coupons plus the
  // final principal must equal the present value of the start date.
  int checked = 0;
  for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
      for (const int periods : {3, 6, 12}) {
        for (const StubPosition stub : {StubPosition::Front, StubPosition::Back}) {
          for (const auto &[start, maturity] : {std::pair{d(2024, 1, 1), d(2029, 1, 1)},
                                                std::pair{d(2024, 2, 20), d(2030, 5, 10)}}) {
            const DateInterval iv{periods, PeriodUnit::Months};
            const std::optional<double> coupon = cdsmodel::par_coupon_rate(
                curve, interp, start, iv, maturity, cdsmodel::DayCount::Act365Fixed, stub,
                cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::None, weekdays());
            ASSERT_TRUE(coupon.has_value()) << basis << " " << periods;

            // Rebuild the instrument at that coupon, with the final principal.
            const cdsmodel::CashFlowConventions terms{.rate = *coupon,
                                                      .day_count = cdsmodel::DayCount::Act365Fixed,
                                                      .stub_length = cdsmodel::StubLength::Short};
            const std::optional<std::vector<CashFlow>> flows = cdsmodel::cash_flows_from_schedule(
                start, maturity, iv, terms, stub, {.add_final = true}, weekdays());
            ASSERT_TRUE(flows.has_value());
            const std::optional<double> priced = cdsmodel::cash_flow_pv(curve, *flows, interp);
            ASSERT_TRUE(priced.has_value());
            const std::optional<double> at_start = cdsmodel::discount_date(curve, start, interp);
            ASSERT_TRUE(at_start.has_value());

            const std::string where = "basis " + std::to_string(basis) + " iv " +
                                      std::to_string(periods) + " stub " +
                                      std::to_string(static_cast<int>(stub)) + " start " +
                                      std::to_string(cdsmodel::to_tdate(start));
            EXPECT_NEAR(*priced, *at_start, 1.0e-10 * std::max(1.0, std::fabs(*at_start)))
                << where << ": the par coupon does not price at par";
            ++checked;
          }
        }
      }
    }
  }
  EXPECT_GT(checked, 60) << checked;
}

TEST(CurveRates, AParCouponOnAFlatCurveIsTheCompoundingEquivalentOfTheLevel) {
  // On a flat curve the par coupon is the level RE-EXPRESSED at the coupon
  // frequency, not the level itself. For an annually-compounded curve at r and
  // a coupon paid f times a year that is f * ((1+r)^(1/f) - 1).
  //
  // The first version of this test asserted "within a basis point of r" and
  // failed at semiannual by 39bp -- which is not noise, it is the whole
  // difference between annual and semiannual compounding. 2 * (1.04^0.5 - 1)
  // is 3.9608%, and the function returned 3.96080%. The assertion was wrong,
  // not the code, and a looser tolerance would have hidden the fact that this
  // test now checks the conversion rather than just the magnitude.
  const double level = 0.04;
  const std::vector<RatePoint> flat{
      {d(2025, 1, 1), level}, {d(2027, 1, 1), level}, {d(2031, 1, 1), level}};
  const std::optional<cdsmodel::ZeroCurve> curve =
      cdsmodel::ZeroCurve::create(d(2024, 1, 1), flat, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());

  for (const int periods : {12, 6, 3}) {
    const double frequency = 12.0 / static_cast<double>(periods);
    const double equivalent = frequency * (std::pow(1.0 + level, 1.0 / frequency) - 1.0);
    const std::optional<double> coupon = cdsmodel::par_coupon_rate(
        *curve, InterpType::FlatForwards, d(2025, 1, 1), DateInterval{periods, PeriodUnit::Months},
        d(2030, 1, 1), cdsmodel::DayCount::Act365Fixed, StubPosition::Front,
        cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::None, weekdays());
    ASSERT_TRUE(coupon.has_value()) << periods;
    // The residual is day-count drift: the schedule falls on actual dates
    // rather than exact fractions of a year, so ACT/365F periods are not all
    // equal. That is tens of microbasis points, not tens of basis points.
    EXPECT_NEAR(*coupon, equivalent, 2.0e-5)
        << "periods " << periods << ": expected the " << frequency << "-times-a-year equivalent of "
        << level;
    // And the direction is fixed: more frequent compounding needs a lower rate.
    if (periods < 12) {
      EXPECT_LT(*coupon, level) << "periods " << periods;
    }
  }
}

TEST(CurveRates, AHigherCurveImpliesAHigherParCoupon) {
  // Monotonicity in the curve level. Nothing enforces it, which is why it is
  // worth asserting: a sign error anywhere in the quotient would pass every
  // value comparison and fail this.
  std::optional<double> previous;
  for (const double level : {0.01, 0.02, 0.04, 0.06, 0.09}) {
    const std::vector<RatePoint> flat{
        {d(2025, 1, 1), level}, {d(2027, 1, 1), level}, {d(2031, 1, 1), level}};
    const std::optional<cdsmodel::ZeroCurve> curve =
        cdsmodel::ZeroCurve::create(d(2024, 1, 1), flat, 1.0, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(curve.has_value()) << level;
    const std::optional<double> coupon = cdsmodel::par_coupon_rate(
        *curve, InterpType::FlatForwards, d(2025, 1, 1), DateInterval{6, PeriodUnit::Months},
        d(2030, 1, 1), cdsmodel::DayCount::Act365Fixed, StubPosition::Front,
        cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::None, weekdays());
    ASSERT_TRUE(coupon.has_value()) << level;
    if (previous) {
      EXPECT_GT(*coupon, *previous) << "level " << level;
    }
    previous = coupon;
  }
}

TEST(CurveRates, TheInputsThatAreRefused) {
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  const DateInterval iv{6, PeriodUnit::Months};
  // An inverted or degenerate span.
  EXPECT_FALSE(cdsmodel::par_coupon_rate(curve, InterpType::FlatForwards, d(2029, 1, 1), iv,
                                         d(2025, 1, 1), cdsmodel::DayCount::Act365Fixed,
                                         StubPosition::Front, cdsmodel::BadDayConv::None,
                                         cdsmodel::BadDayConv::None, weekdays())
                   .has_value());
  EXPECT_FALSE(cdsmodel::par_coupon_rate(curve, InterpType::FlatForwards, d(2026, 1, 1), iv,
                                         d(2026, 1, 1), cdsmodel::DayCount::Act365Fixed,
                                         StubPosition::Front, cdsmodel::BadDayConv::None,
                                         cdsmodel::BadDayConv::None, weekdays())
                   .has_value());
  // A start date before the curve's first date.
  EXPECT_FALSE(cdsmodel::par_coupon_rate(curve, InterpType::FlatForwards, d(2020, 1, 1), iv,
                                         d(2029, 1, 1), cdsmodel::DayCount::Act365Fixed,
                                         StubPosition::Front, cdsmodel::BadDayConv::None,
                                         cdsmodel::BadDayConv::None, weekdays())
                   .has_value())
      << "earlier than min(base date, first point)";
  // And the ordinary case still works, so the refusals are confined.
  EXPECT_TRUE(cdsmodel::par_coupon_rate(curve, InterpType::FlatForwards, d(2025, 1, 1), iv,
                                        d(2029, 1, 1), cdsmodel::DayCount::Act365Fixed,
                                        StubPosition::Front, cdsmodel::BadDayConv::None,
                                        cdsmodel::BadDayConv::None, weekdays())
                  .has_value());
}

TEST(CurveRates, NothingProducesANaN) {
  // UPSTREAM-19's second site, as a property. Upstream returns a NaN forward
  // rate reported as SUCCESS where the two discount factors both underflow or
  // both overflow; the port refuses at the conversion.
  int checked = 0;
  for (const double rate : {-30.0, -24.0, -1.0, 0.0, 0.05, 24.0, 25.0, 30.0}) {
    for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
      const std::vector<RatePoint> points{{d(2054, 1, 1), rate}, {d(2055, 1, 1), rate}};
      const std::optional<cdsmodel::ZeroCurve> curve = cdsmodel::ZeroCurve::create(
          d(2024, 1, 1), points, basis, cdsmodel::DayCount::Act365Fixed);
      if (!curve) {
        continue; // refused on the way in, which is also an answer
      }
      for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
        const std::optional<double> forward = cdsmodel::forward_from_curve(
            *curve, interp, d(2054, 1, 1), d(2055, 1, 1), cdsmodel::DayCount::Act365Fixed, 0);
        if (forward) {
          EXPECT_FALSE(std::isnan(*forward)) << "rate " << rate << " basis " << basis;
        }
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 30) << checked;
}

// =====================================================================
// Replay.
// =====================================================================

namespace {

cdsmodel::ZeroCurve recorded_curve(double basis, long day_count) {
  const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(day_count);
  std::vector<RatePoint> points;
  for (std::size_t i = 0; i < cdsmodel::golden::kCurveRatesPoints; ++i) {
    points.push_back(RatePoint{cdsmodel::from_tdate(cdsmodel::golden::kCurveRatesDates[i]),
                               cdsmodel::golden::kCurveRatesRates[i]});
  }
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(cdsmodel::golden::kCurveRatesBaseDate),
                                      points, basis, *dcc);
}

} // namespace

TEST(CurveRates, MatchesRecordedForwardRates) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kForwardRateCases) {
    const std::optional<cdsmodel::DayCount> quote_dcc =
        cdsmodel::from_upstream_code(c.quote_day_count);
    ASSERT_TRUE(quote_dcc.has_value()) << c.quote_day_count;
    const cdsmodel::ZeroCurve curve = recorded_curve(c.basis, c.curve_day_count);
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    const std::optional<double> got =
        cdsmodel::forward_from_curve(curve, interp, cdsmodel::from_tdate(c.start),
                                     cdsmodel::from_tdate(c.end), *quote_dcc, c.quote_basis);
    const std::string where = "basis " + std::to_string(c.basis) + " interp " +
                              std::to_string(c.interp) + " quote " + std::to_string(c.quote_basis) +
                              " " + std::to_string(c.start);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      EXPECT_NEAR(*got, c.rate,
                  cdsmodel::golden::kCurveRatesTolerance * std::max(1.0, std::fabs(c.rate)))
          << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 600) << checked;
}

TEST(CurveRates, MatchesRecordedParCouponRates) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kParCouponCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.coupon_day_count);
    ASSERT_TRUE(dcc.has_value()) << c.coupon_day_count;
    const std::optional<cdsmodel::BadDayConv> conv =
        cdsmodel::bad_day_conv_from_upstream(c.bad_day);
    ASSERT_TRUE(conv.has_value()) << c.bad_day;
    const cdsmodel::ZeroCurve curve =
        recorded_curve(c.basis, cdsmodel::to_upstream_code(cdsmodel::DayCount::Act365Fixed));
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    const std::optional<double> got = cdsmodel::par_coupon_rate(
        curve, interp, cdsmodel::from_tdate(c.start), DateInterval{c.periods, PeriodUnit::Months},
        cdsmodel::from_tdate(c.maturity), *dcc,
        c.stub_at_end ? StubPosition::Back : StubPosition::Front, *conv, *conv,
        *conv == cdsmodel::BadDayConv::None ? weekdays() : std::vector{cdsmodel::Calendar::none()});
    const std::string where = "basis " + std::to_string(c.basis) + " iv " +
                              std::to_string(c.periods) + " start " + std::to_string(c.start) +
                              " bdc " + std::to_string(c.bad_day);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      EXPECT_NEAR(*got, c.rate,
                  cdsmodel::golden::kCurveRatesTolerance * std::max(1.0, std::fabs(c.rate)))
          << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 400) << checked;
}

} // namespace
