// TIER 2 -- live differential tests for the clean spread curve bootstrap.
//
// The first test in this file is deliberately NOT a comparison against
// upstream. It is the bootstrap's defining property: a curve bootstrapped
// from a set of quoted benchmarks must reprice those benchmarks to zero.
//
// That ordering is a lesson from the root finder slice, where three CI rounds
// were spent discovering that the things I had recorded -- the iteration
// count, the convergence verdict -- were not portable, while the property a
// caller actually depends on was never asserted at all. A recorded rate says
// "macOS got this number". "The curve reprices its own inputs" says the
// bootstrap works, needs no oracle, and holds on every platform.
//
// The comparison against upstream comes second, and it is still bit-exact.

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cds.h"
#include "cerror.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "stub.h"
#include "tcurve.h"
}

namespace {

class BootstrapDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &pts) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : pts) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(pts.size()), 1.0, JPMCDS_ACT_365F);
}

/// The quoted market: the standard CDS maturity ladder.
std::vector<cdsmodel::Benchmark> ladder(cdsmodel::Date today, const std::vector<double> &quotes) {
  const std::vector<cdsmodel::Date> maturities{d(2025, 6, 20), d(2026, 6, 20), d(2027, 6, 20),
                                               d(2029, 6, 20), d(2031, 6, 20), d(2034, 6, 20)};
  std::vector<cdsmodel::Benchmark> out;
  for (std::size_t i = 0; i < quotes.size() && i < maturities.size(); ++i) {
    out.push_back(cdsmodel::Benchmark{maturities[i], quotes[i], true});
  }
  (void)today;
  return out;
}

struct Market {
  cdsmodel::Date today = d(2024, 6, 20);
  std::vector<cdsmodel::RatePoint> disc_pts{{d(2024, 6, 20), 0.028},
                                            {d(2025, 6, 20), 0.031},
                                            {d(2027, 6, 20), 0.034},
                                            {d(2031, 6, 20), 0.037},
                                            {d(2036, 6, 20), 0.039}};
  cdsmodel::ZeroCurve discount =
      *cdsmodel::ZeroCurve::create(today, disc_pts, 1.0, cdsmodel::DayCount::Act365Fixed);
  TCurve *ref_discount = build(today, disc_pts);

  ~Market() { JpmcdsFreeTCurve(ref_discount); }
  Market(const Market &) = delete;
  Market &operator=(const Market &) = delete;
  Market() = default;
};

cdsmodel::Cds conventions(cdsmodel::DayCount dcc = cdsmodel::DayCount::Act360,
                          bool accrual = true) {
  cdsmodel::Cds c;
  c.start = d(2024, 6, 20);
  c.day_count = dcc;
  c.pay_accrual_on_default = accrual;
  return c;
}

