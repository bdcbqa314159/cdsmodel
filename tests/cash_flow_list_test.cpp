// TIER 1 -- the cash flow list container. Properties first, then replay.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include "golden/cash_flow_list_cases.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::CashFlow;

CashFlow cf(cdsmodel::TDate t, double amount) { return CashFlow{cdsmodel::from_tdate(t), amount}; }

std::vector<CashFlow> subset(unsigned mask, double base, int pool_size = 4) {
  static constexpr cdsmodel::TDate kPool[] = {40000, 40500, 41000, 41500, 42000};
  std::vector<CashFlow> flows;
  for (int bit = 0; bit < pool_size; ++bit) {
    if (mask & (1U << static_cast<unsigned>(bit))) {
      flows.push_back(cf(kPool[bit], base + 0.25 * double(bit)));
    }
  }
  return flows;
}

double total(const std::vector<CashFlow> &flows) {
  return std::accumulate(flows.begin(), flows.end(), 0.0,
                         [](double acc, const CashFlow &f) { return acc + f.amount; });
}

// =====================================================================
// Properties. A merge has three jobs and they are separable.
// =====================================================================

TEST(CashFlowList, AMergeIsOrderedConservesMoneyAndLosesNoDate) {
  int checked = 0;
  for (unsigned ma = 0; ma < 32; ++ma) {
    for (unsigned mb = 0; mb < 32; ++mb) {
      const std::vector<CashFlow> a = subset(ma, 1.0, 5);
      const std::vector<CashFlow> b = subset(mb, 100.0, 5);
      const cdsmodel::Expected<std::vector<CashFlow>> merged = cdsmodel::merge_cash_flows(a, b);
      ASSERT_TRUE(merged.has_value()) << ma << "," << mb;
      const std::string where = std::to_string(ma) + "," + std::to_string(mb);

      // 1. Ordered.
      EXPECT_TRUE(cdsmodel::is_ordered_by_date(*merged)) << where;

      // 2. Money is conserved. Exact, not approximate: these are sums of
      //    quarter-integers, every one representable, so a tolerance here
      //    would hide a lost element.
      EXPECT_EQ(total(*merged), total(a) + total(b)) << where;

      // 3. Every input date appears, and no date is invented.
      for (const CashFlow &f : a) {
        EXPECT_NE(
            std::ranges::find_if(*merged, [&](const CashFlow &g) { return g.date == f.date; }),
            merged->end())
            << where << ": a date from a went missing";
      }
      for (const CashFlow &f : b) {
        EXPECT_NE(
            std::ranges::find_if(*merged, [&](const CashFlow &g) { return g.date == f.date; }),
            merged->end())
            << where << ": a date from b went missing";
      }
      for (const CashFlow &f : *merged) {
        const bool from_a =
            std::ranges::any_of(a, [&](const CashFlow &g) { return g.date == f.date; });
        const bool from_b =
            std::ranges::any_of(b, [&](const CashFlow &g) { return g.date == f.date; });
        EXPECT_TRUE(from_a || from_b) << where << ": an invented date";
      }
      ++checked;
    }
  }
  EXPECT_EQ(checked, 1024) << checked;
}

TEST(CashFlowList, MergingTheSameDatesCoalescesThemAndTheSizeIsExact) {
  for (unsigned mask = 1; mask < 32; ++mask) {
    const std::vector<CashFlow> a = subset(mask, 1.0, 5);
    const std::vector<CashFlow> b = subset(mask, 100.0, 5);
    const cdsmodel::Expected<std::vector<CashFlow>> merged = cdsmodel::merge_cash_flows(a, b);
    ASSERT_TRUE(merged.has_value()) << mask;
    // Identical date sets: every pair coalesces, so the result is one list.
    EXPECT_EQ(merged->size(), a.size()) << mask;
    for (std::size_t i = 0; i < merged->size(); ++i) {
      EXPECT_EQ((*merged)[i].amount, a[i].amount + b[i].amount) << mask << " i=" << i;
    }
  }
  // Disjoint date sets coalesce nothing, so sizes add.
  const std::vector<CashFlow> evens{cf(40000, 1.0), cf(41000, 2.0)};
  const std::vector<CashFlow> odds{cf(40500, 3.0), cf(41500, 4.0)};
  const cdsmodel::Expected<std::vector<CashFlow>> merged = cdsmodel::merge_cash_flows(evens, odds);
  ASSERT_TRUE(merged.has_value());
  EXPECT_EQ(merged->size(), 4u);
}

TEST(CashFlowList, MergingWithAnEmptyListReturnsTheOtherOne) {
  const std::vector<CashFlow> a{cf(40000, 1.0), cf(40500, 2.0), cf(41000, 3.0)};
  const std::vector<CashFlow> none{};
  const cdsmodel::Expected<std::vector<CashFlow>> left = cdsmodel::merge_cash_flows(a, none);
  const cdsmodel::Expected<std::vector<CashFlow>> right = cdsmodel::merge_cash_flows(none, a);
  ASSERT_TRUE(left.has_value());
  ASSERT_TRUE(right.has_value());
  EXPECT_EQ(*left, a);
  EXPECT_EQ(*right, a);
  // Two empties merge to an empty, not to a failure.
  const cdsmodel::Expected<std::vector<CashFlow>> neither = cdsmodel::merge_cash_flows(none, none);
  ASSERT_TRUE(neither.has_value());
  EXPECT_TRUE(neither->empty());
  // And an empty list is trivially ordered.
  EXPECT_TRUE(cdsmodel::is_ordered_by_date(none));
}

