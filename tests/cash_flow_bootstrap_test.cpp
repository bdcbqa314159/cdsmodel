// TIER 1 -- the cash-flow bootstrap, without an oracle.
//
// Every module before this one could be pinned to a formula. This one solves
// for a rate, so the thing to assert is not a value but a FIXED POINT: price
// the instrument off the curve you built from it and the price must come back.
// That property needs no upstream and no recorded vector, and it is the only
// check here that would survive the solver being replaced.

#include <cdsmodel/cash_flow_bootstrap.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include "golden/cash_flow_bootstrap_cases.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::CashFlow;
using cdsmodel::InterpType;
using cdsmodel::ZeroCurveBuilder;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// The same two-point seed curve the generator uses.
ZeroCurveBuilder seeded(double seed_rate, long basis = 1) {
  ZeroCurveBuilder curve =
      *ZeroCurveBuilder::create(d(2024, 1, 1), 32, basis, cdsmodel::DayCount::Act365Fixed);
  [[maybe_unused]] bool ok = curve.add_rate(d(2025, 1, 1), seed_rate);
  ok = curve.add_rate(d(2026, 1, 1), seed_rate * 1.3);
  return curve;
}

std::vector<CashFlow> bond(double coupon, int first_year, int years) {
  std::vector<CashFlow> flows;
  for (int i = 0; i < years; ++i) {
    const bool last = i == years - 1;
    flows.push_back(CashFlow{d(first_year + i, 1, 1), last ? 1.0 + coupon : coupon});
  }
  return flows;
}

// =====================================================================
// The fixed point, and the invariants a solve must not break.
// =====================================================================

