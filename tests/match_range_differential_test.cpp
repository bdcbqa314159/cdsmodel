// TIER 2 -- live differential tests for the match-range binary search.
//
// The domain here is small enough to sweep EXHAUSTIVELY rather than sample,
// which is the right call whenever it is affordable: every array of length 1
// to 5 over a small ascending alphabet, crossed with every sought value in and
// around that alphabet. Duplicates are the whole point of this function, so
// the arrays are generated non-decreasing rather than strictly increasing.
//
// Upstream's `exact` is "the index of A value which is equal", and MEASURED to
// be not necessarily the first -- so the comparison asserts it lies within the
// port's range rather than equalling its start. Comparing them directly would
// assert something upstream never promised.
//
// A second claim in this file was also written first and measured second, and
// it was WRONG: see AnUnsortedArrayProducesAgreementNotRefusal.
//
// One test asserts a divergence: UPSTREAM-37, the inconsistent NULL guard. Its
// live reproducer is DISABLED, because it stores through NULL.

#include <cdsmodel/match_range.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

extern "C" {
#include "cerror.h"
#include "cgeneral.h"
#include "cxbsearch.h"
}

namespace {

class MatchRangeDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

/// Upstream's three outputs, and the derived range they encode.
struct RefResult {
  bool accepted = false;
  long exact = -99;
  long lo = -99;
  long hi = -99;

  [[nodiscard]] std::size_t first() const { return static_cast<std::size_t>(lo + 1); }
  [[nodiscard]] std::size_t count() const { return static_cast<std::size_t>(hi - lo - 1); }
};

RefResult reference(long desired, std::vector<long> &xs) {
  RefResult out;
  out.accepted =
      JpmcdsBinarySearchLong(desired, xs.data(), sizeof(long), static_cast<long>(xs.size()),
                             &out.exact, &out.lo, &out.hi) == SUCCESS;
  return out;
}

/// Every non-decreasing array of the given length over values 0..alphabet-1.
void for_each_array(std::size_t length, long alphabet,
                    const std::function<void(std::vector<long> &)> &body) {
  std::vector<long> xs(length, 0);
  const auto recurse = [&](auto &&self, std::size_t index, long lowest) -> void {
    if (index == length) {
      body(xs);
      return;
    }
    for (long value = lowest; value < alphabet; ++value) {
      xs[index] = value;
      self(self, index + 1, value);
    }
  };
  recurse(recurse, 0, 0);
}

TEST_F(MatchRangeDifferential, MatchesExhaustivelyOverEveryShortArray) {
  int compared = 0;
  int with_duplicates = 0;
  int no_match = 0;
  // Non-decreasing sequences of length k over an alphabet of a number
  // C(a+k-1, k), so lengths 1..6 over an alphabet of 5 give
  // 5 + 15 + 35 + 70 + 126 + 210 = 461 arrays, each tried against 7 sought
  // values. The expected total is asserted exactly below: "exhaustive over
  // this domain" is a claim worth pinning, and a threshold like `> 2000`
  // would have silently accepted a sweep that stopped early.
  for (std::size_t length = 1; length <= 6; ++length) {
    for_each_array(length, 5, [&](std::vector<long> &xs) {
      for (long desired = -1; desired <= 5; ++desired) {
        const RefResult ref = reference(desired, xs);
        const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(desired, xs);

        ASSERT_EQ(ref.accepted, got.has_value())
            << "acceptance differs, length " << length << " seek " << desired;
        if (!ref.accepted) {
          return;
        }
        EXPECT_EQ(got->first, ref.first())
            << "first differs, length " << length << " seek " << desired;
        EXPECT_EQ(got->count, ref.count())
            << "count differs, length " << length << " seek " << desired;

        // Upstream's own documented invariant, checked on upstream.
        EXPECT_EQ(ref.hi - ref.lo - 1, static_cast<long>(got->count))
            << "hiBound - loBound - 1 is no longer the match count";

        if (got->count > 0) {
          // `exact` is SOME matching index, not necessarily the first.
          EXPECT_GE(ref.exact, static_cast<long>(got->first));
          EXPECT_LT(ref.exact, static_cast<long>(got->first + got->count));
          EXPECT_EQ(xs[static_cast<std::size_t>(ref.exact)], desired);
          // And every index the port claims really does match.
          for (std::size_t i = got->first; i < got->first + got->count; ++i) {
            EXPECT_EQ(xs[i], desired) << "the port's range includes a non-match";
          }
          if (got->count > 1) {
            ++with_duplicates;
          }
        } else {
          EXPECT_EQ(ref.exact, -1) << "no match, but exact is not -1";
          ++no_match;
        }
        ++compared;
      }
    });
  }
  EXPECT_EQ(compared, 461 * 7) << "the sweep is no longer exhaustive over its domain";
  EXPECT_GT(with_duplicates, 100) << "duplicate runs are the point and were not exercised";
  EXPECT_GT(no_match, 100) << "the no-match path was not exercised";
}

// Upstream's `exact` is not the first match, measured rather than reasoned.
// This is the fact that decides how the comparison above is written, so it is
// asserted directly rather than left as a comment.
TEST_F(MatchRangeDifferential, UpstreamsExactIsNotNecessarilyTheFirstMatch) {
  std::vector<long> xs{10, 10, 20, 30};
  const RefResult ref = reference(10, xs);
  ASSERT_TRUE(ref.accepted);
  EXPECT_EQ(ref.first(), 0u);
  EXPECT_EQ(ref.count(), 2u);
  EXPECT_EQ(ref.exact, 1) << "upstream's exact became the first match; the "
                             "comparison in this file could be tightened";

  const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(10, xs);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->first, 0u) << "the port must report the FIRST match";
  EXPECT_EQ(got->count, 2u);
  EXPECT_EQ(got->exact(), std::optional<std::size_t>{0});
}

