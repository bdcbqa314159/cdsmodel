// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// Recorded PVs are compared with a RELATIVE tolerance, not a ULP count, and
// the reason is the third refinement of this project's verification standard.
//
//   1. Integers and calendars: exact equality, trivially.
//   2. Non-accumulating floating point (the zero curve): bit-exact against
//      upstream in-process; 4 ULPs against doubles recorded on another
//      platform, because exp and log are libm-specific.
//   3. ACCUMULATING floating point (this file): 4 ULPs is not enough. Summing
//      one term per timeline segment amplifies a 1-ULP libm difference into
//      roughly 600 ULPs in the total --
//
//        7352.2330864798769  computed on Linux
//        7352.2330864809865  recorded on macOS
//
//      a relative difference of 1.5e-13, which is nothing financially and far
//      more than four ULPs. A ULP budget does not survive accumulation.
//
// So the bound here is relative, and chosen against the size of errors that
// actually matter rather than to make the test pass. Every port-side defect
// this project has produced changed an answer by orders of magnitude or not
// at all; none would hide under 1e-12. The test also tracks and reports the
// worst relative difference it saw, so the remaining margin is visible
// instead of assumed.
//
// Bit equality remains the bar in the differential suite, where both
// implementations run in one process and share one libm.

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/contingent_leg_cases.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::ZeroCurve rebuild(std::span<const long> dates, std::span<const double> rates) {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < dates.size(); ++i) {
    points.push_back({cdsmodel::from_tdate(dates[i]), rates[i]});
  }
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(cdsmodel::golden::kClBase), points, 1.0,
                                      cdsmodel::DayCount::Act365Fixed);
}

/// Relative bound for replaying accumulated PVs across platforms. See the
/// file header for why this is relative rather than a ULP count, and why this
/// size.
constexpr double kCrossPlatformRelative = 1e-12;

TEST(ContingentLegGolden, PvsMatchRecordedReference) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kClDiscDates, cdsmodel::golden::kClDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kClSpreadDates, cdsmodel::golden::kClSpreadRates);
  const cdsmodel::Date base = cdsmodel::from_tdate(cdsmodel::golden::kClBase);

  double worst_relative = 0.0;
  for (const auto &c : cdsmodel::golden::kContingentLegCases) {
    const cdsmodel::ContingentLeg leg{cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.end),
                                      1.0e7,
                                      c.pay == 0 ? cdsmodel::ProtectionPayment::AtDefault
                                                 : cdsmodel::ProtectionPayment::AtMaturity,
                                      c.protect_start};
    const auto pv = cdsmodel::contingent_leg_pv(leg, base, base, base, disc, spread, c.recovery);
    ASSERT_TRUE(pv.has_value()) << "start=" << c.start << " end=" << c.end;

    const double scale = std::max(1.0, std::fabs(c.pv));
    const double relative = std::fabs(*pv - c.pv) / scale;
    worst_relative = std::max(worst_relative, relative);
    EXPECT_LE(relative, kCrossPlatformRelative)
        << "start=" << c.start << " end=" << c.end << " R=" << c.recovery << " pay=" << c.pay
        << std::setprecision(17) << "\n  got      " << *pv << "\n  recorded " << c.pv
        << "\n  relative " << relative;
  }
  // Track the margin so the failure message carries it. Note what this does
  // NOT do: ctest shows a passing test's output only under -V, so this line is
  // invisible in a green CI run. An earlier attempt to surface it from
  // verify.py was dead code -- `--output-on-failure` means the text never
  // reaches the harness at all.
  //
  // So the margin is visible exactly when it matters: the EXPECT_LE above
  // prints the observed relative difference when it breaches the budget.
  // Measured at roughly 1.5e-13 on x86-64 against a budget of 1e-12, about
  // seven times the headroom -- if cross-libm drift ever grows into that, the
  // assertion reports the number rather than leaving it to be guessed.
  std::cout << "[ INFO     ] worst relative difference " << std::setprecision(3) << worst_relative
            << " against a budget of " << kCrossPlatformRelative << "\n";
}

TEST(ContingentLegGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kContingentLegCases.size(), 600u);
}

// --- financial sanity: an oracle of a different kind ---------------------

TEST(ContingentLeg, FullRecoveryIsWorthNothing) {
  // R = 1 means loss given default is zero, so protection has no value
  // whatever the hazard rate. Checks the port against finance rather than
  // against upstream.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kClDiscDates, cdsmodel::golden::kClDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kClSpreadDates, cdsmodel::golden::kClSpreadRates);
  const cdsmodel::Date base = cdsmodel::from_tdate(cdsmodel::golden::kClBase);

  for (int tenor : {365, 1826, 3653}) {
    for (auto pay :
         {cdsmodel::ProtectionPayment::AtDefault, cdsmodel::ProtectionPayment::AtMaturity}) {
      const cdsmodel::ContingentLeg leg{base, base + days{tenor}, 1.0e7, pay, true};
      const auto pv = cdsmodel::contingent_leg_pv(leg, base, base, base, disc, spread, 1.0);
      ASSERT_TRUE(pv.has_value());
      EXPECT_EQ(*pv, 0.0) << "tenor=" << tenor;
    }
  }
}

