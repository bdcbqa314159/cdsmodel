// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// The recorded vectors here are the DOCUMENTED INTENT, not upstream's
// answers, because this module corrects upstream (UPSTREAM-7). Each case also
// carries whether upstream agreed at generation time, which is what makes the
// correction visible rather than silent.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_list.hpp>

#include "golden/date_list_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono;

std::vector<cdsmodel::Date> offsets(std::initializer_list<int> days) {
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  std::vector<cdsmodel::Date> out;
  for (int d : days) {
    out.push_back(base + std::chrono::days{d});
  }
  return out;
}

std::vector<cdsmodel::Date> take(const std::array<long, cdsmodel::golden::kDateListMax> &raw,
                                 std::size_t n) {
  std::vector<cdsmodel::Date> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(cdsmodel::from_tdate(raw[i]));
  }
  return out;
}

TEST(DateListGolden, TruncationMatchesRecordedIntent) {
  for (const auto &c : cdsmodel::golden::kTruncateCases) {
    const auto in = take(c.dates, c.n);
    const auto expected = take(c.out, c.out_n);
    const auto got = cdsmodel::truncate_date_list(
        in, cdsmodel::from_tdate(c.cut),
        c.side == 0 ? cdsmodel::Keep::Before : cdsmodel::Keep::After, c.inclusive);
    ASSERT_TRUE(got.has_value()) << "rejected a strictly increasing list";
    EXPECT_EQ(*got, expected) << "cut=" << c.cut << " side=" << c.side
                              << " inclusive=" << c.inclusive << " n=" << c.n;
  }
}

TEST(DateListGolden, UpstreamStillDisagreesOnASubstantialShare) {
  // The point of recording the agreement flag. If upstream ever stops
  // disagreeing, the correction in date_list.hpp should be revisited rather
  // than carried forever.
  std::size_t disagreed = 0;
  for (const auto &c : cdsmodel::golden::kTruncateCases) {
    if (!c.upstream_agreed) {
      ++disagreed;
    }
  }
  EXPECT_GT(disagreed, 100u)
      << "upstream now agrees with the documented intent far more often than when these "
         "vectors were recorded -- regenerate and revisit the correction";
  EXPECT_LT(disagreed, cdsmodel::golden::kTruncateCases.size())
      << "upstream disagrees everywhere, which would mean the recorded intent is wrong";
}

// --- port semantics ------------------------------------------------------

TEST(DateListTruncate, KeepsTheCorrectSideAndHonoursInclusive) {
  const auto list = offsets({1, 2, 3, 4, 5});
  const cdsmodel::Date cut = *cdsmodel::make_date(2024, 1, 1) + days{3};

  EXPECT_EQ(*cdsmodel::truncate_date_list(list, cut, cdsmodel::Keep::Before, false),
            offsets({1, 2}));
  EXPECT_EQ(*cdsmodel::truncate_date_list(list, cut, cdsmodel::Keep::Before, true),
            offsets({1, 2, 3}));
  EXPECT_EQ(*cdsmodel::truncate_date_list(list, cut, cdsmodel::Keep::After, false),
            offsets({4, 5}));
  EXPECT_EQ(*cdsmodel::truncate_date_list(list, cut, cdsmodel::Keep::After, true),
            offsets({3, 4, 5}));
}

TEST(DateListTruncate, HandlesTheCasesUpstreamGetsWrong) {
  // Exactly the regime where both upstream implementations fail: fewer than
  // two dates survive.
  const auto list = offsets({5, 6, 7});
  const auto at = [](int d) { return *cdsmodel::make_date(2024, 1, 1) + days{d}; };

  EXPECT_TRUE(cdsmodel::truncate_date_list(list, at(3), cdsmodel::Keep::Before, false)->empty());
  EXPECT_TRUE(cdsmodel::truncate_date_list(list, at(5), cdsmodel::Keep::Before, false)->empty());
  EXPECT_EQ(*cdsmodel::truncate_date_list(list, at(6), cdsmodel::Keep::Before, false),
            offsets({5}));
  EXPECT_TRUE(cdsmodel::truncate_date_list(list, at(9), cdsmodel::Keep::After, false)->empty());
  // A single-element list, cut after its only date.
  EXPECT_EQ(*cdsmodel::truncate_date_list(offsets({5}), at(9), cdsmodel::Keep::Before, false),
            offsets({5}));
}

