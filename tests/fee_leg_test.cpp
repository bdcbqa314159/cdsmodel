// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// Accumulating values, so the cross-platform bar is a relative tolerance, for
// the reason measured in contingent_leg_test.cpp: a sum over coupon periods,
// each carrying its own integral, amplifies libm differences far past any ULP
// budget. Bit equality remains the bar in the differential suite, where both
// implementations share one libm.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/fee_leg_cases.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <span>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Same justification and size as the contingent leg's bound.
constexpr double kCrossPlatformRelative = 1e-12;

cdsmodel::ZeroCurve rebuild(std::span<const long> dates, std::span<const double> rates) {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < dates.size(); ++i) {
    points.push_back({cdsmodel::from_tdate(dates[i]), rates[i]});
  }
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(cdsmodel::golden::kFlBase), points, 1.0,
                                      cdsmodel::DayCount::Act365Fixed);
}

std::vector<cdsmodel::FeePayment> quarterly(cdsmodel::Date start, int periods) {
  std::vector<cdsmodel::FeePayment> out;
  cdsmodel::Date from = start;
  const year_month_day ymd = cdsmodel::to_ymd(start);
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

TEST(FeeLegGolden, PvsMatchRecordedReference) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  double worst_relative = 0.0;
  for (const auto &c : cdsmodel::golden::kFeeLegCases) {
    const auto dcc = cdsmodel::from_upstream_code(c.dcc);
    ASSERT_TRUE(dcc.has_value()) << "recorded an unknown day count " << c.dcc;

    cdsmodel::FeeLeg leg;
    leg.payments = quarterly(today, c.periods);
    leg.notional = 1.0e7;
    leg.coupon_rate = c.coupon;
    leg.day_count = *dcc;
    leg.accrual_pay =
        c.accrual == 1 ? cdsmodel::AccrualPayment::All : cdsmodel::AccrualPayment::None;
    leg.observe_start_of_day = c.obs_start_of_day;

    const auto pv = cdsmodel::fee_leg_pv(leg, today, cdsmodel::from_tdate(c.stepin), today, disc,
                                         spread, c.clean);
    ASSERT_TRUE(pv.has_value()) << "periods=" << c.periods << " coupon=" << c.coupon;

    const double scale = std::max(1.0, std::fabs(c.pv));
    const double relative = std::fabs(*pv - c.pv) / scale;
    worst_relative = std::max(worst_relative, relative);
    EXPECT_LE(relative, kCrossPlatformRelative)
        << "periods=" << c.periods << " coupon=" << c.coupon << " dcc=" << c.dcc
        << " accrual=" << c.accrual << " clean=" << c.clean << std::setprecision(17)
        << "\n  got      " << *pv << "\n  recorded " << c.pv << "\n  relative " << relative;
  }
  (void)worst_relative;
}

TEST(FeeLegGolden, VectorSetIsNotEmpty) { EXPECT_GT(cdsmodel::golden::kFeeLegCases.size(), 1200u); }

// --- financial sanity, independent of upstream ---------------------------

TEST(FeeLeg, ZeroCouponIsWorthNothing) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  for (int periods : {4, 20, 40}) {
    for (auto accrual : {cdsmodel::AccrualPayment::All, cdsmodel::AccrualPayment::None}) {
      cdsmodel::FeeLeg leg;
      leg.payments = quarterly(today, periods);
      leg.notional = 1.0e7;
      leg.coupon_rate = 0.0;
      leg.day_count = cdsmodel::DayCount::Act360;
      leg.accrual_pay = accrual;
      const auto pv = cdsmodel::fee_leg_pv(leg, today, today, today, disc, spread, false);
      ASSERT_TRUE(pv.has_value());
      EXPECT_EQ(*pv, 0.0) << "periods=" << periods;
    }
  }
}

TEST(FeeLeg, PvIsLinearInCouponAndNotional) {
  // Both enter the computation only as a product with the day count fraction,
  // so doubling either must double the PV. Catches a misplaced factor, which
  // differential testing would only catch if upstream got it right -- it does,
  // but this does not depend on that.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  const auto pv_of = [&](double coupon, double notional) {
    cdsmodel::FeeLeg leg;
    leg.payments = quarterly(today, 20);
    leg.notional = notional;
    leg.coupon_rate = coupon;
    leg.day_count = cdsmodel::DayCount::Act360;
    leg.accrual_pay = cdsmodel::AccrualPayment::All;
    return *cdsmodel::fee_leg_pv(leg, today, today, today, disc, spread, false);
  };

  const double base_pv = pv_of(0.01, 1.0e7);
  EXPECT_NEAR(pv_of(0.02, 1.0e7), 2.0 * base_pv, std::fabs(base_pv) * 1e-14);
  EXPECT_NEAR(pv_of(0.01, 2.0e7), 2.0 * base_pv, std::fabs(base_pv) * 1e-14);
}

