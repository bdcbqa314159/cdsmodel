// TIER 2 -- live differential tests for the vanilla CDS.
//
// This is the first suite that compares a PRICE. Everything before it
// compared a date, a year fraction, a discount factor, or one leg in
// isolation; here both legs are built from the same contract terms, composed,
// and the difference taken. If bit-exactness was going to break anywhere it
// would break here, because `contingentLegPV - feeLegPV` subtracts two
// numbers of similar magnitude -- the classic cancellation shape.
//
// The sweep varies what a trader would vary: coupon against the market
// spread, maturity, recovery, day count, accrual convention, bad day
// convention and calendar, clean against dirty, and step-in dates both on and
// off schedule dates.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "buscache.h"
#include "cdate.h"
#include "cds.h"
#include "cerror.h"
#include "convert.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "stub.h"
#include "tcurve.h"
}

namespace {

class CdsDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  void TearDown() override { JpmcdsHolidayEmptyCache(); }
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

/// An upward-sloping discount curve and a flat-ish hazard curve, both built
/// once and shared, so the sweep varies only the contract.
struct Market {
  cdsmodel::Date base;
  std::vector<cdsmodel::RatePoint> disc_pts;
  std::vector<cdsmodel::RatePoint> hazard_pts;
  cdsmodel::ZeroCurve discount;
  cdsmodel::ZeroCurve hazard;
  TCurve *ref_discount;
  TCurve *ref_hazard;

  explicit Market(cdsmodel::Date today, double level)
      : base(today), disc_pts{{d(2024, 6, 20), 0.028},
                              {d(2025, 6, 20), 0.031},
                              {d(2027, 6, 20), 0.034},
                              {d(2031, 6, 20), 0.037},
                              {d(2036, 6, 20), 0.039}},
        hazard_pts{{d(2025, 6, 20), level},
                   {d(2027, 6, 20), level * 1.15},
                   {d(2031, 6, 20), level * 1.3},
                   {d(2036, 6, 20), level * 1.4}},
        discount(
            *cdsmodel::ZeroCurve::create(base, disc_pts, 1.0, cdsmodel::DayCount::Act365Fixed)),
        hazard(
            *cdsmodel::ZeroCurve::create(base, hazard_pts, 1.0, cdsmodel::DayCount::Act365Fixed)),
        ref_discount(build(base, disc_pts)), ref_hazard(build(base, hazard_pts)) {}