TEST(CashFlowList, TheDatesComeOutTheSameWhicheverWayRoundTheMergeRuns) {
  // Deliberately NOT a claim that the amounts are identical: the summation is
  // `a + b`, so swapping the arguments swaps the operand order, and floating
  // point addition is commutative in value but the port is pinned to
  // upstream's order. Dates and sizes must match; amounts are checked by
  // value, which for these inputs is exact.
  int checked = 0;
  for (unsigned ma = 0; ma < 32; ++ma) {
    for (unsigned mb = 0; mb < 32; ++mb) {
      const std::vector<CashFlow> a = subset(ma, 1.0, 5);
      const std::vector<CashFlow> b = subset(mb, 100.0, 5);
      const cdsmodel::Expected<std::vector<CashFlow>> forward = cdsmodel::merge_cash_flows(a, b);
      const cdsmodel::Expected<std::vector<CashFlow>> backward = cdsmodel::merge_cash_flows(b, a);
      ASSERT_TRUE(forward.has_value());
      ASSERT_TRUE(backward.has_value());
      ASSERT_EQ(forward->size(), backward->size()) << ma << "," << mb;
      for (std::size_t i = 0; i < forward->size(); ++i) {
        EXPECT_EQ((*forward)[i].date, (*backward)[i].date) << ma << "," << mb << " i=" << i;
        EXPECT_EQ((*forward)[i].amount, (*backward)[i].amount) << ma << "," << mb << " i=" << i;
      }
      ++checked;
    }
  }
  EXPECT_EQ(checked, 1024);
}

TEST(CashFlowList, AnUnorderedInputIsRefusedFromEitherSide) {
  const std::vector<CashFlow> ordered{cf(40000, 1.0), cf(41000, 2.0)};
  const std::vector<CashFlow> unordered{cf(41000, 1.0), cf(40000, 2.0)};
  EXPECT_FALSE(cdsmodel::merge_cash_flows(unordered, ordered).has_value());
  EXPECT_FALSE(cdsmodel::merge_cash_flows(ordered, unordered).has_value());
  EXPECT_FALSE(cdsmodel::merge_cash_flows(unordered, unordered).has_value());
  EXPECT_TRUE(cdsmodel::merge_cash_flows(ordered, ordered).has_value());

  // Equal adjacent dates are ordered -- non-decreasing, not strictly
  // increasing -- because a list may carry two payments on one date.
  const std::vector<CashFlow> tied{cf(40000, 1.0), cf(40000, 2.0), cf(41000, 3.0)};
  EXPECT_TRUE(cdsmodel::is_ordered_by_date(tied));
  EXPECT_TRUE(cdsmodel::merge_cash_flows(tied, ordered).has_value());
}

TEST(CashFlowList, ParallelArraysOfDifferentLengthsAreRefused) {
  const std::vector<cdsmodel::Date> three{cdsmodel::from_tdate(40000), cdsmodel::from_tdate(40500),
                                          cdsmodel::from_tdate(41000)};
  const std::vector<double> two{1.0, 2.0};
  EXPECT_FALSE(cdsmodel::make_cash_flows(three, two).has_value());
  EXPECT_FALSE(cdsmodel::make_cash_flows(std::vector<cdsmodel::Date>{}, two).has_value())
      << "no dates, two amounts";
  const std::vector<double> matching{1.0, 2.0, 3.0};
  const cdsmodel::Expected<std::vector<CashFlow>> made = cdsmodel::make_cash_flows(three, matching);
  ASSERT_TRUE(made.has_value());
  ASSERT_EQ(made->size(), 3u);
  for (std::size_t i = 0; i < made->size(); ++i) {
    EXPECT_EQ((*made)[i].date, three[i]) << i;
    EXPECT_EQ((*made)[i].amount, matching[i]) << i;
  }
  // Two empties is a legal pairing and makes an empty list.
  const cdsmodel::Expected<std::vector<CashFlow>> empty =
      cdsmodel::make_cash_flows(std::vector<cdsmodel::Date>{}, std::vector<double>{});
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(CashFlowList, MatchesRecordedMerges) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kMergeCases) {
    const std::vector<CashFlow> a = subset(c.a_mask, 1.0);
    const std::vector<CashFlow> b = subset(c.b_mask, 100.0);
    const cdsmodel::Expected<std::vector<CashFlow>> merged = cdsmodel::merge_cash_flows(a, b);
    const std::string where = "masks " + std::to_string(c.a_mask) + "," + std::to_string(c.b_mask);
    ASSERT_TRUE(merged.has_value()) << where;
    ASSERT_EQ(merged->size(), c.n) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate((*merged)[i].date), c.dates[i]) << where << " i=" << i;
      // Exact. No transcendental function is involved anywhere in a merge, so
      // these vectors carry no tolerance question and a near-miss would be a
      // real difference.
      EXPECT_EQ((*merged)[i].amount, c.amounts[i]) << where << " i=" << i;
    }
    ++checked;
  }
  EXPECT_EQ(checked, 256) << checked;
}

} // namespace
