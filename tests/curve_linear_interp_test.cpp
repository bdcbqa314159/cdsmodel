// TIER 1 -- replays tests/golden/curve_linear_interp_cases.hpp, so
// re-expressing a curve on new dates is covered in CI, where no oracle is
// present.
//
// Rates compare at the accumulating tier, not in ULPs: the arithmetic is a
// single interpolation with nothing accumulated, but the file crossed a
// compiler to get here and that alone settles it.

#include <cdsmodel/curve_linear_interp.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/curve_linear_interp_cases.hpp"

#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

std::vector<cdsmodel::RatePoint> curve_of(int id) {
  const cdsmodel::TDate *dates = nullptr;
  const double *rates = nullptr;
  std::size_t n = 0;
  switch (id) {
  case 0:
    dates = kLinInterpCurve0Dates.data();
    rates = kLinInterpCurve0Rates.data();
    n = kLinInterpCurve0Count;
    break;
  case 1:
    dates = kLinInterpCurve1Dates.data();
    rates = kLinInterpCurve1Rates.data();
    n = kLinInterpCurve1Count;
    break;
  case 2:
    dates = kLinInterpCurve2Dates.data();
    rates = kLinInterpCurve2Rates.data();
    n = kLinInterpCurve2Count;
    break;
  default:
    dates = kLinInterpCurve3Dates.data();
    rates = kLinInterpCurve3Rates.data();
    n = kLinInterpCurve3Count;
    break;
  }
  std::vector<cdsmodel::RatePoint> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(cdsmodel::RatePoint{cdsmodel::from_tdate(dates[i]), rates[i]});
  }
  return out;
}

std::vector<cdsmodel::Date> wanted_of(int id) {
  const cdsmodel::TDate *dates = nullptr;
  std::size_t n = 0;
  switch (id) {
  case 0:
    dates = kLinInterpWanted0Dates.data();
    n = kLinInterpWanted0Count;
    break;
  case 1:
    dates = kLinInterpWanted1Dates.data();
    n = kLinInterpWanted1Count;
    break;
  case 2:
    dates = kLinInterpWanted2Dates.data();
    n = kLinInterpWanted2Count;
    break;
  case 3:
    dates = kLinInterpWanted3Dates.data();
    n = kLinInterpWanted3Count;
    break;
  default:
    dates = kLinInterpWanted4Dates.data();
    n = kLinInterpWanted4Count;
    break;
  }
  std::vector<cdsmodel::Date> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(cdsmodel::from_tdate(dates[i]));
  }
  return out;
}

TEST(CurveLinearInterpGolden, ReplaysRecordedCurves) {
  ASSERT_GE(kLinInterpCases.size(), 50u);
  int matched = 0;
  for (const CurveLinInterpCase &c : kLinInterpCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value());
    const std::vector<cdsmodel::RatePoint> points = curve_of(c.curve);
    const std::optional<cdsmodel::ZeroCurve> old_curve = cdsmodel::ZeroCurve::create(
        cdsmodel::from_tdate(kLinInterpBaseDate), points, c.basis, *dcc);
    if (!old_curve) {
      // The port refuses day counts or bases the conversion cannot support,
      // which upstream's TCurve accepts. Pre-existing, from the zero_curve
      // slice, and not this module's business.
      continue;
    }

    const std::optional<cdsmodel::ZeroCurve> got =
        cdsmodel::linear_interp_curve(wanted_of(c.wanted), *old_curve);
    ASSERT_EQ(got.has_value(), c.accepted)
        << "curve " << c.curve << " wanted " << c.wanted << " dcc " << c.day_count;
    if (!c.accepted) {
      continue;
    }
    const std::span<const cdsmodel::RatePoint> result = got->points();
    ASSERT_EQ(result.size(), c.point_count);
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedInterpPoint &want = kLinInterpPoints[c.first_point + i];
      EXPECT_EQ(cdsmodel::to_tdate(result[i].date), want.date) << "date " << i;
      EXPECT_NEAR(want.rate, result[i].rate,
                  kCurveLinInterpTolerance * std::max(1.0, std::abs(want.rate)))
          << "rate " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 50) << "the replay compared almost nothing";
}

