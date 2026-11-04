// TIER 2 -- live differential tests for the premium leg.
//
// The premium leg is the larger half of a CDS and the fiddlier one: a sum over
// coupon periods, each of which may carry its own integral for the accrual
// owed if default interrupts it. So this is the strongest test yet of whether
// bit-exactness survives nested accumulation -- a sum of integrals rather
// than a single one.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cx.h"
#include "feeleg.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class FeeLegDifferential : public ::testing::Test {
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

/// A quarterly schedule on the CDS roll dates (20 Mar/Jun/Sep/Dec).
std::vector<cdsmodel::FeePayment> quarterly_schedule(cdsmodel::Date start, int periods) {
  std::vector<cdsmodel::FeePayment> out;
  cdsmodel::Date from = start;
  const std::chrono::year_month_day ymd = cdsmodel::to_ymd(start);
  int year = static_cast<int>(ymd.year());
  unsigned month = static_cast<unsigned>(ymd.month());
  for (int i = 0; i < periods; ++i) {
    month += 3;
    if (month > 12) {
      month -= 12;
      ++year;
    }
    const cdsmodel::Date to = *cdsmodel::make_date(year, static_cast<int>(month), 20);
    out.push_back({from, to, to});
    from = to;
  }
  return out;
}

/// Upstream's fee leg, built on the stack over caller-owned arrays.
struct ReferenceFeeLeg {
  std::vector<TDate> acc_start;
  std::vector<TDate> acc_end;
  std::vector<TDate> pay;
  TFeeLeg leg{};

  ReferenceFeeLeg(const cdsmodel::FeeLeg &src) {
    for (const cdsmodel::FeePayment &p : src.payments) {
      acc_start.push_back(cdsmodel::to_tdate(p.accrual_start));
      acc_end.push_back(cdsmodel::to_tdate(p.accrual_end));
      pay.push_back(cdsmodel::to_tdate(p.pay));
    }
    leg.nbDates = static_cast<int>(src.payments.size());
    leg.accStartDates = acc_start.data();
    leg.accEndDates = acc_end.data();
    leg.payDates = pay.data();
    leg.notional = src.notional;
    leg.couponRate = src.coupon_rate;
    leg.dcc = cdsmodel::to_upstream_code(src.day_count);
    leg.accrualPayConv =
        src.accrual_pay == cdsmodel::AccrualPayment::All ? ACCRUAL_PAY_ALL : ACCRUAL_PAY_NONE;
    leg.obsStartOfDay = src.observe_start_of_day ? TRUE : FALSE;
  }
};

struct Market {
  cdsmodel::Date base = d(2024, 1, 1);
  std::vector<cdsmodel::RatePoint> disc{{d(2024, 4, 1), 0.0310},
                                        {d(2025, 1, 1), 0.0350},
                                        {d(2027, 1, 1), 0.0400},
                                        {d(2031, 1, 1), 0.0440},
                                        {d(2039, 1, 1), 0.0455}};
  std::vector<cdsmodel::RatePoint> spread{{d(2024, 6, 20), 0.0090},
                                          {d(2025, 6, 20), 0.0130},
                                          {d(2029, 6, 20), 0.0185},
                                          {d(2034, 6, 20), 0.0220}};
};

