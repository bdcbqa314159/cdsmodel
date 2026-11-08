// TIER 2 -- live differential tests for the cash flow list container.
//
// The merge is a single forward pass over two sequences, which is a shape that
// hides off-by-ones at the ends and in ties. So the sweep is exhaustive over
// small lists rather than a handful of examples: every pair of date sets drawn
// from a shared pool, which covers disjoint, fully overlapping, partially
// overlapping, prefix, suffix, empty-on-either-side and duplicate-within-a-list
// without any of them having to be thought of individually.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
}

namespace {

class CashFlowDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

/// RAII wrapper so a failing assertion does not leak a list.
struct Ref {
  TCashFlowList *cfl = nullptr;
  Ref() = default;
  explicit Ref(TCashFlowList *owned) : cfl(owned) {}
  ~Ref() { JpmcdsFreeCFL(cfl); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return cfl != nullptr; }
};

Ref upstream_list(const std::vector<cdsmodel::CashFlow> &flows) {
  std::vector<TDate> dates;
  std::vector<double> amounts;
  for (const cdsmodel::CashFlow &f : flows) {
    dates.push_back(cdsmodel::to_tdate(f.date));
    amounts.push_back(f.amount);
  }
  return Ref(JpmcdsNewCFL(dates.empty() ? nullptr : dates.data(),
                          amounts.empty() ? nullptr : amounts.data(),
                          static_cast<int>(flows.size())));
}

std::vector<cdsmodel::CashFlow> as_flows(const TCashFlowList *cfl) {
  std::vector<cdsmodel::CashFlow> out;
  for (int i = 0; i < cfl->fNumItems; ++i) {
    out.push_back(
        cdsmodel::CashFlow{cdsmodel::from_tdate(cfl->fArray[i].fDate), cfl->fArray[i].fAmount});
  }
  return out;
}

/// Every subset of a shared date pool, as an ordered list with distinct
/// amounts so that a misplaced element is detectable.
std::vector<cdsmodel::CashFlow> subset(unsigned mask, double base) {
  static constexpr cdsmodel::TDate kPool[] = {40000, 40500, 41000, 41500, 42000};
  std::vector<cdsmodel::CashFlow> flows;
  for (unsigned bit = 0; bit < 5; ++bit) {
    if (mask & (1U << bit)) {
      flows.push_back(
          cdsmodel::CashFlow{cdsmodel::from_tdate(kPool[bit]), base + 0.25 * double(bit)});
    }
  }
  return flows;
}

TEST_F(CashFlowDifferential, TheMergeMatchesOverEveryPairOfSubsets) {
  int compared = 0;
  for (unsigned ma = 0; ma < 32; ++ma) {
    for (unsigned mb = 0; mb < 32; ++mb) {
      const std::vector<cdsmodel::CashFlow> a = subset(ma, 1.0);
      const std::vector<cdsmodel::CashFlow> b = subset(mb, 100.0);
      const Ref ra = upstream_list(a);
      const Ref rb = upstream_list(b);
      // An empty list is legal and JpmcdsNewCFL(.., 0) returns one.
      ASSERT_TRUE(ra) << "a mask " << ma;
      ASSERT_TRUE(rb) << "b mask " << mb;

      const Ref merged(JpmcdsMergeCFL(ra.cfl, rb.cfl));
      const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
          cdsmodel::merge_cash_flows(a, b);
      ASSERT_TRUE(merged) << "masks " << ma << "," << mb;
      ASSERT_TRUE(got.has_value()) << "masks " << ma << "," << mb;

      const std::vector<cdsmodel::CashFlow> want = as_flows(merged.cfl);
      const std::string where = "masks " + std::to_string(ma) + "," + std::to_string(mb);
      ASSERT_EQ(got->size(), want.size()) << where;
      for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ((*got)[i].date, want[i].date) << where << " i=" << i;
        // Summed amounts must be bit-identical: one addition, same order.
        EXPECT_EQ((*got)[i].amount, want[i].amount) << where << " i=" << i;
      }
      ++compared;
    }
  }
  EXPECT_EQ(compared, 1024) << compared;
}