// --------------------------------------------- properties, no oracle needed

// The output dates are exactly the requested ones. The clamp applies to the
// date interpolated AT, never to the date stored -- and a port that clamped
// the stored date too would still produce plausible rates.
TEST(CurveLinearInterpGolden, TheOutputDatesAreExactlyTheRequestedOnes) {
  int checked = 0;
  for (const CurveLinInterpCase &c : kLinInterpCases) {
    if (!c.accepted) {
      continue;
    }
    const std::vector<cdsmodel::Date> wanted = wanted_of(c.wanted);
    ASSERT_EQ(wanted.size(), c.point_count);
    for (std::size_t i = 0; i < c.point_count; ++i) {
      EXPECT_EQ(kLinInterpPoints[c.first_point + i].date, cdsmodel::to_tdate(wanted[i]))
          << "the stored date was clamped, not just the interpolation date";
      ++checked;
    }
  }
  EXPECT_GT(checked, 100);
}

// No rate lies outside the old curve's rate range: this routine interpolates
// and clamps, and never extrapolates. A property that would catch a port that
// extended the curve linearly past its ends.
TEST(CurveLinearInterpGolden, NoRateEscapesTheOldCurvesRange) {
  int checked = 0;
  int clamped_cases = 0;
  for (const CurveLinInterpCase &c : kLinInterpCases) {
    if (!c.accepted) {
      continue;
    }
    const std::vector<cdsmodel::RatePoint> points = curve_of(c.curve);
    double lo = points.front().rate;
    double hi = points.front().rate;
    for (const cdsmodel::RatePoint &p : points) {
      lo = std::min(lo, p.rate);
      hi = std::max(hi, p.rate);
    }
    const std::vector<cdsmodel::Date> wanted = wanted_of(c.wanted);
    bool any_outside = false;
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const double rate = kLinInterpPoints[c.first_point + i].rate;
      EXPECT_GE(rate, lo - 1e-15) << "a rate below the old curve's minimum";
      EXPECT_LE(rate, hi + 1e-15) << "a rate above the old curve's maximum";
      if (wanted[i] < points.front().date || wanted[i] > points.back().date) {
        any_outside = true;
      }
      ++checked;
    }
    if (any_outside) {
      ++clamped_cases;
    }
  }
  EXPECT_GT(checked, 100);
  EXPECT_GT(clamped_cases, 0)
      << "no case asked for a date outside the curve, so the clamp is untested";
}

// A one-point curve gives that rate everywhere, whatever is asked for.
TEST(CurveLinearInterpGolden, AOnePointCurveIsFlat) {
  const std::vector<cdsmodel::RatePoint> points = curve_of(2);
  ASSERT_EQ(points.size(), 1u);
  const std::optional<cdsmodel::ZeroCurve> old_curve = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(kLinInterpBaseDate), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(old_curve.has_value());

  const std::vector<cdsmodel::Date> wanted{*cdsmodel::make_date(2020, 1, 1),
                                           *cdsmodel::make_date(2025, 1, 15),
                                           *cdsmodel::make_date(2045, 1, 1)};
  const std::optional<cdsmodel::ZeroCurve> got = cdsmodel::linear_interp_curve(wanted, *old_curve);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->points().size(), 3u);
  for (const cdsmodel::RatePoint &p : got->points()) {
    EXPECT_DOUBLE_EQ(p.rate, points.front().rate);
  }
}

// An empty request is refused. Upstream also refuses it, by reaching
// JpmcdsMallocSafe(0).
TEST(CurveLinearInterpGolden, AnEmptyRequestIsRefused) {
  const std::optional<cdsmodel::ZeroCurve> old_curve = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(kLinInterpBaseDate), curve_of(0), 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(old_curve.has_value());
  EXPECT_FALSE(cdsmodel::linear_interp_curve({}, *old_curve).has_value());
}

} // namespace
