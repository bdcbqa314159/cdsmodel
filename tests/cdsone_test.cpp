// TIER 1 -- the upfront charge. The top of the model, verified with no oracle.
//
// Three kinds of check, in increasing distance from "trust me":
//
//   1. the contract -- the spread solver inverts the charge calculator;
//   2. ISDA's own published numbers, from the example program shipped in the
//      distribution, replayed against its recorded discount curve;
//   3. the recorded sweep, with a tolerance derived from the solver
//      tolerances rather than guessed.
//
// (2) is the only external reference in this entire project. Everything else
// is either upstream's behaviour or a property we reasoned out; those three
// numbers were printed by ISDA.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/cdsone.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/cdsone_cases.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::Date;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Why 1e-9 absolute on a per-unit charge, derived.
///
/// The charge is a price computed against a bootstrapped curve, so its
/// accuracy is set by the bootstrap's stopping rule, not by rounding. The
/// bootstrap converges to `|pv| <= 1e-10`, which leaves the implied hazard
/// rate accurate to about 2e-10 (see bootstrap_test.cpp for that derivation).
/// The priced contract's sensitivity to that rate is at most
/// `(1 - recovery) x duration`, which for the longest contract in the
/// recorded set -- five years, recovery 0 -- is about 5.
///
/// So `|charge error| <= 1e-9` per unit notional. On charges of order 1e-3
/// that is a relative 1e-6: loose enough to survive a different libm, tight
/// enough that a real regression cannot hide under it.
inline constexpr double kChargeTolerance = 1.0e-9;

/// The spread solver runs the charge calculator inside a root find, so two
/// solver tolerances compose. The outer stops at `|charge difference| <= 1e-8`
/// and `d(charge)/d(spread)` is the risky duration, about 4.5 for a five-year
/// contract -- giving roughly 2e-9 -- but the inner bootstrap's own error
/// enters through every evaluation. 1e-6 absolute carries both with room.
inline constexpr double kSpreadTolerance = 1.0e-6;

cdsmodel::ZeroCurve example_discount() {
  const auto &spec = cdsmodel::golden::kCdsoneExampleDiscountCurve;
  std::vector<cdsmodel::RatePoint> pts;
  for (std::size_t i = 0; i < spec.n; ++i) {
    pts.push_back({cdsmodel::from_tdate(spec.dates[i]), spec.rates[i]});
  }
  const cdsmodel::Expected<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(spec.day_count);
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(spec.base), pts, spec.basis,
                                      dcc.value_or(cdsmodel::DayCount::Act365Fixed));
}

