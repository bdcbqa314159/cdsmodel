// TIER 1 -- the clean spread curve bootstrap. Contract first, then replay.
//
// The primary assertion in this file needs no oracle and no recorded number:
// a curve bootstrapped from quoted benchmarks must reprice those benchmarks
// to zero. That is what the bootstrap means, it is identical on every
// platform, and it is the check the root finder slice taught me to write
// first rather than fifth.
//
// The recorded rates are a regression check sitting behind it, with a
// tolerance DERIVED from the solver's guarantee rather than guessed.

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/bootstrap_cases.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::Date;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Why 1e-9 absolute, derived rather than tuned.
///
/// The bootstrap's solver stops when `|pv(s)| <= 1e-10`. Two platforms may
/// therefore return different spreads that both satisfy it, and the spread
/// difference is bounded by `2 * 1e-10 / |d pv / d s|`. That derivative is the
/// risky duration of the premium leg, which is at least about 1 for the
/// shortest benchmark here (one year), giving `|ds| <= 2e-10` on the
/// continuously compounded rate.
///
/// The published rate is then `exp(s_cc) - 1`, whose derivative is `exp(s_cc)`
/// -- about 1.02 for an investment-grade rate and up to roughly 1.7 for the
/// distressed ladders in the recorded set. So the published difference is
/// bounded near 3.4e-10, and 1e-9 leaves a factor of three.
///
/// This is deliberately NOT the relative 1e-12 used for accumulating values.
/// A solver's output is determined by its stopping tolerance, not by the
/// rounding of the arithmetic that got there -- a distinction the root finder
/// slice established the hard way.
inline constexpr double kRateTolerance = 1.0e-9;

cdsmodel::ZeroCurve curve_from(const cdsmodel::golden::BootstrapCurveSpec &spec) {
  std::vector<cdsmodel::RatePoint> pts;
  for (std::size_t i = 0; i < spec.n; ++i) {
    pts.push_back({cdsmodel::from_tdate(spec.dates[i]), spec.rates[i]});
  }
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(spec.base), pts, 1.0,
                                      cdsmodel::DayCount::Act365Fixed);
}

std::vector<cdsmodel::Calendar> no_holidays() {
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

struct Replay {
  std::vector<cdsmodel::Benchmark> marks;
  cdsmodel::Cds conventions;
  double recovery;
  Date today;
  Date start;
};

Replay replay_of(const cdsmodel::golden::BootstrapCase &c) {
  Replay r;
  r.today = cdsmodel::from_tdate(c.today);
  r.start = cdsmodel::from_tdate(c.start);
  r.recovery = c.recovery;
  for (std::size_t i = 0; i < c.nb; ++i) {
    r.marks.push_back(cdsmodel::Benchmark{cdsmodel::from_tdate(c.ends[i]), c.coupons[i], true});
  }
  r.conventions.start = r.start;
  r.conventions.day_count = *cdsmodel::from_upstream_code(c.dcc);
  r.conventions.coupon_interval =
      cdsmodel::DateInterval{c.interval_months, cdsmodel::PeriodUnit::Months};
  r.conventions.pay_accrual_on_default = c.pay_accrual_on_default;
  return r;
}

// =====================================================================
// The contract.
// =====================================================================

TEST(Bootstrap, EveryBootstrappedCurveRepricesItsOwnBenchmarks) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const std::vector<cdsmodel::Calendar> cals = no_holidays();

  int repriced = 0;
  int infeasible = 0;
  for (const auto &c : cdsmodel::golden::kBootstrapCases) {
    const Replay r = replay_of(c);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
        r.today, discount, r.start, r.today, r.today, r.marks, r.recovery, r.conventions, cals);
    if (!curve) {
      ++infeasible;
      continue;
    }
    for (const cdsmodel::Benchmark &b : r.marks) {
      cdsmodel::Cds priced = r.conventions;
      priced.end = b.end;
      priced.coupon_rate = b.coupon_rate;
      const cdsmodel::Expected<double> pv = cdsmodel::cds_price(
          priced, r.today, r.today, r.today, cals, discount, *curve, r.recovery, /*clean=*/true);
      ASSERT_TRUE(pv.has_value()) << cdsmodel::to_tdate(b.end);
      // The solver converged to |pv| <= 1e-10; 1e-9 allows for the repricing
      // going back through cds_price rather than the bootstrap's own objective.
      EXPECT_NEAR(*pv, 0.0, 1.0e-9)
          << "benchmark " << cdsmodel::to_tdate(b.end) << " at " << b.coupon_rate << " repriced to "
          << *pv << " (recovery " << r.recovery << ")";
      ++repriced;
    }
  }
  // 1488 benchmark repricings across the recorded set. The floor sits just
  // below the measured count so a shrinking sweep fails rather than passing
  // quietly.
  EXPECT_GT(repriced, 1400) << "too few benchmarks repriced: " << repriced;
  // Surfaced so a shrinking denominator cannot read as full coverage.
  EXPECT_LT(infeasible, 50) << infeasible << " recorded cases did not bootstrap here";
}