std::vector<cdsmodel::Calendar> no_holidays() {
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

/// Upstream's bootstrap over the same inputs. Caller frees.
TCurve *reference_bootstrap(const Market &m, const std::vector<cdsmodel::Benchmark> &marks,
                            double recovery, const cdsmodel::Cds &conv, cdsmodel::Date start,
                            cdsmodel::Date stepin, cdsmodel::Date cash_settle) {
  std::vector<TDate> ends;
  std::vector<double> coupons;
  std::vector<TBoolean> includes;
  bool any_excluded = false;
  for (const cdsmodel::Benchmark &b : marks) {
    ends.push_back(cdsmodel::to_tdate(b.end));
    coupons.push_back(b.coupon_rate);
    includes.push_back(b.include ? TRUE : FALSE);
    any_excluded = any_excluded || !b.include;
  }
  TDateInterval iv{conv.coupon_interval.periods, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = conv.stub.position == cdsmodel::StubPosition::Back ? TRUE : FALSE;
  stub.longStub = conv.stub.length == cdsmodel::StubLength::Long ? TRUE : FALSE;
  return JpmcdsCleanSpreadCurve(
      cdsmodel::to_tdate(m.today), m.ref_discount, cdsmodel::to_tdate(start),
      cdsmodel::to_tdate(stepin), cdsmodel::to_tdate(cash_settle), static_cast<long>(ends.size()),
      ends.data(), coupons.data(), any_excluded ? includes.data() : nullptr, recovery,
      conv.pay_accrual_on_default ? TRUE : FALSE, &iv, cdsmodel::to_upstream_code(conv.day_count),
      &stub, static_cast<long>(static_cast<char>(conv.bad_day)), const_cast<char *>("NONE"));
}

// =====================================================================
// The contract. No oracle, no recorded numbers.
// =====================================================================

TEST_F(BootstrapDifferential, BootstrappedCurveRepricesItsOwnBenchmarksToZero) {
  const Market m;
  const std::vector<cdsmodel::Calendar> cals = no_holidays();

  int repriced = 0;
  int infeasible = 0;
  for (const std::vector<double> &quotes :
       std::vector<std::vector<double>>{{0.0025, 0.0030, 0.0035, 0.0045, 0.0055, 0.0065},
                                        {0.01, 0.012, 0.014, 0.017, 0.019, 0.021},
                                        {0.05, 0.055, 0.06, 0.07, 0.08, 0.09},
                                        {0.002},
                                        {0.01, 0.01, 0.01, 0.01, 0.01, 0.01}}) {
    for (double recovery : {0.0, 0.2, 0.4, 0.8}) {
      for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
        const std::vector<cdsmodel::Benchmark> marks = ladder(m.today, quotes);
        const cdsmodel::Cds conv = conventions(dcc);
        const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
            m.today, m.discount, conv.start, m.today, m.today, marks, recovery, conv, cals);
        if (!curve) {
          // Not every quote ladder is bootstrappable, and that is not a
          // failure of the port -- see
          // TheFeasibleRegionHasABoundaryBothImplementationsAgreeOn. Counted
          // rather than skipped silently.
          ++infeasible;
          continue;
        }

        for (const cdsmodel::Benchmark &b : marks) {
          cdsmodel::Cds priced = conv;
          priced.end = b.end;
          priced.coupon_rate = b.coupon_rate;
          const cdsmodel::Expected<double> pv = cdsmodel::cds_price(
              priced, m.today, m.today, m.today, cals, m.discount, *curve, recovery,
              /*clean=*/true);
          ASSERT_TRUE(pv.has_value()) << cdsmodel::to_tdate(b.end);
          // The solver converged to |pv| <= 1e-10 at its own base date; the
          // repricing here goes through cds_price, which clamps protection to
          // max(stepin, start) rather than max(today, start). With
          // stepin == today those coincide, so the residual is the solver's
          // tolerance and nothing more.
          EXPECT_NEAR(*pv, 0.0, 1.0e-9)
              << "benchmark " << cdsmodel::to_tdate(b.end) << " at " << b.coupon_rate
              << " repriced to " << *pv << " (recovery " << recovery << ")";
          ++repriced;
        }
      }
    }
  }
  // 188 benchmark repricings over this sweep. The floor exists to catch the
  // denominator silently shrinking, so it sits just below the measured count
  // rather than at a round number.
  EXPECT_GT(repriced, 150) << "too few benchmarks repriced: " << repriced;
  // Surfaced, not hidden: a silently shrinking denominator reads as full
  // coverage. If this climbs, the sweep is testing less than it appears to.
  EXPECT_LT(infeasible, 8) << infeasible
                           << " of the quote/recovery combinations did not "
                              "bootstrap; the sweep is thinner than it looks";
}