std::vector<cdsmodel::Calendar> none_calendar() {
  // Upstream's "None" resolves through a case-insensitive lookup to the
  // built-in NONE calendar: no holidays, Saturday/Sunday weekend.
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

cdsmodel::Cds cds_of(const cdsmodel::golden::CdsoneChargeCase &c) {
  cdsmodel::Cds cds;
  cds.start = cdsmodel::from_tdate(c.start);
  cds.end = cdsmodel::from_tdate(c.end);
  cds.coupon_rate = c.coupon_rate;
  cds.coupon_interval = cdsmodel::DateInterval{c.interval_months, cdsmodel::PeriodUnit::Months};
  cds.stub = cdsmodel::StubMethod{cdsmodel::StubPosition::Front, cdsmodel::StubLength::Short};
  cds.day_count = *cdsmodel::from_upstream_code(c.dcc);
  cds.bad_day = *cdsmodel::bad_day_conv_from_upstream(static_cast<long>(c.bad_day));
  cds.pay_accrual_on_default = c.pay_accrual_on_default;
  return cds;
}

cdsmodel::UpfrontDates dates_of(const cdsmodel::golden::CdsoneChargeCase &c) {
  return cdsmodel::UpfrontDates{cdsmodel::from_tdate(c.today), cdsmodel::from_tdate(c.value),
                                cdsmodel::from_tdate(c.benchmark_start),
                                cdsmodel::from_tdate(c.stepin)};
}

// =====================================================================
// 1. The contract.
// =====================================================================

TEST(Cdsone, SpreadSolverReturnsASpreadThatReproducesTheCharge) {
  // The contract, stated as the solver actually guarantees it.
  //
  // My first version of this test asserted the SPREAD round trip -- that
  // feeding a charge back gives the spread it came from -- and it failed at
  // 7200bps by 7.9e-4. That was not a defect; it was the wrong contract.
  //
  // Measured sensitivity of the charge to the quoted spread, five-year
  // contract, recovery 0.4:
  //
  //     spread    charge    d(charge)/d(spread)
  //       25bps   0.0126     4.98
  //      500bps   0.207      3.32
  //     2000bps   0.487      0.91
  //     3600bps   0.565      0.22
  //     5000bps   0.582      0.053
  //     7200bps   0.585     -0.008
  //
  // The charge SATURATES: at wide spreads survival has already collapsed, so
  // almost all protection is paid regardless and the charge stops responding.
  // Past about 5000bps the derivative changes sign, so the map is not even
  // injective and no implementation can invert it uniquely.
  //
  // What the solver promises is `|charge(returned) - target| <= f_tolerance`,
  // and that is well posed everywhere it converges. So that is what is
  // asserted. The spread round trip is asserted separately, restricted to the
  // region where the derivative is healthy.
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  int verified = 0;
  for (const auto &c : cdsmodel::golden::kCdsoneChargeCases) {
    if (!c.solved || c.clean) {
      continue;
    }
    const cdsmodel::Cds cds = cds_of(c);
    const cdsmodel::UpfrontDates dates = dates_of(c);
    const cdsmodel::Expected<double> target = cdsmodel::cdsone_upfront_charge(
        cds, dates, cals, discount, c.one_spread, c.recovery, /*pay_accrued_at_start=*/false);
    ASSERT_TRUE(target.has_value());
    const cdsmodel::Expected<double> implied = cdsmodel::cdsone_spread(
        cds, dates, cals, discount, *target, c.recovery, /*pay_accrued_at_start=*/false);
    if (!implied) {
      continue; // the bracket is [0, 100]; some targets have no root in it
    }
    const cdsmodel::Expected<double> reproduced = cdsmodel::cdsone_upfront_charge(
        cds, dates, cals, discount, *implied, c.recovery, /*pay_accrued_at_start=*/false);
    ASSERT_TRUE(reproduced.has_value()) << "implied spread " << *implied << " is unpriceable";
    // The outer solver stops at |difference| <= 1e-8; allow the bootstrap's
    // own error on top.
    EXPECT_NEAR(*reproduced, *target, 1.0e-7)
        << "charge " << *target << " -> spread " << *implied << " -> charge " << *reproduced;
    ++verified;
  }
  EXPECT_GT(verified, 100) << "too few charges round-tripped: " << verified;
}

TEST(Cdsone, SpreadRoundTripHoldsWhereTheProblemIsWellConditioned) {
  // Below about 1000bps the derivative of charge against spread is above 2,
  // so a charge tolerance of 1e-8 bounds the spread error near 5e-9 and the
  // round trip recovers the original quote. This is the regime a real market
  // quote lives in; see the comment above for why it cannot hold further out.
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  const cdsmodel::UpfrontDates dates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};

  int round_trips = 0;
  for (double coupon_bps : {0.0, 100.0, 500.0}) {
    cdsmodel::Cds cds;
    cds.start = d(2008, 2, 8);
    cds.end = d(2013, 2, 12);
    cds.coupon_rate = coupon_bps / 10000.0;
    cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
    cds.day_count = cdsmodel::DayCount::Act360;
    cds.bad_day = cdsmodel::BadDayConv::Follow;
    cds.pay_accrual_on_default = true;
    for (double spread_bps : {25.0, 50.0, 100.0, 250.0, 500.0, 1000.0}) {
      for (double recovery : {0.0, 0.4, 0.8}) {
        const double spread = spread_bps / 10000.0;
        const cdsmodel::Expected<double> charge =
            cdsmodel::cdsone_upfront_charge(cds, dates, cals, discount, spread, recovery, false);
        ASSERT_TRUE(charge.has_value()) << spread_bps;
        const cdsmodel::Expected<double> recovered =
            cdsmodel::cdsone_spread(cds, dates, cals, discount, *charge, recovery, false);
        ASSERT_TRUE(recovered.has_value()) << spread_bps;
        EXPECT_NEAR(*recovered, spread, kSpreadTolerance)
            << spread_bps << "bps at recovery " << recovery << " round-tripped to "
            << (*recovered * 10000.0) << "bps";
        ++round_trips;
      }
    }
  }
  EXPECT_GT(round_trips, 50) << "too few round trips: " << round_trips;
}

// =====================================================================
// 2. ISDA's published numbers.
// =====================================================================

