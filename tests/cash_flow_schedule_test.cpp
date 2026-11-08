// TIER 1 -- the cash flow schedule constructors, without an oracle.
//
// The flags are the interesting part: five booleans that each do one small
// thing to an already-built stream, plus two combinations upstream forbids. So
// the properties are written per flag -- what does this one change, and what
// does it leave alone -- rather than as whole-stream comparisons.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include "golden/cash_flow_schedule_cases.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::CashFlow;
using cdsmodel::CashFlowConventions;
using cdsmodel::CashFlowFlags;
using cdsmodel::DateInterval;
using cdsmodel::PeriodUnit;
using cdsmodel::StubLength;
using cdsmodel::StubPosition;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

CashFlowConventions terms(double rate = 0.05,
                          cdsmodel::DayCount dcc = cdsmodel::DayCount::Act365Fixed,
                          StubLength length = StubLength::Short) {
  return CashFlowConventions{.rate = rate,
                             .day_count = dcc,
                             .stub_length = length,
                             .accrual_bad_day = cdsmodel::BadDayConv::None,
                             .pay_bad_day = cdsmodel::BadDayConv::None};
}

double total(const std::vector<CashFlow> &flows) {
  return std::accumulate(flows.begin(), flows.end(), 0.0,
                         [](double acc, const CashFlow &f) { return acc + f.amount; });
}

std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> spans() {
  return {{d(2024, 1, 15), d(2027, 1, 15)},
          {d(2024, 2, 20), d(2027, 5, 10)},
          {d(2024, 1, 31), d(2026, 7, 31)},
          {d(2024, 2, 29), d(2027, 2, 28)},
          {d(2024, 6, 30), d(2029, 12, 31)}};
}

// =====================================================================
// The stream itself.
// =====================================================================