TEST_F(BootstrapDifferential, TheFeasibleRegionHasABoundaryBothImplementationsAgreeOn) {
  // Not every quoted ladder can be bootstrapped, and the boundary is not
  // obvious from the quotes alone -- it depends on the term structure of
  // implied hazard against loss given default.
  //
  // Measured by growing the distressed ladder one benchmark at a time at
  // recovery 0.8 (LGD 0.2). The first four nodes solve to hazard rates of
  // 0.287, 0.326, 0.375 and 0.542; the fifth, a 7-year quoted at 800bp, does
  // not. By then survival to five years is already about 6.6%, and reaching
  // the 7-year quote would need a forward hazard the curve cannot deliver
  // while staying arbitrage-free.
  //
  // The point of this test is not the boundary's location, which is a
  // property of the market data. It is that the port and the 2009 C put it in
  // exactly the same place -- including for the degenerate inputs, where
  // agreeing to reject is as much a part of the contract as agreeing on a
  // number.
  const Market m;
  const cdsmodel::Cds conv = conventions();
  const std::vector<double> distressed{0.05, 0.055, 0.06, 0.07, 0.08, 0.09};

  int agreed_ok = 0;
  int agreed_rejected = 0;
  for (double recovery : {0.0, 0.2, 0.4, 0.6, 0.8}) {
    for (std::size_t n = 1; n <= distressed.size(); ++n) {
      const std::vector<double> quotes(distressed.begin(),
                                       distressed.begin() + static_cast<std::ptrdiff_t>(n));
      const std::vector<cdsmodel::Benchmark> marks = ladder(m.today, quotes);
      TCurve *expected =
          reference_bootstrap(m, marks, recovery, conv, conv.start, m.today, m.today);
      const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
          m.today, m.discount, conv.start, m.today, m.today, marks, recovery, conv, no_holidays());
      ASSERT_EQ(got.has_value(), expected != nullptr)
          << "recovery " << recovery << " with " << n << " benchmarks";
      if (expected != nullptr) {
        ++agreed_ok;
        JpmcdsFreeTCurve(expected);
      } else {
        ++agreed_rejected;
      }
    }
  }
  // Both halves must be exercised or the test proves only one direction.
  EXPECT_GT(agreed_ok, 15) << "too few feasible cases: " << agreed_ok;
  // Exactly two cases in this grid are infeasible: the 5- and 6-benchmark
  // distressed ladders at recovery 0.8. Asserted as a minimum so the
  // infeasible side is known to be exercised, not as a count to preserve.
  EXPECT_GE(agreed_rejected, 2) << "the infeasible side was not reached: " << agreed_rejected;
}

TEST_F(BootstrapDifferential, BootstrappedCurveIsAnnuallyCompoundedAndIncreasingInDate) {
  const Market m;
  const std::vector<cdsmodel::Benchmark> marks =
      ladder(m.today, {0.01, 0.012, 0.014, 0.017, 0.019, 0.021});
  const cdsmodel::Cds conv = conventions();
  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::clean_spread_curve(
      m.today, m.discount, conv.start, m.today, m.today, marks, 0.4, conv, no_holidays());
  ASSERT_TRUE(curve.has_value());

  EXPECT_EQ(curve->basis(), 1.0) << "the published curve must be annually compounded";
  EXPECT_EQ(curve->day_count(), cdsmodel::DayCount::Act365Fixed);
  EXPECT_EQ(curve->base_date(), m.today);
  ASSERT_EQ(curve->points().size(), marks.size());
  for (std::size_t i = 0; i < marks.size(); ++i) {
    EXPECT_EQ(curve->points()[i].date, marks[i].end) << "i=" << i;
    EXPECT_GT(curve->points()[i].rate, 0.0) << "i=" << i;
  }
  // Survival must fall monotonically.
  double previous = 1.0;
  for (const cdsmodel::RatePoint &p : curve->points()) {
    const double survival = curve->zero_price(p.date);
    EXPECT_LT(survival, previous) << "survival rose at " << cdsmodel::to_tdate(p.date);
    previous = survival;
  }
}

// =====================================================================
// The differential comparison.
// =====================================================================

