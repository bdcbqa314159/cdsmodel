// TIER 1 -- interval conversions, multiplied offsets, date counting and
// adjusted offsets. Properties first, then replay.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>

#include <gtest/gtest.h>

#include "golden/date_sup_cases.hpp"

#include <cmath>
#include <optional>
#include <string>

namespace {

using cdsmodel::Date;
using cdsmodel::DateInterval;
using cdsmodel::PeriodUnit;

Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::Calendar weekends() {
  return *cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard);
}

TEST(DateSup, IntervalToYearsAgreesWithTheConventionalValues) {
  const auto years = [](int n, char c) {
    return cdsmodel::interval_to_years(*cdsmodel::make_date_interval(n, c));
  };
  EXPECT_DOUBLE_EQ(years(1, 'A'), 1.0);
  EXPECT_DOUBLE_EQ(years(1, 'Y'), 1.0);
  EXPECT_DOUBLE_EQ(years(1, 'S'), 0.5);
  EXPECT_DOUBLE_EQ(years(1, 'Q'), 0.25);
  EXPECT_DOUBLE_EQ(years(1, 'M'), 1.0 / 12.0);
  EXPECT_DOUBLE_EQ(years(1, 'W'), 7.0 / 365.0);
  EXPECT_DOUBLE_EQ(years(1, 'D'), 1.0 / 365.0);
  EXPECT_DOUBLE_EQ(years(1, 'U'), 28.0 / 365.0);
  // Linear in the period count, and a negative interval gives negative years.
  EXPECT_DOUBLE_EQ(years(4, 'Q'), 1.0);
  EXPECT_DOUBLE_EQ(years(-2, 'S'), -1.0);
}

TEST(DateSup, FrequencyRoundTripsWhereItDividesTwelve) {
  for (long freq : {1, 2, 3, 4, 6, 12}) {
    const std::optional<DateInterval> iv = cdsmodel::frequency_to_interval(freq);
    ASSERT_TRUE(iv.has_value()) << freq;
    const std::optional<double> back = cdsmodel::interval_to_frequency(*iv);
    ASSERT_TRUE(back.has_value()) << freq;
    EXPECT_DOUBLE_EQ(*back, static_cast<double>(freq)) << freq;
  }
  // And does not where it cannot: 12/5 truncates to 2, giving 6 per year.
  const std::optional<DateInterval> five = cdsmodel::frequency_to_interval(5);
  ASSERT_TRUE(five.has_value());
  EXPECT_DOUBLE_EQ(*cdsmodel::interval_to_frequency(*five), 6.0);
}

TEST(DateSup, FrequencyRejectsOutsideOneToTwelve) {
  for (long freq : {-12, -1, 0, 13, 24, 365}) {
    EXPECT_FALSE(cdsmodel::frequency_to_interval(freq).has_value()) << freq;
  }
}

TEST(DateSup, IntervalToFrequencyRejectsNonPositiveIntervals) {
  EXPECT_FALSE(cdsmodel::interval_to_frequency(DateInterval{0, PeriodUnit::Months}).has_value());
  EXPECT_FALSE(cdsmodel::interval_to_frequency(DateInterval{0, PeriodUnit::Days}).has_value());
  EXPECT_FALSE(cdsmodel::interval_to_frequency(DateInterval{-3, PeriodUnit::Months}).has_value());
}

TEST(DateSup, OffsetIsAnchoredNotStepped) {
  // The property upstream's own comment insists on: advancing by n intervals
  // in one go is NOT the same as stepping n times, because month arithmetic
  // clamps. 31 January stepped monthly sticks at the 28th; anchored
  // arithmetic recovers the 31st.
  const DateInterval month{1, PeriodUnit::Months};
  const Date start = d(2024, 1, 31);
  EXPECT_EQ(cdsmodel::date_from_date_and_offset(start, month, 2), d(2024, 3, 31));

  Date stepped = start;
  for (int i = 0; i < 2; ++i) {
    stepped = *cdsmodel::advance(stepped, month);
  }
  EXPECT_EQ(stepped, d(2024, 3, 29)) << "the stepping path should drift, or this test is moot";
  EXPECT_NE(stepped, *cdsmodel::date_from_date_and_offset(start, month, 2));
}

TEST(DateSup, OffsetAtZeroIsIdentityAndNegativeGoesBack) {
  const DateInterval quarter{3, PeriodUnit::Months};
  const Date start = d(2024, 6, 20);
  EXPECT_EQ(cdsmodel::date_from_date_and_offset(start, quarter, 0), start);
  EXPECT_EQ(cdsmodel::date_from_date_and_offset(start, quarter, 1), d(2024, 9, 20));
  EXPECT_EQ(cdsmodel::date_from_date_and_offset(start, quarter, -1), d(2024, 3, 20));
}

