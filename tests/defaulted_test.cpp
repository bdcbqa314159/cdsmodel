// TIER 1 -- accrual owed at a credit event. Properties first, then replay.
//
// This module corrects upstream, so the recorded vectors are the documented
// intent rather than upstream's answers. That makes the properties below more
// load-bearing than usual: with no oracle to appeal to, the case for the
// correction has to stand on what the answer must satisfy.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/defaulted.hpp>

#include <gtest/gtest.h>

#include "golden/defaulted_cases.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::Date;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<cdsmodel::Calendar> no_holidays() {
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

cdsmodel::Cds quarterly() {
  cdsmodel::Cds cds;
  cds.start = d(2024, 3, 20);
  cds.end = d(2029, 3, 20);
  cds.coupon_rate = 0.01;
  cds.coupon_interval = cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months};
  cds.day_count = cdsmodel::DayCount::Act360;
  return cds;
}

// =====================================================================
// The properties the correction rests on.
// =====================================================================

TEST(Defaulted, TheAnswerDoesNotDependOnWhenYouAsk) {
  // The decisive property, and the one upstream violates. Coupon accrued at a
  // credit event is a fact about the event, not about the enquiry. If this
  // test ever fails, the correction is wrong.
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly();

  int events = 0;
  for (int event_offset = 0; event_offset <= 1600; event_offset += 13) {
    const Date event = cds.start + std::chrono::days{event_offset};
    const cdsmodel::Expected<cdsmodel::DefaultAccrual> prompt =
        cdsmodel::default_accrual(cds, event, event, 1.0e7, cals);
    if (!prompt) {
      continue; // the event is outside the schedule
    }
    for (int lag : {0, 1, 7, 45, 50, 95, 200, 400, 2000}) {
      const cdsmodel::Expected<cdsmodel::DefaultAccrual> later =
          cdsmodel::default_accrual(cds, event + std::chrono::days{lag}, event, 1.0e7, cals);
      ASSERT_TRUE(later.has_value()) << "event+" << event_offset << " lag " << lag;
      EXPECT_EQ(later->days, prompt->days)
          << "event+" << event_offset << ": asking " << lag << " days later changed the answer";
      EXPECT_EQ(later->amount, prompt->amount)
          << "event+" << event_offset << ": asking " << lag << " days later changed the amount";
    }
    ++events;
  }
  EXPECT_GT(events, 90) << "too few events covered: " << events;
}

TEST(Defaulted, AccrualIsNeverNegativeAndNeverExceedsAFullPeriod) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  int checked = 0;
  for (int months : {3, 6, 12}) {
    cdsmodel::Cds cds = quarterly();
    cds.coupon_interval = cdsmodel::DateInterval{months, cdsmodel::PeriodUnit::Months};
    for (int event_offset = 0; event_offset <= 1600; event_offset += 7) {
      const Date event = cds.start + std::chrono::days{event_offset};
      for (int lag : {0, 30, 120}) {
        const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
            cdsmodel::default_accrual(cds, event + std::chrono::days{lag}, event, 1.0e7, cals);
        if (!got) {
          continue;
        }
        EXPECT_GT(got->days, 0) << "event+" << event_offset << " months " << months;
        EXPECT_GE(got->amount, 0.0) << "event+" << event_offset;
        // A full period plus the inclusive extra day, generously bounded.
        EXPECT_LE(got->days, 31L * months + 2) << "event+" << event_offset;
        // Coupon * notional * (period / year) is the ceiling on the amount.
        EXPECT_LE(got->amount, cds.coupon_rate * 1.0e7 * (months / 12.0) * 1.1)
            << "event+" << event_offset;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 500) << "too few cases: " << checked;
}

TEST(Defaulted, AccrualGrowsThroughACouponPeriodAndResetsAtTheBoundary) {
  // Walk the event date one day at a time across a coupon boundary. The
  // accrual should climb by one day at a time, then drop back to 1 when a new
  // period starts -- which is also an independent check that the located
  // period really is the one containing the event.
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly();

  long previous = 0;
  int resets = 0;
  for (int offset = 1; offset <= 400; ++offset) {
    const Date event = cds.start + std::chrono::days{offset};
    const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
        cdsmodel::default_accrual(cds, event, event, 1.0e7, cals);
    ASSERT_TRUE(got.has_value()) << offset;
    if (previous != 0) {
      if (got->days == 1) {
        ++resets; // a new coupon period began
      } else {
        EXPECT_EQ(got->days, previous + 1)
            << "accrual jumped at offset " << offset << ": " << previous << " -> " << got->days;
      }
    }
    previous = got->days;
  }
  // Roughly four quarterly boundaries in 400 days.
  EXPECT_GE(resets, 3) << "no coupon boundary was crossed; the walk proves little";
  EXPECT_LE(resets, 6) << resets << " resets in 400 days of a quarterly schedule";
}

TEST(Defaulted, NothingIsOwedBeforeTheEventIsDetermined) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly();
  const Date event = d(2024, 8, 1);
  for (int lead : {1, 5, 30, 200, 2000}) {
    const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
        cdsmodel::default_accrual(cds, event - std::chrono::days{lead}, event, 1.0e7, cals);
    ASSERT_TRUE(got.has_value()) << lead;
    EXPECT_EQ(got->days, 0) << lead;
    EXPECT_EQ(got->amount, 0.0) << lead;
  }
}