TEST_F(BootstrapDifferential, CurveMatchesBitExactlyAcrossTheQuoteSpace) {
  const Market m;
  const std::vector<cdsmodel::Calendar> cals = no_holidays();

  int compared = 0;
  int rejected_by_both = 0;
  for (const std::vector<double> &quotes : std::vector<std::vector<double>>{
           {0.0025, 0.0030, 0.0035, 0.0045, 0.0055, 0.0065}, // upward sloping, tight
           {0.01, 0.012, 0.014, 0.017, 0.019, 0.021},        // upward sloping
           {0.05, 0.048, 0.046, 0.044, 0.042, 0.040},        // INVERTED
           {0.08, 0.09, 0.10, 0.12, 0.14, 0.16},             // distressed
           {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},             // flat
           {0.002},                                          // single benchmark
           {0.01, 0.02}}) {
    for (double recovery : {0.0, 0.2, 0.4, 0.6, 0.8}) {
      for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
        for (bool accrual : {true, false}) {
          const std::vector<cdsmodel::Benchmark> marks = ladder(m.today, quotes);
          const cdsmodel::Cds conv = conventions(dcc, accrual);
          const std::string label =
              std::to_string(quotes.front()) + ".." + std::to_string(quotes.back()) +
              " n=" + std::to_string(marks.size()) + " R=" + std::to_string(recovery) +
              " dcc=" + std::to_string(cdsmodel::to_upstream_code(dcc)) +
              (accrual ? " accr" : " noaccr");

          TCurve *expected =
              reference_bootstrap(m, marks, recovery, conv, conv.start, m.today, m.today);
          const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
              m.today, m.discount, conv.start, m.today, m.today, marks, recovery, conv, cals);

          ASSERT_EQ(got.has_value(), expected != nullptr) << label;
          if (expected == nullptr) {
            ++rejected_by_both;
            continue;
          }
          ASSERT_EQ(static_cast<int>(got->points().size()), expected->fNumItems) << label;
          EXPECT_EQ(cdsmodel::to_tdate(got->base_date()), expected->fBaseDate) << label;
          EXPECT_EQ(got->basis(), expected->fBasis) << label;
          for (int i = 0; i < expected->fNumItems; ++i) {
            const cdsmodel::RatePoint &p = got->points()[static_cast<std::size_t>(i)];
            EXPECT_EQ(cdsmodel::to_tdate(p.date), expected->fArray[i].fDate) << label << " i=" << i;
            // Bit-exact: same libm in this process, and the whole point is
            // that an iterative solve still lands on the same double when the
            // arithmetic is transcribed rather than reimplemented.
            EXPECT_EQ(p.rate, expected->fArray[i].fRate)
                << label << " i=" << i << " delta = " << (p.rate - expected->fArray[i].fRate);
            ++compared;
          }
          JpmcdsFreeTCurve(expected);
        }
      }
    }
  }
  // 588 rate comparisons over this sweep, all bit-exact. The floor is well
  // below that because a meaningful share of the quote/recovery space is
  // legitimately infeasible for BOTH implementations -- the agreement on
  // rejection is asserted above, inside the loop.
  EXPECT_GT(compared, 500) << "sweep thinner than expected: " << compared;
  EXPECT_LT(rejected_by_both, 60) << rejected_by_both
                                  << " combinations produced no curve at "
                                     "all; the comparison is thinner than it looks";
}

TEST_F(BootstrapDifferential, IncludeFlagsMatch) {
  const Market m;
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds conv = conventions();

  // Every non-empty subset of a four-benchmark ladder.
  for (unsigned mask = 1; mask < 16u; ++mask) {
    std::vector<cdsmodel::Benchmark> marks = ladder(m.today, {0.01, 0.012, 0.014, 0.017});
    for (std::size_t i = 0; i < marks.size(); ++i) {
      marks[i].include = ((mask >> i) & 1u) != 0u;
    }
    TCurve *expected = reference_bootstrap(m, marks, 0.4, conv, conv.start, m.today, m.today);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
        m.today, m.discount, conv.start, m.today, m.today, marks, 0.4, conv, cals);
    ASSERT_EQ(got.has_value(), expected != nullptr) << "mask=" << mask;
    if (expected == nullptr) {
      continue;
    }
    ASSERT_EQ(static_cast<int>(got->points().size()), expected->fNumItems) << "mask=" << mask;
    for (int i = 0; i < expected->fNumItems; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(got->points()[static_cast<std::size_t>(i)].date),
                expected->fArray[i].fDate)
          << "mask=" << mask << " i=" << i;
      EXPECT_EQ(got->points()[static_cast<std::size_t>(i)].rate, expected->fArray[i].fRate)
          << "mask=" << mask << " i=" << i;
    }
    JpmcdsFreeTCurve(expected);
  }
}

