// TIER 2 -- live differential tests for the cash-flow bootstrap.
//
// This is the first module in the port whose correctness is not a formula. An
// instrument price goes in and a zero rate comes out, chosen by a root find,
// so the comparison with upstream is bounded by the solver's stopping
// tolerance rather than by rounding: a root accepted on |f| < 1e-10 is pinned
// to the precision of the PRICE, not of the rate.
//
// The property that does not depend on either side is the fixed point. Price
// the instrument off the curve you built from it and you must get the price
// back. That is checked on both implementations independently.

#include <cdsmodel/cash_flow_bootstrap.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "interp.h"
#include "ldate.h"
#include "zcprvt.h"
}

namespace {

class CashFlowBootstrapDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

long upstream_interp(cdsmodel::InterpType t) {
  return t == cdsmodel::InterpType::Linear ? JPMCDS_LINEAR_INTERP : JPMCDS_FLAT_FORWARDS;
}

struct Ref {
  ZCurve *zc = nullptr;
  TCashFlowList *cfl = nullptr;
  ~Ref() {
    JpmcdsZCFree(zc);
    JpmcdsFreeCFL(cfl);
  }
  Ref() = default;
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
};

/// A curve and a cash flow list, built identically on both sides.
struct Case {
  std::vector<std::pair<cdsmodel::Date, double>> seed;
  std::vector<cdsmodel::CashFlow> flows;
  double price = 1.0;
  std::string name;
};

void build_upstream(Ref &ref, const Case &c, long basis) {
  ref.zc = JpmcdsZCMake(cdsmodel::to_tdate(d(2024, 1, 1)), 32, basis, JPMCDS_ACT_365F);
  if (ref.zc == nullptr) {
    return;
  }
  for (const auto &[date, rate] : c.seed) {
    JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(date), rate);
  }
  std::vector<TDate> dates;
  std::vector<double> amounts;
  for (const cdsmodel::CashFlow &f : c.flows) {
    dates.push_back(cdsmodel::to_tdate(f.date));
    amounts.push_back(f.amount);
  }
  ref.cfl = JpmcdsNewCFL(dates.data(), amounts.data(), static_cast<int>(dates.size()));
}

cdsmodel::ZeroCurveBuilder build_port(const Case &c, long basis) {
  cdsmodel::ZeroCurveBuilder curve = *cdsmodel::ZeroCurveBuilder::create(
      d(2024, 1, 1), 32, basis, cdsmodel::DayCount::Act365Fixed);
  for (const auto &[date, rate] : c.seed) {
    [[maybe_unused]] const bool ok = curve.add_rate(date, rate);
  }
  return curve;
}

/// Coupon bond style instruments across positive, zero and negative curves.
std::vector<Case> cases() {
  const auto bond = [](double coupon, int first_year, int years, double price, double seed_rate,
                       std::string name) {
    Case c;
    c.price = price;
    c.name = std::move(name);
    c.seed = {{d(2025, 1, 1), seed_rate}, {d(2026, 1, 1), seed_rate * 1.3}};
    for (int i = 0; i < years; ++i) {
      const bool last = i == years - 1;
      c.flows.push_back(cdsmodel::CashFlow{d(first_year + i, 1, 1), last ? 1.0 + coupon : coupon});
    }
    return c;
  };
  return {
      bond(0.04, 2027, 5, 0.95, 0.030, "coupon 4%, discount"),
      bond(0.04, 2027, 5, 1.00, 0.030, "coupon 4%, par"),
      bond(0.04, 2027, 5, 1.08, 0.030, "coupon 4%, premium"),
      bond(0.00, 2027, 5, 0.88, 0.030, "zero coupon"),
      bond(0.00, 2027, 5, 1.04, -0.005, "zero coupon, NEGATIVE curve"),
      bond(0.01, 2027, 5, 1.06, -0.005, "coupon 1%, NEGATIVE curve"),
      bond(0.02, 2027, 3, 0.99, 0.001, "short, near-zero curve"),
      bond(0.06, 2028, 8, 1.12, 0.045, "long, high coupon"),
  };
}