TEST(Defaulted, ScalesLinearlyInNotionalAndCoupon) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  cdsmodel::Cds cds = quarterly();
  const Date event = d(2024, 8, 1);

  const cdsmodel::Expected<cdsmodel::DefaultAccrual> base =
      cdsmodel::default_accrual(cds, event, event, 1.0e7, cals);
  ASSERT_TRUE(base.has_value());
  ASSERT_GT(base->amount, 0.0);

  const cdsmodel::Expected<cdsmodel::DefaultAccrual> double_notional =
      cdsmodel::default_accrual(cds, event, event, 2.0e7, cals);
  ASSERT_TRUE(double_notional.has_value());
  EXPECT_EQ(double_notional->days, base->days) << "notional must not move the day count";
  EXPECT_NEAR(double_notional->amount, 2.0 * base->amount, 1.0e-9);

  cds.coupon_rate = 0.03;
  const cdsmodel::Expected<cdsmodel::DefaultAccrual> triple_coupon =
      cdsmodel::default_accrual(cds, event, event, 1.0e7, cals);
  ASSERT_TRUE(triple_coupon.has_value());
  EXPECT_EQ(triple_coupon->days, base->days) << "coupon must not move the day count";
  EXPECT_NEAR(triple_coupon->amount, 3.0 * base->amount, 1.0e-9);

  // A zero coupon owes nothing but still accrues days.
  cds.coupon_rate = 0.0;
  const cdsmodel::Expected<cdsmodel::DefaultAccrual> zero =
      cdsmodel::default_accrual(cds, event, event, 1.0e7, cals);
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero->days, base->days);
  EXPECT_EQ(zero->amount, 0.0);
}

TEST(Defaulted, Rejected) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly();
  // A credit event before the contract started.
  for (int before : {1, 30, 400}) {
    EXPECT_FALSE(cdsmodel::default_accrual(cds, d(2024, 8, 1),
                                           cds.start - std::chrono::days{before}, 1.0e7, cals)
                     .has_value())
        << before;
  }
  // A credit event past the final accrual end.
  for (int after : {2, 30, 400}) {
    const Date event = cds.end + std::chrono::days{after};
    EXPECT_FALSE(cdsmodel::default_accrual(cds, event, event, 1.0e7, cals).has_value()) << after;
  }
  // A contract whose schedule cannot be built.
  cdsmodel::Cds bad = cds;
  bad.coupon_interval = cdsmodel::DateInterval{0, cdsmodel::PeriodUnit::Months};
  EXPECT_FALSE(
      cdsmodel::default_accrual(bad, d(2024, 8, 1), d(2024, 8, 1), 1.0e7, cals).has_value());
}

// =====================================================================
// Replay of the recorded intent.
// =====================================================================

TEST(Defaulted, MatchesRecordedIntent) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  int checked = 0;
  int disagreed_with_upstream = 0;
  for (const auto &c : cdsmodel::golden::kDefaultedCases) {
    cdsmodel::Cds cds;
    cds.start = cdsmodel::from_tdate(c.start);
    cds.end = cdsmodel::from_tdate(c.end);
    cds.coupon_rate = c.coupon_rate;
    cds.coupon_interval = cdsmodel::DateInterval{c.interval_months, cdsmodel::PeriodUnit::Months};
    cds.day_count = *cdsmodel::from_upstream_code(c.dcc);
    cds.bad_day = *cdsmodel::bad_day_conv_from_upstream(static_cast<long>(c.bad_day));

    const cdsmodel::Expected<cdsmodel::DefaultAccrual> got = cdsmodel::default_accrual(
        cds, cdsmodel::from_tdate(c.trade), cdsmodel::from_tdate(c.event), c.notional, cals);
    const std::string where = "trade " + std::to_string(c.trade) + " event " +
                              std::to_string(c.event) + " dcc " + std::to_string(c.dcc) + " ivl " +
                              std::to_string(c.interval_months);
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (!c.solved) {
      continue;
    }
    EXPECT_EQ(got->days, c.days) << where;
    // Accumulating floating point replayed on a possibly different libm.
    EXPECT_NEAR(got->amount, c.amount, 1.0e-12 * std::max(1.0, std::fabs(c.amount))) << where;
    if (!c.upstream_agreed) {
      ++disagreed_with_upstream;
    }
    ++checked;
  }
  EXPECT_GT(checked, 800) << "too few cases replayed: " << checked;
  // If this hits zero, upstream was fixed and the correction needs revisiting.
  EXPECT_GT(disagreed_with_upstream, 100)
      << "upstream now agrees everywhere; revisit the UPSTREAM-11 correction";
}

} // namespace
