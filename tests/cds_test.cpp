// TIER 1 -- the vanilla CDS. Port semantics plus replay of recorded prices.
//
// This is the suite that makes the price verified somewhere other than a
// laptop with an oracle on it. The recorded vectors carry their own market
// curves, so a price is never checked against a curve that drifted.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/cds_cases.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::Date;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::ZeroCurve curve_from(const cdsmodel::golden::CurveSpec &spec) {
  std::vector<cdsmodel::RatePoint> pts;
  for (std::size_t i = 0; i < spec.n; ++i) {
    pts.push_back({cdsmodel::from_tdate(spec.dates[i]), spec.rates[i]});
  }
  const cdsmodel::Expected<cdsmodel::ZeroCurve> c = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(spec.base), pts, 1.0, cdsmodel::DayCount::Act365Fixed);
  return *c;
}

std::vector<cdsmodel::Calendar> calendars_for(long weekend) {
  return {*cdsmodel::Calendar::create({}, weekend == 0 ? cdsmodel::Weekend::None
                                                       : cdsmodel::Weekend::Standard)};
}

/// A plain 5-year quarterly contract on a mid hazard curve, for the
/// property-style tests that do not need the recorded sweep.
struct Fixture {
  cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kCdsDiscountCurve);
  cdsmodel::ZeroCurve hazard = curve_from(cdsmodel::golden::kCdsHazardCurves[1]);
  Date today = cdsmodel::from_tdate(cdsmodel::golden::kCdsToday);
  std::vector<cdsmodel::Calendar> cals = calendars_for(0x60);
  cdsmodel::Cds cds{};

  Fixture() {
    cds.start = d(2024, 3, 20);
    cds.end = d(2029, 6, 20);
    cds.coupon_rate = 0.01;
    cds.day_count = cdsmodel::DayCount::Act360;
  }

  cdsmodel::Expected<double> price(double coupon) {
    cdsmodel::Cds c = cds;
    c.coupon_rate = coupon;
    return cdsmodel::cds_price(c, today, today, today, cals, discount, hazard, 0.4, true);
  }
};

TEST(Cds, MatchesRecordedPrices) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kCdsDiscountCurve);
  std::vector<cdsmodel::ZeroCurve> hazards;
  for (const auto &spec : cdsmodel::golden::kCdsHazardCurves) {
    hazards.push_back(curve_from(spec));
  }
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kCdsToday);

  int checked = 0;
  for (const auto &c : cdsmodel::golden::kCdsPriceCases) {
    cdsmodel::Cds cds;
    cds.start = cdsmodel::from_tdate(c.start);
    cds.end = cdsmodel::from_tdate(c.end);
    cds.coupon_rate = c.coupon;
    cds.coupon_interval = cdsmodel::DateInterval{c.interval_months, cdsmodel::PeriodUnit::Months};
    cds.stub = cdsmodel::StubMethod{
        c.stub_pos == 1 ? cdsmodel::StubPosition::Back : cdsmodel::StubPosition::Front,
        c.stub_len == 1 ? cdsmodel::StubLength::Long : cdsmodel::StubLength::Short};
    const cdsmodel::Expected<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.dcc);
    ASSERT_TRUE(dcc.has_value()) << c.dcc;
    cds.day_count = *dcc;
    const cdsmodel::Expected<cdsmodel::BadDayConv> bdc =
        cdsmodel::bad_day_conv_from_upstream(static_cast<long>(c.bad_day));
    ASSERT_TRUE(bdc.has_value()) << c.bad_day;
    cds.bad_day = *bdc;
    cds.pay_accrual_on_default = c.pay_accrual_on_default;

    const std::vector<cdsmodel::Calendar> cals = calendars_for(c.weekend);
    const cdsmodel::Expected<double> got =
        cdsmodel::cds_price(cds, today, today + std::chrono::days{c.settle_offset},
                            today + std::chrono::days{c.stepin_offset}, cals, discount,
                            hazards[static_cast<std::size_t>(c.hazard)], c.recovery, c.clean);
    ASSERT_TRUE(got.has_value()) << "case " << checked;
    // Recorded on one platform, replayed on another: the libm differs, and a
    // price accumulates over coupon periods, so the standard for accumulating
    // floating point applies -- relative 1e-12, not bit equality.
    const double tol = 1.0e-12 * std::max(1.0, std::fabs(c.price));
    EXPECT_NEAR(*got, c.price, tol)
        << "case " << checked << ": hazard=" << c.hazard << " end=" << c.end
        << " coupon=" << c.coupon << " dcc=" << c.dcc << " bdc=" << c.bad_day << " stepin+"
        << c.stepin_offset << " settle+" << c.settle_offset;
    ++checked;
  }
  EXPECT_EQ(checked, static_cast<int>(cdsmodel::golden::kCdsPriceCases.size()));
  EXPECT_GT(checked, 1500) << "recorded price vectors thinner than expected: " << checked;
}