TEST(DateSup, MakeDateArrayIsTheOffsetSequence) {
  const DateInterval month{1, PeriodUnit::Months};
  const std::optional<std::vector<Date>> got =
      cdsmodel::make_date_array(d(2024, 1, 31), month, 2, 4);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ((*got)[static_cast<std::size_t>(i)],
              *cdsmodel::date_from_date_and_offset(d(2024, 1, 31), month, 2 + i))
        << "i=" << i;
  }
  // Zero dates is legal and empty; a negative count is not.
  EXPECT_TRUE(cdsmodel::make_date_array(d(2024, 1, 31), month, 0, 0)->empty());
  EXPECT_FALSE(cdsmodel::make_date_array(d(2024, 1, 31), month, 0, -1).has_value());
}

TEST(DateSup, CountDatesIsConsistentWithSteppingTheSameInterval) {
  // An independent check on the estimate-then-walk algorithm: the reported
  // count must be the largest n with `from + n*interval` still inside the
  // span, and the extra days must be the gap left over.
  const DateInterval quarter{3, PeriodUnit::Months};
  const Date from = d(2024, 3, 20);
  for (Date to : {d(2024, 6, 20), d(2025, 1, 1), d(2029, 6, 20), d(2031, 12, 20)}) {
    const std::optional<cdsmodel::DateCount> got = cdsmodel::count_dates(from, to, quarter);
    ASSERT_TRUE(got.has_value()) << cdsmodel::to_tdate(to);
    const std::optional<Date> last =
        cdsmodel::date_from_date_and_offset(from, quarter, got->intervals);
    const std::optional<Date> past =
        cdsmodel::date_from_date_and_offset(from, quarter, got->intervals + 1);
    ASSERT_TRUE(last.has_value());
    ASSERT_TRUE(past.has_value());
    EXPECT_LE(*last, to) << "the counted date overshot";
    EXPECT_GT(*past, to) << "one more interval would still have fitted";
    EXPECT_EQ(got->extra_days, cdsmodel::to_tdate(to) - cdsmodel::to_tdate(*last));
  }
}

TEST(DateSup, CountDatesRejectsAZeroOrOpposingInterval) {
  const Date from = d(2024, 3, 20);
  const Date to = d(2029, 3, 20);
  EXPECT_FALSE(cdsmodel::count_dates(from, to, DateInterval{0, PeriodUnit::Months}).has_value());
  // Counting forwards with a backwards interval, and vice versa.
  EXPECT_FALSE(cdsmodel::count_dates(from, to, DateInterval{-3, PeriodUnit::Months}).has_value());
  EXPECT_FALSE(cdsmodel::count_dates(to, from, DateInterval{3, PeriodUnit::Months}).has_value());
  // But a backwards interval over a backwards span is fine.
  EXPECT_TRUE(cdsmodel::count_dates(to, from, DateInterval{-3, PeriodUnit::Months}).has_value());
}

TEST(DateSup, BusinessDaysOffsetAlwaysLandsOnABusinessDay) {
  const cdsmodel::Calendar cal = weekends();
  for (int count : {-5, -1, 1, 2, 5, 20}) {
    for (Date start : {d(2024, 3, 22), d(2024, 3, 23), d(2024, 3, 24), d(2024, 6, 15)}) {
      const cdsmodel::AdjustedOffset offset{DateInterval{count, PeriodUnit::Days},
                                            cdsmodel::OffsetBasis::BusinessDays,
                                            cdsmodel::BadDayConv::None};
      const std::optional<Date> got = cdsmodel::advance_adjusted(start, offset, cal);
      ASSERT_TRUE(got.has_value()) << count;
      const std::chrono::weekday wd = cdsmodel::day_of_week(*got);
      EXPECT_NE(wd, std::chrono::Saturday) << count;
      EXPECT_NE(wd, std::chrono::Sunday) << count;
    }
  }
}

TEST(DateSup, BusinessPeriodCarriesTheMonthEnd) {
  // 28 February 2024 is a Wednesday and 29 February is a Thursday, so the
  // business end of February 2024 is the 29th. Advancing a month from it must
  // land on the end of March, not the 29th.
  const cdsmodel::Calendar cal = weekends();
  const cdsmodel::AdjustedOffset offset{DateInterval{1, PeriodUnit::Months},
                                        cdsmodel::OffsetBasis::BusinessPeriod,
                                        cdsmodel::BadDayConv::Previous};
  const std::optional<Date> from_eom = cdsmodel::advance_adjusted(d(2024, 2, 29), offset, cal);
  ASSERT_TRUE(from_eom.has_value());
  EXPECT_EQ(*from_eom, d(2024, 3, 29)) << "31 Mar 2024 is a Sunday, so Previous gives the 29th";

  // A mid-month start gets no carry: the 15th plus a month is the 15th.
  const std::optional<Date> from_mid = cdsmodel::advance_adjusted(d(2024, 2, 15), offset, cal);
  ASSERT_TRUE(from_mid.has_value());
  EXPECT_EQ(*from_mid, d(2024, 3, 15));
}

