// TIER 1 -- the bad day substitution table, without an oracle.
//
// A lookup table has no arithmetic to verify, so the properties are about what
// the two directions guarantee TOGETHER, and about which of them survives a
// malformed map. That distinction is the whole content of this module.

#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include "golden/bad_day_map_cases.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace {

using cdsmodel::BadDayPair;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

const cdsmodel::Date kA = d(2024, 1, 6);
const cdsmodel::Date kB = d(2024, 1, 7);
const cdsmodel::Date kC = d(2024, 1, 8);
const cdsmodel::Date kD = d(2024, 1, 9);

std::vector<cdsmodel::Date> probes() {
  return {d(2024, 1, 5), kA, kB, kC, kD, d(2024, 1, 10), d(1999, 6, 1), d(2099, 12, 31)};
}

// =====================================================================
// What a WELL-FORMED map guarantees.
// =====================================================================

TEST(BadDayMap, AWellFormedMapRoundTripsInBothDirections) {
  const std::vector<BadDayPair> map{{kA, kC}, {kB, kD}};
  ASSERT_TRUE(cdsmodel::is_well_formed(map)) << "the premise";
  for (const BadDayPair &pair : map) {
    EXPECT_EQ(cdsmodel::bad_to_good(map, pair.bad), pair.good);
    EXPECT_EQ(cdsmodel::good_to_bad(map, pair.good), pair.bad);
    // And the round trip closes both ways round.
    EXPECT_EQ(cdsmodel::good_to_bad(map, cdsmodel::bad_to_good(map, pair.bad)), pair.bad);
    EXPECT_EQ(cdsmodel::bad_to_good(map, cdsmodel::good_to_bad(map, pair.good)), pair.good);
  }
}

TEST(BadDayMap, AWellFormedLookupReachesAFixedPointInOneStep) {
  // This is what "well formed" buys, and the only property a chained map
  // breaks: applying the lookup twice must change nothing.
  const std::vector<BadDayPair> map{{kA, kC}, {kB, kD}};
  ASSERT_TRUE(cdsmodel::is_well_formed(map));
  for (const cdsmodel::Date probe : probes()) {
    const cdsmodel::Date once = cdsmodel::bad_to_good(map, probe);
    EXPECT_EQ(cdsmodel::bad_to_good(map, once), once)
        << "at " << cdsmodel::to_tdate(probe) << ": one step was not enough";
  }
}

TEST(BadDayMap, ADateOutsideTheMapIsReturnedUnchanged) {
  const std::vector<BadDayPair> map{{kA, kC}};
  for (const cdsmodel::Date probe : probes()) {
    const bool is_bad = probe == kA;
    const bool is_good = probe == kC;
    if (!is_bad) {
      EXPECT_EQ(cdsmodel::bad_to_good(map, probe), probe) << cdsmodel::to_tdate(probe);
    }
    if (!is_good) {
      EXPECT_EQ(cdsmodel::good_to_bad(map, probe), probe) << cdsmodel::to_tdate(probe);
    }
  }
}

TEST(BadDayMap, AnEmptyMapIsTheIdentity) {
  const std::vector<BadDayPair> none{};
  EXPECT_TRUE(cdsmodel::is_well_formed(none)) << "vacuously";
  for (const cdsmodel::Date probe : probes()) {
    EXPECT_EQ(cdsmodel::bad_to_good(none, probe), probe);
    EXPECT_EQ(cdsmodel::good_to_bad(none, probe), probe);
  }
  EXPECT_TRUE(cdsmodel::fix_bad_dates(none, {}).empty());
  const std::vector<cdsmodel::Date> dates = probes();
  EXPECT_EQ(cdsmodel::fix_bad_dates(none, dates), dates);
}

TEST(BadDayMap, FixingAListIsTheLookupAppliedPointwise) {
  const std::vector<BadDayPair> map{{kA, kC}, {kB, kD}};
  const std::vector<cdsmodel::Date> dates = probes();
  const std::vector<cdsmodel::Date> fixed = cdsmodel::fix_bad_dates(map, dates);
  ASSERT_EQ(fixed.size(), dates.size()) << "a substitution never adds or drops a date";
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(fixed[i], cdsmodel::bad_to_good(map, dates[i])) << i;
  }
}