TEST(Bootstrap, SurvivalFallsMonotonicallyOnEveryFeasibleCurve) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kBootstrapCases) {
    const Replay r = replay_of(c);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
        r.today, discount, r.start, r.today, r.today, r.marks, r.recovery, r.conventions, cals);
    if (!curve) {
      continue;
    }
    // A curve that passed the bootstrap's forward-hazard check cannot imply a
    // rising survival probability. This is the property that check defends,
    // asserted on the output rather than trusted from the input.
    double previous = 1.0;
    for (const cdsmodel::RatePoint &p : curve->points()) {
      const double survival = curve->zero_price(p.date);
      EXPECT_LE(survival, previous) << "survival rose at " << cdsmodel::to_tdate(p.date)
                                    << " (recovery " << r.recovery << ")";
      EXPECT_GT(survival, 0.0) << cdsmodel::to_tdate(p.date);
      previous = survival;
    }
    ++checked;
  }
  EXPECT_GT(checked, 300) << "too few curves checked: " << checked;
}

// =====================================================================
// Replay.
// =====================================================================

TEST(Bootstrap, MatchesRecordedRatesAndFeasibility) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const std::vector<cdsmodel::Calendar> cals = no_holidays();

  int feasible = 0;
  int rejected = 0;
  for (const auto &c : cdsmodel::golden::kBootstrapCases) {
    const Replay r = replay_of(c);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
        r.today, discount, r.start, r.today, r.today, r.marks, r.recovery, r.conventions, cals);
    const std::string where = "nb=" + std::to_string(c.nb) + " R=" + std::to_string(c.recovery) +
                              " dcc=" + std::to_string(c.dcc) +
                              " q0=" + std::to_string(c.coupons[0]);

    // Feasibility is part of the contract: the boundary of the bootstrappable
    // region must sit in the same place. Unlike the root finder's convergence
    // verdict, this one IS portable -- the objective is monotone in the
    // spread, so whether a root exists in [0, 1e10] is a property of the
    // market data and not of the iteration path.
    ASSERT_EQ(got.has_value(), c.n != 0) << where;
    if (c.n == 0) {
      ++rejected;
      continue;
    }
    ASSERT_EQ(got->points().size(), c.n) << where;
    EXPECT_EQ(got->basis(), 1.0) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(got->points()[i].date), c.ends[i]) << where << " i=" << i;
      EXPECT_NEAR(got->points()[i].rate, c.rates[i], kRateTolerance) << where << " i=" << i;
    }
    ++feasible;
  }
  EXPECT_GT(feasible, 300) << "too few feasible cases replayed: " << feasible;
  EXPECT_GT(rejected, 20) << "the infeasible side is barely represented: " << rejected;
}

// =====================================================================
// Port semantics.
// =====================================================================

TEST(Bootstrap, PublishesAnnualCompoundingNotTheContinuousRatesItSolvedWith) {
  // Upstream solves continuously because it is faster and converts at the end
  // "since that is traditional". A caller reading points() must see the annual
  // rates, and annual is strictly below continuous for a positive rate.
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kBootstrapToday);
  cdsmodel::Cds conv;
  conv.start = today;
  const std::vector<cdsmodel::Benchmark> marks{{d(2027, 6, 20), 0.05, true},
                                               {d(2031, 6, 20), 0.06, true}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
      today, discount, today, today, today, marks, 0.4, conv, no_holidays());
  ASSERT_TRUE(curve.has_value());
  EXPECT_EQ(curve->basis(), 1.0);

  for (const cdsmodel::RatePoint &p : curve->points()) {
    // annual = exp(cc) - 1 > cc for cc > 0, so the published rate must exceed
    // the continuous one it came from. Recover cc and check the direction.
    const cdsmodel::Expected<double> cc = cdsmodel::convert_compound_rate(
        p.rate, 1.0, cdsmodel::DayCount::Act365Fixed, cdsmodel::kContinuousBasis,
        cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(cc.has_value());
    EXPECT_GT(p.rate, *cc) << "annual rate should exceed the continuous rate it converted from";
    EXPECT_NEAR(*cc, std::log(1.0 + p.rate), 1.0e-15);
  }
}

TEST(Bootstrap, ExcludedBenchmarksDoNotAppearInTheCurve) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kBootstrapToday);
  cdsmodel::Cds conv;
  conv.start = today;

  std::vector<cdsmodel::Benchmark> marks{{d(2025, 6, 20), 0.01, true},
                                         {d(2026, 6, 20), 0.012, false},
                                         {d(2027, 6, 20), 0.014, true},
                                         {d(2029, 6, 20), 0.017, false}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
      today, discount, today, today, today, marks, 0.4, conv, no_holidays());
  ASSERT_TRUE(curve.has_value());
  ASSERT_EQ(curve->points().size(), 2u);
  EXPECT_EQ(curve->points()[0].date, d(2025, 6, 20));
  EXPECT_EQ(curve->points()[1].date, d(2027, 6, 20));

  // And the result is identical to passing only the included ones, which is
  // what makes the include flag a filter rather than a weighting.
  const std::vector<cdsmodel::Benchmark> only_included{{d(2025, 6, 20), 0.01, true},
                                                       {d(2027, 6, 20), 0.014, true}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> direct = cdsmodel::clean_spread_curve(
      today, discount, today, today, today, only_included, 0.4, conv, no_holidays());
  ASSERT_TRUE(direct.has_value());
  ASSERT_EQ(direct->points().size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(curve->points()[i].rate, direct->points()[i].rate) << "i=" << i;
  }
}

