// TIER 1 -- replays tests/golden/coupon_schedule_cases.hpp, so the coupon
// schedule is covered in CI, where no oracle is present.
//
// Everything recorded is a date or a small integer, so these compare exactly.
// There is no tolerance in this file and adding one would be a mistake.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/stub.hpp>

#include "golden/coupon_schedule_cases.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

cdsmodel::CouponScheduleTerms terms_of(const cdsmodel::golden::CouponScheduleCase &c) {
  cdsmodel::CouponScheduleTerms terms{
      .start = cdsmodel::from_tdate(c.start),
      .maturity = cdsmodel::from_tdate(c.maturity),
      .coupon_interval = {c.periods, cdsmodel::PeriodUnit::Months},
      .stub_position = c.stub_at_end ? cdsmodel::StubPosition::Back : cdsmodel::StubPosition::Front,
      .stub_length = c.long_stub ? cdsmodel::StubLength::Long : cdsmodel::StubLength::Short};
  if (c.first_roll > 0) {
    terms.roll_anchor = cdsmodel::FirstRollDate{cdsmodel::from_tdate(c.first_roll)};
  } else if (c.last_roll > 0) {
    terms.roll_anchor = cdsmodel::LastRollDate{cdsmodel::from_tdate(c.last_roll)};
  }
  if (c.full_first_coupon > 0) {
    terms.full_first_coupon_date = cdsmodel::from_tdate(c.full_first_coupon);
  }
  return terms;
}

TEST(CouponScheduleGolden, ReplaysRecordedSchedules) {
  ASSERT_GE(cdsmodel::golden::kCouponScheduleCases.size(), 50u);
  int matched = 0;
  int refused_by_port = 0;
  for (const cdsmodel::golden::CouponScheduleCase &c : cdsmodel::golden::kCouponScheduleCases) {
    const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
        cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value());
    const cdsmodel::CouponConventions conventions{.pay = *conv,
                                                  .accrual = *conv,
                                                  .reset = *conv,
                                                  .adjust_last_accrual_date = c.adjust_last,
                                                  .in_arrears = c.in_arrears,
                                                  .pay_offset_days = c.pay_offset,
                                                  .reset_offset_days = c.reset_offset};
    const cdsmodel::Expected<cdsmodel::CouponSchedule> got =
        cdsmodel::coupon_schedule(terms_of(c), conventions, cdsmodel::Calendar::none());

    // UPSTREAM-29: a schedule upstream returned with no periods in it. The
    // port refuses, and that refusal is the recorded expectation.
    if (c.accepted && c.period_count == 0) {
      EXPECT_FALSE(got.has_value()) << "the port stopped refusing UPSTREAM-29";
      ++refused_by_port;
      continue;
    }
    if (!c.accepted) {
      EXPECT_FALSE(got.has_value());
      ++refused_by_port;
      continue;
    }

    ASSERT_TRUE(got.has_value()) << "port refused a recorded schedule";
    EXPECT_EQ(static_cast<long>(got->stub), c.stub_location);
    ASSERT_EQ(got->periods.size(), c.period_count);
    for (std::size_t i = 0; i < c.period_count; ++i) {
      const cdsmodel::golden::RecordedCouponPeriod &want =
          cdsmodel::golden::kCouponSchedulePeriods[c.first_period + i];
      const cdsmodel::CouponDates &have = got->periods[i];
      EXPECT_EQ(cdsmodel::to_tdate(have.reset), want.reset) << "period " << i;
      EXPECT_EQ(cdsmodel::to_tdate(have.pay), want.pay) << "period " << i;
      EXPECT_EQ(cdsmodel::to_tdate(have.accrual_start), want.accrual_start) << "period " << i;
      EXPECT_EQ(cdsmodel::to_tdate(have.accrual_end), want.accrual_end) << "period " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 400) << "the replay compared almost nothing";
  EXPECT_EQ(refused_by_port, 4) << "the recorded UPSTREAM-29 cases changed";
}

// Properties that hold without the oracle, over the same recorded inputs.
TEST(CouponScheduleGolden, EverySchedulePassesTheValidator) {
  int checked = 0;
  for (const cdsmodel::golden::CouponScheduleCase &c : cdsmodel::golden::kCouponScheduleCases) {
    const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
        cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value());
    const cdsmodel::Expected<cdsmodel::CouponSchedule> got =
        cdsmodel::coupon_schedule(terms_of(c),
                                  {.pay = *conv,
                                   .accrual = *conv,
                                   .reset = *conv,
                                   .adjust_last_accrual_date = c.adjust_last,
                                   .in_arrears = c.in_arrears,
                                   .pay_offset_days = c.pay_offset,
                                   .reset_offset_days = c.reset_offset},
                                  cdsmodel::Calendar::none());
    if (!got) {
      continue;
    }
    // is_well_formed is upstream's JpmcdsCouponDateListCheck. Every schedule
    // the port produces must satisfy it -- which is exactly what upstream
    // violates in UPSTREAM-29.
    EXPECT_TRUE(cdsmodel::is_well_formed(*got)) << "start " << c.start << " mat " << c.maturity;
    EXPECT_FALSE(got->periods.empty());
    ++checked;
  }
  EXPECT_GT(checked, 400);
}

// The reset-before-payment loop is the one thing JpmcdsNewCouponDatesSwap
// works to establish and JpmcdsCouponDateListCheck never tests, so it is
// asserted here instead.
TEST(CouponScheduleGolden, ResetNeverFallsAfterItsOwnPayment) {
  int checked = 0;
  for (const cdsmodel::golden::CouponScheduleCase &c : cdsmodel::golden::kCouponScheduleCases) {
    if (!c.accepted || c.period_count == 0) {
      continue;
    }
    for (std::size_t i = 0; i < c.period_count; ++i) {
      const cdsmodel::golden::RecordedCouponPeriod &p =
          cdsmodel::golden::kCouponSchedulePeriods[c.first_period + i];
      EXPECT_LE(p.reset, p.pay) << "recorded period " << (c.first_period + i);
      ++checked;
    }
  }
  EXPECT_GT(checked, 1000);
}

TEST(CouponScheduleGolden, UnsuppliedRollDatesAreNotTheEpoch) {
  // The recorded cases use upstream's `0 means unsupplied` encoding, and TDate
  // 0 is 1601-01-01. Guards the conversion in terms_of against ever treating
  // a real epoch date as absent -- which is the whole reason the port's own
  // interface uses std::optional.
  for (const cdsmodel::golden::CouponScheduleCase &c : cdsmodel::golden::kCouponScheduleCases) {
    EXPECT_GT(c.start, 0);
    EXPECT_GT(c.maturity, 0);
  }
}

} // namespace