// The parameter comment says hiBound is -1 when no greater value exists; the
// function comment and the code say arraySize. Pinned so the port's choice
// rests on the code.
TEST_F(MatchRangeDifferential, HiBoundIsArraySizeNotMinusOne) {
  std::vector<long> xs{10, 20, 20, 30, 40};
  const RefResult ref = reference(99, xs);
  ASSERT_TRUE(ref.accepted);
  EXPECT_EQ(ref.hi, 5) << "the parameter comment's -1 would make this fail";
  EXPECT_EQ(ref.lo, 4);
  EXPECT_EQ(ref.exact, -1);

  const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(99, xs);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->first, 5u) << "past the end, the insertion point is the size";
  EXPECT_EQ(got->count, 0u);
}

// An unsorted array: what ACTUALLY happens.
//
// Upstream's comment implies the iteration cap is how disorder is caught:
// "If the input array is not sorted in ascending order, then the output is
// undefined. No checks are performed on the order of xArray." And there is a
// `count == 0` branch reporting "x array not in increasing order".
//
// I wrote this test asserting at least one unsorted array would be refused.
// It failed: NONE of them were. The cap is `arraySize + 1` iterations on a
// halving search, so it is roughly exponentially generous -- it is a runaway
// backstop, not a detector. Disorder produces a confident wrong answer, which
// is what the documentation actually says and what the test now asserts.
//
// Swept exhaustively over the short arrays so the claim rests on a count
// rather than on four hand-picked examples.
TEST_F(MatchRangeDifferential, AnUnsortedArrayProducesAgreementNotRefusal) {
  int compared = 0;
  int refused_by_both = 0;
  int unsorted_arrays = 0;

  // Every array of length 2..4 over 0..3, keeping only the ones that are NOT
  // non-decreasing.
  for (std::size_t length = 2; length <= 4; ++length) {
    std::vector<long> xs(length, 0);
    const auto recurse = [&](auto &&self, std::size_t index) -> void {
      if (index == length) {
        bool sorted = true;
        for (std::size_t i = 1; i < length; ++i) {
          if (xs[i] < xs[i - 1]) {
            sorted = false;
          }
        }
        if (sorted) {
          return;
        }
        ++unsorted_arrays;
        for (long desired = -1; desired <= 4; ++desired) {
          std::vector<long> copy = xs;
          const RefResult ref = reference(desired, copy);
          const std::optional<cdsmodel::MatchRange> got =
              cdsmodel::match_range<long>(desired, copy);
          ASSERT_EQ(ref.accepted, got.has_value())
              << "the two sides disagree about an unsorted array";
          if (!ref.accepted) {
            ++refused_by_both;
          } else {
            // The answer is undefined by the documentation, but it is not
            // therefore arbitrary: the port reproduces whatever upstream
            // computes, so a caller feeding it unsorted input gets the same
            // wrong answer from both.
            EXPECT_EQ(got->first, ref.first());
            EXPECT_EQ(got->count, ref.count());
          }
          ++compared;
        }
        return;
      }
      for (long value = 0; value < 4; ++value) {
        xs[index] = value;
        self(self, index + 1);
      }
    };
    recurse(recurse, 0);
  }

  EXPECT_GT(unsorted_arrays, 100) << "no unsorted arrays were generated";
  EXPECT_GT(compared, 600);
  EXPECT_EQ(refused_by_both, 0)
      << "an unsorted array was refused; the iteration cap has started firing, "
         "which would make the 'output is undefined' documentation wrong in a "
         "new way";
}

