// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include "golden/calendar_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<cdsmodel::Date> recorded_holidays() {
  std::vector<cdsmodel::Date> v;
  for (long t : cdsmodel::golden::kCalendarHolidays) {
    v.push_back(cdsmodel::from_tdate(t));
  }
  return v;
}

TEST(CalendarGolden, PredicatesMatchRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kCalendarDayCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value()) << "recorded an unrepresentable weekend set " << c.weekends;
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());
    const cdsmodel::Date day = cdsmodel::from_tdate(c.tdate);

    EXPECT_EQ(cal->is_weekend(day), c.is_weekend)
        << "is_weekend, weekends=" << c.weekends << " tdate=" << c.tdate;
    EXPECT_EQ(cal->is_holiday(day), c.is_holiday)
        << "is_holiday, weekends=" << c.weekends << " tdate=" << c.tdate;
    EXPECT_EQ(cal->is_business_day(day), c.is_business_day)
        << "is_business_day, weekends=" << c.weekends << " tdate=" << c.tdate;
  }
}

TEST(CalendarGolden, StoredHolidaysMatchRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kCalendarStoredCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value());
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());

    ASSERT_EQ(static_cast<int>(cal->holidays().size()), c.count)
        << "kept count, weekends=" << c.weekends;
    for (int i = 0; i < c.count; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(cal->holidays()[static_cast<std::size_t>(i)]),
                c.kept[static_cast<std::size_t>(i)])
          << "kept[" << i << "], weekends=" << c.weekends;
    }
  }
}

TEST(CalendarGolden, VectorSetsAreNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kCalendarDayCases.size(), 2000u);
  EXPECT_EQ(cdsmodel::golden::kCalendarStoredCases.size(), cdsmodel::golden::kWeekendSets.size());
}

// --- port semantics needing no oracle ------------------------------------

TEST(WeekendMask, BitLayoutMatchesUpstreamFlags) {
  // Upstream: MONDAY 0x01 .. SUNDAY 0x40, selected by 1 << (tdate % 7).
  // The port selects by iso_encoding() - 1. These must coincide.
  static_assert(cdsmodel::to_upstream_weekends(cdsmodel::Weekend::Monday) == 0x01);
  static_assert(cdsmodel::to_upstream_weekends(cdsmodel::Weekend::Saturday) == 0x20);
  static_assert(cdsmodel::to_upstream_weekends(cdsmodel::Weekend::Sunday) == 0x40);
  static_assert(cdsmodel::to_upstream_weekends(cdsmodel::Weekend::Standard) == 0x60);
  static_assert(cdsmodel::to_upstream_weekends(cdsmodel::Weekend::None) == 0x00);

  static_assert(cdsmodel::includes(cdsmodel::Weekend::Standard, Saturday));
  static_assert(cdsmodel::includes(cdsmodel::Weekend::Standard, Sunday));
  static_assert(!cdsmodel::includes(cdsmodel::Weekend::Standard, Monday));
  static_assert(!cdsmodel::includes(cdsmodel::Weekend::None, Sunday));
  SUCCEED();
}

TEST(WeekendMask, TheEpochIsAMondayWhichIsWhyTheLayoutWorks) {
  // Upstream's `1 << (tdate % 7)` only lines up with Monday-is-bit-0 because
  // TDate 0 is a Monday. If that ever stopped being true the mask would be
  // silently rotated.
  EXPECT_EQ(weekday{cdsmodel::kDateEpoch}, Monday);
  EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::kDateEpoch) % 7, 0);
}

TEST(WeekendMask, RejectsFlagsOutsideTheSevenBits) {
  EXPECT_FALSE(cdsmodel::weekend_from_upstream(-1).has_value());
  EXPECT_FALSE(cdsmodel::weekend_from_upstream(0x80).has_value());
  EXPECT_FALSE(cdsmodel::weekend_from_upstream(1000).has_value());
  EXPECT_TRUE(cdsmodel::weekend_from_upstream(0x7F).has_value());
}