TEST(Cds, MatchesRecordedParSpreads) {
  const cdsmodel::ZeroCurve discount = curve_from(cdsmodel::golden::kCdsDiscountCurve);
  std::vector<cdsmodel::ZeroCurve> hazards;
  for (const auto &spec : cdsmodel::golden::kCdsHazardCurves) {
    hazards.push_back(curve_from(spec));
  }
  const Date today = cdsmodel::from_tdate(cdsmodel::golden::kCdsToday);
  const std::vector<cdsmodel::Calendar> cals = calendars_for(0x60);

  int checked = 0;
  for (const auto &c : cdsmodel::golden::kCdsParSpreadCases) {
    cdsmodel::Cds cds;
    cds.start = cdsmodel::from_tdate(c.start);
    cds.end = cdsmodel::from_tdate(c.end);
    cds.day_count = *cdsmodel::from_upstream_code(c.dcc);
    cds.pay_accrual_on_default = c.pay_accrual_on_default;
    const cdsmodel::Expected<double> got = cdsmodel::cds_par_spread(
        cds, today, today, cals, discount, hazards[static_cast<std::size_t>(c.hazard)], 0.4);
    ASSERT_TRUE(got.has_value()) << "case " << checked;
    EXPECT_NEAR(*got, c.spread, 1.0e-12 * std::max(1.0, std::fabs(c.spread))) << "case " << checked;
    ++checked;
  }
  EXPECT_EQ(checked, static_cast<int>(cdsmodel::golden::kCdsParSpreadCases.size()));
}

TEST(Cds, PriceDecreasesInTheCoupon) {
  // Buying protection: a higher coupon is worse for the buyer. Monotone, and
  // close to linear, because the coupon enters only through the annuity.
  Fixture f;
  std::optional<double> previous;
  for (double coupon : {0.0, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1}) {
    const cdsmodel::Expected<double> p = f.price(coupon);
    ASSERT_TRUE(p.has_value()) << coupon;
    if (previous) {
      EXPECT_LT(*p, *previous) << "coupon " << coupon << " did not reduce the price";
    }
    previous = *p;
  }
}

TEST(Cds, ParSpreadIsTheCouponThatPricesToZero) {
  // The defining property, and a genuine cross-check: par spread is computed
  // as protection/annuity, while the price is computed as protection minus
  // annuity*coupon. Agreement is not a tautology.
  Fixture f;
  for (std::size_t h = 0; h < cdsmodel::golden::kCdsHazardCurves.size(); ++h) {
    f.hazard = curve_from(cdsmodel::golden::kCdsHazardCurves[h]);
    const cdsmodel::Expected<double> par =
        cdsmodel::cds_par_spread(f.cds, f.today, f.today, f.cals, f.discount, f.hazard, 0.4);
    ASSERT_TRUE(par.has_value()) << h;
    const cdsmodel::Expected<double> at_par = f.price(*par);
    ASSERT_TRUE(at_par.has_value()) << h;
    // Both legs are O(1) in size here, so the residual is rounding on a
    // difference of two numbers near the annuity's magnitude.
    EXPECT_NEAR(*at_par, 0.0, 1.0e-14)
        << "hazard " << h << ": par spread " << *par << " priced to " << *at_par;
  }
}

TEST(Cds, ProtectStartShiftsTheProtectionLegBackOneDay) {
  // The defect cds_differential_test found. Stated as a port-level property so
  // it cannot regress silently: the stored leg start is one day before the
  // contract start, because the stored date means "protection from end of day".
  const Date start = d(2024, 3, 20);
  const Date end = d(2029, 6, 20);
  const cdsmodel::ContingentLeg with = *cdsmodel::cds_contingent_leg(start, end, 1.0, true);
  const cdsmodel::ContingentLeg without = *cdsmodel::cds_contingent_leg(start, end, 1.0, false);
  EXPECT_EQ(with.start, start - std::chrono::days{1});
  EXPECT_EQ(without.start, start);
  EXPECT_EQ(with.end, end);
  EXPECT_EQ(with.pay, cdsmodel::ProtectionPayment::AtDefault);
}

