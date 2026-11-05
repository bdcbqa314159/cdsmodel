// TIER 1 -- curve interpolation, without an oracle.
//
// An interpolator has one property that matters more than any value: it must
// reproduce the curve it interpolates. Everything else here is about the
// boundaries -- where it stops interpolating and starts clamping, and the base
// date, which is the one point whose discount factor must come out exactly 1.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/interp_curve_cases.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::InterpType;
using cdsmodel::RatePoint;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<RatePoint> rising(int count = 6) {
  std::vector<RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(
        RatePoint{*cdsmodel::make_date(2024 + i, 1, 1), 0.02 + 0.004 * static_cast<double>(i)});
  }
  return points;
}

cdsmodel::ZeroCurve curve_at(double basis,
                             cdsmodel::DayCount dcc = cdsmodel::DayCount::Act365Fixed) {
  return *cdsmodel::ZeroCurve::create(d(2024, 1, 1), rising(), basis, dcc);
}

// =====================================================================
// The property that matters: it reproduces its own curve.
// =====================================================================

TEST(InterpCurve, EveryCurvePointInterpolatesToItsOwnRate) {
  // Under BOTH interpolation types, and bit-exactly. A scheme that merely
  // came close at its own knots would be wrong in a way no tolerance should
  // absorb: those rates are inputs, not results.
  int checked = 0;
  for (const double basis : {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      const cdsmodel::ZeroCurve curve = curve_at(basis, dcc);
      for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
        for (const RatePoint &point : curve.points()) {
          const std::optional<double> got = cdsmodel::interp_rate(curve, point.date, interp);
          ASSERT_TRUE(got.has_value()) << basis;
          EXPECT_EQ(*got, point.rate) << "basis " << basis << " interp " << static_cast<int>(interp)
                                      << " at " << cdsmodel::to_tdate(point.date);
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(InterpCurve, ExtrapolationIsFlatInBothDirections) {
  // "Do not extrapolate, regardless of what the interpolation type is."
  for (const double basis : {1.0, cdsmodel::kContinuousBasis}) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    const double first = curve.points().front().rate;
    const double last = curve.points().back().rate;
    for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
      for (const cdsmodel::Date before : {d(1990, 1, 1), d(2020, 6, 1), d(2024, 12, 31)}) {
        EXPECT_EQ(cdsmodel::interp_rate(curve, before, interp), std::optional{first})
            << "before the curve, at " << cdsmodel::to_tdate(before);
      }
      for (const cdsmodel::Date after : {d(2030, 1, 2), d(2050, 1, 1), d(2099, 12, 31)}) {
        EXPECT_EQ(cdsmodel::interp_rate(curve, after, interp), std::optional{last})
            << "after the curve, at " << cdsmodel::to_tdate(after);
      }
    }
  }
}

TEST(InterpCurve, AnInterpolatedRateStaysWithinItsBracket) {
  // Neither scheme may overshoot: a rate between two curve points must lie
  // between their rates. For a monotone curve that is the whole envelope, and
  // it is the cheapest check that a conversion has not been inverted.
  int checked = 0;
  for (const double basis : {1.0, 2.0, 12.0, cdsmodel::kContinuousBasis}) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    const std::span<const RatePoint> points = curve.points();
    for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
      for (std::size_t i = 1; i < points.size(); ++i) {
        const cdsmodel::TDate lo = cdsmodel::to_tdate(points[i - 1].date);
        const cdsmodel::TDate hi = cdsmodel::to_tdate(points[i].date);
        for (const double fraction : {0.1, 0.25, 0.5, 0.75, 0.9}) {
          const cdsmodel::Date at = cdsmodel::from_tdate(
              lo + static_cast<cdsmodel::TDate>(fraction * static_cast<double>(hi - lo)));
          const std::optional<double> got = cdsmodel::interp_rate(curve, at, interp);
          ASSERT_TRUE(got.has_value());
          const std::string where = "basis " + std::to_string(basis) + " segment " +
                                    std::to_string(i) + " at " + std::to_string(fraction);
          EXPECT_GE(*got, points[i - 1].rate - 1e-15) << where;
          EXPECT_LE(*got, points[i].rate + 1e-15) << where;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 150) << checked;
}

TEST(InterpCurve, LinearInterpolationIsExactlyLinearInTheDate) {
  // The linear arm has a closed form, so it can be checked against arithmetic
  // rather than against a recorded number.
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  const std::span<const RatePoint> points = curve.points();
  for (std::size_t i = 1; i < points.size(); ++i) {
    const double lo = static_cast<double>(cdsmodel::to_tdate(points[i - 1].date));
    const double hi = static_cast<double>(cdsmodel::to_tdate(points[i].date));
    for (const double fraction : {0.25, 0.5, 0.75}) {
      const double at = lo + fraction * (hi - lo);
      const std::optional<double> got = cdsmodel::interp_rate_exact(curve, at, InterpType::Linear);
      ASSERT_TRUE(got.has_value());
      const double expected =
          points[i - 1].rate + (points[i].rate - points[i - 1].rate) * (at - lo) / (hi - lo);
      EXPECT_NEAR(*got, expected, 1.0e-14) << "segment " << i << " at " << fraction;
    }
  }
}

// =====================================================================
// The base date, and the branch that protects it.
// =====================================================================

TEST(InterpCurve, TheDiscountFactorAtTheBaseDateIsExactlyOne) {
  // This is what the base-date branch in flat forwards exists for. Upstream's
  // comment: "This ensures that basedate will be associated with a 'kink' in
  // the forwards, and discount(base) == 1.0". Asserted as the property rather
  // than as the branch.
  const std::vector<RatePoint> straddling{{d(2024, 1, 1), 0.03}, {d(2027, 1, 1), 0.05}};
  int checked = 0;
  for (const double basis : {1.0, 2.0, 12.0, cdsmodel::kContinuousBasis}) {
    const std::optional<cdsmodel::ZeroCurve> curve = cdsmodel::ZeroCurve::create(
        d(2025, 6, 1), straddling, basis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(curve.has_value()) << basis;
    const std::optional<double> rate =
        cdsmodel::interp_rate(*curve, curve->base_date(), InterpType::FlatForwards);
    ASSERT_TRUE(rate.has_value()) << basis;
    const std::optional<double> discount =
        cdsmodel::rate_to_discount(*rate, curve->base_date(), curve->base_date(),
                                   curve->day_count(), static_cast<long>(basis));
    ASSERT_TRUE(discount.has_value()) << basis;
    EXPECT_EQ(*discount, 1.0) << "basis " << basis << ": no time, so no discounting";
    ++checked;
  }
  EXPECT_EQ(checked, 4);
}

TEST(InterpCurve, TheRateIsFlatOnEachSideOfAStraddledBaseDate) {
  // The other half of the same branch: between the two bracketing points the
  // rate does not vary, it jumps once at the base date. That is the "kink".
  const std::vector<RatePoint> straddling{{d(2024, 1, 1), 0.03}, {d(2027, 1, 1), 0.05}};
  const std::optional<cdsmodel::ZeroCurve> curve =
      cdsmodel::ZeroCurve::create(d(2025, 6, 1), straddling, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  const auto rate_at = [&](cdsmodel::Date at) {
    return cdsmodel::interp_rate(*curve, at, InterpType::FlatForwards);
  };
  // Before the base date: flat at the lower rate.
  EXPECT_EQ(rate_at(d(2024, 6, 1)), std::optional{0.03});
  EXPECT_EQ(rate_at(d(2025, 5, 31)), std::optional{0.03});
  // On and after it: flat at the upper rate.
  EXPECT_EQ(rate_at(d(2025, 6, 1)), std::optional{0.05});
  EXPECT_EQ(rate_at(d(2026, 6, 1)), std::optional{0.05});
}

// =====================================================================
// Fractional dates, and what refuses them.
// =====================================================================

TEST(InterpCurve, AFractionalDateInterpolatesBetweenItsWholeNeighbours) {
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  const double anchor = static_cast<double>(cdsmodel::to_tdate(d(2026, 1, 1)));
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    const std::optional<double> at_whole = cdsmodel::interp_rate_exact(curve, anchor, interp);
    const std::optional<double> at_next = cdsmodel::interp_rate_exact(curve, anchor + 1.0, interp);
    ASSERT_TRUE(at_whole.has_value());
    ASSERT_TRUE(at_next.has_value());
    for (const double offset : {0.25, 0.5, 0.75}) {
      const std::optional<double> at_fraction =
          cdsmodel::interp_rate_exact(curve, anchor + offset, interp);
      ASSERT_TRUE(at_fraction.has_value()) << offset;
      const double lo = std::min(*at_whole, *at_next);
      const double hi = std::max(*at_whole, *at_next);
      EXPECT_GE(*at_fraction, lo - 1e-15) << offset;
      EXPECT_LE(*at_fraction, hi + 1e-15) << offset;
    }
    // And a whole date passed as a double is the whole-date answer.
    EXPECT_EQ(cdsmodel::interp_rate_exact(curve, anchor, interp),
              cdsmodel::interp_rate(curve, d(2026, 1, 1), interp));
  }
}

TEST(InterpCurve, TheCurvesThatReachTheUnportedBranchesCannotBeBuilt) {
  // Stated as a property so "the port is missing a branch" and "the port
  // cannot represent the input" stay distinguishable.
  const std::vector<RatePoint> points = rising(4);
  for (const cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360, cdsmodel::DayCount::Act365,
        cdsmodel::DayCount::EffectiveRate}) {
    EXPECT_FALSE(cdsmodel::ZeroCurve::create(d(2024, 1, 1), points, 1.0, dcc).has_value())
        << "day count " << cdsmodel::to_upstream_code(dcc);
  }
  for (const double basis : {cdsmodel::kDiscountFactorBasis, cdsmodel::kSimpleBasis,
                             cdsmodel::kDiscountRateBasis, 366.0, -1.0}) {
    EXPECT_FALSE(
        cdsmodel::ZeroCurve::create(d(2024, 1, 1), points, basis, cdsmodel::DayCount::Act365Fixed)
            .has_value())
        << "basis " << basis;
  }
}

// =====================================================================
// Replay.
// =====================================================================

TEST(InterpCurve, MatchesRecordedInterpolations) {
  int checked = 0;
  int fractional = 0;
  for (const auto &c : cdsmodel::golden::kInterpCurveCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value()) << c.day_count;
    std::vector<RatePoint> points;
    for (std::size_t i = 0; i < c.n; ++i) {
      points.push_back(RatePoint{cdsmodel::from_tdate(c.dates[i]), c.rates[i]});
    }
    const std::optional<cdsmodel::ZeroCurve> curve =
        cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(c.base_date), points, c.basis, *dcc);
    ASSERT_TRUE(curve.has_value()) << "basis " << c.basis << " dcc " << c.day_count;
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    const std::optional<double> got = cdsmodel::interp_rate_exact(*curve, c.at, interp);
    const std::string where = "base " + std::to_string(c.base_date) + " basis " +
                              std::to_string(c.basis) + " dcc " + std::to_string(c.day_count) +
                              " interp " + std::to_string(c.interp) + " at " + std::to_string(c.at);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      EXPECT_NEAR(*got, c.rate,
                  cdsmodel::golden::kInterpCurveUlps * std::max(1.0, std::fabs(c.rate)) *
                      std::numeric_limits<double>::epsilon())
          << where;
    }
    if (c.at != static_cast<double>(static_cast<cdsmodel::TDate>(c.at))) {
      ++fractional;
    }
    ++checked;
  }
  EXPECT_GT(checked, 400) << checked;
  EXPECT_GT(fractional, 50)
      << "the recording must include fractional dates, which are the reason the "
         "exact form takes a double: "
      << fractional;
}

} // namespace