TEST(CalendarConstruction, RejectsNonStrictlyIncreasingHolidays) {
  const cdsmodel::Weekend w = cdsmodel::Weekend::Standard;
  EXPECT_TRUE(cdsmodel::Calendar::create({}, w).has_value());

  const std::vector<cdsmodel::Date> decreasing{d(2024, 3, 13), d(2024, 3, 12)};
  EXPECT_FALSE(cdsmodel::Calendar::create(decreasing, w).has_value());

  const std::vector<cdsmodel::Date> duplicated{d(2024, 3, 12), d(2024, 3, 12)};
  EXPECT_FALSE(cdsmodel::Calendar::create(duplicated, w).has_value());

  const std::vector<cdsmodel::Date> ok{d(2024, 3, 12), d(2024, 3, 13)};
  EXPECT_TRUE(cdsmodel::Calendar::create(ok, w).has_value());
}

TEST(CalendarConstruction, RejectsTheEpochDayAsAHoliday) {
  // Upstream's check starts at lastDate = 0 and tests `thisDate <= lastDate`,
  // so TDate 0 can never be a holiday.
  EXPECT_FALSE(
      cdsmodel::Calendar::create(std::vector{cdsmodel::kDateEpoch}, cdsmodel::Weekend::Standard)
          .has_value());
  EXPECT_TRUE(cdsmodel::Calendar::create(std::vector{cdsmodel::kDateEpoch + days{1}},
                                         cdsmodel::Weekend::Standard)
                  .has_value());
}

TEST(CalendarConstruction, DropsHolidaysFallingOnWeekends) {
  // 2024-03-16 Sat, 2024-03-17 Sun.
  const std::vector<cdsmodel::Date> holidays{d(2024, 3, 15), d(2024, 3, 16), d(2024, 3, 17),
                                             d(2024, 3, 18)};
  const auto standard = cdsmodel::Calendar::create(holidays, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(standard.has_value());
  EXPECT_EQ(standard->holidays().size(), 2u) << "Sat and Sun entries should be stripped";
  // And the stripped ones report as not-a-holiday, even though supplied.
  EXPECT_FALSE(standard->is_holiday(d(2024, 3, 16)));
  EXPECT_FALSE(standard->is_holiday(d(2024, 3, 17)));
  // They are still non-business days -- because they are weekends.
  EXPECT_FALSE(standard->is_business_day(d(2024, 3, 16)));

  // With no weekends, nothing is stripped.
  const auto all = cdsmodel::Calendar::create(holidays, cdsmodel::Weekend::None);
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->holidays().size(), 4u);
  EXPECT_TRUE(all->is_holiday(d(2024, 3, 16)));
}

TEST(CalendarBuiltIns, NoneHasStandardWeekendsDespiteItsName) {
  const cdsmodel::Calendar none = cdsmodel::Calendar::none();
  EXPECT_TRUE(none.holidays().empty());
  EXPECT_FALSE(none.is_business_day(d(2024, 6, 1))); // a Saturday
  EXPECT_TRUE(none.is_business_day(d(2024, 6, 3)));  // a Monday
}

TEST(CalendarBuiltIns, NoWeekendsMakesEveryDayABusinessDay) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::no_weekends();
  for (int i = 0; i < 14; ++i) {
    EXPECT_TRUE(cal.is_business_day(d(2024, 6, 1) + days{i})) << "offset " << i;
  }
}

TEST(CalendarValue, IsCopyableAndIndependent) {
  // The point of the port: a calendar is a value, not a cache entry. Two
  // callers cannot interfere, and there is nothing to invalidate.
  const auto a =
      cdsmodel::Calendar::create(std::vector{d(2024, 7, 4)}, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(a.has_value());
  cdsmodel::Calendar b = *a;
  EXPECT_EQ(b.holidays().size(), a->holidays().size());
  EXPECT_FALSE(b.is_business_day(d(2024, 7, 4)));
  EXPECT_FALSE(a->is_business_day(d(2024, 7, 4)));
}

} // namespace
