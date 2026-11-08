// TIER 2 -- live differential tests for the upfront charge.
//
// The top of the stack, and the first suite that can check the port against
// something ISDA themselves published: the three numbers printed by the
// example program shipped in the distribution.
//
// Order of business, following the habit the last two slices established:
//
//   1. the contract -- the spread solver must invert the charge calculator,
//      which needs no oracle and no recorded number;
//   2. the shipped example, which is an external reference rather than a
//      self-consistency check;
//   3. the differential sweep, bit-exact.
//
// One structural note on the example. It builds its discount curve with
// JpmcdsBuildIRZeroCurve, the single public entry point of the 5,126-line
// interest-rate stack that this port does not implement. That is not a gap in
// the CDS model: the port takes a discount curve as an INPUT. So the curve is
// built here by the oracle and handed to the port as data, which is exactly
// how a caller with a curve already in hand would use it.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/cdsone.hpp>
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
#include "busday.h"
#include "cdate.h"
#include "cds.h"
#include "cdsone.h"
#include "cerror.h"
#include "convert.h"
#include "cx.h"
#include "date_sup.h"
#include "dateconv.h"
#include "ldate.h"
#include "macros.h"
#include "stub.h"
#include "tcurve.h"
#include "zerocurve.h"
}

namespace {

class CdsoneDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Convert any upstream TCurve into the port's ZeroCurve, preserving basis
/// and day count. Used to hand the oracle-built IR curve to the port.
cdsmodel::ZeroCurve port_curve(const TCurve *tc) {
  std::vector<cdsmodel::RatePoint> pts;
  for (int i = 0; i < tc->fNumItems; ++i) {
    pts.push_back({cdsmodel::from_tdate(tc->fArray[i].fDate), tc->fArray[i].fRate});
  }
  const cdsmodel::Expected<cdsmodel::DayCount> dcc =
      cdsmodel::from_upstream_code(tc->fDayCountConv);
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(tc->fBaseDate), pts, tc->fBasis,
                                      dcc.value_or(cdsmodel::DayCount::Act365Fixed));
}

/// Exactly BuildExampleZeroCurve() from examples/c/src/main.c. Caller frees.
TCurve *example_ir_curve() {
  const char *types = "MMMMMSSSSSSSSS";
  const char *expiries[14] = {"1M", "2M", "3M", "6M", "9M", "1Y", "2Y",
                              "3Y", "4Y", "5Y", "6Y", "7Y", "8Y", "9Y"};
  double rates[14];
  for (double &r : rates) {
    r = 1e-9;
  }
  const TDate base = JpmcdsDate(2008, 1, 3);
  long mm_dcc = 0;
  long dcc = 0;
  TDateInterval ivl;
  double freq = 0.0;
  if (JpmcdsStringToDayCountConv(const_cast<char *>("Act/360"), &mm_dcc) != SUCCESS ||
      JpmcdsStringToDayCountConv(const_cast<char *>("30/360"), &dcc) != SUCCESS ||
      JpmcdsStringToDateInterval(const_cast<char *>("6M"), const_cast<char *>("t"), &ivl) !=
          SUCCESS ||
      JpmcdsDateIntervalToFreq(&ivl, &freq) != SUCCESS) {
    return nullptr;
  }
  const int n = 14;
  std::vector<TDate> dates(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    TDateInterval tmp;
    if (JpmcdsStringToDateInterval(const_cast<char *>(expiries[i]), const_cast<char *>("t"),
                                   &tmp) != SUCCESS) {
      return nullptr;
    }
    if (JpmcdsDateFwdThenAdjust(base, &tmp, JPMCDS_BAD_DAY_NONE, const_cast<char *>("None"),
                                &dates[static_cast<std::size_t>(i)]) != SUCCESS) {
      return nullptr;
    }
  }
  return JpmcdsBuildIRZeroCurve(base, const_cast<char *>(types), dates.data(), rates, n, mm_dcc,
                                static_cast<long>(freq), static_cast<long>(freq), dcc, dcc, 'F',
                                const_cast<char *>("None"));
}

