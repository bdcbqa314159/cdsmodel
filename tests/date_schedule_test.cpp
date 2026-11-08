// TIER 1 -- schedule generation, without an oracle.
//
// These functions share a grid and differ only at the ends, so the properties
// are written to separate those two things: first that the grid is regular and
// strictly increasing, then that each function puts the boundary dates where
// its contract says.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_schedule.hpp>

#include <gtest/gtest.h>

#include "golden/date_schedule_cases.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::DateInterval;
using cdsmodel::PeriodUnit;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<DateInterval> intervals() {
  return {{1, PeriodUnit::Months},  {2, PeriodUnit::Months},  {3, PeriodUnit::Months},
          {4, PeriodUnit::Months},  {6, PeriodUnit::Months},  {7, PeriodUnit::Months},
          {12, PeriodUnit::Months}, {13, PeriodUnit::Months}, {30, PeriodUnit::Days},
          {91, PeriodUnit::Days}};
}

std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> spans() {
  return {{d(2024, 1, 15), d(2027, 1, 15)}, {d(2024, 1, 31), d(2027, 1, 31)},
          {d(2024, 2, 29), d(2027, 2, 28)}, {d(2024, 1, 15), d(2024, 4, 15)},
          {d(2024, 3, 1), d(2029, 11, 30)}, {d(2024, 6, 30), d(2026, 12, 31)}};
}

// =====================================================================
// The grid.
// =====================================================================

TEST(DateSchedule, EverySchedulePlacesTheBoundaryDatesWhereItsContractSays) {
  int checked = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " iv " +
                                  std::to_string(iv.periods) + " stubAtEnd " +
                                  (stub_at_end ? "1" : "0");

        // date_list: both boundaries present, and they are the ends.
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> plain =
            cdsmodel::date_list(start, maturity, iv, stub_at_end);
        ASSERT_TRUE(plain.has_value()) << where;
        ASSERT_FALSE(plain->empty()) << where;
        EXPECT_EQ(plain->front(), start) << where << ": date_list must begin at the start";
        EXPECT_EQ(plain->back(), maturity) << where << ": and end at the maturity";
        EXPECT_TRUE(std::ranges::is_sorted(*plain, std::ranges::less{}))
            << where << ": and be strictly increasing";
        EXPECT_EQ(std::ranges::adjacent_find(*plain), plain->end())
            << where << ": with no repeated date";

        // date_list_extended: the far boundary is pushed OFF the grid when
        // there is a stub, and present when there is not.
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> extended =
            cdsmodel::date_list_extended(start, maturity, iv, stub_at_end);
        ASSERT_TRUE(extended.has_value()) << where;
        ASSERT_FALSE(extended->empty()) << where;
        if (stub_at_end) {
          EXPECT_EQ(extended->front(), start)
              << where << ": a stub at the end leaves the start on the grid";
          EXPECT_GE(extended->back(), maturity)
              << where << ": and carries the list to the maturity or past it";
        } else {
          EXPECT_EQ(extended->back(), maturity)
              << where << ": a stub at the start leaves the maturity on the grid";
          EXPECT_LE(extended->front(), start) << where << ": and begins at the start or before it";
        }
        EXPECT_TRUE(std::ranges::is_sorted(*extended, std::ranges::less{})) << where;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(DateSchedule, TheTwoGeneratorsDifferOnlyAtTheStubBoundary) {
  // The first version of this asserted that with no stub the two generators
  // are identical, on the reasoning that there is nothing for "extended" to
  // move. That is wrong, and the leap day shows why.
  //
  // 2024-02-29 to 2027-02-28 counts as an integral 36 months, so there is no
  // stub. But stepping BACK 36 months from 2027-02-28 lands on 2024-02-28,
  // not the 29th: month arithmetic clamps, and clamping is not invertible.
  // So `date_list`, which overwrites the boundary with the requested start,
  // gives 2024-02-29, while `date_list_extended`, which stays on the grid,
  // gives 2024-02-28.
  //
  // Both are right. The property worth asserting is the narrower one: they
  // differ at most in the single boundary element on the stub side, and
  // `date_list`'s boundary is always exactly what the caller asked for.
  int checked = 0;
  int boundary_differences = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        const cdsmodel::Expected<cdsmodel::DateCount> span =
            cdsmodel::count_dates(start, maturity, iv);
        if (!span || span->extra_days != 0) {
          continue; // with a stub the lists are genuinely different lengths
        }
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> plain =
            cdsmodel::date_list(start, maturity, iv, stub_at_end);
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> extended =
            cdsmodel::date_list_extended(start, maturity, iv, stub_at_end);
        ASSERT_TRUE(plain.has_value());
        ASSERT_TRUE(extended.has_value());
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " iv " +
                                  std::to_string(iv.periods) + " stubAtEnd " +
                                  (stub_at_end ? "1" : "0");
        ASSERT_EQ(plain->size(), extended->size()) << where << ": no stub, so the same length";

        // The boundary on the stub side is the only element allowed to differ.
        const std::size_t boundary = stub_at_end ? plain->size() - 1 : 0;
        for (std::size_t i = 0; i < plain->size(); ++i) {
          if (i == boundary) {
            continue;
          }
          EXPECT_EQ((*plain)[i], (*extended)[i]) << where << " i=" << i;
        }
        if ((*plain)[boundary] != (*extended)[boundary]) {
          ++boundary_differences;
        }
        // And date_list's boundaries are always exactly what was asked for.
        EXPECT_EQ(plain->front(), start) << where;
        EXPECT_EQ(plain->back(), maturity) << where;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 20) << checked;
  EXPECT_GT(boundary_differences, 0)
      << "if the grid were invertible the two would always agree, and this test "
         "would be asserting nothing";
}