TEST(FeeLeg, PayingAccrualOnDefaultIsWorthMoreToTheSeller) {
  // ACCRUAL_PAY_ALL adds the part-period accrual, so the leg can only be
  // worth more than the convention that forfeits it.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  for (int periods : {4, 20, 40}) {
    const auto pv_of = [&](cdsmodel::AccrualPayment accrual) {
      cdsmodel::FeeLeg leg;
      leg.payments = quarterly(today, periods);
      leg.notional = 1.0e7;
      leg.coupon_rate = 0.01;
      leg.day_count = cdsmodel::DayCount::Act360;
      leg.accrual_pay = accrual;
      return *cdsmodel::fee_leg_pv(leg, today, today, today, disc, spread, false);
    };
    EXPECT_GT(pv_of(cdsmodel::AccrualPayment::All), pv_of(cdsmodel::AccrualPayment::None))
        << "periods=" << periods;
  }
}

TEST(FeeLeg, LongerSchedulesAreWorthMore) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  double previous = 0.0;
  for (int periods : {1, 2, 4, 8, 20, 40}) {
    cdsmodel::FeeLeg leg;
    leg.payments = quarterly(today, periods);
    leg.notional = 1.0e7;
    leg.coupon_rate = 0.01;
    leg.day_count = cdsmodel::DayCount::Act360;
    leg.accrual_pay = cdsmodel::AccrualPayment::All;
    const double pv = *cdsmodel::fee_leg_pv(leg, today, today, today, disc, spread, false);
    EXPECT_GT(pv, previous) << "periods=" << periods;
    previous = pv;
  }
}

TEST(FeeLeg, AccruedInterestIsZeroOnAScheduleDateAndPositiveInside) {
  // The clean-price deduction. Zero on an accrual boundary, positive between.
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);

  cdsmodel::FeeLeg leg;
  leg.payments = quarterly(today, 20);
  leg.notional = 1.0e7;
  leg.coupon_rate = 0.01;
  leg.day_count = cdsmodel::DayCount::Act360;
  leg.accrual_pay = cdsmodel::AccrualPayment::All;

  const auto gap = [&](cdsmodel::Date stepin) {
    const double dirty = *cdsmodel::fee_leg_pv(leg, today, stepin, today, disc, spread, false);
    const double clean = *cdsmodel::fee_leg_pv(leg, today, stepin, today, disc, spread, true);
    return dirty - clean;
  };

  // On the first accrual start there is nothing accrued.
  EXPECT_EQ(gap(leg.payments.front().accrual_start), 0.0);
  // On a later accrual boundary, also nothing.
  EXPECT_EQ(gap(leg.payments[4].accrual_start), 0.0);
  // Inside a period, accrual has built up and grows with time.
  const double mid = gap(leg.payments[4].accrual_start + days{30});
  const double later = gap(leg.payments[4].accrual_start + days{60});
  EXPECT_GT(mid, 0.0);
  EXPECT_GT(later, mid);
}

TEST(FeeLeg, RejectsAnEmptySchedule) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kFlDiscDates, cdsmodel::golden::kFlDiscRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kFlSpreadDates, cdsmodel::golden::kFlSpreadRates);
  const cdsmodel::Date today = cdsmodel::from_tdate(cdsmodel::golden::kFlScheduleStart);
  cdsmodel::FeeLeg leg;
  leg.notional = 1.0e7;
  leg.coupon_rate = 0.01;
  EXPECT_FALSE(cdsmodel::fee_leg_pv(leg, today, today, today, disc, spread, false).has_value());
  // And a value or step-in date before today, which upstream also rejects.
  leg.payments = quarterly(today, 4);
  EXPECT_FALSE(
      cdsmodel::fee_leg_pv(leg, today, today - days{1}, today, disc, spread, false).has_value());
  EXPECT_FALSE(
      cdsmodel::fee_leg_pv(leg, today, today, today - days{1}, disc, spread, false).has_value());
}

} // namespace
