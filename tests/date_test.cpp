// TIER 1 -- runs anywhere, including CI with no upstream oracle present.
// Port semantics plus replay of reference answers recorded in
// tests/golden/date_cases.hpp.

#include <cdsmodel/date.hpp>

#include "golden/date_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace {

using namespace std::chrono;

// --- replay of recorded upstream answers ---------------------------------

TEST(DateGolden, TDateDecompositionMatchesRecordedReference) {
  for (const cdsmodel::golden::DateCase &c : cdsmodel::golden::kDateCases) {
    const cdsmodel::Date d = cdsmodel::from_tdate(c.tdate);
    const year_month_day ymd = cdsmodel::to_ymd(d);

    EXPECT_EQ(cdsmodel::to_tdate(d), c.tdate) << "round trip at " << c.tdate;
    EXPECT_EQ(static_cast<int>(ymd.year()), c.year) << "year at " << c.tdate;
    EXPECT_EQ(static_cast<unsigned>(ymd.month()), c.month) << "month at " << c.tdate;
    EXPECT_EQ(static_cast<unsigned>(ymd.day()), c.day) << "day at " << c.tdate;
    EXPECT_EQ(static_cast<long>(cdsmodel::day_of_week(d).c_encoding()), c.weekday)
        << "weekday at " << c.tdate;
    EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::end_of_month(d)), c.eom) << "eom at " << c.tdate;
  }
}

TEST(DateGolden, ConstructionMatchesRecordedReference) {
  for (const cdsmodel::golden::MakeCase &c : cdsmodel::golden::kMakeCases) {
    const auto got = cdsmodel::make_date(c.year, c.month, c.day);
    ASSERT_EQ(got.has_value(), c.ok) << "validity for " << c.year << "-" << c.month << "-" << c.day;
    if (c.ok) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.tdate)
          << "value for " << c.year << "-" << c.month << "-" << c.day;
    }
  }
}

TEST(DateGolden, NormalizationMatchesRecordedReference) {
  for (const cdsmodel::golden::NormalizeCase &c : cdsmodel::golden::kNormalizeCases) {
    const auto got = cdsmodel::normalize_ymd(c.year, c.month, c.day);
    ASSERT_EQ(got.has_value(), c.ok) << "validity for " << c.year << "-" << c.month << "-" << c.day;
    if (c.ok) {
      EXPECT_EQ(static_cast<int>(got->year()), c.out_year)
          << "year for " << c.year << "-" << c.month << "-" << c.day;
      EXPECT_EQ(static_cast<unsigned>(got->month()), c.out_month)
          << "month for " << c.year << "-" << c.month << "-" << c.day;
      EXPECT_EQ(static_cast<unsigned>(got->day()), c.out_day)
          << "day for " << c.year << "-" << c.month << "-" << c.day;
    }
  }
}

