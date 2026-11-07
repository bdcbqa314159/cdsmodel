// TIER 1 -- replays tests/golden/match_range_cases.hpp, so the match-range
// search is covered in CI, where no oracle is present.
//
// Everything recorded is a small integer, so these compare EXACTLY. There is
// no tolerance in this file and adding one would be a mistake.
//
// The domain is EXHAUSTIVE -- every non-decreasing array of length 1 to 5 over
// the alphabet 0..3, against every sought value from -1 to 4 -- so this is one
// of the few places in the project where "the port agrees with upstream" can
// be said without a qualifier about which inputs were tried.

#include <cdsmodel/match_range.hpp>

#include "golden/match_range_cases.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace cdsmodel::golden;

std::span<const long> array_of(const MatchRangeCase &c) {
  return std::span<const long>{kMatchRangeValues.data() + c.first_value, c.length};
}

TEST(MatchRangeGolden, ReplaysEveryRecordedSearch) {
  ASSERT_EQ(kMatchRangeCases.size(), 750u) << "the recorded domain changed size";
  int matched = 0;
  int with_duplicates = 0;
  int no_match = 0;
  int refused = 0;

  for (const MatchRangeCase &c : kMatchRangeCases) {
    const std::span<const long> xs = array_of(c);
    const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(c.desired, xs);
    ASSERT_EQ(got.has_value(), c.accepted) << "length " << c.length << " seek " << c.desired;
    if (!c.accepted) {
      ++refused;
      continue;
    }

    // Upstream's three outputs, converted by its own documented invariant.
    const auto want_first = static_cast<std::size_t>(c.lo_bound + 1);
    const auto want_count = static_cast<std::size_t>(c.hi_bound - c.lo_bound - 1);
    EXPECT_EQ(got->first, want_first) << "length " << c.length << " seek " << c.desired;
    EXPECT_EQ(got->count, want_count) << "length " << c.length << " seek " << c.desired;
    EXPECT_EQ(got->empty(), want_count == 0);

    if (want_count > 0) {
      // The weaker property upstream actually promises: `exact` indexes SOME
      // equal element, not necessarily the first.
      EXPECT_GE(c.exact, static_cast<long>(got->first));
      EXPECT_LT(c.exact, static_cast<long>(got->first + got->count));
      EXPECT_EQ(xs[static_cast<std::size_t>(c.exact)], c.desired);
      EXPECT_EQ(got->exact(), std::optional<std::size_t>{got->first});
      if (want_count > 1) {
        ++with_duplicates;
      }
    } else {
      EXPECT_EQ(c.exact, -1);
      EXPECT_FALSE(got->exact().has_value());
      ++no_match;
    }
    ++matched;
  }
  EXPECT_EQ(matched + refused, 750);
  EXPECT_GT(with_duplicates, 100) << "duplicate runs are the point and were not exercised";
  EXPECT_GT(no_match, 100) << "the no-match path was not exercised";
}

// --------------------------------------------- properties, no oracle needed

// Every index the range claims really does hold the sought value, and the
// elements either side of the range do not. This is the definition of the
// function, checked against the arrays rather than against the recording.
TEST(MatchRangeGolden, TheRangeIsExactlyTheRunOfEqualElements) {
  int checked = 0;
  int boundary_checks = 0;
  for (const MatchRangeCase &c : kMatchRangeCases) {
    if (!c.accepted) {
      continue;
    }
    const std::span<const long> xs = array_of(c);
    const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(c.desired, xs);
    ASSERT_TRUE(got.has_value());

    for (std::size_t i = got->first; i < got->first + got->count; ++i) {
      EXPECT_EQ(xs[i], c.desired) << "the range includes a non-match";
      ++checked;
    }
    if (got->first > 0) {
      EXPECT_NE(xs[got->first - 1], c.desired) << "a match was left outside the range, below";
      ++boundary_checks;
    }
    const std::size_t after = got->first + got->count;
    if (after < xs.size()) {
      EXPECT_NE(xs[after], c.desired) << "a match was left outside the range, above";
      ++boundary_checks;
    }
  }
  EXPECT_GT(checked, 500);
  EXPECT_GT(boundary_checks, 500) << "the range boundaries were barely tested";
}

// `first` is the insertion point even when nothing matches: everything before
// it is strictly less, everything from it on is strictly greater. That is what
// makes the sentinel-free representation total.
TEST(MatchRangeGolden, FirstIsAnInsertionPointWhenNothingMatches) {
  int checked = 0;
  for (const MatchRangeCase &c : kMatchRangeCases) {
    if (!c.accepted) {
      continue;
    }
    const std::span<const long> xs = array_of(c);
    const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<long>(c.desired, xs);
    ASSERT_TRUE(got.has_value());
    if (!got->empty()) {
      continue;
    }
    for (std::size_t i = 0; i < got->first; ++i) {
      EXPECT_LT(xs[i], c.desired) << "an element before the insertion point is not less";
    }
    for (std::size_t i = got->first; i < xs.size(); ++i) {
      EXPECT_GT(xs[i], c.desired) << "an element at or after it is not greater";
    }
    ++checked;
  }
  EXPECT_GT(checked, 100);
}

// An empty array is refused, which is upstream's REQUIRE(arraySize > 0).
TEST(MatchRangeGolden, AnEmptyArrayIsRefused) {
  const std::vector<long> empty;
  EXPECT_FALSE(cdsmodel::match_range<long>(10, empty).has_value());
}

// The function is a template, so it works on any ordered type -- upstream's is
// a `long` search with a byte stride, which is how it reached fields inside
// arrays of structs. Checked on a second type so the template is not just
// decoration.
TEST(MatchRangeGolden, TheSearchIsNotSpecificToLong) {
  const std::vector<int> xs{10, 20, 20, 30};
  const std::optional<cdsmodel::MatchRange> got = cdsmodel::match_range<int>(20, xs);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->first, 1u);
  EXPECT_EQ(got->count, 2u);

  const std::vector<double> ds{1.5, 2.5, 2.5, 3.5};
  const std::optional<cdsmodel::MatchRange> dgot = cdsmodel::match_range<double>(2.5, ds);
  ASSERT_TRUE(dgot.has_value());
  EXPECT_EQ(dgot->first, 1u);
  EXPECT_EQ(dgot->count, 2u);
}

} // namespace