TEST(DateListTruncate, RejectsUnsortedInput) {
  EXPECT_FALSE(cdsmodel::truncate_date_list(offsets({3, 1, 2}), *cdsmodel::make_date(2024, 1, 1),
                                            cdsmodel::Keep::Before, false)
                   .has_value());
  EXPECT_FALSE(cdsmodel::truncate_date_list(offsets({1, 1, 2}), *cdsmodel::make_date(2024, 1, 1),
                                            cdsmodel::Keep::Before, false)
                   .has_value());
}

TEST(DateListMerge, IsAUnionAndRejectsUnsortedInput) {
  EXPECT_EQ(*cdsmodel::merge_date_lists(offsets({1, 3, 5}), offsets({2, 3, 6})),
            offsets({1, 2, 3, 5, 6}));
  EXPECT_EQ(*cdsmodel::merge_date_lists({}, offsets({2, 4})), offsets({2, 4}));
  EXPECT_TRUE(cdsmodel::merge_date_lists({}, {})->empty());
  // Upstream documents this precondition and does not check it, returning
  // [2,4,5,1,3] for the first case here -- neither sorted nor a union.
  EXPECT_FALSE(cdsmodel::merge_date_lists(offsets({5, 1, 3}), offsets({2, 4})).has_value());
  EXPECT_FALSE(cdsmodel::merge_date_lists(offsets({1, 1}), offsets({2})).has_value());
}

TEST(DateListAddDates, HonoursTheDuplicateFlag) {
  const auto base = offsets({1, 3, 5});
  EXPECT_EQ(*cdsmodel::add_dates(base, offsets({3, 4}), true), offsets({1, 3, 3, 4, 5}));
  EXPECT_EQ(*cdsmodel::add_dates(base, offsets({3, 4}), false), offsets({1, 3, 4, 5}));
  EXPECT_EQ(*cdsmodel::add_dates(base, {}, false), base);
}

TEST(DateListSubtract, RemovesAndKeepsEverythingElse) {
  const auto list = offsets({1, 2, 3, 4, 5, 6});
  EXPECT_EQ(cdsmodel::subtract_date_lists(list, offsets({2, 4})), offsets({1, 3, 5, 6}));
  // The cases upstream gets wrong: it loses the largest survivor, and returns
  // nothing at all when the subtrahend is empty.
  EXPECT_EQ(cdsmodel::subtract_date_lists(offsets({1, 2, 3}), offsets({9})), offsets({1, 2, 3}));
  EXPECT_EQ(cdsmodel::subtract_date_lists(offsets({1, 2, 3}), {}), offsets({1, 2, 3}));
  EXPECT_TRUE(cdsmodel::subtract_date_lists(offsets({1, 2}), offsets({1, 2})).empty());
  // Unsorted input is allowed here, as upstream documents; output is sorted.
  EXPECT_EQ(cdsmodel::subtract_date_lists(offsets({5, 1, 3, 2}), offsets({3, 1})), offsets({2, 5}));
}

TEST(DateListDistinct, RemovesAdjacentRunsOnly) {
  EXPECT_EQ(cdsmodel::distinct(offsets({1, 1, 2, 3, 3, 3, 5})), offsets({1, 2, 3, 5}));
  // Adjacent only, matching upstream: unsorted input loses nothing.
  EXPECT_EQ(cdsmodel::distinct(offsets({3, 1, 3, 1, 2})), offsets({3, 1, 3, 1, 2}));
  // sorted_distinct is the one that handles unsorted input.
  EXPECT_EQ(cdsmodel::sorted_distinct(offsets({3, 1, 3, 1, 2})), offsets({1, 2, 3}));
}

} // namespace