TEST_F(FeeLegDifferential, PvIsBitIdenticalAcrossSchedulesAndConventions) {
  const Market m;
  TCurve *disc = build(m.base, m.disc);
  TCurve *spread = build(m.base, m.spread);
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(spread, nullptr);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(m.base, m.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(m.base, m.spread, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc_c.has_value());

  std::size_t compared = 0;
  // One payment exercises the branch where upstream builds no shared timeline.
  for (int periods : {1, 2, 4, 20, 40, 60}) {
    for (double coupon : {0.0, 0.0025, 0.0100, 0.0500}) {
      for (auto accrual : {cdsmodel::AccrualPayment::All, cdsmodel::AccrualPayment::None}) {
        for (bool obs_start : {true, false}) {
          for (bool clean : {true, false}) {
            for (auto dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
                             cdsmodel::DayCount::Thirty360}) {
              cdsmodel::FeeLeg leg;
              leg.payments = quarterly_schedule(d(2024, 3, 20), periods);
              leg.notional = 1.0e7;
              leg.coupon_rate = coupon;
              leg.day_count = dcc;
              leg.accrual_pay = accrual;
              leg.observe_start_of_day = obs_start;

              ReferenceFeeLeg ref{leg};
              double expected = 0.0;
              const bool ref_ok =
                  JpmcdsFeeLegPV(&ref.leg, cdsmodel::to_tdate(m.base), cdsmodel::to_tdate(m.base),
                                 cdsmodel::to_tdate(m.base), disc, spread, clean ? TRUE : FALSE,
                                 &expected) == SUCCESS;
              const auto actual =
                  cdsmodel::fee_leg_pv(leg, m.base, m.base, m.base, *disc_c, *spread_c, clean);

              ASSERT_EQ(actual.has_value(), ref_ok)
                  << "validity, periods=" << periods << " coupon=" << coupon
                  << " dcc=" << cdsmodel::to_upstream_code(dcc);
              if (ref_ok) {
                EXPECT_EQ(*actual, expected)
                    << "periods=" << periods << " coupon=" << coupon
                    << " dcc=" << cdsmodel::to_upstream_code(dcc)
                    << " accrual=" << (accrual == cdsmodel::AccrualPayment::All ? "ALL" : "NONE")
                    << " obsStart=" << obs_start << " clean=" << clean
                    << " delta=" << (*actual - expected);
                ++compared;
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 500u) << "the sweep barely ran";
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(FeeLegDifferential, PvIsBitIdenticalWithStepinInsideAPeriod) {
  // Stepping in mid-period is the case that exercises the accrued-interest
  // deduction and the sub-period accrual integral. A sweep anchored on
  // schedule dates never reaches either.
  const Market m;
  TCurve *disc = build(m.base, m.disc);
  TCurve *spread = build(m.base, m.spread);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(m.base, m.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(m.base, m.spread, 1.0, cdsmodel::DayCount::Act365Fixed);

  cdsmodel::FeeLeg leg;
  leg.payments = quarterly_schedule(d(2024, 3, 20), 20);
  leg.notional = 1.0e7;
  leg.coupon_rate = 0.01;
  leg.day_count = cdsmodel::DayCount::Act360;
  leg.accrual_pay = cdsmodel::AccrualPayment::All;
  ReferenceFeeLeg ref{leg};

  for (int stepin_off : {0, 1, 15, 45, 89, 90, 91, 180, 400, 1000}) {
    for (bool clean : {true, false}) {
      const cdsmodel::Date today = d(2024, 3, 20);
      const cdsmodel::Date stepin = today + std::chrono::days{stepin_off};
      double expected = 0.0;
      const bool ref_ok = JpmcdsFeeLegPV(&ref.leg, cdsmodel::to_tdate(today),
                                         cdsmodel::to_tdate(stepin), cdsmodel::to_tdate(today),
                                         disc, spread, clean ? TRUE : FALSE, &expected) == SUCCESS;
      const auto actual =
          cdsmodel::fee_leg_pv(leg, today, stepin, today, *disc_c, *spread_c, clean);
      ASSERT_EQ(actual.has_value(), ref_ok) << "stepin_off=" << stepin_off;
      if (ref_ok) {
        EXPECT_EQ(*actual, expected) << "stepin_off=" << stepin_off << " clean=" << clean;
      }
    }
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(FeeLegDifferential, AccrualOnDefaultIsBitIdenticalOnItsOwn) {
  // The accrual integral is also a public entry point upstream, so it gets
  // tested directly rather than only through the leg.
  const Market m;
  TCurve *disc = build(m.base, m.disc);
  TCurve *spread = build(m.base, m.spread);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(m.base, m.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(m.base, m.spread, 1.0, cdsmodel::DayCount::Act365Fixed);

  for (int start_off : {0, 30, 200, 900}) {
    for (int span : {30, 91, 182, 365}) {
      for (int stepin_off : {-10, 0, 5, 40}) {
        const cdsmodel::Date start = m.base + std::chrono::days{start_off};
        const cdsmodel::Date end = start + std::chrono::days{span};
        const cdsmodel::Date stepin = start + std::chrono::days{stepin_off};

        double expected = 0.0;
        const bool ref_ok =
            JpmcdsAccrualOnDefaultPVWithTimeLine(
                cdsmodel::to_tdate(m.base), cdsmodel::to_tdate(stepin), cdsmodel::to_tdate(start),
                cdsmodel::to_tdate(end), 25000.0, disc, spread, nullptr, &expected) == SUCCESS;
        const auto actual = cdsmodel::accrual_on_default_pv(m.base, stepin, start, end, 25000.0,
                                                            *disc_c, *spread_c, {});
        ASSERT_EQ(actual.has_value(), ref_ok)
            << "start=" << start_off << " span=" << span << " stepin=" << stepin_off;
        if (ref_ok) {
          EXPECT_EQ(*actual, expected)
              << "start=" << start_off << " span=" << span << " stepin=" << stepin_off;
        }
      }
    }
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(FeeLegDifferential, TheTaylorBranchIsReachedAndStillBitIdentical) {
  // As with the contingent leg, the small-denominator expansion needs
  // near-zero hazard and rates. It is a different expansion here, so it needs
  // its own scenario rather than relying on the other module's coverage.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> tiny{{d(2025, 1, 1), 1e-9}, {d(2034, 1, 1), 1e-9}};
  TCurve *disc = build(base, tiny);
  TCurve *spread = build(base, tiny);
  const auto disc_c = cdsmodel::ZeroCurve::create(base, tiny, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(base, tiny, 1.0, cdsmodel::DayCount::Act365Fixed);

  for (int periods : {4, 20}) {
    cdsmodel::FeeLeg leg;
    leg.payments = quarterly_schedule(d(2024, 3, 20), periods);
    leg.notional = 1.0e7;
    leg.coupon_rate = 0.01;
    leg.day_count = cdsmodel::DayCount::Act360;
    leg.accrual_pay = cdsmodel::AccrualPayment::All;
    ReferenceFeeLeg ref{leg};

    double expected = 0.0;
    ASSERT_EQ(JpmcdsFeeLegPV(&ref.leg, cdsmodel::to_tdate(base), cdsmodel::to_tdate(base),
                             cdsmodel::to_tdate(base), disc, spread, FALSE, &expected),
              SUCCESS);
    const auto actual = cdsmodel::fee_leg_pv(leg, base, base, base, *disc_c, *spread_c, false);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*actual, expected) << "Taylor branch, periods=" << periods;
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

} // namespace