TEST(CashFlowSchedule, EveryStreamIsOrderedAndReachesTheMaturity) {
  int checked = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int periods : {3, 6, 12}) {
      for (const StubPosition position : {StubPosition::Front, StubPosition::Back}) {
        for (const StubLength length : {StubLength::Short, StubLength::Long}) {
          const cdsmodel::Expected<std::vector<CashFlow>> flows =
              cdsmodel::cash_flows_from_schedule(
                  start, maturity, DateInterval{periods, PeriodUnit::Months},
                  terms(0.05, cdsmodel::DayCount::Act365Fixed, length), position, {}, {});
          ASSERT_TRUE(flows.has_value());
          ASSERT_FALSE(flows->empty());
          const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " iv " +
                                    std::to_string(periods) + " stub " +
                                    std::to_string(static_cast<int>(position)) + "/" +
                                    std::to_string(static_cast<int>(length));
          EXPECT_TRUE(std::ranges::is_sorted(*flows, {}, &CashFlow::date)) << where;
          EXPECT_EQ(flows->back().date, maturity)
              << where << ": the last payment is at the maturity";
          EXPECT_GT(flows->front().date, d(2000, 1, 1)) << where;
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 50) << checked;
}

TEST(CashFlowSchedule, TheAccrualAddsUpToTheWholeSpan) {
  // Every amount is rate * yearFraction(period), and ACT/365F is additive in
  // days, so the amounts must sum to rate * yearFraction(start, maturity).
  // This is the property that would catch a period boundary computed from the
  // wrong neighbour -- a shift no per-flow comparison against upstream would
  // notice, since both sides would shift together.
  int checked = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int periods : {3, 6, 12}) {
      for (const StubPosition position : {StubPosition::Front, StubPosition::Back}) {
        const cdsmodel::Expected<std::vector<CashFlow>> flows = cdsmodel::cash_flows_from_schedule(
            start, maturity, DateInterval{periods, PeriodUnit::Months}, terms(), position,
            {.keep_start_date = true}, {});
        ASSERT_TRUE(flows.has_value());
        const cdsmodel::Expected<double> span =
            cdsmodel::year_fraction(start, maturity, cdsmodel::DayCount::Act365Fixed);
        ASSERT_TRUE(span.has_value());
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " iv " +
                                  std::to_string(periods) + " stub " +
                                  std::to_string(static_cast<int>(position));
        // Accumulating over up to 24 periods, so the accumulating tier.
        EXPECT_NEAR(total(*flows), 0.05 * *span, 1.0e-12 * std::fabs(0.05 * *span)) << where;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 20) << checked;
}

TEST(CashFlowSchedule, TheAmountsScaleLinearlyInTheRate) {
  for (const StubPosition position : {StubPosition::Front, StubPosition::Back}) {
    const cdsmodel::Expected<std::vector<CashFlow>> single = cdsmodel::cash_flows_from_schedule(
        d(2024, 2, 20), d(2027, 5, 10), DateInterval{3, PeriodUnit::Months}, terms(0.05), position,
        {.keep_start_date = true}, {});
    const cdsmodel::Expected<std::vector<CashFlow>> doubled = cdsmodel::cash_flows_from_schedule(
        d(2024, 2, 20), d(2027, 5, 10), DateInterval{3, PeriodUnit::Months}, terms(0.10), position,
        {.keep_start_date = true}, {});
    ASSERT_TRUE(single.has_value());
    ASSERT_TRUE(doubled.has_value());
    ASSERT_EQ(single->size(), doubled->size());
    for (std::size_t i = 0; i < single->size(); ++i) {
      EXPECT_EQ((*doubled)[i].date, (*single)[i].date) << i;
      EXPECT_EQ((*doubled)[i].amount, 2.0 * (*single)[i].amount) << i;
    }
  }
}

// =====================================================================
// One property per flag.
// =====================================================================

TEST(CashFlowSchedule, AddFinalAddsExactlyOneToTheLastPaymentAndNothingElse) {
  const auto build = [](const CashFlowFlags &flags) {
    return cdsmodel::cash_flows_from_schedule(d(2024, 1, 15), d(2027, 1, 15),
                                              DateInterval{6, PeriodUnit::Months}, terms(),
                                              StubPosition::Front, flags, {});
  };
  const cdsmodel::Expected<std::vector<CashFlow>> plain = build({.keep_start_date = true});
  const cdsmodel::Expected<std::vector<CashFlow>> with_final =
      build({.add_final = true, .keep_start_date = true});
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(with_final.has_value());
  ASSERT_EQ(plain->size(), with_final->size());
  for (std::size_t i = 0; i + 1 < plain->size(); ++i) {
    EXPECT_EQ((*with_final)[i], (*plain)[i]) << "flow " << i << " must be untouched";
  }
  EXPECT_EQ(with_final->back().date, plain->back().date);
  EXPECT_EQ(with_final->back().amount, plain->back().amount + 1.0);
}

TEST(CashFlowSchedule, SubtractInitialTakesOneFromTheFirstAndKeepsItInTheStream) {
  // The leading flow has a zero amount and is normally dropped. Subtracting
  // one makes it non-zero, so it survives -- the two effects are coupled, and
  // upstream's comment about the exact zero comparison is what couples them.
  const cdsmodel::Expected<std::vector<CashFlow>> plain = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 15), d(2027, 1, 15), DateInterval{6, PeriodUnit::Months}, terms(),
      StubPosition::Front, {}, {});
  const cdsmodel::Expected<std::vector<CashFlow>> priced = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 15), d(2027, 1, 15), DateInterval{6, PeriodUnit::Months}, terms(),
      StubPosition::Front, {.subtract_initial = true}, {});
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(priced.has_value());
  EXPECT_EQ(priced->size(), plain->size() + 1)
      << "the leading flow is dropped without the flag and kept with it";
  EXPECT_EQ(priced->front().amount, -1.0);
  EXPECT_EQ(priced->front().date, d(2024, 1, 15));
  // And everything after the first is the plain stream.
  for (std::size_t i = 0; i < plain->size(); ++i) {
    EXPECT_EQ((*priced)[i + 1], (*plain)[i]) << i;
  }
}

TEST(CashFlowSchedule, KeepStartDateRetainsTheZeroFlowWithoutChangingAnyAmount) {
  const cdsmodel::Expected<std::vector<CashFlow>> dropped = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 15), d(2027, 1, 15), DateInterval{6, PeriodUnit::Months}, terms(),
      StubPosition::Front, {}, {});
  const cdsmodel::Expected<std::vector<CashFlow>> kept = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 15), d(2027, 1, 15), DateInterval{6, PeriodUnit::Months}, terms(),
      StubPosition::Front, {.keep_start_date = true}, {});
  ASSERT_TRUE(dropped.has_value());
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept->size(), dropped->size() + 1);
  EXPECT_EQ(kept->front().amount, 0.0);
  EXPECT_EQ(total(*kept), total(*dropped)) << "a zero flow cannot change the total";
}