TEST(DateSchedule, PayDatesIsTheScheduleWithoutItsFirstDate) {
  int checked = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> full =
            cdsmodel::date_list(start, maturity, iv, stub_at_end);
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> pay =
            cdsmodel::pay_dates(start, maturity, iv, stub_at_end);
        ASSERT_TRUE(full.has_value());
        ASSERT_TRUE(pay.has_value());
        ASSERT_FALSE(full->empty());
        EXPECT_EQ(pay->size(), full->size() - 1);
        EXPECT_TRUE(std::ranges::equal(*pay, std::span{*full}.subspan(1)))
            << cdsmodel::to_tdate(start) << " iv " << iv.periods;
        // The start date is gone, and the maturity is still there.
        EXPECT_TRUE(pay->empty() || pay->front() != start);
        EXPECT_TRUE(pay->empty() || pay->back() == maturity);
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(DateSchedule, RemovingTheStartFromAOneDateScheduleLeavesNothing) {
  // start == maturity gives a single-date list, and taking the start out of it
  // leaves an empty schedule rather than an error. Upstream decrements a count
  // to zero and returns the list; a vector erases to empty.
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> one =
      cdsmodel::date_list(d(2024, 1, 15), d(2024, 1, 15), {3, PeriodUnit::Months}, true);
  ASSERT_TRUE(one.has_value());
  ASSERT_EQ(one->size(), 1u);
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> none =
      cdsmodel::pay_dates(d(2024, 1, 15), d(2024, 1, 15), {3, PeriodUnit::Months}, true);
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->empty());
}

TEST(DateSchedule, NoRollDateIsPlainExtendedGeneration) {
  for (const auto &[start, maturity] : spans()) {
    for (const DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> rolled =
            cdsmodel::date_list_extended_roll(start, maturity, std::nullopt, iv, stub_at_end);
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> plain =
            cdsmodel::date_list_extended(start, maturity, iv, stub_at_end);
        ASSERT_EQ(rolled.has_value(), plain.has_value());
        if (rolled) {
          EXPECT_EQ(*rolled, *plain) << cdsmodel::to_tdate(start) << " iv " << iv.periods;
        }
      }
    }
  }
}

TEST(DateSchedule, AStubAtTheEndMakesTheRollDateIrrelevant) {
  // Upstream's very first branch: `rollDate == 0 || stubAtEnd == TRUE`. So a
  // stub at the end short-circuits before the roll logic is reached at all,
  // whatever roll date is supplied.
  for (const auto &[start, maturity] : spans()) {
    for (const DateInterval iv : intervals()) {
      const cdsmodel::Expected<std::vector<cdsmodel::Date>> plain =
          cdsmodel::date_list_extended(start, maturity, iv, true);
      for (const cdsmodel::Date roll : {d(2020, 3, 31), d(2023, 7, 1), start}) {
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> rolled =
            cdsmodel::date_list_extended_roll(start, maturity, roll, iv, true);
        ASSERT_EQ(rolled.has_value(), plain.has_value());
        if (rolled) {
          EXPECT_EQ(*rolled, *plain) << cdsmodel::to_tdate(start) << " iv " << iv.periods;
        }
      }
    }
  }
}

