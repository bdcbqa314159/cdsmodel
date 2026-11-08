// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// IMPORTANT, and the first version of this file got it wrong:
//
// The recorded doubles here are compared with EXPECT_DOUBLE_EQ (4 ULPs), NOT
// with exact equality -- while the differential suite next door uses exact
// equality. That is not inconsistency, it is two different claims:
//
//   * TIER 2 compares the port against upstream **in the same process**, so
//     both calls reach the same libm. `exp` and `log` return identical bits,
//     and anything else differing is a real porting error. Exact equality is
//     the right bar, and it holds on arm64/clang, x86-64/gcc and MSVC.
//
//   * TIER 1 compares the port against doubles **recorded on another
//     machine**. `exp` and `log` are not specified to the last bit and differ
//     between macOS libm, glibc and the MSVC CRT. Vectors generated on one
//     platform are off by about 1 ULP on another.
//
// So "bit-exact" is a property of two implementations sharing one libm, not a
// property of a number. Demanding bit equality from a recorded transcendental
// value asserts that every libm agrees, which is false -- and it failed on
// Linux and Windows while passing on macOS, which is exactly how a
// platform-specific assumption hides.

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

TEST(ZeroCurveGolden, RatesAndPricesMatchRecordedReferenceWithinLibmTolerance) {
  std::vector<cdsmodel::ZeroCurve> curves;
  for (const auto &def : cdsmodel::golden::kZeroCurves) {
    curves.push_back(rebuild(def));
  }
  for (const auto &c : cdsmodel::golden::kZeroCurveCases) {
    ASSERT_LT(c.curve, curves.size());
    const cdsmodel::Date date = cdsmodel::from_tdate(c.tdate);
    // 4 ULPs, which is what EXPECT_DOUBLE_EQ allows. Enough to absorb a
    // differing libm, far too tight to absorb a porting error: the defects
    // this project has actually produced moved results by far more than that,
    // or not at all.
    EXPECT_DOUBLE_EQ(curves[c.curve].zero_rate(date), c.rate)
        << "rate, curve " << c.curve << " tdate " << c.tdate;
    EXPECT_DOUBLE_EQ(curves[c.curve].zero_price(date), c.price)
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

// =====================================================================
// Added with the tcurve.c slice.
// =====================================================================

TEST(ZeroCurve, RateValidityMatchesUpstreamsRule) {
  // Now lives in rate_conversion.hpp, its upstream home.
  // Upstream's JpmcdsRateValidYearFrac: continuous accepts any rate, a
  // compounding frequency n accepts rate > -n.
  EXPECT_TRUE(cdsmodel::rate_valid_year_frac(-1e300, 1.0, cdsmodel::kContinuousBasis));
  EXPECT_TRUE(cdsmodel::rate_valid_year_frac(1e300, 1.0, cdsmodel::kContinuousBasis));
  for (double basis : {1.0, 2.0, 4.0, 12.0, 365.0}) {
    EXPECT_TRUE(cdsmodel::rate_valid_year_frac(0.0, 1.0, basis)) << basis;
    EXPECT_TRUE(cdsmodel::rate_valid_year_frac(-basis + 1.0e-9, 1.0, basis)) << basis;
    EXPECT_FALSE(cdsmodel::rate_valid_year_frac(-basis, 1.0, basis))
        << basis << " (boundary is exclusive)";
    EXPECT_FALSE(cdsmodel::rate_valid_year_frac(-basis - 1.0e-9, 1.0, basis)) << basis;
  }
}

TEST(ZeroCurve, EveryConstructibleCurveCanBePriced) {
  // The invariant UPSTREAM-15 violates and the port guarantees: if `create`
  // returns a curve, no discount factor it yields is ever NaN.
  //
  // The distinction between NaN and infinity is the whole point, and the
  // first version of this test got it wrong by demanding finiteness. A
  // continuous-basis rate of -400 over three years is exp(1200), which
  // overflows to +inf -- in the port AND in upstream, which accepts the same
  // curve and returns the same infinity. That is a VALUE: the answer is
  // enormous. NaN is not a value; it is "I could not compute this", used as
  // an error channel because JpmcdsZeroPrice returns a bare double and has
  // nowhere else to put a failure.
  //
  // So the guarantee is "never NaN", not "never overflows". Upstream has
  // neither: JpmcdsMakeTCurve accepts a 30/360 curve and an empty curve, and
  // JpmcdsZeroPrice returns NaN for both.
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  int priced = 0;
  int refused = 0;
  for (double basis :
       {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis, 0.0, -2.0, 512.0, 1000.0}) {
    for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
                                   cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
                                   cdsmodel::DayCount::Act365, cdsmodel::DayCount::EffectiveRate}) {
      for (double rate : {0.05, 0.0, -0.5, -0.999, -1.0, -1.5, -3.0, -400.0, 1.0e3}) {
        std::vector<cdsmodel::RatePoint> pts;
        for (int years = 1; years <= 4; ++years) {
          pts.push_back({*cdsmodel::make_date(2024 + years, 1, 1), rate});
        }
        const cdsmodel::Expected<cdsmodel::ZeroCurve> curve =
            cdsmodel::ZeroCurve::create(base, pts, basis, dcc);
        if (!curve) {
          ++refused;
          continue;
        }
        for (int months = 0; months <= 60; months += 7) {
          const cdsmodel::Date at = *cdsmodel::make_date(2024 + months / 12, 1 + months % 12, 1);
          const double z = curve->zero_price(at);
          ASSERT_FALSE(std::isnan(z))
              << "basis " << basis << " rate " << rate << " at " << cdsmodel::to_tdate(at)
              << " gave NaN -- a discount factor must never be an error channel";
          ASSERT_GE(z, 0.0) << "basis " << basis << " rate " << rate;
          // And for any rate a market could quote, it is finite too.
          if (rate > -1.0 && rate < 1.0) {
            ASSERT_TRUE(std::isfinite(z)) << "basis " << basis << " rate " << rate;
          }
          ++priced;
        }
      }
    }
  }
  // 810 discount factors across the curves `create` accepts, from a grid of
  // 540 (basis, day count, rate) combinations of which 450 are refused.
  EXPECT_GT(priced, 700) << "too few discount factors checked: " << priced;
  EXPECT_GT(refused, 100) << "the rejection paths were barely exercised: " << refused;
}

TEST(ZeroCurve, ARateAtOrBelowMinusBasisIsRefused) {
  // The concrete shape of PORT-11: these were accepted before the rate check
  // existed, and zero_price returned inf or NaN.
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> at_one{{*cdsmodel::make_date(2029, 1, 1), -1.0}};
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, at_one, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
  const std::vector<cdsmodel::RatePoint> below{{*cdsmodel::make_date(2029, 1, 1), -1.5}};
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, below, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
  // Just inside the boundary is fine, and prices finitely.
  const std::vector<cdsmodel::RatePoint> inside{{*cdsmodel::make_date(2029, 1, 1), -0.9999}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> ok =
      cdsmodel::ZeroCurve::create(base, inside, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(ok.has_value());
  EXPECT_TRUE(std::isfinite(ok->zero_price(*cdsmodel::make_date(2029, 1, 1))));
  // One bad point among good ones is enough to refuse the whole curve.
  const std::vector<cdsmodel::RatePoint> mixed{{*cdsmodel::make_date(2025, 1, 1), 0.03},
                                               {*cdsmodel::make_date(2026, 1, 1), -2.0},
                                               {*cdsmodel::make_date(2027, 1, 1), 0.04}};
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, mixed, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
}

} // namespace
