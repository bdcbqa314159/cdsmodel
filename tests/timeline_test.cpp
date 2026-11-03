// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
//
// Compared exactly: these are dates, not transcendental values, so unlike the
// zero curve vectors they reproduce bit-for-bit on every platform.

#include <cdsmodel/date.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/timeline_cases.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <span>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::ZeroCurve rebuild(std::span<const long> dates, std::span<const double> rates) {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < dates.size(); ++i) {
    points.push_back({cdsmodel::from_tdate(dates[i]), rates[i]});
  }
  return *cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(cdsmodel::golden::kTimelineBase), points,
                                      1.0, cdsmodel::DayCount::Act365Fixed);
}

TEST(TimelineGolden, MatchesRecordedReferenceExactly) {
  const cdsmodel::ZeroCurve disc =
      rebuild(cdsmodel::golden::kTimelineDiscountDates, cdsmodel::golden::kTimelineDiscountRates);
  const cdsmodel::ZeroCurve spread =
      rebuild(cdsmodel::golden::kTimelineSpreadDates, cdsmodel::golden::kTimelineSpreadRates);
  for (const auto &c : cdsmodel::golden::kTimelineCases) {
    std::vector<cdsmodel::Date> expected;
    for (std::size_t i = 0; i < c.n; ++i) {
      expected.push_back(cdsmodel::from_tdate(c.dates[i]));
    }
    const auto got = cdsmodel::risky_timeline(cdsmodel::from_tdate(c.start),
                                              cdsmodel::from_tdate(c.end), disc, spread);
    ASSERT_TRUE(got.has_value()) << "start=" << c.start << " end=" << c.end;
    EXPECT_EQ(*got, expected) << "start=" << c.start << " end=" << c.end;
  }
}

TEST(TimelineGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kTimelineCases.size(), 80u);
}

// --- port semantics ------------------------------------------------------

TEST(Timeline, ContainsBothEndpointsAndNothingOutsideThem) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const auto disc = cdsmodel::ZeroCurve::create(
      base, std::vector<cdsmodel::RatePoint>{{d(2025, 1, 1), 0.03}, {d(2030, 1, 1), 0.04}}, 1.0,
      cdsmodel::DayCount::Act365Fixed);
  const auto spread =
      cdsmodel::ZeroCurve::create(base, std::vector<cdsmodel::RatePoint>{{d(2026, 6, 1), 0.01}},
                                  1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc.has_value());
  ASSERT_TRUE(spread.has_value());

  const cdsmodel::Date start = d(2024, 6, 1);
  const cdsmodel::Date end = d(2029, 6, 1);
  const auto tl = cdsmodel::risky_timeline(start, end, *disc, *spread);
  ASSERT_TRUE(tl.has_value());

  EXPECT_EQ(tl->front(), start);
  EXPECT_EQ(tl->back(), end);
  EXPECT_TRUE(std::ranges::is_sorted(*tl));
  EXPECT_EQ(std::ranges::adjacent_find(*tl), tl->end()) << "timeline has a duplicate date";
  for (cdsmodel::Date date : *tl) {
    EXPECT_GE(date, start);
    EXPECT_LE(date, end);
  }
  // The curve points inside the window are present; the ones outside are not.
  EXPECT_NE(std::ranges::find(*tl, d(2025, 1, 1)), tl->end());
  EXPECT_NE(std::ranges::find(*tl, d(2026, 6, 1)), tl->end());
  EXPECT_EQ(std::ranges::find(*tl, d(2030, 1, 1)), tl->end()) << "a date past the end survived";
}

TEST(Timeline, MinimalTimelineIsExactlyTheTwoEndpoints) {
  // This is the regime where upstream's truncation is defective (UPSTREAM-7).
  // It cannot be reached through a timeline, because both endpoints are in the
  // list before either truncation runs, so two dates always survive. Pinned
  // here so a future refactor that truncates before merging gets caught.
  const cdsmodel::Date base = d(2024, 1, 1);
  const auto disc =
      cdsmodel::ZeroCurve::create(base, std::vector<cdsmodel::RatePoint>{{d(2030, 1, 1), 0.04}},
                                  1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread =
      cdsmodel::ZeroCurve::create(base, std::vector<cdsmodel::RatePoint>{{d(2031, 1, 1), 0.01}},
                                  1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc.has_value());

  const cdsmodel::Date start = d(2025, 3, 10);
  const auto tl = cdsmodel::risky_timeline(start, start + days{1}, *disc, *spread);
  ASSERT_TRUE(tl.has_value());
  ASSERT_EQ(tl->size(), 2u);
  EXPECT_EQ((*tl)[0], start);
  EXPECT_EQ((*tl)[1], start + days{1});
}

TEST(Timeline, RejectsANonPositiveWindow) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const auto curve =
      cdsmodel::ZeroCurve::create(base, std::vector<cdsmodel::RatePoint>{{d(2030, 1, 1), 0.04}},
                                  1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  const cdsmodel::Date day = d(2026, 1, 1);
  EXPECT_FALSE(cdsmodel::risky_timeline(day, day, *curve, *curve).has_value());
  EXPECT_FALSE(cdsmodel::risky_timeline(day, day - days{1}, *curve, *curve).has_value());
  EXPECT_FALSE(cdsmodel::truncate_timeline({}, day, day).has_value());
}

TEST(Timeline, TruncateTimelineAcceptsAnEmptyCriticalSet) {
  const cdsmodel::Date start = d(2024, 3, 1);
  const cdsmodel::Date end = d(2025, 3, 1);
  const auto tl = cdsmodel::truncate_timeline({}, start, end);
  ASSERT_TRUE(tl.has_value());
  EXPECT_EQ(tl->size(), 2u);
  EXPECT_EQ(tl->front(), start);
  EXPECT_EQ(tl->back(), end);
}

} // namespace