TEST(DateSchedule, ARollDateAfterTheStartIsRefused) {
  // Upstream: "Start date cannot be before roll date." Only reachable on an
  // integral span, since otherwise the roll date is dropped first --
  // UPSTREAM-24.
  const cdsmodel::Date start = d(2024, 1, 15);
  const cdsmodel::Date maturity = d(2027, 1, 15);
  const DateInterval iv{3, PeriodUnit::Months};
  ASSERT_EQ(cdsmodel::count_dates(start, maturity, iv)->extra_days, 0) << "the premise";
  EXPECT_FALSE(
      cdsmodel::date_list_extended_roll(start, maturity, d(2024, 6, 1), iv, false).has_value());
  EXPECT_TRUE(
      cdsmodel::date_list_extended_roll(start, maturity, d(2023, 6, 1), iv, false).has_value());
  EXPECT_TRUE(cdsmodel::date_list_extended_roll(start, maturity, start, iv, false).has_value());
}

// =====================================================================
// Business day adjustment.
// =====================================================================

TEST(DateSchedule, AdjustmentLandsEveryDateOnABusinessDayAndIsIdempotent) {
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> schedule =
      cdsmodel::date_list(d(2024, 1, 6), d(2027, 1, 6), {1, PeriodUnit::Months}, true);
  ASSERT_TRUE(schedule.has_value());
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified}) {
    const cdsmodel::Expected<std::vector<cdsmodel::Date>> adjusted =
        cdsmodel::adjust_date_list(*schedule, conv, calendars);
    ASSERT_TRUE(adjusted.has_value()) << static_cast<char>(conv);
    ASSERT_EQ(adjusted->size(), schedule->size());
    for (const cdsmodel::Date date : *adjusted) {
      EXPECT_TRUE(calendars.front().is_business_day(date))
          << static_cast<char>(conv) << " left " << cdsmodel::to_tdate(date)
          << " off a business day";
    }
    // Adjusting again must change nothing: the result is already adjusted.
    const cdsmodel::Expected<std::vector<cdsmodel::Date>> again =
        cdsmodel::adjust_date_list(*adjusted, conv, calendars);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, *adjusted) << static_cast<char>(conv) << " is not idempotent";
    // Non-decreasing, but NOT necessarily strictly increasing -- adjustment
    // can collapse two dates onto one business day, and upstream keeps the
    // duplicate rather than coalescing it.
    EXPECT_TRUE(std::ranges::is_sorted(*adjusted)) << static_cast<char>(conv);
  }

  // None leaves the schedule exactly as it was, business day or not.
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> untouched =
      cdsmodel::adjust_date_list(*schedule, cdsmodel::BadDayConv::None, calendars);
  ASSERT_TRUE(untouched.has_value());
  EXPECT_EQ(*untouched, *schedule);

  // And an empty schedule adjusts to an empty schedule.
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> nothing = cdsmodel::adjust_date_list(
      std::span<const cdsmodel::Date>{}, cdsmodel::BadDayConv::Follow, calendars);
  ASSERT_TRUE(nothing.has_value());
  EXPECT_TRUE(nothing->empty());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(DateSchedule, MatchesRecordedSchedules) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kDateScheduleCases) {
    const DateInterval iv{c.periods, c.unit == 'M' ? PeriodUnit::Months : PeriodUnit::Days};
    const cdsmodel::Date start = cdsmodel::from_tdate(c.start);
    const cdsmodel::Date maturity = cdsmodel::from_tdate(c.maturity);
    cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
        cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "no case ran");
    switch (c.which) {
    case 0:
      got = cdsmodel::date_list(start, maturity, iv, c.stub_at_end);
      break;
    case 1:
      got = cdsmodel::date_list_extended(start, maturity, iv, c.stub_at_end);
      break;
    case 2:
      got = cdsmodel::pay_dates(start, maturity, iv, c.stub_at_end);
      break;
    default: {
      // Upstream's roll sentinel is TDate 0; the port's is nullopt.
      // The roll date is a genuine optional PARAMETER -- upstream's sentinel
      // is TDate 0, the port's is nullopt. Not an error channel.
      const std::optional<cdsmodel::Date> roll =
          c.roll == 0 ? std::nullopt : std::optional{cdsmodel::from_tdate(c.roll)};
      got = cdsmodel::date_list_extended_roll(start, maturity, roll, iv, c.stub_at_end);
      break;
    }
    }
    const std::string where = "which " + std::to_string(c.which) + " start " +
                              std::to_string(c.start) + " iv " + std::to_string(c.periods) +
                              c.unit + " stubAtEnd " + (c.stub_at_end ? "1" : "0") + " roll " +
                              std::to_string(c.roll);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (!got) {
      continue;
    }
    ASSERT_EQ(got->size(), c.n) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      // Exact. Date arithmetic, so a near-miss would be a real difference.
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i]), c.dates[i]) << where << " i=" << i;
    }
    ++checked;
  }
  EXPECT_GT(checked, 1000) << checked;
}

} // namespace
