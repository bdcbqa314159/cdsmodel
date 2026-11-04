// TIER 1 -- the schedule generator. Port semantics plus recorded vectors.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_list.hpp>
#include <cdsmodel/schedule.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include "golden/schedule_cases.hpp"

#include <vector>

namespace {

using cdsmodel::Date;
using cdsmodel::DateInterval;
using cdsmodel::make_regular_date_list;
using cdsmodel::PeriodUnit;
using cdsmodel::StubLength;
using cdsmodel::StubMethod;
using cdsmodel::StubPosition;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<long> tdates(const std::vector<Date> &dates) {
  std::vector<long> out;
  for (Date date : dates) {
    out.push_back(cdsmodel::to_tdate(date));
  }
  return out;
}

std::vector<long> schedule(Date start, Date end, int periods, PeriodUnit unit, StubPosition pos,
                           StubLength len) {
  const auto got =
      make_regular_date_list(start, end, DateInterval{periods, unit}, StubMethod{pos, len});
  return got ? tdates(*got) : std::vector<long>{};
}

constexpr DateInterval k3M{3, PeriodUnit::Months};
constexpr StubMethod kFrontShort{StubPosition::Front, StubLength::Short};

TEST(Schedule, OnCycleIsIndependentOfStub) {
  // Two years of quarters divides evenly, so every stub choice agrees.
  const std::vector<long> expected = schedule(d(2024, 3, 20), d(2026, 3, 20), 3, PeriodUnit::Months,
                                              StubPosition::Front, StubLength::Short);
  ASSERT_EQ(expected.size(), 9u);
  for (StubPosition pos : {StubPosition::Front, StubPosition::Back}) {
    for (StubLength len : {StubLength::Short, StubLength::Long}) {
      EXPECT_EQ(schedule(d(2024, 3, 20), d(2026, 3, 20), 3, PeriodUnit::Months, pos, len),
                expected);
    }
  }
}

TEST(Schedule, BothEndpointsAlwaysPresentAndStrictlyIncreasing) {
  for (StubPosition pos : {StubPosition::Front, StubPosition::Back}) {
    for (StubLength len : {StubLength::Short, StubLength::Long}) {
      for (int months : {1, 3, 6, 12}) {
        const Date start = d(2024, 1, 31);
        const Date end = d(2027, 5, 14);
        const auto got = make_regular_date_list(
            start, end, DateInterval{months, PeriodUnit::Months}, StubMethod{pos, len});
        ASSERT_TRUE(got.has_value());
        ASSERT_GE(got->size(), 2u);
        EXPECT_EQ(got->front(), start);
        EXPECT_EQ(got->back(), end);
        EXPECT_TRUE(cdsmodel::is_strictly_increasing_dates(*got)) << months << " months";
      }
    }
  }
}

TEST(Schedule, FrontStubAnchorsOnEndAndBackStubOnStart) {
  // 14 months of quarters: four regular periods and two months left over.
  const Date start = d(2024, 1, 15);
  const Date end = d(2025, 3, 15);
  const auto front = make_regular_date_list(start, end, k3M, kFrontShort);
  const auto back =
      make_regular_date_list(start, end, k3M, StubMethod{StubPosition::Back, StubLength::Short});
  ASSERT_TRUE(front.has_value());
  ASSERT_TRUE(back.has_value());

  // Front stub: interior dates sit on the 15th counted back from the end, so
  // the odd period is the first one, 15 Jan -> 15 Mar.
  EXPECT_EQ(tdates(*front),
            tdates({start, d(2024, 3, 15), d(2024, 6, 15), d(2024, 9, 15), d(2024, 12, 15), end}));
  // Back stub: interior dates counted forward from the start, so the odd
  // period is the last one, 15 Jan -> 15 Mar of the following year.
  EXPECT_EQ(tdates(*back),
            tdates({start, d(2024, 4, 15), d(2024, 7, 15), d(2024, 10, 15), d(2025, 1, 15), end}));
}

TEST(Schedule, LongStubAbsorbsTheAdjacentRegularPeriod) {
  const Date start = d(2024, 1, 15);
  const Date end = d(2025, 3, 15);
  const auto front_short = make_regular_date_list(start, end, k3M, kFrontShort);
  const auto front_long =
      make_regular_date_list(start, end, k3M, StubMethod{StubPosition::Front, StubLength::Long});
  const auto back_long =
      make_regular_date_list(start, end, k3M, StubMethod{StubPosition::Back, StubLength::Long});
  ASSERT_TRUE(front_short.has_value());
  ASSERT_TRUE(front_long.has_value());
  ASSERT_TRUE(back_long.has_value());

  // One fewer date, and it is the second one that went: 15 Jan -> 15 Jun.
  EXPECT_EQ(front_long->size(), front_short->size() - 1);
  EXPECT_EQ(tdates(*front_long),
            tdates({start, d(2024, 6, 15), d(2024, 9, 15), d(2024, 12, 15), end}));
  // Symmetrically, the back long stub drops the penultimate date.
  EXPECT_EQ(tdates(*back_long),
            tdates({start, d(2024, 4, 15), d(2024, 7, 15), d(2024, 10, 15), end}));
}

TEST(Schedule, ShorterThanOnePeriodGivesJustTheEndpoints) {
  // totalDates == 1, so the long-stub branch is unreachable and both lengths
  // return the same two dates.
  for (StubPosition pos : {StubPosition::Front, StubPosition::Back}) {
    for (StubLength len : {StubLength::Short, StubLength::Long}) {
      const auto got =
          make_regular_date_list(d(2024, 6, 20), d(2024, 7, 15), k3M, StubMethod{pos, len});
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(tdates(*got), tdates({d(2024, 6, 20), d(2024, 7, 15)}));
    }
  }
}

TEST(Schedule, MonthEndAnchoringClampsAndRecovers) {
  // The decision that makes anchoring matter: each date is computed from the
  // anchor, not from its predecessor. Stepping month by month from 31 January
  // would clamp to the 28th and stay there; anchored arithmetic returns to the
  // 31st whenever the month is long enough.
  const auto got = make_regular_date_list(d(2024, 1, 31), d(2024, 7, 31),
                                          DateInterval{1, PeriodUnit::Months}, kFrontShort);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(tdates(*got), tdates({d(2024, 1, 31), d(2024, 2, 29), d(2024, 3, 31), d(2024, 4, 30),
                                  d(2024, 5, 31), d(2024, 6, 30), d(2024, 7, 31)}));
}

TEST(Schedule, DayIntervals) {
  // Front stub anchors on the end, so the weekly dates count back from 20 Jan
  // and the odd five-day period lands first.
  const auto front = make_regular_date_list(d(2024, 1, 1), d(2024, 1, 20),
                                            DateInterval{7, PeriodUnit::Days}, kFrontShort);
  ASSERT_TRUE(front.has_value());
  EXPECT_EQ(tdates(*front), tdates({d(2024, 1, 1), d(2024, 1, 6), d(2024, 1, 13), d(2024, 1, 20)}));

  // Back stub counts forward from 1 Jan, so the odd period lands last.
  const auto back =
      make_regular_date_list(d(2024, 1, 1), d(2024, 1, 20), DateInterval{7, PeriodUnit::Days},
                             StubMethod{StubPosition::Back, StubLength::Short});
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(tdates(*back), tdates({d(2024, 1, 1), d(2024, 1, 8), d(2024, 1, 15), d(2024, 1, 20)}));
}

TEST(Schedule, Rejected) {
  const Date start = d(2024, 1, 15);
  // end not strictly after start
  EXPECT_FALSE(make_regular_date_list(start, start, k3M, kFrontShort).has_value());
  EXPECT_FALSE(make_regular_date_list(start, d(2024, 1, 14), k3M, kFrontShort).has_value());
  // non-positive period: upstream REQUIREs prd > 0, and it is also what makes
  // the loop terminate.
  EXPECT_FALSE(make_regular_date_list(start, d(2025, 1, 15), DateInterval{0, PeriodUnit::Months},
                                      kFrontShort)
                   .has_value());
  EXPECT_FALSE(make_regular_date_list(start, d(2025, 1, 15), DateInterval{-3, PeriodUnit::Months},
                                      kFrontShort)
                   .has_value());
}

TEST(Schedule, HugePeriodGivesJustTheEndpoints) {
  // A period longer than the span cannot produce an interior date, so the
  // first step leaves the span and the schedule collapses to its endpoints.
  // Measured against upstream, which agrees for every one of these.
  for (int days : {1000000, 100000000, 1000000000, 2000000000}) {
    const auto got = make_regular_date_list(d(2024, 1, 1), d(2024, 6, 1),
                                            DateInterval{days, PeriodUnit::Days}, kFrontShort);
    ASSERT_TRUE(got.has_value()) << days;
    EXPECT_EQ(tdates(*got), tdates({d(2024, 1, 1), d(2024, 6, 1)})) << days;
  }
  // This is also why detail::advance_multiple's range check is defensive
  // rather than load-bearing: reaching |count| >= 2 requires the count == 1
  // step to land inside the span, which bounds `periods` by the span length,
  // so periods * count cannot overflow while periods > 0. The check stays
  // because DateInterval is an aggregate and nothing stops a future caller
  // from reaching make_regular_date_list by another route -- but no input
  // that passes the precondition can trigger it.
}

TEST(Schedule, MatchesRecordedVectors) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kScheduleCases) {
    const DateInterval iv{c.periods, c.unit == 'M' ? PeriodUnit::Months : PeriodUnit::Days};
    const StubMethod stub{c.position == 1 ? StubPosition::Back : StubPosition::Front,
                          c.length == 1 ? StubLength::Long : StubLength::Short};
    const auto got = make_regular_date_list(cdsmodel::from_tdate(c.start),
                                            cdsmodel::from_tdate(c.end), iv, stub);
    ASSERT_EQ(got.has_value(), c.n != 0) << c.start << ".." << c.end << " " << c.periods << c.unit;
    if (c.n == 0) {
      continue;
    }
    ASSERT_EQ(got->size(), c.n) << c.start << ".." << c.end << " " << c.periods << c.unit;
    for (std::size_t i = 0; i < c.n; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i]), c.dates[i])
          << "case " << c.start << ".." << c.end << " " << c.periods << c.unit << " at " << i;
    }
    ++checked;
  }
  EXPECT_GT(checked, 300) << "recorded schedule vectors thinner than expected: " << checked;
}

} // namespace