TEST_F(CashFlowDifferential, DuplicateDatesWithinOneListAreHandledTheSameWay) {
  // Upstream advances one side per comparison, so a duplicate inside a list is
  // carried through uncoalesced, and which copy pairs with the other list's
  // entry depends on the loop. Pinned rather than reasoned about.
  const std::vector<std::vector<cdsmodel::CashFlow>> lists{
      {{cdsmodel::from_tdate(40000), 1.0}, {cdsmodel::from_tdate(40000), 2.0}},
      {{cdsmodel::from_tdate(40000), 5.0}},
      {{cdsmodel::from_tdate(40000), 5.0}, {cdsmodel::from_tdate(40000), 6.0}},
      {{cdsmodel::from_tdate(39000), 7.0},
       {cdsmodel::from_tdate(40000), 8.0},
       {cdsmodel::from_tdate(40000), 9.0}},
  };
  int compared = 0;
  for (const auto &a : lists) {
    for (const auto &b : lists) {
      const Ref ra = upstream_list(a);
      const Ref rb = upstream_list(b);
      ASSERT_TRUE(ra);
      ASSERT_TRUE(rb);
      const Ref merged(JpmcdsMergeCFL(ra.cfl, rb.cfl));
      ASSERT_TRUE(merged);
      const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
          cdsmodel::merge_cash_flows(a, b);
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(*got, as_flows(merged.cfl)) << "a=" << a.size() << " b=" << b.size();
      ++compared;
    }
  }
  EXPECT_EQ(compared, 16);
}

TEST_F(CashFlowDifferential, TheSummationOrderIsUpstreamsOrder) {
  // a + b, not b + a. One addition, but the merged amount is observable and
  // floating-point addition is not commutative in the bits when the operands
  // differ hugely in magnitude.
  const double big = 1.0e17;
  const double small = 1.0;
  const std::vector<cdsmodel::CashFlow> a{{cdsmodel::from_tdate(40000), big}};
  const std::vector<cdsmodel::CashFlow> b{{cdsmodel::from_tdate(40000), small}};
  const Ref ra = upstream_list(a);
  const Ref rb = upstream_list(b);
  ASSERT_TRUE(ra);
  ASSERT_TRUE(rb);
  const Ref merged(JpmcdsMergeCFL(ra.cfl, rb.cfl));
  ASSERT_TRUE(merged);
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got = cdsmodel::merge_cash_flows(a, b);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 1u);
  EXPECT_EQ((*got)[0].amount, merged.cfl->fArray[0].fAmount);
  EXPECT_EQ((*got)[0].amount, big + small);
}

TEST_F(CashFlowDifferential, NewCFLMatchesIncludingTheEmptyList) {
  for (int n : {0, 1, 2, 7}) {
    std::vector<cdsmodel::Date> dates;
    std::vector<double> amounts;
    for (int i = 0; i < n; ++i) {
      dates.push_back(cdsmodel::from_tdate(40000 + 100 * i));
      amounts.push_back(1.5 * double(i + 1));
    }
    std::vector<TDate> raw_dates;
    for (const cdsmodel::Date d : dates) {
      raw_dates.push_back(cdsmodel::to_tdate(d));
    }
    const Ref ref(JpmcdsNewCFL(raw_dates.empty() ? nullptr : raw_dates.data(),
                               amounts.empty() ? nullptr : amounts.data(), n));
    ASSERT_TRUE(ref) << n;
    EXPECT_EQ(ref.cfl->fNumItems, n);

    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
        cdsmodel::make_cash_flows(dates, amounts);
    ASSERT_TRUE(got.has_value()) << n;
    EXPECT_EQ(*got, as_flows(ref.cfl)) << n;
  }
  // A length the port can refuse and upstream cannot express: one count for
  // two arrays.
  const std::vector<cdsmodel::Date> two{cdsmodel::from_tdate(40000), cdsmodel::from_tdate(40100)};
  const std::vector<double> one{1.0};
  EXPECT_FALSE(cdsmodel::make_cash_flows(two, one).has_value());
}