TEST(DateGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kDateCases.size(), 400u);
  EXPECT_GT(cdsmodel::golden::kMakeCases.size(), 200u);
  EXPECT_GT(cdsmodel::golden::kNormalizeCases.size(), 90u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(DateEpoch, EpochIsPinnedAtCompileTime) {
  static_assert(cdsmodel::to_tdate(cdsmodel::kDateEpoch) == 0);
  static_assert(cdsmodel::kDateEpoch == sys_days{year{1601} / January / 1});
  // The whole project rests on this number: 1970-01-01 is TDate 134774.
  static_assert(cdsmodel::to_tdate(sys_days{year{1970} / January / 1}) == 134774);
  // And the example program's trade date, from upstream's own main.c.
  static_assert(cdsmodel::to_tdate(sys_days{year{2008} / January / 3}) == 148655);
  SUCCEED();
}

TEST(DateEpoch, RoundTripsOverTheWholeRange) {
  for (cdsmodel::TDate t = 0; t < 219000; ++t) {
    ASSERT_EQ(cdsmodel::to_tdate(cdsmodel::from_tdate(t)), t) << "at " << t;
  }
}

TEST(DateConstruction, RejectsPreEpochYears) {
  EXPECT_FALSE(cdsmodel::make_date(1600, 12, 31).has_value());
  EXPECT_TRUE(cdsmodel::make_date(1601, 1, 1).has_value());
}

TEST(DateConstruction, RejectsImpossibleCalendarDays) {
  EXPECT_FALSE(cdsmodel::make_date(2023, 4, 31).has_value());
  EXPECT_FALSE(cdsmodel::make_date(1900, 2, 29).has_value()); // 1900 is not leap
  EXPECT_TRUE(cdsmodel::make_date(2000, 2, 29).has_value());  // 2000 is
  EXPECT_FALSE(cdsmodel::make_date(2024, 13, 1).has_value());
  EXPECT_FALSE(cdsmodel::make_date(2024, 0, 1).has_value());
}

TEST(DateNormalize, ClampsOverlongDayButRejectsOutOfRangeDay) {
  // The asymmetry upstream chose, preserved deliberately.
  const auto clamped = cdsmodel::normalize_ymd(2023, 4, 31);
  ASSERT_TRUE(clamped.has_value());
  EXPECT_EQ(static_cast<unsigned>(clamped->day()), 30u);

  EXPECT_FALSE(cdsmodel::normalize_ymd(2023, 4, 32).has_value());
  EXPECT_FALSE(cdsmodel::normalize_ymd(2023, 4, 0).has_value());
}

TEST(DateNormalize, CarriesMonthIntoYearInBothDirections) {
  const auto fwd = cdsmodel::normalize_ymd(2023, 13, 1);
  ASSERT_TRUE(fwd.has_value());
  EXPECT_EQ(static_cast<int>(fwd->year()), 2024);
  EXPECT_EQ(static_cast<unsigned>(fwd->month()), 1u);

  const auto back = cdsmodel::normalize_ymd(2023, 0, 1);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(static_cast<int>(back->year()), 2022);
  EXPECT_EQ(static_cast<unsigned>(back->month()), 12u);

  const auto far_back = cdsmodel::normalize_ymd(2023, -14, 1);
  ASSERT_TRUE(far_back.has_value());
  EXPECT_EQ(static_cast<int>(far_back->year()), 2021);
  EXPECT_EQ(static_cast<unsigned>(far_back->month()), 10u);
}

TEST(DateMonth, DaysInMonthIsTypeSafeWhereUpstreamWasNot) {
  // Upstream's JpmcdsDaysInMonth(year, 13) read past the end of a 13-element
  // table. std::chrono::month makes a 13th month unrepresentable.
  EXPECT_EQ(cdsmodel::days_in_month(year{2024} / February), 29u);
  EXPECT_EQ(cdsmodel::days_in_month(year{2023} / February), 28u);
  EXPECT_EQ(cdsmodel::days_in_month(year{2023} / April), 30u);
  EXPECT_EQ(cdsmodel::days_in_month(year{2023} / December), 31u);
}

TEST(DateMonth, EndOfMonthStaysInItsMonth) {
  const auto d = *cdsmodel::make_date(2024, 2, 5);
  const auto eom = cdsmodel::end_of_month(d);
  EXPECT_EQ(cdsmodel::to_ymd(eom).year(), year{2024});
  EXPECT_EQ(cdsmodel::to_ymd(eom).month(), February);
  EXPECT_EQ(static_cast<unsigned>(cdsmodel::to_ymd(eom).day()), 29u);
}

TEST(DateLeap, FollowsGregorianCenturyRules) {
  EXPECT_FALSE(cdsmodel::is_leap(year{1700}));
  EXPECT_FALSE(cdsmodel::is_leap(year{1800}));
  EXPECT_FALSE(cdsmodel::is_leap(year{1900}));
  EXPECT_TRUE(cdsmodel::is_leap(year{2000}));
  EXPECT_TRUE(cdsmodel::is_leap(year{2024}));
  EXPECT_FALSE(cdsmodel::is_leap(year{2100}));
}

} // namespace
