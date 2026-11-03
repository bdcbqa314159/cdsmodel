// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include "golden/business_days_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
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

TEST(BusinessDaysGolden, OffsetsMatchRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kBusDayOffsetCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value());
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());
    const auto got = cdsmodel::add_business_days(cdsmodel::from_tdate(c.from), c.count, *cal);
    ASSERT_EQ(got.has_value(), c.ok) << "validity, from=" << c.from << " n=" << c.count;
    if (c.ok) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.result)
          << "value, weekends=" << c.weekends << " from=" << c.from << " n=" << c.count;
    }
  }
}

TEST(BusinessDaysGolden, BusinessEndOfMonthMatchesRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kBusDayEomCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value());
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());
    const auto got = cdsmodel::business_end_of_month(cdsmodel::from_tdate(c.tdate), *cal);
    ASSERT_EQ(got.has_value(), c.ok) << "validity, tdate=" << c.tdate;
    if (c.ok) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.eom)
          << "value, weekends=" << c.weekends << " tdate=" << c.tdate;
    }
  }
}

TEST(BusinessDaysOffset, ZeroLeavesEvenANonBusinessDayAlone) {
  // Not the same as adjusting onto a business day: upstream returns the input.
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const cdsmodel::Date saturday = d(2024, 6, 1);
  ASSERT_EQ(weekday{saturday}, Saturday);
  EXPECT_EQ(cdsmodel::add_business_days(saturday, 0, cal), saturday);
}

TEST(BusinessDaysOffset, FiveBusinessDaysIsOneWeekFromAWeekday) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const cdsmodel::Date monday = d(2024, 6, 3);
  EXPECT_EQ(cdsmodel::add_business_days(monday, 5, cal), monday + days{7});
  EXPECT_EQ(cdsmodel::add_business_days(monday, -5, cal), monday - days{7});
  EXPECT_EQ(cdsmodel::add_business_days(monday, 1, cal), d(2024, 6, 4));
  // Friday + 1 skips the weekend.
  EXPECT_EQ(cdsmodel::add_business_days(d(2024, 6, 7), 1, cal), d(2024, 6, 10));
}

TEST(BusinessDaysOffset, RoundTripsWithCounting) {
  // Moving n business days then counting back should give n. This holds even
  // though business_days_between is not antisymmetric, because both are
  // evaluated in the same direction.
  const auto cal = cdsmodel::Calendar::create(std::vector{d(2024, 7, 4), d(2024, 7, 19)},
                                              cdsmodel::Weekend::Standard);
  ASSERT_TRUE(cal.has_value());
  for (int start = 0; start < 28; ++start) {
    const cdsmodel::Date from = d(2024, 7, 1) + days{start};
    for (long n : {1L, 3L, 7L, 15L}) {
      const auto to = cdsmodel::add_business_days(from, n, *cal);
      ASSERT_TRUE(to.has_value());
      EXPECT_EQ(*cdsmodel::business_days_between(from, *to, *cal), n)
          << "start offset " << start << " n=" << n;
    }
  }
}

TEST(BusinessDaysOffset, DeclinesWhenNoBusinessDayExists) {
  const auto cal = cdsmodel::Calendar::create({}, *cdsmodel::weekend_from_upstream(0x7F));
  ASSERT_TRUE(cal.has_value());
  EXPECT_FALSE(cdsmodel::add_business_days(d(2024, 6, 3), 1, *cal).has_value());
  // Zero still returns, since it never walks.
  EXPECT_EQ(cdsmodel::add_business_days(d(2024, 6, 3), 0, *cal), d(2024, 6, 3));
}

TEST(BusinessDaysOffset, DeclinesAnAbsurdCountInsteadOfWalkingForever) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  EXPECT_FALSE(cdsmodel::add_business_days(d(2024, 6, 3), std::numeric_limits<long>::max(), cal)
                   .has_value());
  EXPECT_FALSE(cdsmodel::add_business_days(d(2024, 6, 3), std::numeric_limits<long>::min(), cal)
                   .has_value());
}

TEST(BusinessDaysOffset, WorksNearBothEndsOfTheRepresentableRange) {
  // PORT-8: the "room available" check computed `to_tdate(from) - kMinTDate`
  // in a signed type, which overflows wherever kMinTDate sits near the
  // minimum of std::chrono::days::rep -- 64-bit on libstdc++, 32-bit on
  // libc++. The effect was that ordinary BACKWARD offsets were rejected, and
  // only the Linux leg of CI noticed.
  //
  // This exercises both directions from dates spread across the range, so the
  // room arithmetic is covered wherever the bounds happen to land.
  const cdsmodel::Calendar cal = cdsmodel::Calendar::no_weekends();
  for (cdsmodel::TDate t : {cdsmodel::TDate{1000}, cdsmodel::TDate{148655},
                            cdsmodel::TDate{1000000}, cdsmodel::kMaxTDate / 2}) {
    const cdsmodel::Date from = cdsmodel::from_tdate(t);
    for (long n : {-100L, -1L, 1L, 100L}) {
      const auto got = cdsmodel::add_business_days(from, n, cal);
      ASSERT_TRUE(got.has_value()) << "rejected an ordinary offset at tdate " << t << " n=" << n;
      // With no weekends and no holidays, n business days is n calendar days.
      EXPECT_EQ(cdsmodel::to_tdate(*got), t + n) << "tdate " << t << " n=" << n;
    }
  }
}

TEST(BusinessDaysOffset, RejectsOffsetsBeyondTheStatedLimit) {
  // The limit is stated rather than left to the date range, because on a
  // 64-bit days::rep a count of 2^62 is "representable" and would spin.
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  EXPECT_TRUE(cdsmodel::add_business_days(d(2024, 6, 3), 1000, cal).has_value());
  EXPECT_FALSE(cdsmodel::add_business_days(d(2024, 6, 3), cdsmodel::kMaxBusinessDayOffset + 1, cal)
                   .has_value());
  EXPECT_FALSE(
      cdsmodel::add_business_days(d(2024, 6, 3), -(cdsmodel::kMaxBusinessDayOffset + 1), cal)
          .has_value());
}

TEST(BusinessDaysEom, LandsOnTheLastBusinessDayOfTheSameMonth) {
  // June 2024 ends on Sunday the 30th, so the business EOM is Friday the 28th.
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const auto got = cdsmodel::business_end_of_month(d(2024, 6, 15), cal);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, d(2024, 6, 28));
  EXPECT_EQ(cdsmodel::to_ymd(*got).month(), cdsmodel::to_ymd(d(2024, 6, 15)).month());
  EXPECT_TRUE(cal.is_business_day(*got));

  // A holiday on the last business day pushes it back further.
  const auto with_holiday =
      cdsmodel::Calendar::create(std::vector{d(2024, 6, 28)}, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(with_holiday.has_value());
  EXPECT_EQ(cdsmodel::business_end_of_month(d(2024, 6, 15), *with_holiday), d(2024, 6, 27));
}

} // namespace