TEST(Bootstrap, Rejected) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kBootstrapToday);
  cdsmodel::Cds conv;
  conv.start = today;
  const std::vector<cdsmodel::Calendar> cals = no_holidays();

  // No benchmarks at all.
  EXPECT_FALSE(
      cdsmodel::clean_spread_curve(today, discount, today, today, today, {}, 0.4, conv, cals)
          .has_value());
  // All excluded.
  const std::vector<cdsmodel::Benchmark> none{{d(2027, 6, 20), 0.01, false}};
  EXPECT_FALSE(
      cdsmodel::clean_spread_curve(today, discount, today, today, today, none, 0.4, conv, cals)
          .has_value());
  // Dates not strictly increasing.
  const std::vector<cdsmodel::Benchmark> backwards{{d(2029, 6, 20), 0.01, true},
                                                   {d(2027, 6, 20), 0.012, true}};
  EXPECT_FALSE(
      cdsmodel::clean_spread_curve(today, discount, today, today, today, backwards, 0.4, conv, cals)
          .has_value());
  // Degenerate recovery rates. The guess is coupon/(1-recovery), unguarded.
  // recovery == 1 makes it infinite and recovery > 1 makes it negative; both
  // fall outside the solver's [0, 1e10] bounds.
  const std::vector<cdsmodel::Benchmark> ok{{d(2027, 6, 20), 0.01, true}};
  for (double recovery : {1.0, 1.5}) {
    EXPECT_FALSE(
        cdsmodel::clean_spread_curve(today, discount, today, today, today, ok, recovery, conv, cals)
            .has_value())
        << "recovery " << recovery;
  }
  // recovery == 1 with a ZERO coupon makes the guess 0.0/0.0, and a NaN guess
  // passes the solver's bounds check because neither comparison holds for a
  // NaN. It still fails, which is why this is a non-finding.
  const std::vector<cdsmodel::Benchmark> zero_coupon{{d(2027, 6, 20), 0.0, true}};
  EXPECT_FALSE(cdsmodel::clean_spread_curve(today, discount, today, today, today, zero_coupon, 1.0,
                                            conv, cals)
                   .has_value());
}

TEST(Bootstrap, NegativeRecoveryIsAcceptedNotRejected) {
  // Worth pinning because I assumed the opposite and was wrong.
  //
  // A recovery below zero means a loss exceeding notional. Unusual, but the
  // arithmetic is coherent -- loss given default is 1 - recovery, so a
  // recovery of -0.5 is an LGD of 1.5 -- and upstream permits it. Measured
  // against the oracle: recovery -0.5 and -2.0 both bootstrap, and the solved
  // hazards fall as LGD rises, which is the right direction.
  //
  // Only the guess formula constrains recovery: coupon/(1 - recovery) must
  // land inside [0, 1e10], which excludes recovery >= 1 and nothing below it.
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kBootstrapToday);
  cdsmodel::Cds conv;
  conv.start = today;
  const std::vector<cdsmodel::Benchmark> marks{{d(2025, 6, 20), 0.01, true},
                                               {d(2027, 6, 20), 0.01, true}};

  std::optional<double> previous_rate;
  for (double recovery : {0.4, 0.0, -0.5, -2.0}) {
    const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
        today, discount, today, today, today, marks, recovery, conv, no_holidays());
    ASSERT_TRUE(curve.has_value()) << "recovery " << recovery;
    const double rate = curve->points()[0].rate;
    EXPECT_GT(rate, 0.0) << "recovery " << recovery;
    // Hazard is roughly spread / (1 - recovery), so a larger LGD gives a
    // smaller hazard for the same quote. Strictly decreasing across the list.
    if (previous_rate) {
      EXPECT_LT(rate, *previous_rate)
          << "recovery " << recovery << " did not reduce the implied hazard";
    }
    previous_rate = rate;
  }
}

TEST(Bootstrap, AZeroCouponLadderGivesAZeroHazardCurve) {
  // A quoted spread of zero means no default risk, so every solved rate must
  // be zero and survival must stay at one.
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kBootstrapDiscountCurve);
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kBootstrapToday);
  cdsmodel::Cds conv;
  conv.start = today;
  const std::vector<cdsmodel::Benchmark> marks{
      {d(2025, 6, 20), 0.0, true}, {d(2027, 6, 20), 0.0, true}, {d(2031, 6, 20), 0.0, true}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
      today, discount, today, today, today, marks, 0.4, conv, no_holidays());
  ASSERT_TRUE(curve.has_value());
  for (const cdsmodel::RatePoint &p : curve->points()) {
    EXPECT_EQ(p.rate, 0.0) << cdsmodel::to_tdate(p.date);
    EXPECT_EQ(curve->zero_price(p.date), 1.0) << cdsmodel::to_tdate(p.date);
  }
}

} // namespace