TEST(BadDayMap, FixingCanCollapseTwoDatesOntoOneAndDoesNotResort) {
  // Worth asserting because it is easy to assume a date list stays distinct
  // and sorted through a substitution. It does not, on either count, and
  // upstream has the same property with nothing downstream re-sorting.
  const std::vector<BadDayPair> collapsing{{kA, kD}, {kB, kD}};
  const std::vector<cdsmodel::Date> fixed = cdsmodel::fix_bad_dates(collapsing, {{kA, kB}});
  ASSERT_EQ(fixed.size(), 2u);
  EXPECT_EQ(fixed[0], kD);
  EXPECT_EQ(fixed[1], kD) << "two distinct dates became one";

  const std::vector<BadDayPair> reordering{{kA, kD}, {kD, kA}};
  const std::vector<cdsmodel::Date> swapped = cdsmodel::fix_bad_dates(reordering, {{kA, kB, kD}});
  ASSERT_EQ(swapped.size(), 3u);
  EXPECT_FALSE(std::ranges::is_sorted(swapped)) << "and the result is not sorted";
}

// =====================================================================
// What a MALFORMED map does, and what is_well_formed says about it.
// =====================================================================

TEST(BadDayMap, EachMalformedShapeIsRejectedForItsOwnReason) {
  // Three conditions, each corresponding to one property the lookups assume.
  EXPECT_FALSE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kC}, {kA, kD}}))
      << "a repeated bad day shadows the later entry";
  EXPECT_FALSE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kD}, {kB, kD}}))
      << "a repeated good day makes the reverse lookup ambiguous";
  EXPECT_FALSE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kB}, {kB, kC}}))
      << "a chain means one step does not reach a fixed point";
  EXPECT_FALSE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kA}}))
      << "a self-mapping is a chain of length one";
  EXPECT_FALSE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kB}, {kB, kA}}))
      << "a two-cycle is a chain in both directions";
  // And the well-formed shapes pass.
  EXPECT_TRUE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kC}}));
  EXPECT_TRUE(cdsmodel::is_well_formed(std::vector<BadDayPair>{{kA, kC}, {kB, kD}}));
}

TEST(BadDayMap, TheFirstMatchWinsInEitherColumn) {
  // The consequence of a linear scan, reproduced rather than tidied: a
  // duplicated entry makes everything after it unreachable.
  const std::vector<BadDayPair> duplicate_bad{{kA, kC}, {kA, kD}};
  EXPECT_EQ(cdsmodel::bad_to_good(duplicate_bad, kA), kC) << "not kD";
  const std::vector<BadDayPair> duplicate_good{{kA, kD}, {kB, kD}};
  EXPECT_EQ(cdsmodel::good_to_bad(duplicate_good, kD), kA) << "not kB";
}

TEST(BadDayMap, AChainedMapBreaksTheFixedPointButNothingElse) {
  // Precisely scoped: a chain costs the one-step property and leaves the
  // others intact. That is why it is an observation rather than a defect --
  // the lookup still does exactly what it says.
  const std::vector<BadDayPair> chained{{kA, kB}, {kB, kC}};
  ASSERT_FALSE(cdsmodel::is_well_formed(chained));

  // The one-step property is gone.
  const cdsmodel::Date once = cdsmodel::bad_to_good(chained, kA);
  EXPECT_EQ(once, kB);
  EXPECT_NE(cdsmodel::bad_to_good(chained, once), once) << "a second step moves again";

  // Every individual lookup is still exactly what the map says.
  for (const BadDayPair &pair : chained) {
    EXPECT_EQ(cdsmodel::bad_to_good(chained, pair.bad), pair.good);
  }
  // And dates outside the map are still untouched.
  EXPECT_EQ(cdsmodel::bad_to_good(chained, kD), kD);
}

// =====================================================================
// Replay.
// =====================================================================

TEST(BadDayMap, MatchesRecordedLookups) {
  int checked = 0;
  int substituted = 0;
  for (const auto &c : cdsmodel::golden::kBadDayMapCases) {
    std::vector<BadDayPair> map;
    for (std::size_t i = 0; i < c.n; ++i) {
      map.push_back(BadDayPair{cdsmodel::from_tdate(c.bad[i]), cdsmodel::from_tdate(c.good[i])});
    }
    const cdsmodel::Date probe = cdsmodel::from_tdate(c.probe);
    const cdsmodel::Date got =
        c.direction == 0 ? cdsmodel::bad_to_good(map, probe) : cdsmodel::good_to_bad(map, probe);
    const std::string where = std::string{c.shape} + " dir " + std::to_string(c.direction) +
                              " probe " + std::to_string(c.probe);
    EXPECT_EQ(cdsmodel::to_tdate(got), c.result) << where;
    if (c.result != c.probe) {
      ++substituted;
    }
    ++checked;
  }
  EXPECT_EQ(checked, 160) << checked;
  EXPECT_GT(substituted, 20) << "the sweep must actually substitute, not just return its input: "
                             << substituted;
}

} // namespace
