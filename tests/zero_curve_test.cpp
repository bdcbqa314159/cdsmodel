// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// The recorded doubles are compared with EXPECT_EQ, not EXPECT_DOUBLE_EQ.
// That is deliberate: the differential suite established that the port
// reproduces upstream's floating point bit-for-bit, and a tolerance here
// would quietly give that guarantee away.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/zero_curve_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::ZeroCurve rebuild(const cdsmodel::golden::ZeroCurveDef &def) {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < def.n; ++i) {
    points.push_back({cdsmodel::from_tdate(def.dates[i]), def.rates[i]});
  }
  const auto dcc = cdsmodel::from_upstream_code(def.day_count);
  const auto curve = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(cdsmodel::golden::kZeroCurveBase), points, def.basis, *dcc);
  return *curve;
}

TEST(ZeroCurveGolden, RatesAndPricesAreBitIdenticalToRecordedReference) {
  std::vector<cdsmodel::ZeroCurve> curves;
  for (const auto &def : cdsmodel::golden::kZeroCurves) {
    curves.push_back(rebuild(def));
  }
  for (const auto &c : cdsmodel::golden::kZeroCurveCases) {
    ASSERT_LT(c.curve, curves.size());
    const cdsmodel::Date date = cdsmodel::from_tdate(c.tdate);
    EXPECT_EQ(curves[c.curve].zero_rate(date), c.rate)
        << "rate, curve " << c.curve << " tdate " << c.tdate;
    EXPECT_EQ(curves[c.curve].zero_price(date), c.price)
        << "price, curve " << c.curve << " tdate " << c.tdate;
  }
}

TEST(ZeroCurveGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kZeroCurveCases.size(), 1500u);
  EXPECT_GE(cdsmodel::golden::kZeroCurves.size(), 9u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(ZeroCurveConstruction, RejectsWhatTheArithmeticCannotSupport) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> ok{{d(2025, 1, 1), 0.03}, {d(2026, 1, 1), 0.04}};

  EXPECT_TRUE(
      cdsmodel::ZeroCurve::create(base, ok, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
  // No points.
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, {}, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
  // Dates not strictly increasing -- the binary search assumes ascending.
  const std::vector<cdsmodel::RatePoint> unsorted{{d(2026, 1, 1), 0.04}, {d(2025, 1, 1), 0.03}};
  EXPECT_FALSE(cdsmodel::ZeroCurve::create(base, unsorted, 1.0, cdsmodel::DayCount::Act365Fixed)
                   .has_value());
  // Day counts the rate conversion cannot handle. Upstream reports these per
  // call, from inside a hot loop; the port refuses the curve instead.
  for (cdsmodel::DayCount bad :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::EffectiveRate}) {
    EXPECT_FALSE(cdsmodel::ZeroCurve::create(base, ok, 1.0, bad).has_value())
        << "accepted day count " << cdsmodel::to_upstream_code(bad);
  }
  // Bases that are neither continuous nor a period count in [1, 365].
  for (double bad : {0.0, 0.5, 366.0, -1.0, 4999.0}) {
    EXPECT_FALSE(
        cdsmodel::ZeroCurve::create(base, ok, bad, cdsmodel::DayCount::Act365Fixed).has_value())
        << "accepted basis " << bad;
  }
  EXPECT_TRUE(cdsmodel::ZeroCurve::create(base, ok, cdsmodel::kContinuousBasis,
                                          cdsmodel::DayCount::Act365Fixed)
                  .has_value());
}

TEST(ZeroCurve, DiscountFactorAtTheBaseDateIsExactlyOne) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points{{d(2025, 1, 1), 0.04}, {d(2030, 1, 1), 0.045}};
  const auto curve =
      cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  EXPECT_EQ(curve->zero_price(base), 1.0) << "not merely close to 1 -- exactly 1";
}

TEST(ZeroCurve, DiscountFactorsDecreaseWithMaturityForAPositiveCurve) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points{
      {d(2025, 1, 1), 0.030}, {d(2027, 1, 1), 0.038}, {d(2034, 1, 1), 0.045}};
  const auto curve =
      cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());

  double previous = 1.0;
  for (int offset = 1; offset <= 4000; offset += 37) {
    const double price = curve->zero_price(base + days{offset});
    EXPECT_LT(price, previous) << "discount factor failed to decrease at offset " << offset;
    EXPECT_GT(price, 0.0) << "discount factor went non-positive at offset " << offset;
    previous = price;
  }
}

TEST(ZeroCurve, ForwardPriceComposesWithSpotPrices) {
  // P(0,b) == P(0,a) * P(a,b) up to the rounding of one extra divide and
  // multiply. This is the one place a tolerance is appropriate: it is an
  // algebraic identity about the port, not a claim about matching upstream.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points{{d(2025, 1, 1), 0.031}, {d(2029, 1, 1), 0.042}};
  const auto curve =
      cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());

  for (int start : {30, 200, 900, 1800}) {
    for (int span : {1, 90, 365, 1095}) {
      const cdsmodel::Date a = base + days{start};
      const cdsmodel::Date b = a + days{span};
      EXPECT_NEAR(curve->zero_price(b), curve->zero_price(a) * curve->forward_zero_price(a, b),
                  1e-15)
          << "start=" << start << " span=" << span;
    }
  }
}

TEST(ZeroCurve, FlatCurveGivesTheAnalyticDiscountFactor) {
  // An oracle of a different kind: for a flat continuously compounded curve
  // the discount factor has a closed form, so this checks the port against
  // mathematics rather than against upstream.
  const cdsmodel::Date base = d(2024, 1, 1);
  constexpr double rate = 0.05;
  const std::vector<cdsmodel::RatePoint> points{{d(2025, 1, 1), rate}, {d(2035, 1, 1), rate}};
  const auto curve = cdsmodel::ZeroCurve::create(base, points, cdsmodel::kContinuousBasis,
                                                 cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());

  for (int offset : {1, 100, 365, 1000, 3650}) {
    const double years = static_cast<double>(offset) / 365.0;
    EXPECT_NEAR(curve->zero_price(base + days{offset}), std::exp(-rate * years), 1e-15)
        << "offset " << offset;
  }
}

} // namespace