TEST(Cdsone, ReproducesTheNumbersIsdaPublished) {
  // examples/c/src/main.c prints these three, with a notional of 1e7 and
  // "%f" so six decimals. They are the only published output in the
  // distribution, and the only external reference this project has.
  //
  // The discount curve is the one that example builds, recorded because it
  // comes from the unported interest-rate stack -- 22 nodes generated from 14
  // quoted instruments, every rate 1e-9.
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  constexpr double kNotional = 1.0e7;

  cdsmodel::Cds cds;
  cds.start = d(2008, 2, 8);
  cds.end = d(2008, 2, 12);
  cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
  cds.day_count = cdsmodel::DayCount::Act360;
  cds.bad_day = cdsmodel::BadDayConv::Follow;
  cds.pay_accrual_on_default = true;
  const cdsmodel::UpfrontDates dates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};

  struct Published {
    double coupon_bps;
    double charge;
  };
  for (const Published &p : {Published{0.0, 38651.944609}, Published{3600.0, -10682.519172},
                             Published{7200.0, -60016.982954}}) {
    cds.coupon_rate = p.coupon_bps / 10000.0;
    const cdsmodel::Expected<double> got = cdsmodel::cdsone_upfront_charge(
        cds, dates, cals, discount, 3600.0 / 10000.0, 0.4, /*pay_accrued_at_start=*/false);
    ASSERT_TRUE(got.has_value()) << p.coupon_bps << "bps";
    // Half a unit in the last printed decimal.
    EXPECT_NEAR(*got * kNotional, p.charge, 5.0e-7)
        << "coupon " << p.coupon_bps << "bps: ISDA published " << p.charge << ", port gives "
        << (*got * kNotional);
  }

  // The discount factors the same example prints, all 1.000000.
  for (Date date : {d(2008, 1, 3), d(2009, 1, 3), d(2017, 1, 3)}) {
    EXPECT_NEAR(discount.zero_price(date), 1.0, 5.0e-7) << cdsmodel::to_tdate(date);
  }
}

// =====================================================================
// 3. The recorded sweep.
// =====================================================================

TEST(Cdsone, MatchesRecordedCharges) {
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  int checked = 0;
  int rejected = 0;
  for (const auto &c : cdsmodel::golden::kCdsoneChargeCases) {
    const cdsmodel::Expected<double> got = cdsmodel::cdsone_upfront_charge(
        cds_of(c), dates_of(c), cals, discount, c.one_spread, c.recovery, c.clean);
    const std::string where = "cpn=" + std::to_string(c.coupon_rate) +
                              " s=" + std::to_string(c.one_spread) +
                              " R=" + std::to_string(c.recovery) + (c.clean ? " clean" : " dirty");
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (!c.solved) {
      ++rejected;
      continue;
    }
    EXPECT_NEAR(*got, c.charge, kChargeTolerance) << where;
    ++checked;
  }
  EXPECT_GT(checked, 350) << "too few charges replayed: " << checked;
  EXPECT_LT(rejected, 60) << rejected << " recorded cases produced no charge here";
}

TEST(Cdsone, MatchesRecordedSpreads) {
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  const cdsmodel::UpfrontDates dates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};

  int checked = 0;
  for (const auto &c : cdsmodel::golden::kCdsoneSpreadCases) {
    cdsmodel::Cds cds;
    cds.start = d(2008, 2, 8);
    cds.end = cdsmodel::from_tdate(c.end);
    cds.coupon_rate = c.coupon_rate;
    cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
    cds.day_count = cdsmodel::DayCount::Act360;
    cds.bad_day = cdsmodel::BadDayConv::Follow;
    cds.pay_accrual_on_default = true;

    const cdsmodel::Expected<double> got =
        cdsmodel::cdsone_spread(cds, dates, cals, discount, c.charge, c.recovery, false);
    ASSERT_EQ(got.has_value(), c.solved)
        << "cpn=" << c.coupon_rate << " R=" << c.recovery << " charge=" << c.charge;
    if (!c.solved) {
      continue;
    }
    EXPECT_NEAR(*got, c.spread, kSpreadTolerance)
        << "cpn=" << c.coupon_rate << " R=" << c.recovery << " charge=" << c.charge;
    ++checked;
  }
  EXPECT_GT(checked, 20) << "too few spreads replayed: " << checked;
}

// =====================================================================
// Port semantics.
// =====================================================================