// An empty array is refused by both: upstream's REQUIRE(arraySize > 0).
TEST_F(MatchRangeDifferential, AnEmptyArrayIsRefusedByBoth) {
  std::vector<long> empty;
  long exact = -99;
  long lo = -99;
  long hi = -99;
  EXPECT_NE(JpmcdsBinarySearchLong(10, empty.data(), sizeof(long), 0, &exact, &lo, &hi), SUCCESS);
  EXPECT_FALSE(cdsmodel::match_range<long>(10, empty).has_value());
}

// ------------------------------------------------------------ UPSTREAM-37

// The main path's guards tell a caller NULL is supported; the in-range case
// honours that. Asserted so the invitation is on record.
TEST_F(MatchRangeDifferential, Upstream37NullOutputsAreHonouredOnTheMainPathOnly) {
  std::vector<long> xs{10, 20, 20, 30, 40};
  long exact = -99;
  // In range: the guarded path. NULL for both optional outputs is fine.
  EXPECT_EQ(JpmcdsBinarySearchLong(20, xs.data(), sizeof(long), 5, &exact, nullptr, nullptr),
            SUCCESS)
      << "the main path stopped honouring NULL, so the invitation is withdrawn";
  EXPECT_EQ(exact, 1);

  // Out of range and single-element are the three unguarded early returns.
  // Not called here -- see the DISABLED tests below.

  // The port has no output pointers at all, so every path behaves the same.
  const std::optional<cdsmodel::MatchRange> below = cdsmodel::match_range<long>(5, xs);
  const std::optional<cdsmodel::MatchRange> above = cdsmodel::match_range<long>(99, xs);
  std::vector<long> one{10};
  const std::optional<cdsmodel::MatchRange> single = cdsmodel::match_range<long>(10, one);
  ASSERT_TRUE(below.has_value());
  ASSERT_TRUE(above.has_value());
  ASSERT_TRUE(single.has_value());
  EXPECT_EQ(*below, (cdsmodel::MatchRange{0, 0}));
  EXPECT_EQ(*above, (cdsmodel::MatchRange{5, 0}));
  EXPECT_EQ(*single, (cdsmodel::MatchRange{0, 1}));
}

TEST_F(MatchRangeDifferential, DISABLED_Upstream37BelowRangeStoresThroughNull) {
  std::vector<long> xs{10, 20, 30};
  long exact = -99;
  // `*loBound = -1; *hiBound = 0;` with no guard. UBSan: "store to null
  // pointer of type 'long'" at cxbsearch.c:84. Never enabled.
  JpmcdsBinarySearchLong(5, xs.data(), sizeof(long), 3, &exact, nullptr, nullptr);
}

TEST_F(MatchRangeDifferential, DISABLED_Upstream37AboveRangeStoresThroughNull) {
  std::vector<long> xs{10, 20, 30};
  long exact = -99;
  JpmcdsBinarySearchLong(99, xs.data(), sizeof(long), 3, &exact, nullptr, nullptr);
}

TEST_F(MatchRangeDifferential, DISABLED_Upstream37SingleElementStoresThroughNull) {
  std::vector<long> xs{10};
  long exact = -99;
  JpmcdsBinarySearchLong(10, xs.data(), sizeof(long), 1, &exact, nullptr, nullptr);
}

} // namespace