  ~Market() {
    JpmcdsFreeTCurve(ref_discount);
    JpmcdsFreeTCurve(ref_hazard);
  }
  Market(const Market &) = delete;
  Market &operator=(const Market &) = delete;
};

struct Case {
  cdsmodel::Date start;
  cdsmodel::Date end;
  double coupon;
  int interval_months;
  cdsmodel::StubPosition stub_pos;
  cdsmodel::StubLength stub_len;
  cdsmodel::DayCount dcc;
  cdsmodel::BadDayConv bad_day;
  const char *calendar;
  bool pay_accrual_on_default;
  bool clean;
  double recovery;
  int stepin_offset; // days after `today`
};

std::string label(const Case &c) {
  return std::to_string(cdsmodel::to_tdate(c.start)) + ".." +
         std::to_string(cdsmodel::to_tdate(c.end)) + " cpn=" + std::to_string(c.coupon) + " " +
         std::to_string(c.interval_months) +
         "M dcc=" + std::to_string(cdsmodel::to_upstream_code(c.dcc)) +
         " bdc=" + std::string(1, static_cast<char>(c.bad_day)) + "/" + c.calendar +
         (c.pay_accrual_on_default ? " accr" : " noaccr") + (c.clean ? " clean" : " dirty") +
         " R=" + std::to_string(c.recovery) + " stepin+" + std::to_string(c.stepin_offset);
}

/// The port's price, or nullopt.
cdsmodel::Expected<double> port_price(const Market &m, const Case &c, cdsmodel::Date today,
                                      cdsmodel::Date settle, cdsmodel::Date stepin,
                                      std::span<const cdsmodel::Calendar> cals) {
  cdsmodel::Cds cds;
  cds.start = c.start;
  cds.end = c.end;
  cds.coupon_rate = c.coupon;
  cds.coupon_interval = cdsmodel::DateInterval{c.interval_months, cdsmodel::PeriodUnit::Months};
  cds.stub = cdsmodel::StubMethod{c.stub_pos, c.stub_len};
  cds.day_count = c.dcc;
  cds.bad_day = c.bad_day;
  cds.pay_accrual_on_default = c.pay_accrual_on_default;
  return cdsmodel::cds_price(cds, today, settle, stepin, cals, m.discount, m.hazard, c.recovery,
                             c.clean);
}

/// Upstream's price, or nullopt if it returned FAILURE.
cdsmodel::Expected<double> reference_price(const Market &m, const Case &c, cdsmodel::Date today,
                                           cdsmodel::Date settle, cdsmodel::Date stepin) {
  TDateInterval iv{c.interval_months, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = c.stub_pos == cdsmodel::StubPosition::Back ? TRUE : FALSE;
  stub.longStub = c.stub_len == cdsmodel::StubLength::Long ? TRUE : FALSE;
  double price = 0.0;
  const int rc = JpmcdsCdsPrice(
      cdsmodel::to_tdate(today), cdsmodel::to_tdate(settle), cdsmodel::to_tdate(stepin),
      cdsmodel::to_tdate(c.start), cdsmodel::to_tdate(c.end), c.coupon,
      c.pay_accrual_on_default ? TRUE : FALSE, &iv, &stub, cdsmodel::to_upstream_code(c.dcc),
      static_cast<long>(static_cast<char>(c.bad_day)), const_cast<char *>(c.calendar),
      m.ref_discount, m.ref_hazard, c.recovery, c.clean ? TRUE : FALSE, &price);
  if (rc != SUCCESS) {
    return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
  }
  return price;
}

/// "NONE" is no holidays with a Saturday/Sunday weekend; "NO_WEEKENDS" is no
/// holidays and no weekend at all. Both are permanently in upstream's cache.
std::vector<cdsmodel::Calendar> calendars_for(const char *name) {
  if (std::string{name} == "NO_WEEKENDS") {
    return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::None)};
  }
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

TEST_F(CdsDifferential, PriceMatchesBitExactlyAcrossTheContractSpace) {
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market market(today, 0.012);

  std::vector<Case> cases;
  // Coupon against the market: below par, at par-ish, above par, and zero.
  for (double coupon : {0.0, 0.005, 0.01, 0.02, 0.05}) {
    // Maturities on and off the roll dates, including a stub.
    for (cdsmodel::Date end : {d(2025, 6, 20), d(2029, 6, 20), d(2031, 12, 20), d(2027, 8, 7)}) {
      for (int months : {3, 6, 12}) {
        for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
                                       cdsmodel::DayCount::Thirty360}) {
          for (bool accr : {true, false}) {
            for (bool clean : {true, false}) {
              cases.push_back(Case{d(2024, 3, 20), end, coupon, months,
                                   cdsmodel::StubPosition::Front, cdsmodel::StubLength::Short, dcc,
                                   cdsmodel::BadDayConv::None, "NONE", accr, clean, 0.4, 0});
            }
          }
        }
      }
    }
  }

  int compared = 0;
  int rejected = 0;
  for (const Case &c : cases) {
    const std::vector<cdsmodel::Calendar> cals = calendars_for(c.calendar);
    const cdsmodel::Date stepin = today + std::chrono::days{c.stepin_offset};
    const cdsmodel::Expected<double> expected = reference_price(market, c, today, today, stepin);
    const cdsmodel::Expected<double> got = port_price(market, c, today, today, stepin, cals);
    ASSERT_EQ(got.has_value(), expected.has_value()) << label(c);
    if (!expected) {
      ++rejected;
      continue;
    }
    // Bit-exact: both sides run the same libm in this process, so anything
    // short of equality would mean the port reordered an operation.
    EXPECT_EQ(*got, *expected) << label(c) << "\n  delta = " << (*got - *expected);
    ++compared;
  }
  EXPECT_GT(compared, 600) << "sweep thinner than expected: " << compared;
  EXPECT_EQ(rejected, 0) << rejected << " cases upstream refused to price";
}