TEST(Cdsone, AtParTheChargeIsZero) {
  // When the contract coupon equals the quoted spread AND the contract shares
  // the benchmark's start date, the upfront charge must vanish: the flat curve
  // was implied from exactly this instrument.
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  for (double spread_bps : {25.0, 100.0, 500.0, 3600.0}) {
    cdsmodel::Cds cds;
    cds.start = d(2008, 2, 2); // the benchmark start, so the two coincide
    cds.end = d(2013, 2, 12);
    cds.coupon_rate = spread_bps / 10000.0;
    cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
    cds.day_count = cdsmodel::DayCount::Act360;
    cds.bad_day = cdsmodel::BadDayConv::Follow;
    cds.pay_accrual_on_default = true;
    // Step-in and value on `today` so the priced contract and the benchmark
    // see the same clamps.
    const cdsmodel::UpfrontDates dates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 1)};
    const cdsmodel::Expected<double> charge = cdsmodel::cdsone_upfront_charge(
        cds, dates, cals, discount, spread_bps / 10000.0, 0.4, /*pay_accrued_at_start=*/true);
    ASSERT_TRUE(charge.has_value()) << spread_bps;
    EXPECT_NEAR(*charge, 0.0, 1.0e-9)
        << "at par (" << spread_bps << "bps) the charge should vanish, got " << *charge;
  }
}

TEST(Cdsone, ChargeRisesWithTheQuotedSpreadAndFallsWithTheCoupon) {
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  const cdsmodel::UpfrontDates dates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};
  const auto charge_at = [&](double coupon_bps, double spread_bps) {
    cdsmodel::Cds cds;
    cds.start = d(2008, 2, 8);
    cds.end = d(2013, 2, 12);
    cds.coupon_rate = coupon_bps / 10000.0;
    cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
    cds.day_count = cdsmodel::DayCount::Act360;
    cds.bad_day = cdsmodel::BadDayConv::Follow;
    cds.pay_accrual_on_default = true;
    return cdsmodel::cdsone_upfront_charge(cds, dates, cals, discount, spread_bps / 10000.0, 0.4,
                                           false);
  };

  // Protection is worth more when the market quotes a wider spread.
  std::optional<double> previous;
  for (double spread_bps : {25.0, 100.0, 500.0, 1000.0, 3600.0}) {
    const cdsmodel::Expected<double> charge = charge_at(100.0, spread_bps);
    ASSERT_TRUE(charge.has_value()) << spread_bps;
    if (previous) {
      EXPECT_GT(*charge, *previous) << "spread " << spread_bps << "bps did not raise the charge";
    }
    previous = *charge;
  }
  // And paying a larger coupon is worse for the buyer.
  previous.reset();
  for (double coupon_bps : {0.0, 100.0, 500.0, 1000.0, 3600.0}) {
    const cdsmodel::Expected<double> charge = charge_at(coupon_bps, 500.0);
    ASSERT_TRUE(charge.has_value()) << coupon_bps;
    if (previous) {
      EXPECT_LT(*charge, *previous) << "coupon " << coupon_bps << "bps did not reduce the charge";
    }
    previous = *charge;
  }
}

TEST(Cdsone, BenchmarkStartAndContractStartAreIndependent) {
  // Two different jobs for two same-typed dates, which is why UpfrontDates
  // exists. Moving the benchmark start changes the implied curve; moving the
  // contract start changes what is being priced. Neither should be ignored.
  const cdsmodel::ZeroCurve discount = example_discount();
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  cdsmodel::Cds cds;
  cds.start = d(2008, 2, 8);
  cds.end = d(2013, 2, 12);
  cds.coupon_rate = 0.01;
  cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
  cds.day_count = cdsmodel::DayCount::Act360;
  cds.bad_day = cdsmodel::BadDayConv::Follow;
  cds.pay_accrual_on_default = true;

  const cdsmodel::UpfrontDates base{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};
  const cdsmodel::Expected<double> reference =
      cdsmodel::cdsone_upfront_charge(cds, base, cals, discount, 0.05, 0.4, false);
  ASSERT_TRUE(reference.has_value());

  cdsmodel::UpfrontDates moved_benchmark = base;
  moved_benchmark.benchmark_start = d(2008, 3, 2);
  const cdsmodel::Expected<double> a =
      cdsmodel::cdsone_upfront_charge(cds, moved_benchmark, cals, discount, 0.05, 0.4, false);
  ASSERT_TRUE(a.has_value());
  EXPECT_NE(*a, *reference) << "moving the benchmark start changed nothing";

  cdsmodel::Cds moved_contract = cds;
  moved_contract.start = d(2008, 1, 8);
  const cdsmodel::Expected<double> b =
      cdsmodel::cdsone_upfront_charge(moved_contract, base, cals, discount, 0.05, 0.4, false);
  ASSERT_TRUE(b.has_value());
  EXPECT_NE(*b, *reference) << "moving the contract start changed nothing";
}

} // namespace