TEST(CashFlowBootstrap, TheInstrumentRepricesToItsOwnInput) {
  int checked = 0;
  for (const double seed_rate : {0.030, 0.001, -0.005, 0.060}) {
    for (const double coupon : {0.0, 0.01, 0.04, 0.07}) {
      for (const double price : {0.85, 0.95, 1.00, 1.06, 1.15}) {
        for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
          ZeroCurveBuilder curve = seeded(seed_rate);
          const std::vector<CashFlow> flows = bond(coupon, 2027, 5);
          if (!cdsmodel::add_cash_flow_list(curve, flows, price, std::nullopt, interp)) {
            continue; // refused is an answer; the fixed point only binds a solve
          }
          const cdsmodel::Expected<double> repriced =
              cdsmodel::present_value_cash_flows(curve, flows, interp);
          ASSERT_TRUE(repriced.has_value());
          const std::string where = "seed " + std::to_string(seed_rate) + " coupon " +
                                    std::to_string(coupon) + " price " + std::to_string(price);
          // The root find stops on |f| < 1e-10 of price, so that is the bar --
          // not a rounding tolerance. Tightening this would be a claim about
          // Brent's internals rather than about the bootstrap.
          EXPECT_NEAR(*repriced, price, 1.0e-9) << where;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(CashFlowBootstrap, ASolveLeavesEveryCachedDiscountConsistent) {
  // The builder's invariant, across a root find that rewrites a rate in place
  // dozens of times. Upstream's equivalent write is unpaired and survives only
  // because of where the solver happens to stop; `set_rate` makes it
  // structural, and this is the check that says so.
  int checked = 0;
  for (const double seed_rate : {0.030, -0.005}) {
    for (const double price : {0.90, 1.00, 1.10}) {
      for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
        ZeroCurveBuilder curve = seeded(seed_rate);
        const std::vector<CashFlow> flows = bond(0.03, 2027, 5);
        if (!cdsmodel::add_cash_flow_list(curve, flows, price, std::nullopt, interp)) {
          continue;
        }
        for (const ZeroCurveBuilder::Point &p : curve.points()) {
          const cdsmodel::Expected<double> fresh = curve.compute_discount(p.date, p.rate);
          ASSERT_TRUE(fresh.has_value());
          EXPECT_EQ(*fresh, p.discount)
              << "price " << price << " at " << cdsmodel::to_tdate(p.date);
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 50) << checked;
}

TEST(CashFlowBootstrap, EveryPaymentDateEndsUpInTheCurve) {
  // What JpmcdsZCAddCFLPoints exists to guarantee, and what UPSTREAM-23 stops
  // it doing for negative rates. Asserted on both signs.
  for (const double seed_rate : {0.030, -0.005}) {
    for (const double price : {0.92, 1.07}) {
      ZeroCurveBuilder curve = seeded(seed_rate);
      const std::vector<CashFlow> flows = bond(0.0, 2027, 5);
      ASSERT_TRUE(
          cdsmodel::add_cash_flow_list(curve, flows, price, std::nullopt, InterpType::Linear))
          << seed_rate << " " << price;
      for (const CashFlow &f : flows) {
        EXPECT_TRUE(curve.find_exact(f.date).has_value())
            << "seed " << seed_rate << " price " << price << " date " << cdsmodel::to_tdate(f.date);
      }
    }
  }
}

TEST(CashFlowBootstrap, APricierInstrumentImpliesALowerRate) {
  // Economic monotonicity: paying more for the same payments means accepting a
  // lower yield. Nothing in the code enforces it, which is what makes it worth
  // asserting -- a sign error in the objective would pass every value check
  // and fail this.
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    std::optional<double> previous;
    for (const double price : {0.80, 0.90, 1.00, 1.10, 1.20}) {
      ZeroCurveBuilder curve = seeded(0.03);
      const std::vector<CashFlow> flows = bond(0.0, 2027, 5);
      ASSERT_TRUE(cdsmodel::add_cash_flow_list(curve, flows, price, std::nullopt, interp)) << price;
      const std::optional<std::size_t> at = curve.find_exact(flows.back().date);
      ASSERT_TRUE(at.has_value());
      const double rate = curve.points()[*at].rate;
      if (previous) {
        EXPECT_LT(rate, *previous) << "price " << price << " did not lower the rate";
      }
      previous = rate;
    }
  }
}

TEST(CashFlowBootstrap, ASingleUncoveredPaymentNeedsNoSolve) {
  // One payment at the target date gives the discount factor directly. The
  // result must be exactly the quotient -- no solver, so no tolerance.
  ZeroCurveBuilder curve = seeded(0.03);
  const std::vector<CashFlow> flows{{d(2030, 1, 1), 1.25}};
  const double price = 1.0;
  ASSERT_TRUE(cdsmodel::add_cash_flow_list(curve, flows, price, std::nullopt, InterpType::Linear));
  const std::optional<std::size_t> at = curve.find_exact(d(2030, 1, 1));
  ASSERT_TRUE(at.has_value());
  EXPECT_EQ(curve.points()[*at].discount, price / 1.25);

  // Upstream rejects a zero on either side of that division, and so does the
  // port: a zero future value divides by zero, and a zero present value
  // implies a discount factor of zero, which no curve can hold.
  ZeroCurveBuilder zero_amount = seeded(0.03);
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(zero_amount, {{{d(2030, 1, 1), 0.0}}}, 1.0,
                                            std::nullopt, InterpType::Linear));
  ZeroCurveBuilder zero_price = seeded(0.03);
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(zero_price, {{{d(2030, 1, 1), 1.25}}}, 0.0,
                                            std::nullopt, InterpType::Linear));
}

TEST(CashFlowBootstrap, TheInputsThatAreRefused) {
  const std::vector<CashFlow> flows = bond(0.04, 2027, 5);

  // An empty list -- upstream reads flows.back() and segfaults (UPSTREAM-21).
  ZeroCurveBuilder a = seeded(0.03);
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(a, {}, 1.0, std::nullopt, InterpType::Linear));

  // Unordered payments: the whole algorithm is a forward walk.
  ZeroCurveBuilder b = seeded(0.03);
  const std::vector<CashFlow> shuffled{{d(2029, 1, 1), 0.04}, {d(2027, 1, 1), 1.04}};
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(b, shuffled, 1.0, std::nullopt, InterpType::Linear));

  // A target date the curve already covers.
  ZeroCurveBuilder c = seeded(0.03);
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(c, flows, 1.0, d(2025, 6, 1), InterpType::Linear));

  // Nothing in the list reaching beyond the curve.
  ZeroCurveBuilder e = seeded(0.03);
  ASSERT_TRUE(e.add_rate(d(2040, 1, 1), 0.04));
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(e, flows, 1.0, d(2045, 1, 1), InterpType::Linear));

  // Nothing uncovered -- UPSTREAM-22's silent success.
  ZeroCurveBuilder f = seeded(0.03);
  ASSERT_TRUE(f.add_rate(flows.back().date, 0.04));
  EXPECT_FALSE(cdsmodel::add_cash_flow_list(f, flows, 1.0, d(2045, 1, 1), InterpType::Linear));

  // Every refusal must leave the curve untouched.
  for (const ZeroCurveBuilder *curve : {&a, &b, &c}) {
    EXPECT_EQ(curve->points().size(), 2u);
  }
}

TEST(CashFlowBootstrap, AnEmptyRunOfCashFlowsIsWorthNothing) {
  const ZeroCurveBuilder curve = seeded(0.03);
  const cdsmodel::Expected<double> none =
      cdsmodel::present_value_cash_flows(curve, {}, InterpType::Linear);
  ASSERT_TRUE(none.has_value()) << "not an error -- upstream's iLo > iHi check has no subject";
  EXPECT_EQ(*none, 0.0);
}