TEST(CashFlowSchedule, PostMaturityZeroPaymentExtendsPastMaturityAndZeroesTheLast) {
  const cdsmodel::Date maturity = d(2027, 1, 15);
  const cdsmodel::Expected<std::vector<CashFlow>> extended = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 15), maturity, DateInterval{6, PeriodUnit::Months}, terms(), StubPosition::Front,
      {.post_maturity_zero_payment = true}, {});
  ASSERT_TRUE(extended.has_value());
  ASSERT_FALSE(extended->empty());
  EXPECT_GT(extended->back().date, maturity) << "the schedule runs one interval past maturity";
  EXPECT_EQ(extended->back().amount, 0.0) << "and that final payment is zero";
}

TEST(CashFlowSchedule, TheTwoIllegalFlagCombinationsAreRefused) {
  const auto build = [](const CashFlowFlags &flags, StubPosition position) {
    return cdsmodel::cash_flows_from_schedule(d(2024, 1, 15), d(2027, 1, 15),
                                              DateInterval{6, PeriodUnit::Months}, terms(),
                                              position, flags, {});
  };
  // A prestart zero payment cannot coexist with either of the two flags that
  // also want to say something about the start date.
  EXPECT_FALSE(build({.subtract_initial = true, .prestart_zero_payment = true}, StubPosition::Front)
                   .has_value());
  EXPECT_FALSE(build({.prestart_zero_payment = true, .keep_start_date = true}, StubPosition::Front)
                   .has_value());
  // A post-maturity zero payment cannot coexist with a stub at the end, nor
  // with a final principal payment.
  EXPECT_FALSE(build({.post_maturity_zero_payment = true}, StubPosition::Back).has_value());
  EXPECT_FALSE(build({.add_final = true, .post_maturity_zero_payment = true}, StubPosition::Front)
                   .has_value());
  // Each half on its own is fine.
  EXPECT_TRUE(build({.prestart_zero_payment = true}, StubPosition::Front).has_value());
  EXPECT_TRUE(build({.post_maturity_zero_payment = true}, StubPosition::Front).has_value());
  EXPECT_TRUE(
      build({.subtract_initial = true, .keep_start_date = true}, StubPosition::Front).has_value());
}

TEST(CashFlowSchedule, AnInvertedOrDegenerateSpanIsRefused) {
  for (const auto &[start, maturity] :
       {std::pair{d(2027, 1, 15), d(2024, 1, 15)}, std::pair{d(2024, 1, 15), d(2024, 1, 15)}}) {
    EXPECT_FALSE(cdsmodel::cash_flows_from_schedule(start, maturity,
                                                    DateInterval{6, PeriodUnit::Months}, terms(),
                                                    StubPosition::Front, {}, {})
                     .has_value())
        << cdsmodel::to_tdate(start) << ".." << cdsmodel::to_tdate(maturity);
  }
}

TEST(CashFlowSchedule, ALongStubProducesFewerPeriodsThanAShortOne) {
  // Coalescing merges the odd period into its neighbour, so a long stub can
  // only ever have the same number of flows or fewer -- never more.
  int shorter = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int periods : {3, 6, 12}) {
      for (const StubPosition position : {StubPosition::Front, StubPosition::Back}) {
        const auto build = [&](StubLength length) {
          return cdsmodel::cash_flows_from_schedule(
              start, maturity, DateInterval{periods, PeriodUnit::Months},
              terms(0.05, cdsmodel::DayCount::Act365Fixed, length), position,
              {.keep_start_date = true}, {});
        };
        const cdsmodel::Expected<std::vector<CashFlow>> brief = build(StubLength::Short);
        const cdsmodel::Expected<std::vector<CashFlow>> merged = build(StubLength::Long);
        ASSERT_TRUE(brief.has_value());
        ASSERT_TRUE(merged.has_value());
        EXPECT_LE(merged->size(), brief->size()) << cdsmodel::to_tdate(start) << " iv " << periods;
        if (merged->size() < brief->size()) {
          ++shorter;
        }
        // Both must still end at the maturity and still accrue the same span.
        EXPECT_EQ(merged->back().date, brief->back().date);
        EXPECT_NEAR(total(*merged), total(*brief), 1.0e-12 * std::fabs(total(*brief)))
            << "coalescing moves period boundaries but not the total accrual";
      }
    }
  }
  EXPECT_GT(shorter, 0) << "the sweep must contain a span with a stub to coalesce";
}