TEST_F(CashFlowBootstrapDifferential, TheBootstrappedRateMatchesUpstream) {
  int compared = 0;
  for (const Case &c : cases()) {
    for (long basis : {1L, 2L}) {
      for (cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        Ref ref;
        build_upstream(ref, c, basis);
        ASSERT_NE(ref.zc, nullptr) << c.name;
        ASSERT_NE(ref.cfl, nullptr) << c.name;
        cdsmodel::ZeroCurveBuilder port = build_port(c, basis);

        const int rc =
            JpmcdsZCAddCashFlowList(ref.zc, ref.cfl, c.price, 0L, upstream_interp(interp), nullptr);
        const bool ok = cdsmodel::add_cash_flow_list(port, c.flows, c.price, std::nullopt, interp);
        const std::string where = c.name + " basis " + std::to_string(basis) + " interp " +
                                  std::to_string(upstream_interp(interp));
        ASSERT_EQ(ok, rc == SUCCESS) << where;
        if (!ok) {
          continue;
        }

        // The solved rate sits at the instrument's maturity on both sides.
        const TDate maturity = cdsmodel::to_tdate(c.flows.back().date);
        const int ref_index = JpmcdsZCFindDateExact(ref.zc, maturity);
        ASSERT_GE(ref_index, 0) << where;
        const std::optional<std::size_t> port_index = port.find_exact(c.flows.back().date);
        ASSERT_TRUE(port_index.has_value()) << where;

        // The rate is a SOLVER output, so it is bounded by the stopping
        // tolerance, not by rounding. f_tolerance is 1e-10 on a price of
        // about 1, and the rate-to-price sensitivity over these maturities is
        // order 5, so 1e-10 of price is order 2e-11 of rate. A 4-ULP claim
        // here would be a claim about Brent's internals.
        const double got = port.points()[*port_index].rate;
        const double want = ref.zc->rate[ref_index];
        EXPECT_NEAR(got, want, 1.0e-9) << where << ": solved rate";
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 20) << compared;
}

TEST_F(CashFlowBootstrapDifferential, BothSidesRepriceTheirOwnInstrument) {
  // The property that needs no oracle, checked on both implementations so a
  // shared blind spot would show as both failing rather than as agreement.
  int compared = 0;
  for (const Case &c : cases()) {
    for (cdsmodel::InterpType interp :
         {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
      Ref ref;
      build_upstream(ref, c, 1);
      ASSERT_NE(ref.zc, nullptr);
      cdsmodel::ZeroCurveBuilder port = build_port(c, 1);

      const int rc =
          JpmcdsZCAddCashFlowList(ref.zc, ref.cfl, c.price, 0L, upstream_interp(interp), nullptr);
      const bool ok = cdsmodel::add_cash_flow_list(port, c.flows, c.price, std::nullopt, interp);
      ASSERT_EQ(ok, rc == SUCCESS) << c.name;
      if (!ok) {
        continue;
      }
      const std::string where = c.name + " interp " + std::to_string(upstream_interp(interp));

      double ref_pv = 0.0;
      ASSERT_EQ(JpmcdsJpmcdsZCPresentValueCFL(ref.zc, ref.cfl, 0, ref.cfl->fNumItems - 1,
                                              upstream_interp(interp), nullptr, &ref_pv),
                SUCCESS)
          << where;
      const cdsmodel::Expected<double> port_pv =
          cdsmodel::present_value_cash_flows(port, c.flows, interp);
      ASSERT_TRUE(port_pv.has_value()) << where;

      // Both must return the input price to within the solver's tolerance.
      EXPECT_NEAR(ref_pv, c.price, 1.0e-9) << where << ": upstream is not a fixed point";
      EXPECT_NEAR(*port_pv, c.price, 1.0e-9) << where << ": the port is not a fixed point";
      // And they must agree with each other more closely than either agrees
      // with the price, since they solved the same problem.
      EXPECT_NEAR(*port_pv, ref_pv, 1.0e-10) << where;
      ++compared;
    }
  }
  EXPECT_GT(compared, 10) << compared;
}

TEST_F(CashFlowBootstrapDifferential, ThePresentValueOfASubrangeMatches) {
  // present_value_cash_flows takes a span where upstream takes iLo and iHi,
  // so every subrange is swept rather than the whole list only -- that is
  // where the cached-discount shortcut switches on and off.
  Case c = cases().front();
  Ref ref;
  build_upstream(ref, c, 1);
  ASSERT_NE(ref.zc, nullptr);
  cdsmodel::ZeroCurveBuilder port = build_port(c, 1);
  // Put every flow date in both curves so the shortcut fires.
  for (const cdsmodel::CashFlow &f : c.flows) {
    JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(f.date), 0.035);
    ASSERT_TRUE(port.add_rate(f.date, 0.035));
  }

  int compared = 0;
  const int n = static_cast<int>(c.flows.size());
  for (int lo = 0; lo < n; ++lo) {
    for (int hi = lo; hi < n; ++hi) {
      for (cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        double want = 0.0;
        const int rc = JpmcdsJpmcdsZCPresentValueCFL(ref.zc, ref.cfl, lo, hi,
                                                     upstream_interp(interp), nullptr, &want);
        const cdsmodel::Expected<double> got = cdsmodel::present_value_cash_flows(
            port,
            std::span<const cdsmodel::CashFlow>{c.flows}.subspan(
                static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo + 1)),
            interp);
        ASSERT_EQ(got.has_value(), rc == SUCCESS) << lo << ".." << hi;
        if (got) {
          // Sums of cached discounts times amounts: accumulating, but short.
          EXPECT_NEAR(*got, want,
                      4.0 * std::max(1.0, std::fabs(want)) * std::numeric_limits<double>::epsilon())
              << lo << ".." << hi;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 20) << compared;
}

TEST_F(CashFlowBootstrapDifferential, IndexBoundsAreRejectedTheSameWay) {
  // The port expresses the subrange as a span, so these errors cannot be
  // written. This checks the upstream half of that claim -- that they really
  // are errors and not something the port is quietly dropping.
  Case c = cases().front();
  Ref ref;
  build_upstream(ref, c, 1);
  ASSERT_NE(ref.zc, nullptr);
  double out = 0.0;
  const int n = ref.cfl->fNumItems;
  EXPECT_EQ(
      JpmcdsJpmcdsZCPresentValueCFL(ref.zc, ref.cfl, -1, 2, JPMCDS_LINEAR_INTERP, nullptr, &out),
      FAILURE)
      << "negative iLo";
  EXPECT_EQ(
      JpmcdsJpmcdsZCPresentValueCFL(ref.zc, ref.cfl, 3, 1, JPMCDS_LINEAR_INTERP, nullptr, &out),
      FAILURE)
      << "iLo > iHi";
  EXPECT_EQ(
      JpmcdsJpmcdsZCPresentValueCFL(ref.zc, ref.cfl, 0, n, JPMCDS_LINEAR_INTERP, nullptr, &out),
      FAILURE)
      << "iHi past the end";
  // An empty span is the port's equivalent of iLo > iHi, and it is zero, not
  // an error -- the present value of no cash flows.
  cdsmodel::ZeroCurveBuilder port = build_port(c, 1);
  const cdsmodel::Expected<double> none = cdsmodel::present_value_cash_flows(
      port, std::span<const cdsmodel::CashFlow>{}, cdsmodel::InterpType::Linear);
  ASSERT_TRUE(none.has_value());
  EXPECT_EQ(*none, 0.0);
}

// =====================================================================
// The divergences.
// =====================================================================

TEST_F(CashFlowBootstrapDifferential,
       DISABLED_Upstream21_AnEmptyListCrashesIncludingInTheErrorHandler) {
  // Disabled because it takes the process with it. The first statement reads
  // cfl->fArray[cfl->fNumItems-1] when date==0, and the `done:` handler reads
  // it again on EVERY failure path. JpmcdsNewEmptyCFL(0) gives fNumItems=0
  // with fArray=NULL, so both are NULL[-1]. Measured: SIGSEGV, status 139,
  // both with date==0 and with an explicit date -- in the latter case the
  // function correctly detects the error and then crashes reporting it.
  //
  // Live evidence: ./build/debug/defect_evidence u21 --unsafe
  ZCurve *zc = JpmcdsZCMake(cdsmodel::to_tdate(d(2024, 1, 1)), 8, 1, JPMCDS_ACT_365F);
  JpmcdsZCAddRate(zc, cdsmodel::to_tdate(d(2025, 1, 1)), 0.05);
  TCashFlowList *empty = JpmcdsNewEmptyCFL(0);
  (void)JpmcdsZCAddCashFlowList(zc, empty, 1.0, 0L, JPMCDS_LINEAR_INTERP, nullptr);
  JpmcdsFreeCFL(empty);
  JpmcdsZCFree(zc);
}

TEST_F(CashFlowBootstrapDifferential, Upstream21_ThePortRefusesAnEmptyListInsteadOfReadingFromIt) {
  cdsmodel::ZeroCurveBuilder port =
      *cdsmodel::ZeroCurveBuilder::create(d(2024, 1, 1), 8, 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(port.add_rate(d(2025, 1, 1), 0.05));
  EXPECT_FALSE(
      cdsmodel::add_cash_flow_list(port, {}, 1.0, std::nullopt, cdsmodel::InterpType::Linear));
  EXPECT_FALSE(
      cdsmodel::add_cash_flow_list(port, {}, 1.0, d(2030, 1, 1), cdsmodel::InterpType::Linear));
  EXPECT_EQ(port.points().size(), 1u) << "and nothing was added on the way out";
}

TEST_F(CashFlowBootstrapDifferential, Upstream22_SuccessWithNothingAdded) {
  // When the final payment falls exactly on the curve's last date, every
  // branch declines: the guard admits equality, the search for the first
  // uncovered payment walks off the end, and both remaining branches test
  // false. Upstream returns SUCCESS having changed nothing.
  Ref ref;
  ref.zc = JpmcdsZCMake(cdsmodel::to_tdate(d(2024, 1, 1)), 16, 1, JPMCDS_ACT_365F);
  ASSERT_NE(ref.zc, nullptr);
  for (int i = 1; i <= 3; ++i) {
    ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2024 + i, 1, 1)), 0.03 + 0.002 * i),
              SUCCESS);
  }
  const TDate curve_end = ref.zc->date[ref.zc->numItems - 1];

  TDate dates[2] = {cdsmodel::to_tdate(d(2025, 1, 1)), curve_end};
  double amounts[2] = {0.05, 1.05};
  ref.cfl = JpmcdsNewCFL(dates, amounts, 2);
  ASSERT_NE(ref.cfl, nullptr);

  const int before = ref.zc->numItems;
  const TDate asked_for = cdsmodel::to_tdate(d(2030, 1, 1));
  EXPECT_EQ(JpmcdsZCAddCashFlowList(ref.zc, ref.cfl, 1.0, asked_for, JPMCDS_LINEAR_INTERP, nullptr),
            SUCCESS)
      << "upstream reports success";
  EXPECT_EQ(ref.zc->numItems, before) << "having added nothing";
  EXPECT_LT(JpmcdsZCFindDateExact(ref.zc, asked_for), 0)
      << "and without the date the caller asked for";

  // The port refuses: there was nothing to solve for.
  cdsmodel::ZeroCurveBuilder port =
      *cdsmodel::ZeroCurveBuilder::create(d(2024, 1, 1), 16, 1, cdsmodel::DayCount::Act365Fixed);
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(port.add_rate(d(2024 + i, 1, 1), 0.03 + 0.002 * i));
  }
  const std::vector<cdsmodel::CashFlow> flows{{d(2025, 1, 1), 0.05},
                                              {cdsmodel::from_tdate(curve_end), 1.05}};
  EXPECT_FALSE(
      cdsmodel::add_cash_flow_list(port, flows, 1.0, d(2030, 1, 1), cdsmodel::InterpType::Linear));
  EXPECT_EQ(port.points().size(), 3u) << "and says so without changing anything";
}

