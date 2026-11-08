// TIER 1 -- replays tests/golden/swap_bootstrap_flows_cases.hpp, so the swap
// benchmark's bootstrap flows are covered in CI, where no oracle is present.
//
// Dates compare exactly; amounts at the ACCUMULATING tier. The amounts are a
// single `rate * yearFraction` with nothing accumulated, which would argue for
// ULPs -- but the file crossed a compiler to get here, and that reason alone
// decides it. This project has already shipped one CI failure by applying a
// ULP tolerance to recorded vectors.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/swap_bootstrap_flows.hpp>

#include "golden/swap_bootstrap_flows_cases.hpp"

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

TEST(SwapBootstrapFlowsGolden, ReplaysRecordedCouponDates) {
  ASSERT_GE(kSwapCouponDateCases.size(), 50u);
  int matched = 0;
  for (const SwapCouponDateCase &c : kSwapCouponDateCases) {
    const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
        cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value());
    const cdsmodel::Expected<std::vector<cdsmodel::Date>> got = cdsmodel::swap_coupon_dates(
        cdsmodel::from_tdate(c.value_date), cdsmodel::from_tdate(c.maturity),
        {c.months, cdsmodel::PeriodUnit::Months}, c.stub_at_end, {.conv = *conv}, calendars());
    if (!c.accepted) {
      EXPECT_FALSE(got.has_value());
      continue;
    }
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->size(), c.date_count);
    for (std::size_t i = 0; i < c.date_count; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i]), kSwapCouponDates[c.first_date + i]) << "date " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 150);
}

TEST(SwapBootstrapFlowsGolden, ReplaysRecordedCashFlows) {
  ASSERT_GE(kSwapBootstrapFlowCases.size(), 50u);
  int matched = 0;
  int refused = 0;
  for (const SwapBootstrapFlowCase &c : kSwapBootstrapFlowCases) {
    const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
        cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value());
    const cdsmodel::Expected<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value());

    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got = cdsmodel::swap_cash_flows(
        cdsmodel::from_tdate(c.value_date), cdsmodel::from_tdate(c.maturity), c.stub_at_end, c.rate,
        {c.months, cdsmodel::PeriodUnit::Months}, *dcc, {.conv = *conv}, calendars());

    if (!c.accepted) {
      EXPECT_FALSE(got.has_value());
      ++refused;
      continue;
    }
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->size(), c.flow_count);
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const RecordedSwapFlow &want = kSwapBootstrapFlows[c.first_flow + i];
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].date), want.date) << "flow date " << i;
      EXPECT_NEAR(want.amount, (*got)[i].amount,
                  kSwapBootstrapFlowsTolerance * std::max(1.0, std::abs(want.amount)))
          << "flow amount " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 1000);
}

// --------------------------------------------- properties, no oracle needed

// The last flow carries the principal, so it is always at least 1.0 for a
// non-negative rate. The structural fact that UPSTREAM-32's `+= 1.0` exists
// for, asserted where it cannot write out of bounds.
TEST(SwapBootstrapFlowsGolden, TheFinalFlowCarriesThePrincipal) {
  int checked = 0;
  for (const SwapBootstrapFlowCase &c : kSwapBootstrapFlowCases) {
    if (!c.accepted || c.flow_count == 0 || c.rate < 0.0) {
      continue;
    }
    const RecordedSwapFlow &last = kSwapBootstrapFlows[c.first_flow + c.flow_count - 1];
    EXPECT_GE(last.amount, 1.0) << "the principal is missing from the final flow";
    ++checked;
  }
  EXPECT_GT(checked, 500);
}

// A zero rate is a single redemption of exactly 1.0 and nothing else. The
// "bizarre rate==0.0 case" upstream gives its own path.
TEST(SwapBootstrapFlowsGolden, AZeroRateIsOneRedemptionOfExactlyOne) {
  int checked = 0;
  for (const SwapBootstrapFlowCase &c : kSwapBootstrapFlowCases) {
    if (!c.accepted || c.rate != 0.0) {
      continue;
    }
    ASSERT_EQ(c.flow_count, 1u) << "a zero-rate benchmark has one flow";
    EXPECT_DOUBLE_EQ(kSwapBootstrapFlows[c.first_flow].amount, 1.0);
    ++checked;
  }
  EXPECT_GT(checked, 100);
}

// The value date is never among the coupon dates: JpmcdsNewPayDates drops it.
// This is the property whose edge case is UPSTREAM-32 -- dropping the only
// date leaves nothing.
TEST(SwapBootstrapFlowsGolden, TheValueDateIsNeverACouponDate) {
  int checked = 0;
  for (const SwapCouponDateCase &c : kSwapCouponDateCases) {
    if (!c.accepted) {
      continue;
    }
    for (std::size_t i = 0; i < c.date_count; ++i) {
      EXPECT_NE(kSwapCouponDates[c.first_date + i], c.value_date)
          << "the value date survived into the coupon dates";
      ++checked;
    }
  }
  EXPECT_GT(checked, 500);
}

// UPSTREAM-32, pinned without the oracle: the degenerate span yields an empty
// coupon-date list, and a non-zero rate on it must be refused rather than
// written through.
TEST(SwapBootstrapFlowsGolden, Upstream32AnEmptyScheduleIsRefusedNotWrittenThrough) {
  const cdsmodel::Date same = *cdsmodel::make_date(2024, 1, 15);
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> dates = cdsmodel::swap_coupon_dates(
      same, same, {6, cdsmodel::PeriodUnit::Months}, true, {}, calendars());
  ASSERT_TRUE(dates.has_value()) << "date generation itself does not fail";
  EXPECT_TRUE(dates->empty());

  EXPECT_FALSE(cdsmodel::swap_cash_flows(same, same, true, 0.04, {6, cdsmodel::PeriodUnit::Months},
                                         cdsmodel::DayCount::Act360, {}, calendars())
                   .has_value())
      << "the port stopped refusing UPSTREAM-32";

  // The zero-rate path takes its own route and is safe even here -- which is
  // the irony of UPSTREAM-32: the bizarre RATE got a guard, the bizarre
  // MATURITY did not.
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> zero =
      cdsmodel::swap_cash_flows(same, same, true, 0.0, {6, cdsmodel::PeriodUnit::Months},
                                cdsmodel::DayCount::Act360, {}, calendars());
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero->size(), 1u);
  EXPECT_DOUBLE_EQ(zero->front().amount, 1.0);
}

} // namespace