TEST(DateSup, WeekdaysBasisRequiresADayInterval) {
  const cdsmodel::Calendar cal = weekends();
  const cdsmodel::AdjustedOffset months{DateInterval{1, PeriodUnit::Months},
                                        cdsmodel::OffsetBasis::Weekdays,
                                        cdsmodel::BadDayConv::None};
  EXPECT_FALSE(cdsmodel::advance_adjusted(d(2024, 6, 15), months, cal).has_value());

  const cdsmodel::AdjustedOffset days{DateInterval{3, PeriodUnit::Days},
                                      cdsmodel::OffsetBasis::Weekdays, cdsmodel::BadDayConv::None};
  EXPECT_TRUE(cdsmodel::advance_adjusted(d(2024, 6, 15), days, cal).has_value());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(DateSup, MatchesRecordedIntervalConversions) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kIntervalYearsCases) {
    const std::optional<DateInterval> iv = cdsmodel::make_date_interval(c.periods, c.letter);
    ASSERT_TRUE(iv.has_value()) << c.periods << c.letter;
    EXPECT_EQ(cdsmodel::interval_to_years(*iv), c.years) << c.periods << c.letter;
    const std::optional<double> freq = cdsmodel::interval_to_frequency(*iv);
    ASSERT_EQ(freq.has_value(), c.has_frequency) << c.periods << c.letter;
    if (c.has_frequency) {
      EXPECT_EQ(*freq, c.frequency) << c.periods << c.letter;
    }
    ++checked;
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(DateSup, MatchesRecordedOffsets) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kOffsetCases) {
    const std::optional<DateInterval> iv = cdsmodel::make_date_interval(c.periods, c.letter);
    ASSERT_TRUE(iv.has_value());
    const std::optional<Date> got =
        cdsmodel::date_from_date_and_offset(cdsmodel::from_tdate(c.start), *iv, c.index);
    const std::string where = std::to_string(c.periods) + c.letter + " x " +
                              std::to_string(c.index) + " from " + std::to_string(c.start);
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (c.solved) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.result) << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 900) << checked;
}

TEST(DateSup, MatchesRecordedCounts) {
  int checked = 0;
  int rejected = 0;
  for (const auto &c : cdsmodel::golden::kCountCases) {
    const std::optional<DateInterval> iv = cdsmodel::make_date_interval(c.periods, c.letter);
    ASSERT_TRUE(iv.has_value());
    const std::optional<cdsmodel::DateCount> got =
        cdsmodel::count_dates(cdsmodel::from_tdate(c.from), cdsmodel::from_tdate(c.to), *iv);
    const std::string where = std::to_string(c.periods) + c.letter + " " + std::to_string(c.from) +
                              " -> " + std::to_string(c.to);
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (!c.solved) {
      ++rejected;
      continue;
    }
    EXPECT_EQ(got->intervals, c.intervals) << where;
    EXPECT_EQ(got->extra_days, c.extra_days) << where;
    ++checked;
  }
  EXPECT_GT(checked, 300) << checked;
  EXPECT_GT(rejected, 50) << "the rejection paths are barely represented: " << rejected;
}

TEST(DateSup, MatchesRecordedAdjustedOffsets) {
  const cdsmodel::Calendar cal = weekends();
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kFwdAdjustCases) {
    const std::optional<DateInterval> iv = cdsmodel::make_date_interval(c.periods, c.letter);
    ASSERT_TRUE(iv.has_value());
    // The recorded basis is upstream's isBusDays; BUSINESS splits on the unit
    // letter, which is why the port names the two cases separately.
    cdsmodel::OffsetBasis basis = cdsmodel::OffsetBasis::CalendarPeriod;
    if (c.basis == 1) {
      basis = c.letter == 'D' ? cdsmodel::OffsetBasis::BusinessDays
                              : cdsmodel::OffsetBasis::BusinessPeriod;
    } else if (c.basis == 2) {
      basis = cdsmodel::OffsetBasis::Weekdays;
    }
    const cdsmodel::AdjustedOffset offset{
        *iv, basis, *cdsmodel::bad_day_conv_from_upstream(static_cast<long>(c.bad_day))};
    const std::optional<Date> got =
        cdsmodel::advance_adjusted(cdsmodel::from_tdate(c.start), offset, cal);
    const std::string where = std::to_string(c.periods) + c.letter + " basis " +
                              std::to_string(c.basis) + " conv " + std::string(1, c.bad_day) +
                              " from " + std::to_string(c.start);
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (c.solved) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.result) << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 350) << checked;
}

} // namespace