TEST_F(BootstrapDifferential, DegenerateRecoveryRatesAreRejectedByBoth) {
  // Upstream's guess is coupon/(1-recovery) with no guard. recovery == 1 with
  // a non-zero coupon gives an infinite guess; recovery > 1 a negative one;
  // recovery == 1 with a ZERO coupon gives 0.0/0.0, and a NaN guess passes
  // the solver's bounds check because neither comparison is true of a NaN.
  // All three fail, which is why this is a non-finding rather than a defect.
  const Market m;
  const cdsmodel::Cds conv = conventions();
  struct Case {
    double recovery;
    double coupon;
    const char *what;
  };
  for (const Case &c : {Case{1.0, 0.01, "R=1, coupon>0: guess is +inf"},
                        Case{1.0, 0.0, "R=1, coupon=0: guess is NaN"},
                        Case{1.5, 0.01, "R>1: guess is negative"}, Case{-0.5, 0.01, "R<0"}}) {
    const std::vector<cdsmodel::Benchmark> marks = ladder(m.today, {c.coupon, c.coupon, c.coupon});
    TCurve *expected =
        reference_bootstrap(m, marks, c.recovery, conv, conv.start, m.today, m.today);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
        m.today, m.discount, conv.start, m.today, m.today, marks, c.recovery, conv, no_holidays());
    EXPECT_EQ(got.has_value(), expected != nullptr) << c.what;
    JpmcdsFreeTCurve(expected);
  }
}

TEST_F(BootstrapDifferential, NonIncreasingBenchmarkDatesAreRejectedByBoth) {
  const Market m;
  const cdsmodel::Cds conv = conventions();
  const std::vector<std::vector<cdsmodel::Date>> bad{
      {d(2027, 6, 20), d(2025, 6, 20)}, // decreasing
      {d(2026, 6, 20), d(2026, 6, 20)}, // equal
  };
  for (const std::vector<cdsmodel::Date> &dates : bad) {
    std::vector<cdsmodel::Benchmark> marks;
    for (cdsmodel::Date end : dates) {
      marks.push_back(cdsmodel::Benchmark{end, 0.01, true});
    }
    TCurve *expected = reference_bootstrap(m, marks, 0.4, conv, conv.start, m.today, m.today);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
        m.today, m.discount, conv.start, m.today, m.today, marks, 0.4, conv, no_holidays());
    EXPECT_EQ(got.has_value(), expected != nullptr)
        << cdsmodel::to_tdate(dates[0]) << " then " << cdsmodel::to_tdate(dates[1]);
    JpmcdsFreeTCurve(expected);
  }
}

TEST_F(BootstrapDifferential, ANegativeForwardHazardIsRejectedByBoth) {
  // A steeply inverted quote curve implies a negative forward hazard rate
  // between two benchmarks, which the bootstrap must refuse rather than
  // return a curve whose survival probability rises.
  const Market m;
  const cdsmodel::Cds conv = conventions();
  int rejected = 0;
  for (const std::vector<double> &quotes :
       std::vector<std::vector<double>>{{0.20, 0.02, 0.02}, {0.50, 0.01, 0.01}}) {
    const std::vector<cdsmodel::Benchmark> marks = ladder(m.today, quotes);
    TCurve *expected = reference_bootstrap(m, marks, 0.4, conv, conv.start, m.today, m.today);
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::clean_spread_curve(
        m.today, m.discount, conv.start, m.today, m.today, marks, 0.4, conv, no_holidays());
    ASSERT_EQ(got.has_value(), expected != nullptr) << quotes.front();
    if (!got) {
      ++rejected;
    }
    JpmcdsFreeTCurve(expected);
  }
  EXPECT_GT(rejected, 0) << "the inverted-curve cases did not exercise the rejection path";
}

} // namespace
