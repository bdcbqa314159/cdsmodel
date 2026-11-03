// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include "golden/business_days_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<cdsmodel::Date> recorded_holidays() {
  std::vector<cdsmodel::Date> v;
  for (long t : cdsmodel::golden::kBusDayHolidays) {
    v.push_back(cdsmodel::from_tdate(t));
  }
  return v;
}

TEST(BusinessDaysGolden, MatchesRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kBusDayCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value());
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());

    const auto got = cdsmodel::business_days_between(cdsmodel::from_tdate(c.from),
                                                     cdsmodel::from_tdate(c.to), *cal);
    ASSERT_EQ(got.has_value(), c.ok) << "validity, weekends=" << c.weekends;
    if (c.ok) {
      EXPECT_EQ(*got, c.days) << "value, weekends=" << c.weekends << " from=" << c.from
                              << " to=" << c.to;
    }
  }
}

TEST(BusinessDaysGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kBusDayCases.size(), 600u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(BusinessDaysDiff, SameDateIsZero) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  for (int i = 0; i < 14; ++i) {
    const cdsmodel::Date day = d(2024, 6, 1) + days{i};
    EXPECT_EQ(cdsmodel::business_days_between(day, day, cal), 0L) << "offset " << i;
  }
}

TEST(BusinessDaysDiff, AFullWeekIsFiveUnderStandardWeekends) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const cdsmodel::Date monday = d(2024, 6, 3);
  EXPECT_EQ(cdsmodel::business_days_between(monday, monday + days{7}, cal), 5L);
  EXPECT_EQ(cdsmodel::business_days_between(monday, monday + days{14}, cal), 10L);
  EXPECT_EQ(cdsmodel::business_days_between(monday, monday - days{7}, cal), -5L);
}

TEST(BusinessDaysDiff, NoWeekendsCountsCalendarDays) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::no_weekends();
  EXPECT_EQ(cdsmodel::business_days_between(d(2024, 6, 1), d(2024, 6, 30), cal), 29L);
  EXPECT_EQ(cdsmodel::business_days_between(d(2024, 6, 30), d(2024, 6, 1), cal), -29L);
}

TEST(BusinessDaysDiff, IsNotAntisymmetric) {
  // Upstream's forward and backward tables are deliberately not negatives of
  // each other. This is PRESERVED behaviour, not a bug in the port, and the
  // adversarial harness must not assume antisymmetry here the way it does for
  // the day count conventions.
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  int asymmetric = 0;
  for (int i = 0; i < 28; ++i) {
    for (int j = 0; j < 28; ++j) {
      const cdsmodel::Date a = d(2024, 6, 1) + days{i};
      const cdsmodel::Date b = d(2024, 6, 1) + days{j};
      const long fwd = *cdsmodel::business_days_between(a, b, cal);
      const long rev = *cdsmodel::business_days_between(b, a, cal);
      if (fwd != -rev) {
        ++asymmetric;
      }
    }
  }
  EXPECT_GT(asymmetric, 0) << "business_days_between became antisymmetric, which means the "
                              "transcribed tables were changed";
}

TEST(BusinessDaysDiff, HolidaysReduceTheCountInBothDirections) {
  const auto cal =
      cdsmodel::Calendar::create(std::vector{d(2024, 6, 5)}, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(cal.has_value());
  const cdsmodel::Calendar plain = cdsmodel::Calendar::none();
  const cdsmodel::Date monday = d(2024, 6, 3);
  const cdsmodel::Date friday = d(2024, 6, 7);

  EXPECT_EQ(*cdsmodel::business_days_between(monday, friday, *cal),
            *cdsmodel::business_days_between(monday, friday, plain) - 1);
  EXPECT_EQ(*cdsmodel::business_days_between(friday, monday, *cal),
            *cdsmodel::business_days_between(friday, monday, plain) + 1);
}

TEST(BusinessDaysDiff, RejectsAnAllWeekendCalendarlessSpan) {
  // With no business weekday the count is still well defined (zero business
  // days), unlike adjustment which cannot terminate. Assert we do not
  // accidentally reject it.
  const auto cal = cdsmodel::Calendar::create({}, *cdsmodel::weekend_from_upstream(0x7F));
  ASSERT_TRUE(cal.has_value());
  const auto got = cdsmodel::business_days_between(d(2024, 6, 3), d(2024, 6, 10), *cal);
  ASSERT_TRUE(got.has_value()) << "counting does not need a business day to exist";
  EXPECT_EQ(*got, 0L);
}

} // namespace