TEST(Cds, FeeLegLastAccrualEndIsUnadjustedAndPayDateIsNot) {
  // The convention that makes a final period accrue past its own pay date.
  cdsmodel::Cds cds;
  cds.start = d(2024, 3, 20);
  cds.end = d(2028, 11, 30); // a Thursday; 30 Nov 2028 needs no roll
  cds.bad_day = cdsmodel::BadDayConv::Follow;
  const std::vector<cdsmodel::Calendar> cals = calendars_for(0x60);

  const cdsmodel::Expected<cdsmodel::FeeLeg> leg = cdsmodel::cds_fee_leg(cds, 1.0, cals);
  ASSERT_TRUE(leg.has_value());
  EXPECT_EQ(leg->payments.back().accrual_end, cds.end + std::chrono::days{1});
  EXPECT_TRUE(leg->observe_start_of_day);

  // Without protect_start the extra day goes away and so does the flag.
  cds.protect_start = false;
  const cdsmodel::Expected<cdsmodel::FeeLeg> plain = cdsmodel::cds_fee_leg(cds, 1.0, cals);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->payments.back().accrual_end, cds.end);
  EXPECT_FALSE(plain->observe_start_of_day);
}

TEST(Cds, FeeLegFirstDateIsNeverBadDayAdjusted) {
  cdsmodel::Cds cds;
  cds.start = d(2024, 3, 23); // a Saturday
  cds.end = d(2026, 3, 23);
  cds.bad_day = cdsmodel::BadDayConv::Follow;
  const std::vector<cdsmodel::Calendar> cals = calendars_for(0x60);

  const cdsmodel::Expected<cdsmodel::FeeLeg> leg = cdsmodel::cds_fee_leg(cds, 1.0, cals);
  ASSERT_TRUE(leg.has_value());
  EXPECT_EQ(leg->payments.front().accrual_start, cds.start) << "the first date was adjusted";
  // Every later boundary IS adjusted, so none of them lands on a weekend --
  // 23 Mar 2025 is a Sunday and 23 Mar 2026 a Monday, so this is not vacuous.
  for (std::size_t i = 1; i < leg->payments.size(); ++i) {
    const std::chrono::weekday wd = cdsmodel::day_of_week(leg->payments[i].accrual_start);
    EXPECT_NE(wd, std::chrono::Saturday) << "i=" << i;
    EXPECT_NE(wd, std::chrono::Sunday) << "i=" << i;
  }
}

TEST(Cds, ZeroLengthIsLegalOnlyWithProtectStart) {
  cdsmodel::Cds cds;
  cds.start = d(2024, 6, 20);
  cds.end = d(2024, 6, 20);
  const std::vector<cdsmodel::Calendar> cals = calendars_for(0x60);

  const cdsmodel::Expected<cdsmodel::FeeLeg> with = cdsmodel::cds_fee_leg(cds, 1.0, cals);
  ASSERT_TRUE(with.has_value());
  EXPECT_EQ(with->payments.size(), 1u);
  EXPECT_EQ(with->payments[0].accrual_end, cds.start + std::chrono::days{1});

  cds.protect_start = false;
  EXPECT_FALSE(cdsmodel::cds_fee_leg(cds, 1.0, cals).has_value());
}

TEST(Cds, Rejected) {
  Fixture f;
  // end before start
  cdsmodel::Cds backwards = f.cds;
  backwards.end = backwards.start - std::chrono::days{1};
  EXPECT_FALSE(cdsmodel::cds_price(backwards, f.today, f.today, f.today, f.cals, f.discount,
                                   f.hazard, 0.4, true)
                   .has_value());
  // step-in before today
  EXPECT_FALSE(cdsmodel::cds_price(f.cds, f.today, f.today, f.today - std::chrono::days{1}, f.cals,
                                   f.discount, f.hazard, 0.4, true)
                   .has_value());
  // a coupon interval that cannot build a schedule
  cdsmodel::Cds bad_interval = f.cds;
  bad_interval.coupon_interval = cdsmodel::DateInterval{0, cdsmodel::PeriodUnit::Months};
  EXPECT_FALSE(cdsmodel::cds_price(bad_interval, f.today, f.today, f.today, f.cals, f.discount,
                                   f.hazard, 0.4, true)
                   .has_value());
}

TEST(Cds, StepInAfterMaturityPricesAsMinusThePremiumLeg) {
  // Documented upstream behaviour: the protection leg is skipped entirely
  // rather than computed as an empty leg, so the price is exactly the negative
  // of the premium leg.
  Fixture f;
  const Date stepin = f.cds.end + std::chrono::days{1};
  const cdsmodel::Expected<double> price =
      cdsmodel::cds_price(f.cds, f.today, f.today, stepin, f.cals, f.discount, f.hazard, 0.4, true);
  const cdsmodel::Expected<double> fee = cdsmodel::cds_fee_leg_pv(
      f.cds, f.today, f.today, stepin, 1.0, f.cals, f.discount, f.hazard, true);
  ASSERT_TRUE(price.has_value());
  ASSERT_TRUE(fee.has_value());
  EXPECT_EQ(*price, -*fee);
}

} // namespace