TEST_F(CashFlowDifferential, AnEmptyAllocationIsGenuinelyZeroed) {
  // JpmcdsMallocSafe is malloc FOLLOWED BY memset -- the "Don't use calloc"
  // comment means it reproduces calloc, not that it skips the zeroing. So
  // "NewEmpty" really is empty, and std::vector's value-initialisation matches
  // it. Asserted because reading only the malloc line suggests otherwise.
  const Ref ref(JpmcdsNewEmptyCFL(6));
  ASSERT_TRUE(ref);
  ASSERT_EQ(ref.cfl->fNumItems, 6);
  for (int i = 0; i < ref.cfl->fNumItems; ++i) {
    EXPECT_EQ(ref.cfl->fArray[i].fDate, 0) << i;
    EXPECT_EQ(ref.cfl->fArray[i].fAmount, 0.0) << i;
  }
  // NOT std::vector<CashFlow>(6): a default-constructed Date is 1970-01-01,
  // the sys_days epoch, while upstream's zeroed fDate is TDate 0 -- which is
  // 1601-01-01. The port's Date has no zero, so the equivalent has to name the
  // date it means.
  const std::vector<cdsmodel::CashFlow> ported(6, cdsmodel::CashFlow{cdsmodel::from_tdate(0), 0.0});
  EXPECT_EQ(ported, as_flows(ref.cfl));
  EXPECT_NE(cdsmodel::CashFlow{}.date, cdsmodel::from_tdate(0))
      << "which is why vector<CashFlow>(n) is not JpmcdsNewEmptyCFL(n)";

  // And the boundaries of the count.
  const Ref zero(JpmcdsNewEmptyCFL(0));
  ASSERT_TRUE(zero) << "zero items is accepted";
  EXPECT_EQ(zero.cfl->fNumItems, 0);
  EXPECT_EQ(zero.cfl->fArray, nullptr);
  const Ref negative(JpmcdsNewEmptyCFL(-1));
  EXPECT_FALSE(negative) << "a negative count is refused";
}

TEST_F(CashFlowDifferential, Upstream20_AnUnorderedMergeSucceedsAndMeansNothing) {
  // The header documents exactly one precondition -- non-NULL -- and checks
  // it. Ordering is neither documented nor checked, and the result is not a
  // sorted list, not a stable interleaving, and not an error.
  const std::vector<cdsmodel::CashFlow> a{{cdsmodel::from_tdate(40000), 1.0},
                                          {cdsmodel::from_tdate(20000), 2.0},
                                          {cdsmodel::from_tdate(30000), 3.0}};
  const std::vector<cdsmodel::CashFlow> b{{cdsmodel::from_tdate(25000), 10.0},
                                          {cdsmodel::from_tdate(15000), 20.0}};
  const Ref ra = upstream_list(a);
  const Ref rb = upstream_list(b);
  ASSERT_TRUE(ra);
  ASSERT_TRUE(rb);

  const Ref merged(JpmcdsMergeCFL(ra.cfl, rb.cfl));
  ASSERT_TRUE(merged) << "upstream reports success";
  const std::vector<cdsmodel::CashFlow> got = as_flows(merged.cfl);
  ASSERT_EQ(got.size(), 5u) << "every element is present";
  const std::vector<cdsmodel::TDate> order{25000, 15000, 40000, 20000, 30000};
  for (std::size_t i = 0; i < order.size(); ++i) {
    EXPECT_EQ(cdsmodel::to_tdate(got[i].date), order[i]) << "i=" << i;
  }
  // Not sorted, and not a stable interleaving of the inputs either.
  EXPECT_GT(cdsmodel::to_tdate(got[0].date), cdsmodel::to_tdate(got[1].date))
      << "the output is not ordered";

  // The documented precondition IS checked, on both sides.
  EXPECT_EQ(JpmcdsMergeCFL(nullptr, rb.cfl), nullptr);
  EXPECT_EQ(JpmcdsMergeCFL(ra.cfl, nullptr), nullptr);

  // The port refuses, whichever side is out of order.
  EXPECT_FALSE(cdsmodel::merge_cash_flows(a, b).has_value());
  EXPECT_FALSE(cdsmodel::merge_cash_flows(b, a).has_value());
  const std::vector<cdsmodel::CashFlow> ordered{{cdsmodel::from_tdate(20000), 1.0},
                                                {cdsmodel::from_tdate(30000), 2.0}};
  EXPECT_FALSE(cdsmodel::merge_cash_flows(ordered, b).has_value()) << "b alone is enough";
  EXPECT_TRUE(cdsmodel::merge_cash_flows(ordered, ordered).has_value());
}

} // namespace