TEST_F(CdsDifferential, PriceMatchesAcrossBadDayConventionsAndCalendars) {
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market market(today, 0.018);

  int compared = 0;
  for (const char *cal : {"NONE", "NO_WEEKENDS"}) {
    for (cdsmodel::BadDayConv bdc :
         {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
          cdsmodel::BadDayConv::Modified}) {
      for (cdsmodel::Date end : {d(2026, 6, 20), d(2028, 11, 30), d(2029, 2, 28)}) {
        for (bool accr : {true, false}) {
          const Case c{d(2024, 3, 20),
                       end,
                       0.01,
                       3,
                       cdsmodel::StubPosition::Front,
                       cdsmodel::StubLength::Short,
                       cdsmodel::DayCount::Act360,
                       bdc,
                       cal,
                       accr,
                       true,
                       0.4,
                       0};
          const std::vector<cdsmodel::Calendar> cals = calendars_for(cal);
          const cdsmodel::Expected<double> expected =
              reference_price(market, c, today, today, today);
          const cdsmodel::Expected<double> got = port_price(market, c, today, today, today, cals);
          ASSERT_EQ(got.has_value(), expected.has_value()) << label(c);
          if (expected) {
            EXPECT_EQ(*got, *expected) << label(c);
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_EQ(compared, 2 * 4 * 3 * 2);
}

TEST_F(CdsDifferential, PriceMatchesForStepInAndSettlementOffCycle) {
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market market(today, 0.02);

  int compared = 0;
  // Step-in inside a coupon period is what exercises the accrued deduction;
  // a settlement date later than today is what exercises the forward value.
  for (int stepin_offset : {0, 1, 5, 17, 40, 95}) {
    for (int settle_offset : {0, 1, 3, 30}) {
      for (bool clean : {true, false}) {
        const Case c{d(2024, 3, 20),
                     d(2029, 6, 20),
                     0.01,
                     3,
                     cdsmodel::StubPosition::Front,
                     cdsmodel::StubLength::Short,
                     cdsmodel::DayCount::Act360,
                     cdsmodel::BadDayConv::None,
                     "NONE",
                     true,
                     clean,
                     0.4,
                     stepin_offset};
        const cdsmodel::Date stepin = today + std::chrono::days{stepin_offset};
        const cdsmodel::Date settle = today + std::chrono::days{settle_offset};
        const std::vector<cdsmodel::Calendar> cals = calendars_for(c.calendar);
        const cdsmodel::Expected<double> expected =
            reference_price(market, c, today, settle, stepin);
        const cdsmodel::Expected<double> got = port_price(market, c, today, settle, stepin, cals);
        ASSERT_EQ(got.has_value(), expected.has_value()) << label(c) << " settle+" << settle_offset;
        if (expected) {
          EXPECT_EQ(*got, *expected) << label(c) << " settle+" << settle_offset;
          ++compared;
        }
      }
    }
  }
  EXPECT_EQ(compared, 6 * 4 * 2);
}

TEST_F(CdsDifferential, RecoverySweepMatches) {
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market market(today, 0.03);
  int compared = 0;
  for (double recovery : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0}) {
    for (double level : {0.002, 0.05}) {
      const Market m(today, level);
      const Case c{d(2024, 3, 20),
                   d(2029, 6, 20),
                   0.01,
                   3,
                   cdsmodel::StubPosition::Front,
                   cdsmodel::StubLength::Short,
                   cdsmodel::DayCount::Act360,
                   cdsmodel::BadDayConv::None,
                   "NONE",
                   true,
                   true,
                   recovery,
                   0};
      const std::vector<cdsmodel::Calendar> cals = calendars_for(c.calendar);
      const cdsmodel::Expected<double> expected = reference_price(m, c, today, today, today);
      const cdsmodel::Expected<double> got = port_price(m, c, today, today, today, cals);
      ASSERT_EQ(got.has_value(), expected.has_value()) << label(c);
      if (expected) {
        EXPECT_EQ(*got, *expected) << label(c);
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, 12);
}

TEST_F(CdsDifferential, ParSpreadMatches) {
  const cdsmodel::Date today = d(2024, 6, 20);
  int compared = 0;
  for (double level : {0.002, 0.012, 0.05, 0.2}) {
    const Market m(today, level);
    for (cdsmodel::Date end : {d(2025, 6, 20), d(2029, 6, 20), d(2031, 12, 20), d(2027, 8, 7)}) {
      for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
        for (bool accr : {true, false}) {
          TDateInterval iv{3, 'M', 0};
          TStubMethod stub;
          stub.stubAtEnd = FALSE;
          stub.longStub = FALSE;
          TDate end_t = cdsmodel::to_tdate(end);
          double expected = 0.0;
          const int rc = JpmcdsCdsParSpreads(
              cdsmodel::to_tdate(today), cdsmodel::to_tdate(today),
              cdsmodel::to_tdate(d(2024, 3, 20)), 1, &end_t, accr ? TRUE : FALSE, &iv, &stub,
              cdsmodel::to_upstream_code(dcc), 'N', const_cast<char *>("NONE"), m.ref_discount,
              m.ref_hazard, 0.4, &expected);

          cdsmodel::Cds cds;
          cds.start = d(2024, 3, 20);
          cds.end = end;
          cds.coupon_rate = 0.0; // deliberately: par spread must ignore it
          cds.day_count = dcc;
          cds.pay_accrual_on_default = accr;
          const std::vector<cdsmodel::Calendar> cals = calendars_for("NONE");
          const cdsmodel::Expected<double> got =
              cdsmodel::cds_par_spread(cds, today, today, cals, m.discount, m.hazard, 0.4);
          ASSERT_EQ(got.has_value(), rc == SUCCESS)
              << "level=" << level << " end=" << cdsmodel::to_tdate(end);
          if (rc == SUCCESS) {
            EXPECT_EQ(*got, expected)
                << "level=" << level << " end=" << cdsmodel::to_tdate(end)
                << " dcc=" << cdsmodel::to_upstream_code(dcc) << (accr ? " accr" : " noaccr");
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_EQ(compared, 4 * 4 * 2 * 2);
}

TEST_F(CdsDifferential, ParSpreadIgnoresTheContractCoupon) {
  // Documented upstream behaviour: JpmcdsCdsParSpreads takes no coupon at all
  // and prices the annuity at 1.0. The port keeps the coupon in the struct, so
  // it must be demonstrably inert here or the struct is misleading.
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market m(today, 0.015);
  const std::vector<cdsmodel::Calendar> cals = calendars_for("NONE");
  std::optional<double> first;
  for (double coupon : {0.0, 0.01, 0.5, -0.3}) {
    cdsmodel::Cds cds;
    cds.start = d(2024, 3, 20);
    cds.end = d(2029, 6, 20);
    cds.coupon_rate = coupon;
    const cdsmodel::Expected<double> got =
        cdsmodel::cds_par_spread(cds, today, today, cals, m.discount, m.hazard, 0.4);
    ASSERT_TRUE(got.has_value()) << coupon;
    if (!first) {
      first = *got;
    }
    EXPECT_EQ(*got, *first) << "coupon " << coupon << " changed the par spread";
  }
}

TEST_F(CdsDifferential, ZeroLengthProtectStartAccruesOneDay) {
  // end == start is legal with protectStart, and upstream special-cases it to
  // bypass the schedule generator's end > start requirement.
  const cdsmodel::Date today = d(2024, 6, 20);
  const Market m(today, 0.015);
  const Case c{d(2024, 6, 20),
               d(2024, 6, 20),
               0.01,
               3,
               cdsmodel::StubPosition::Front,
               cdsmodel::StubLength::Short,
               cdsmodel::DayCount::Act360,
               cdsmodel::BadDayConv::None,
               "NONE",
               true,
               true,
               0.4,
               0};
  const std::vector<cdsmodel::Calendar> cals = calendars_for(c.calendar);
  const cdsmodel::Expected<double> expected = reference_price(m, c, today, today, today);
  const cdsmodel::Expected<double> got = port_price(m, c, today, today, today, cals);
  ASSERT_EQ(got.has_value(), expected.has_value());
  if (expected) {
    EXPECT_EQ(*got, *expected);
  }

  // And the leg itself: one period, accruing to start + 1 day.
  cdsmodel::Cds cds;
  cds.start = c.start;
  cds.end = c.end;
  cds.coupon_rate = c.coupon;
  const cdsmodel::Expected<cdsmodel::FeeLeg> leg = cdsmodel::cds_fee_leg(cds, 1.0, cals);
  ASSERT_TRUE(leg.has_value());
  ASSERT_EQ(leg->payments.size(), 1u);
  EXPECT_EQ(leg->payments[0].accrual_start, c.start);
  EXPECT_EQ(leg->payments[0].accrual_end, c.start + std::chrono::days{1});
  EXPECT_EQ(leg->payments[0].pay, c.start);
}

TEST_F(CdsDifferential, FeeLegScheduleMatchesUpstreamDateForDate) {
  // The composed price could agree while the schedules differ, if the
  // disagreement happened to cancel. Compare the schedules directly too.
  const cdsmodel::Date today = d(2024, 6, 20);
  int compared = 0;
  for (const char *cal : {"NONE", "NO_WEEKENDS"}) {
    for (cdsmodel::BadDayConv bdc : {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
                                     cdsmodel::BadDayConv::Modified}) {
      for (cdsmodel::Date end : {d(2026, 6, 20), d(2028, 11, 30), d(2027, 8, 7)}) {
        for (bool protect_start : {true, false}) {
          TDateInterval iv{3, 'M', 0};
          TStubMethod stub;
          stub.stubAtEnd = FALSE;
          stub.longStub = FALSE;
          TFeeLeg *ref = JpmcdsCdsFeeLegMake(
              cdsmodel::to_tdate(d(2024, 3, 20)), cdsmodel::to_tdate(end), TRUE, &iv, &stub, 1.0,
              0.01, JPMCDS_ACT_360, static_cast<long>(static_cast<char>(bdc)),
              const_cast<char *>(cal), protect_start ? TRUE : FALSE);

          cdsmodel::Cds cds;
          cds.start = d(2024, 3, 20);
          cds.end = end;
          cds.coupon_rate = 0.01;
          cds.bad_day = bdc;
          cds.protect_start = protect_start;
          const std::vector<cdsmodel::Calendar> cals = calendars_for(cal);
          const cdsmodel::Expected<cdsmodel::FeeLeg> got = cdsmodel::cds_fee_leg(cds, 1.0, cals);

          ASSERT_EQ(got.has_value(), ref != nullptr)
              << cal << " " << static_cast<char>(bdc) << " " << cdsmodel::to_tdate(end);
          if (ref == nullptr) {
            continue;
          }
          ASSERT_EQ(static_cast<int>(got->payments.size()), ref->nbDates);
          for (int i = 0; i < ref->nbDates; ++i) {
            const cdsmodel::FeePayment &p = got->payments[static_cast<std::size_t>(i)];
            EXPECT_EQ(cdsmodel::to_tdate(p.accrual_start), ref->accStartDates[i]) << "i=" << i;
            EXPECT_EQ(cdsmodel::to_tdate(p.accrual_end), ref->accEndDates[i]) << "i=" << i;
            EXPECT_EQ(cdsmodel::to_tdate(p.pay), ref->payDates[i]) << "i=" << i;
          }
          EXPECT_EQ(got->observe_start_of_day, ref->obsStartOfDay != FALSE);
          ++compared;
          JpmcdsFeeLegFree(ref);
        }
      }
    }
  }
  EXPECT_EQ(compared, 2 * 3 * 3 * 2);
}

} // namespace
