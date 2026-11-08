// TIER 1 -- replays tests/golden/fee_leg_flows_cases.hpp, so the fee leg's
// non-contingent flows are covered in CI, where no oracle is present.
//
// These are the flows that occur if there is no default, so they carry no
// discounting and no survival probability -- which makes several properties
// available that a priced quantity would not have: the amounts are linear in
// the coupon rate and in the notional, and they scale exactly.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>

#include "golden/fee_leg_flows_cases.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

const std::vector<cdsmodel::Calendar> &calendars() {
  static const std::vector<cdsmodel::Calendar> v{cdsmodel::Calendar::none()};
  return v;
}

cdsmodel::Expected<cdsmodel::Cds> cds_of(const FeeLegFlowsCase &c) {
  const cdsmodel::Expected<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
  const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
      cdsmodel::bad_day_conv_from_upstream(c.conv);
  if (!dcc || !conv) {
    return cdsmodel::unexpected(!dcc ? dcc.error() : (conv.error()));
  }
  return cdsmodel::Cds{.start = cdsmodel::from_tdate(c.start),
                       .end = cdsmodel::from_tdate(c.end),
                       .coupon_rate = c.coupon_rate,
                       .coupon_interval = {c.months, cdsmodel::PeriodUnit::Months},
                       .day_count = *dcc,
                       .bad_day = *conv};
}

TEST(FeeLegFlowsGolden, ReplaysEveryRecordedCase) {
  ASSERT_GE(kFeeLegFlowsCases.size(), 50u);
  int matched = 0;
  int refused = 0;
  for (const FeeLegFlowsCase &c : kFeeLegFlowsCases) {
    const cdsmodel::Expected<cdsmodel::Cds> cds = cds_of(c);
    ASSERT_TRUE(cds.has_value());
    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
        cdsmodel::cds_fee_leg_flows(*cds, kFeeLegFlowsNotional, calendars());
    ASSERT_EQ(got.has_value(), c.accepted) << "months " << c.months << " dcc " << c.day_count;
    if (!c.accepted) {
      ++refused;
      continue;
    }
    ASSERT_EQ(got->size(), c.flow_count);
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const RecordedFeeFlow &want = kFeeLegFlows[c.first_flow + i];
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].date), want.date) << "date " << i;
      EXPECT_NEAR(want.amount, (*got)[i].amount,
                  kFeeLegFlowsTolerance * std::max(1.0, std::abs(want.amount)))
          << "amount " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 400);
}

// ------------------------------------------- properties, no oracle needed

// Linear in the notional: doubling it doubles every amount and moves no date.
// A property a PRICE would not have, available here only because these flows
// are undiscounted.
TEST(FeeLegFlowsGolden, AmountsScaleExactlyWithTheNotional) {
  int checked = 0;
  for (const FeeLegFlowsCase &c : kFeeLegFlowsCases) {
    if (!c.accepted || c.flow_count == 0) {
      continue;
    }
    const cdsmodel::Expected<cdsmodel::Cds> cds = cds_of(c);
    ASSERT_TRUE(cds.has_value());
    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> one =
        cdsmodel::cds_fee_leg_flows(*cds, 1.0, calendars());
    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> many =
        cdsmodel::cds_fee_leg_flows(*cds, 2.0e7, calendars());
    ASSERT_TRUE(one.has_value());
    ASSERT_TRUE(many.has_value());
    ASSERT_EQ(one->size(), many->size());
    for (std::size_t i = 0; i < one->size(); ++i) {
      EXPECT_EQ((*one)[i].date, (*many)[i].date) << "a date moved with the notional";
      EXPECT_NEAR((*many)[i].amount, (*one)[i].amount * 2.0e7,
                  1.0e-9 * std::max(1.0, std::abs((*many)[i].amount)))
          << "flow " << i;
    }
    ++checked;
    if (checked > 60) {
      break;
    }
  }
  EXPECT_GT(checked, 50);
}

// A zero coupon rate gives zero amounts on the same dates -- the schedule
// survives, the money does not.
TEST(FeeLegFlowsGolden, AZeroCouponGivesZeroAmountsOnTheSameDates) {
  int checked = 0;
  for (const FeeLegFlowsCase &c : kFeeLegFlowsCases) {
    if (!c.accepted || c.coupon_rate != 0.0 || c.flow_count == 0) {
      continue;
    }
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      EXPECT_DOUBLE_EQ(kFeeLegFlows[c.first_flow + i].amount, 0.0)
          << "a zero coupon produced a non-zero amount";
      ++checked;
    }
  }
  EXPECT_GT(checked, 100) << "no zero-coupon case was recorded";
}

// Dates are non-decreasing and every amount is finite. A schedule that went
// backwards would still produce plausible individual numbers.
TEST(FeeLegFlowsGolden, EveryScheduleIsOrderedWithFiniteAmounts) {
  int checked = 0;
  for (const FeeLegFlowsCase &c : kFeeLegFlowsCases) {
    if (!c.accepted) {
      continue;
    }
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const RecordedFeeFlow &f = kFeeLegFlows[c.first_flow + i];
      EXPECT_TRUE(std::isfinite(f.amount));
      EXPECT_GT(f.date, c.start) << "a flow dated at or before the contract start";
      if (i > 0) {
        EXPECT_GE(f.date, kFeeLegFlows[c.first_flow + i - 1].date) << "dates went backwards";
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 3000);
}

// The sign of every amount follows the sign of the coupon rate, since the year
// fraction and notional are both positive here. Catches a sign slip that a
// per-amount tolerance would accept.
TEST(FeeLegFlowsGolden, TheSignOfEveryAmountFollowsTheCoupon) {
  int negative_cases = 0;
  for (const FeeLegFlowsCase &c : kFeeLegFlowsCases) {
    if (!c.accepted || c.coupon_rate == 0.0) {
      continue;
    }
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const double amount = kFeeLegFlows[c.first_flow + i].amount;
      if (amount == 0.0) {
        continue; // a zero-length accrual period
      }
      EXPECT_EQ(amount > 0.0, c.coupon_rate > 0.0)
          << "amount " << amount << " against coupon " << c.coupon_rate;
    }
    if (c.coupon_rate < 0.0) {
      ++negative_cases;
    }
  }
  EXPECT_GT(negative_cases, 50) << "no negative-coupon case was recorded";
}

} // namespace