TEST(CashFlowBootstrap, BootstrappingOntoAnEmptyCurveUsesLinearRegardless) {
  // Upstream overrides the caller's interpolation when the curve has no
  // points, because flat forwards has nothing to be flat between. Both
  // requests must therefore give the same curve.
  std::vector<double> rates;
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    ZeroCurveBuilder curve =
        *ZeroCurveBuilder::create(d(2024, 1, 1), 32, 1, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(curve.empty());
    const std::vector<CashFlow> flows = bond(0.03, 2027, 4);
    ASSERT_TRUE(cdsmodel::add_cash_flow_list(curve, flows, 0.97, std::nullopt, interp));
    const std::optional<std::size_t> at = curve.find_exact(flows.back().date);
    ASSERT_TRUE(at.has_value());
    rates.push_back(curve.points()[*at].rate);
  }
  ASSERT_EQ(rates.size(), 2u);
  EXPECT_EQ(rates[0], rates[1]) << "the interpolation request is overridden, so bit-identical";
}

// =====================================================================
// Replay.
// =====================================================================

TEST(CashFlowBootstrap, MatchesRecordedBootstraps) {
  int compared = 0;
  int divergences = 0;
  for (const auto &c : cdsmodel::golden::kCashFlowBootstrapCases) {
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    ZeroCurveBuilder curve = seeded(c.seed_rate, c.basis);
    const std::vector<CashFlow> flows = bond(c.coupon, c.first_year, c.years);
    const bool ok = cdsmodel::add_cash_flow_list(curve, flows, c.price, std::nullopt, interp);
    const std::string where = "coupon " + std::to_string(c.coupon) + " price " +
                              std::to_string(c.price) + " basis " + std::to_string(c.basis) +
                              " interp " + std::to_string(c.interp);
    ASSERT_EQ(ok, c.accepted) << where;
    if (!ok) {
      continue;
    }

    // The solved rate sits at the instrument's maturity on both sides, and it
    // is a solver output -- compared at the recorded tolerance, which is the
    // f-tolerance of the root find and not a rounding bound.
    const std::optional<std::size_t> at = curve.find_exact(flows.back().date);
    ASSERT_TRUE(at.has_value()) << where;
    const cdsmodel::TDate maturity = cdsmodel::to_tdate(flows.back().date);
    cdsmodel::Expected<double> recorded_rate =
        cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not yet recorded");
    for (std::size_t i = 0; i < c.n; ++i) {
      if (c.dates[i] == maturity) {
        recorded_rate = c.rates[i];
      }
    }
    ASSERT_TRUE(recorded_rate.has_value()) << where << ": the maturity is in every record";
    EXPECT_NEAR(curve.points()[*at].rate, *recorded_rate, cdsmodel::golden::kSolverTolerance)
        << where;

    if (c.n_dropped == 0) {
      // No filter fired, so the whole curve must agree.
      ASSERT_EQ(curve.points().size(), c.n) << where;
      for (std::size_t i = 0; i < c.n; ++i) {
        EXPECT_EQ(cdsmodel::to_tdate(curve.points()[i].date), c.dates[i]) << where << " i=" << i;
        EXPECT_NEAR(curve.points()[i].rate, c.rates[i], cdsmodel::golden::kSolverTolerance)
            << where << " i=" << i;
      }
    } else {
      // UPSTREAM-23 fired. The port places what upstream dropped, so the
      // difference must be exactly those dates -- no more, no fewer.
      ++divergences;
      EXPECT_EQ(curve.points().size(), c.n + c.n_dropped) << where;
      for (std::size_t i = 0; i < c.n_dropped; ++i) {
        const cdsmodel::Date dropped = cdsmodel::from_tdate(c.dropped[i]);
        EXPECT_TRUE(curve.find_exact(dropped).has_value())
            << where << ": the port must place " << c.dropped[i];
      }
      for (std::size_t i = 0; i < c.n; ++i) {
        EXPECT_TRUE(curve.find_exact(cdsmodel::from_tdate(c.dates[i])).has_value())
            << where << ": and must keep everything upstream had";
      }
    }
    ++compared;
  }
  EXPECT_EQ(compared, 60) << compared;
  // Twelve of the sixty recorded curves lost at least one cash flow date to
  // UPSTREAM-23's sign filter. The count is asserted so that a change in
  // upstream's behaviour shows up as a failure rather than as fewer checks.
  EXPECT_EQ(divergences, 12) << "the recorded UPSTREAM-23 cases: " << divergences;
}

} // namespace