/// The example's contract terms. `"1S"` is semi-annual, `"f/s"` front/short,
/// Act/360, bad day Follow, calendar "None" -- which upstream resolves
/// case-insensitively via stricmp to the built-in NONE: no holidays, with a
/// Saturday/Sunday weekend.
cdsmodel::Cds example_cds(double coupon_bps) {
  cdsmodel::Cds cds;
  cds.start = d(2008, 2, 8);
  cds.end = d(2008, 2, 12);
  cds.coupon_rate = coupon_bps / 10000.0;
  cds.coupon_interval = cdsmodel::DateInterval{6, cdsmodel::PeriodUnit::Months};
  cds.stub = cdsmodel::StubMethod{cdsmodel::StubPosition::Front, cdsmodel::StubLength::Short};
  cds.day_count = cdsmodel::DayCount::Act360;
  cds.bad_day = cdsmodel::BadDayConv::Follow;
  cds.pay_accrual_on_default = true;
  return cds;
}

cdsmodel::UpfrontDates example_dates() {
  return cdsmodel::UpfrontDates{d(2008, 2, 1), d(2008, 2, 1), d(2008, 2, 2), d(2008, 2, 9)};
}

std::vector<cdsmodel::Calendar> none_calendar() {
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

/// Upstream's upfront charge over the same inputs.
cdsmodel::Expected<double> reference_charge(const cdsmodel::Cds &cds,
                                            const cdsmodel::UpfrontDates &dates, TCurve *discount,
                                            double one_spread, double recovery, bool clean) {
  TDateInterval iv{cds.coupon_interval.periods, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = cds.stub.position == cdsmodel::StubPosition::Back ? TRUE : FALSE;
  stub.longStub = cds.stub.length == cdsmodel::StubLength::Long ? TRUE : FALSE;
  double result = 0.0;
  const int rc = JpmcdsCdsoneUpfrontCharge(
      cdsmodel::to_tdate(dates.today), cdsmodel::to_tdate(dates.value),
      cdsmodel::to_tdate(dates.benchmark_start), cdsmodel::to_tdate(dates.stepin),
      cdsmodel::to_tdate(cds.start), cdsmodel::to_tdate(cds.end), cds.coupon_rate,
      cds.pay_accrual_on_default ? TRUE : FALSE, &iv, &stub,
      cdsmodel::to_upstream_code(cds.day_count), static_cast<long>(static_cast<char>(cds.bad_day)),
      const_cast<char *>("None"), discount, one_spread, recovery, clean ? TRUE : FALSE, &result);
  if (rc != SUCCESS) {
    return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
  }
  return result;
}

cdsmodel::Expected<double> reference_spread(const cdsmodel::Cds &cds,
                                            const cdsmodel::UpfrontDates &dates, TCurve *discount,
                                            double charge, double recovery, bool clean) {
  TDateInterval iv{cds.coupon_interval.periods, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = FALSE;
  stub.longStub = FALSE;
  double result = 0.0;
  const int rc = JpmcdsCdsoneSpread(
      cdsmodel::to_tdate(dates.today), cdsmodel::to_tdate(dates.value),
      cdsmodel::to_tdate(dates.benchmark_start), cdsmodel::to_tdate(dates.stepin),
      cdsmodel::to_tdate(cds.start), cdsmodel::to_tdate(cds.end), cds.coupon_rate,
      cds.pay_accrual_on_default ? TRUE : FALSE, &iv, &stub,
      cdsmodel::to_upstream_code(cds.day_count), static_cast<long>(static_cast<char>(cds.bad_day)),
      const_cast<char *>("None"), discount, charge, recovery, clean ? TRUE : FALSE, &result);
  if (rc != SUCCESS) {
    return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
  }
  return result;
}

// =====================================================================
// 1. The contract: the solver must invert the calculator.
// =====================================================================

TEST_F(CdsoneDifferential, SpreadSolverInvertsTheUpfrontCharge) {
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  const cdsmodel::UpfrontDates dates = example_dates();

  int round_trips = 0;
  for (double coupon_bps : {0.0, 100.0, 500.0, 3600.0}) {
    const cdsmodel::Cds cds = example_cds(coupon_bps);
    for (double spread_bps : {25.0, 100.0, 500.0, 1000.0, 3600.0, 7200.0}) {
      for (double recovery : {0.0, 0.4, 0.8}) {
        const double spread = spread_bps / 10000.0;
        const cdsmodel::Expected<double> charge = cdsmodel::cdsone_upfront_charge(
            cds, dates, cals, discount, spread, recovery, /*pay_accrued_at_start=*/false);
        if (!charge) {
          continue;
        }
        const cdsmodel::Expected<double> recovered = cdsmodel::cdsone_spread(
            cds, dates, cals, discount, *charge, recovery, /*pay_accrued_at_start=*/false);
        ASSERT_TRUE(recovered.has_value())
            << "could not invert a charge of " << *charge << " at spread " << spread;
        // The outer solve stops at |charge difference| <= 1e-8; the charge is
        // per unit notional, so 1e-8 of charge maps to roughly 1e-8/duration
        // in spread. The contract here is four years short, so the duration is
        // small and the spread tolerance has to be correspondingly loose.
        EXPECT_NEAR(*recovered, spread, 1.0e-6) << "round trip lost the spread: " << spread
                                                << " -> " << *charge << " -> " << *recovered;
        ++round_trips;
      }
    }
  }
  EXPECT_GT(round_trips, 50) << "too few round trips: " << round_trips;
  JpmcdsFreeTCurve(ir);
}

TEST_F(CdsoneDifferential, ChargeFallsAsTheContractCouponRises) {
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  std::optional<double> previous;
  for (double coupon_bps : {0.0, 500.0, 1800.0, 3600.0, 5400.0, 7200.0}) {
    const cdsmodel::Expected<double> charge = cdsmodel::cdsone_upfront_charge(
        example_cds(coupon_bps), example_dates(), cals, discount, 0.36, 0.4, false);
    ASSERT_TRUE(charge.has_value()) << coupon_bps;
    if (previous) {
      EXPECT_LT(*charge, *previous) << "coupon " << coupon_bps << "bps did not reduce the charge";
    }
    previous = *charge;
  }
  JpmcdsFreeTCurve(ir);
}

// =====================================================================
// 2. The shipped example -- an external reference.
// =====================================================================

TEST_F(CdsoneDifferential, ReproducesTheShippedExampleToThePrintedPrecision) {
  // The three numbers printed by examples/c/src/main.c, which are the only
  // published output in the whole distribution. Notional is 1e7.
  //
  // The IR curve comes from the oracle because JpmcdsBuildIRZeroCurve belongs
  // to the unported interest-rate stack. Every other number below is the
  // port's.
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();
  constexpr double kNotional = 1.0e7;

  struct Expected {
    double coupon_bps;
    double charge;
  };
  // Printed with %f, so six decimals; compare to half a unit of the last one.
  for (const Expected &e : {Expected{0.0, 38651.944609}, Expected{3600.0, -10682.519172},
                            Expected{7200.0, -60016.982954}}) {
    const cdsmodel::Expected<double> got = cdsmodel::cdsone_upfront_charge(
        example_cds(e.coupon_bps), example_dates(), cals, discount, 3600.0 / 10000.0, 0.4,
        /*pay_accrued_at_start=*/false);
    ASSERT_TRUE(got.has_value()) << e.coupon_bps << "bps";
    EXPECT_NEAR(*got * kNotional, e.charge, 5.0e-7)
        << "coupon " << e.coupon_bps << "bps: expected " << e.charge << ", port gives "
        << (*got * kNotional);
  }
  // And the discount factors the example prints, which are all 1.000000
  // because every input rate is 1e-9.
  for (cdsmodel::Date date : {d(2008, 1, 3), d(2009, 1, 3), d(2017, 1, 3)}) {
    EXPECT_NEAR(discount.zero_price(date), 1.0, 5.0e-7) << cdsmodel::to_tdate(date);
  }
  JpmcdsFreeTCurve(ir);
}

TEST_F(CdsoneDifferential, TheShippedExampleMatchesUpstreamBitExactly) {
  // Same three cases, but against the live oracle rather than the printed
  // text, so the comparison is to all 17 digits instead of six decimals.
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  for (double coupon_bps : {0.0, 3600.0, 7200.0}) {
    const cdsmodel::Cds cds = example_cds(coupon_bps);
    const cdsmodel::Expected<double> expected =
        reference_charge(cds, example_dates(), ir, 0.36, 0.4, false);
    const cdsmodel::Expected<double> got =
        cdsmodel::cdsone_upfront_charge(cds, example_dates(), cals, discount, 0.36, 0.4, false);
    ASSERT_TRUE(expected.has_value()) << coupon_bps;
    ASSERT_TRUE(got.has_value()) << coupon_bps;
    EXPECT_EQ(*got, *expected) << "coupon " << coupon_bps << "bps, delta = " << (*got - *expected);
  }
  JpmcdsFreeTCurve(ir);
}

// =====================================================================
// 3. The differential sweep.
// =====================================================================

TEST_F(CdsoneDifferential, UpfrontChargeMatchesBitExactlyAcrossTheQuoteSpace) {
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  int compared = 0;
  int rejected_by_both = 0;
  for (double coupon_bps : {0.0, 100.0, 500.0, 3600.0, 7200.0}) {
    for (double spread_bps : {25.0, 100.0, 500.0, 3600.0, 7200.0}) {
      for (double recovery : {0.0, 0.2, 0.4, 0.8}) {
        for (bool clean : {true, false}) {
          for (cdsmodel::DayCount dcc :
               {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
            for (cdsmodel::BadDayConv bdc :
                 {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
              cdsmodel::Cds cds = example_cds(coupon_bps);
              cds.day_count = dcc;
              cds.bad_day = bdc;
              // A longer contract than the example's four days, so the legs
              // have real schedules behind them.
              cds.end = d(2013, 2, 12);
              const std::string label =
                  "cpn=" + std::to_string(coupon_bps) + " s=" + std::to_string(spread_bps) +
                  " R=" + std::to_string(recovery) + (clean ? " clean" : " dirty") +
                  " dcc=" + std::to_string(cdsmodel::to_upstream_code(dcc));

              const cdsmodel::Expected<double> expected =
                  reference_charge(cds, example_dates(), ir, spread_bps / 10000.0, recovery, clean);
              const cdsmodel::Expected<double> got = cdsmodel::cdsone_upfront_charge(
                  cds, example_dates(), cals, discount, spread_bps / 10000.0, recovery, clean);
              ASSERT_EQ(got.has_value(), expected.has_value()) << label;
              if (!expected) {
                ++rejected_by_both;
                continue;
              }
              EXPECT_EQ(*got, *expected) << label << " delta = " << (*got - *expected);
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 300) << "sweep thinner than expected: " << compared;
  EXPECT_LT(rejected_by_both, 120) << rejected_by_both << " combinations produced no charge";
  JpmcdsFreeTCurve(ir);
}

TEST_F(CdsoneDifferential, SpreadSolverMatchesUpstream) {
  TCurve *ir = example_ir_curve();
  ASSERT_NE(ir, nullptr);
  const cdsmodel::ZeroCurve discount = port_curve(ir);
  const std::vector<cdsmodel::Calendar> cals = none_calendar();

  int compared = 0;
  for (double coupon_bps : {0.0, 500.0, 3600.0}) {
    cdsmodel::Cds cds = example_cds(coupon_bps);
    cds.end = d(2013, 2, 12);
    for (double recovery : {0.0, 0.4, 0.8}) {
      // Charges spanning both signs, generated from real spreads so each is
      // known to be attainable.
      for (double seed_bps : {100.0, 500.0, 3600.0}) {
        const cdsmodel::Expected<double> charge =
            reference_charge(cds, example_dates(), ir, seed_bps / 10000.0, recovery, false);
        if (!charge) {
          continue;
        }
        const cdsmodel::Expected<double> expected =
            reference_spread(cds, example_dates(), ir, *charge, recovery, false);
        const cdsmodel::Expected<double> got =
            cdsmodel::cdsone_spread(cds, example_dates(), cals, discount, *charge, recovery, false);
        ASSERT_EQ(got.has_value(), expected.has_value())
            << "cpn=" << coupon_bps << " R=" << recovery << " seed=" << seed_bps;
        if (!expected) {
          continue;
        }
        EXPECT_EQ(*got, *expected) << "cpn=" << coupon_bps << " R=" << recovery
                                   << " seed=" << seed_bps << " delta = " << (*got - *expected);
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 15) << "too few spread solves compared: " << compared;
  JpmcdsFreeTCurve(ir);
}

} // namespace