TEST(ContingentLeg, ProtectionIsWorthMoreForLongerTenorsAndLowerRecovery) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kClDiscDates, cdsmodel::golden::kClDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kClSpreadDates, cdsmodel::golden::kClSpreadRates);
  const cdsmodel::Date base = cdsmodel::from_tdate(cdsmodel::golden::kClBase);
  const auto pv_of = [&](int tenor, double recovery) {
    const cdsmodel::ContingentLeg leg{base, base + days{tenor}, 1.0e7,
                                      cdsmodel::ProtectionPayment::AtDefault, true};
    return *cdsmodel::contingent_leg_pv(leg, base, base, base, disc, spread, recovery);
  };

  double previous = 0.0;
  for (int tenor : {91, 365, 730, 1826, 3653, 5479}) {
    const double pv = pv_of(tenor, 0.4);
    EXPECT_GT(pv, previous) << "protection got cheaper at tenor " << tenor;
    previous = pv;
  }
  double richer = std::numeric_limits<double>::infinity();
  for (double recovery : {0.0, 0.25, 0.40, 0.75, 0.99}) {
    const double pv = pv_of(1826, recovery);
    EXPECT_LT(pv, richer) << "protection got dearer at recovery " << recovery;
    EXPECT_GE(pv, 0.0);
    richer = pv;
  }
}

TEST(ContingentLeg, PvIsBoundedByTheWorstCaseLoss) {
  // Protection can never be worth more than the undiscounted loss, since
  // default is uncertain and payment is discounted.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kClDiscDates, cdsmodel::golden::kClDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kClSpreadDates, cdsmodel::golden::kClSpreadRates);
  const cdsmodel::Date base = cdsmodel::from_tdate(cdsmodel::golden::kClBase);
  constexpr double notional = 1.0e7;

  for (int tenor : {365, 1826, 5479}) {
    for (double recovery : {0.0, 0.4, 0.8}) {
      const cdsmodel::ContingentLeg leg{base, base + days{tenor}, notional,
                                        cdsmodel::ProtectionPayment::AtDefault, true};
      const auto pv = cdsmodel::contingent_leg_pv(leg, base, base, base, disc, spread, recovery);
      ASSERT_TRUE(pv.has_value());
      EXPECT_GE(*pv, 0.0);
      EXPECT_LE(*pv, notional * (1.0 - recovery))
          << "pv exceeds the worst-case loss, tenor=" << tenor << " R=" << recovery;
    }
  }
}

TEST(ContingentLeg, PayAtMaturityIsWorthLessThanPayAtDefault) {
  // Same loss, paid later, discounted more. A structural inequality the port
  // should satisfy regardless of the curves.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kClDiscDates, cdsmodel::golden::kClDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kClSpreadDates, cdsmodel::golden::kClSpreadRates);
  const cdsmodel::Date base = cdsmodel::from_tdate(cdsmodel::golden::kClBase);

  for (int tenor : {365, 1826, 3653, 5479}) {
    const cdsmodel::ContingentLeg at_default{base, base + days{tenor}, 1.0e7,
                                             cdsmodel::ProtectionPayment::AtDefault, true};
    const cdsmodel::ContingentLeg at_maturity{base, base + days{tenor}, 1.0e7,
                                              cdsmodel::ProtectionPayment::AtMaturity, true};
    const double a = *cdsmodel::contingent_leg_pv(at_default, base, base, base, disc, spread, 0.4);
    const double m = *cdsmodel::contingent_leg_pv(at_maturity, base, base, base, disc, spread, 0.4);
    EXPECT_LT(m, a) << "paying at maturity was not cheaper at tenor " << tenor;
  }
}

TEST(ContingentLeg, RejectsAZeroSurvivalProbability) {
  // UPSTREAM-8's resolution: the port rejects a non-finite result rather than
  // guarding a proxy threshold. A hazard rate high enough to underflow the
  // survival probability to exactly zero is the one case that fails.
  const cdsmodel::Date base = d(2024, 1, 1);
  const cdsmodel::Date mat = d(2034, 1, 1);
  const auto disc = cdsmodel::ZeroCurve::create(
      base, std::vector<cdsmodel::RatePoint>{{base + days{1}, 0.03}, {mat, 0.03}},
      cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc.has_value());

  const auto pv_at_hazard = [&](double hazard) {
    const auto spread = cdsmodel::ZeroCurve::create(
        base, std::vector<cdsmodel::RatePoint>{{base + days{1}, hazard}, {mat, hazard}},
        cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
    const cdsmodel::ContingentLeg leg{base, mat, 1.0e7, cdsmodel::ProtectionPayment::AtDefault,
                                      true};
    return cdsmodel::contingent_leg_pv(leg, base, base, base, *disc, *spread, 0.4);
  };

  // Extreme but finite: survival underflows towards zero yet the logs stay
  // finite, and the answer converges on the certain-default limit.
  const auto extreme = pv_at_hazard(50.0);
  ASSERT_TRUE(extreme.has_value()) << "rejected a case the arithmetic handles";
  EXPECT_NEAR(*extreme, 1.0e7 * 0.6, 1.0e4) << "should approach notional * (1 - R)";

  // Survival exactly zero: log(0) is -inf and the result is not finite.
  EXPECT_FALSE(pv_at_hazard(200.0).has_value());
}

} // namespace