TEST(CashFlowSchedule, AdjustmentMovesDatesWithoutChangingTheNumberOfPayments) {
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  const cdsmodel::Expected<std::vector<CashFlow>> unadjusted = cdsmodel::cash_flows_from_schedule(
      d(2024, 1, 6), d(2026, 7, 4), DateInterval{3, PeriodUnit::Months}, terms(),
      StubPosition::Front, {.add_final = true}, calendars);
  ASSERT_TRUE(unadjusted.has_value());

  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified}) {
    CashFlowConventions conventions = terms();
    conventions.accrual_bad_day = conv;
    conventions.pay_bad_day = conv;
    const cdsmodel::Expected<std::vector<CashFlow>> adjusted = cdsmodel::cash_flows_from_schedule(
        d(2024, 1, 6), d(2026, 7, 4), DateInterval{3, PeriodUnit::Months}, conventions,
        StubPosition::Front, {.add_final = true}, calendars);
    ASSERT_TRUE(adjusted.has_value()) << static_cast<char>(conv);
    EXPECT_EQ(adjusted->size(), unadjusted->size())
        << static_cast<char>(conv) << ": adjustment must not add or drop a payment";
    for (const CashFlow &flow : *adjusted) {
      EXPECT_TRUE(calendars.front().is_business_day(flow.date))
          << static_cast<char>(conv) << " left " << cdsmodel::to_tdate(flow.date)
          << " off a business day";
    }
    EXPECT_TRUE(std::ranges::is_sorted(*adjusted, {}, &CashFlow::date)) << static_cast<char>(conv);
  }
}

// =====================================================================
// Replay.
// =====================================================================

TEST(CashFlowSchedule, MatchesRecordedStreams) {
  int checked = 0;
  int refused = 0;
  for (const auto &c : cdsmodel::golden::kCashFlowScheduleCases) {
    const cdsmodel::Expected<cdsmodel::BadDayConv> conv =
        c.bad_day == 'N' ? cdsmodel::Expected<cdsmodel::BadDayConv>{cdsmodel::BadDayConv::None}
                         : cdsmodel::bad_day_conv_from_upstream(c.bad_day);
    ASSERT_TRUE(conv.has_value()) << c.bad_day;
    const CashFlowConventions conventions{
        .rate = c.rate,
        .day_count = cdsmodel::DayCount::Act365Fixed,
        .stub_length = (c.stub_placement & 2) != 0 ? StubLength::Long : StubLength::Short,
        .accrual_bad_day = *conv,
        .pay_bad_day = *conv};
    const StubPosition position =
        (c.stub_placement & 1) != 0 ? StubPosition::Back : StubPosition::Front;
    const CashFlowFlags flags{.subtract_initial = (c.flags & 2) != 0,
                              .add_final = (c.flags & 1) != 0,
                              .prestart_zero_payment = (c.flags & 4) != 0,
                              .post_maturity_zero_payment = (c.flags & 8) != 0,
                              .keep_start_date = (c.flags & 16) != 0};
    const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

    const cdsmodel::Expected<std::vector<CashFlow>> got = cdsmodel::cash_flows_from_schedule(
        cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.maturity),
        DateInterval{c.periods, PeriodUnit::Months}, conventions, position, flags, calendars);
    const std::string where = std::to_string(c.start) + " iv " + std::to_string(c.periods) +
                              " stub " + std::to_string(c.stub_placement) + " flags " +
                              std::to_string(c.flags) + " bdc " + std::to_string(c.bad_day);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (!got) {
      ++refused;
      continue;
    }
    ASSERT_EQ(got->size(), c.n) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].date), c.dates[i]) << where << " i=" << i;
      // rate * yearFrac, one multiplication, so exact.
      EXPECT_EQ((*got)[i].amount, c.amounts[i]) << where << " i=" << i;
    }
    ++checked;
  }
  EXPECT_GT(checked, 700) << checked;
  EXPECT_GT(refused, 200) << "the recorded illegal combinations: " << refused;
}

} // namespace
