// TIER 1 -- ldate.c's four year helpers, verified with no oracle present.
//
// THERE IS NO tests/golden/ldate_year_cases.hpp, and the reason is better than
// convenience.
//
// Three of these four functions have a DEFINITION, not just a reference
// implementation. The Gregorian leap rule is "divisible by 4, except
// centuries, except multiples of 400"; the start of a year is the first of
// January; the year of a date is its year. Recording upstream's answers and
// replaying them would verify the port against a 2009 C file. Checking the
// definitions verifies it against the calendar -- a higher rung on the oracle
// ladder, and available here only because these functions are small enough to
// have a closed form.
//
// The fourth, `days_left_this_year`, has no definition independent of the day
// count conventions, so it is checked against its own composition and against
// invariants that hold across conventions.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace {

/// The Gregorian rule, written out. Deliberately NOT
/// `std::chrono::year::is_leap()` -- that is what the port calls, so using it
/// here would be checking a function against itself.
constexpr bool gregorian_leap(int y) {
  if (y % 4 != 0) {
    return false;
  }
  if (y % 100 != 0) {
    return true;
  }
  return y % 400 == 0;
}

TEST(LdateYearGolden, TheLeapRuleMatchesItsDefinition) {
  int leap = 0;
  int common = 0;
  for (int y = 1601; y <= 2400; ++y) {
    const cdsmodel::Date jan1 = *cdsmodel::make_date(y, 1, 1);
    const cdsmodel::Date dec31 = *cdsmodel::make_date(y, 12, 31);
    const bool want = gregorian_leap(y);
    EXPECT_EQ(cdsmodel::is_leap_year(jan1), want) << "year " << y;
    EXPECT_EQ(cdsmodel::is_leap_year(dec31), want) << "year " << y << " from December";
    if (want) {
      ++leap;
    } else {
      ++common;
    }
  }
  // 800 years: 200 multiples of 4, minus 8 centuries, plus 2 multiples of 400
  // (1600 is outside the range, so 2000 and 2400).
  EXPECT_EQ(leap, 200 - 8 + 2);
  EXPECT_EQ(leap + common, 800);
}

// A leap year is exactly a year with 366 days, which is an independent check
// on the predicate: it compares the rule against the calendar's own length.
TEST(LdateYearGolden, ALeapYearIsExactlyAYearOf366Days) {
  for (int y = 1900; y <= 2200; ++y) {
    const cdsmodel::Date start = cdsmodel::year_start(*cdsmodel::make_date(y, 6, 15));
    const cdsmodel::Date next = cdsmodel::next_year_start(*cdsmodel::make_date(y, 6, 15));
    const long length = (next - start).count();
    EXPECT_EQ(length, cdsmodel::is_leap_year(start) ? 366 : 365) << "year " << y;
  }
}

TEST(LdateYearGolden, YearStartIsTheFirstOfJanuaryAndYearOfIsIdempotent) {
  int checked = 0;
  for (int y = 1900; y <= 2200; y += 7) {
    for (const auto [m, d] : {std::pair{1, 1}, {2, 28}, {6, 15}, {12, 31}}) {
      const std::optional<cdsmodel::Date> date = cdsmodel::make_date(y, m, d);
      ASSERT_TRUE(date.has_value()) << y << "-" << m << "-" << d;

      EXPECT_EQ(cdsmodel::year_of(*date), y);

      const cdsmodel::Date start = cdsmodel::year_start(*date);
      const std::chrono::year_month_day ymd = cdsmodel::to_ymd(start);
      EXPECT_EQ(static_cast<int>(ymd.year()), y);
      EXPECT_EQ(ymd.month(), std::chrono::January);
      EXPECT_EQ(ymd.day(), std::chrono::day{1});

      // year_start is a projection: applying it twice changes nothing.
      EXPECT_EQ(cdsmodel::year_start(start), start);
      // and the year survives it.
      EXPECT_EQ(cdsmodel::year_of(start), y);

      // next_year_start is year_start of one year later.
      EXPECT_EQ(cdsmodel::year_of(cdsmodel::next_year_start(*date)), y + 1);
      EXPECT_GT(cdsmodel::next_year_start(*date), *date);
      EXPECT_LE(start, *date);
      ++checked;
    }
  }
  EXPECT_GT(checked, 150);
}

// Under an ACT convention the answer really is a count of days, so it must
// equal the calendar difference exactly. That is the one convention where the
// function's name is honest.
TEST(LdateYearGolden, UnderActTheAnswerIsTheCalendarDifference) {
  int checked = 0;
  for (const int y : {2023, 2024, 2100, 2000}) {
    for (int doy = 0; doy < 365; doy += 11) {
      const cdsmodel::Date date = *cdsmodel::make_date(y, 1, 1) + std::chrono::days{doy};
      if (cdsmodel::year_of(date) != y) {
        continue;
      }
      const long calendar = (cdsmodel::next_year_start(date) - date).count();
      for (const cdsmodel::DayCount dcc :
           {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
        const std::optional<long> got = cdsmodel::days_left_this_year(date, dcc);
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(*got, calendar) << "year " << y << " day " << doy;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 200);
}

// From 1 January, an ACT convention sees the year's own length -- 365 or 366 --
// while 30/360 always sees 360. The clearest statement of why the function's
// name is misleading.
TEST(LdateYearGolden, FromTheFirstOfJanuaryTheConventionsDisagreeByDesign) {
  for (const int y : {2023, 2024}) {
    const cdsmodel::Date jan1 = *cdsmodel::make_date(y, 1, 1);
    const long expected_act = cdsmodel::is_leap_year(jan1) ? 366 : 365;
    EXPECT_EQ(cdsmodel::days_left_this_year(jan1, cdsmodel::DayCount::Act365Fixed),
              std::optional<long>{expected_act})
        << "year " << y;
    EXPECT_EQ(cdsmodel::days_left_this_year(jan1, cdsmodel::DayCount::Thirty360),
              std::optional<long>{360})
        << "year " << y;
  }
}

} // namespace