TEST_F(CashFlowBootstrapDifferential, Upstream23_NegativeInterpolatedRatesAreDroppedSilently) {
  // `if (rate >= 0.0)` in JpmcdsZCAddCFLPoints, with no else. A zero-coupon
  // bond over a negative curve, varying only the price, so the only thing
  // that changes is the SIGN of the solved rate.
  struct Row {
    double price;
    const char *name;
  };
  int positive_placed = 0;
  int negative_placed = 0;
  for (const Row &row : {Row{0.97, "positive solved rate"}, Row{1.06, "negative solved rate"}}) {
    Ref ref;
    ref.zc = JpmcdsZCMake(cdsmodel::to_tdate(d(2024, 1, 1)), 16, 1, JPMCDS_ACT_365F);
    ASSERT_NE(ref.zc, nullptr);
    JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2025, 1, 1)), -0.005);
    JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2026, 1, 1)), -0.008);

    TDate dates[5];
    double amounts[5];
    for (int i = 0; i < 5; ++i) {
      dates[i] = cdsmodel::to_tdate(d(2027 + i, 1, 1));
      amounts[i] = 0.0;
    }
    amounts[4] = 1.0;
    ref.cfl = JpmcdsNewCFL(dates, amounts, 5);
    ASSERT_NE(ref.cfl, nullptr);

    ASSERT_EQ(
        JpmcdsZCAddCashFlowList(ref.zc, ref.cfl, row.price, 0L, JPMCDS_LINEAR_INTERP, nullptr),
        SUCCESS)
        << row.name;
    int placed = 0;
    for (const TDate date : dates) {
      if (JpmcdsZCFindDateExact(ref.zc, date) >= 0) {
        ++placed;
      }
    }
    if (row.price < 1.0) {
      positive_placed = placed;
    } else {
      negative_placed = placed;
    }

    // The port places every cash flow date regardless of sign.
    cdsmodel::ZeroCurveBuilder port =
        *cdsmodel::ZeroCurveBuilder::create(d(2024, 1, 1), 16, 1, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(port.add_rate(d(2025, 1, 1), -0.005));
    ASSERT_TRUE(port.add_rate(d(2026, 1, 1), -0.008));
    std::vector<cdsmodel::CashFlow> flows;
    for (int i = 0; i < 5; ++i) {
      flows.push_back(cdsmodel::CashFlow{cdsmodel::from_tdate(dates[i]), amounts[i]});
    }
    ASSERT_TRUE(cdsmodel::add_cash_flow_list(port, flows, row.price, std::nullopt,
                                             cdsmodel::InterpType::Linear))
        << row.name;
    int port_placed = 0;
    for (const cdsmodel::CashFlow &f : flows) {
      if (port.find_exact(f.date)) {
        ++port_placed;
      }
    }
    EXPECT_EQ(port_placed, 5) << row.name << ": the port drops nothing";
  }

  EXPECT_GT(positive_placed, negative_placed)
      << "the sign of the solved rate changes how many points the curve ends up with: "
      << positive_placed << " vs " << negative_placed;
}

} // namespace
