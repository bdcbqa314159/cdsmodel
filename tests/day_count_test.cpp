// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include "golden/day_count_cases.hpp"

#include <gtest/gtest.h>

namespace {

TEST(DayCountGolden, MatchesRecordedReference) {
  for (const cdsmodel::golden::DayCountCase &c : cdsmodel::golden::kDayCountCases) {
    const auto dcc = cdsmodel::from_upstream_code(c.dcc);
    ASSERT_TRUE(dcc.has_value()) << "recorded an unknown convention code " << c.dcc;
    const cdsmodel::Date from = cdsmodel::from_tdate(c.from);
    const cdsmodel::Date to = cdsmodel::from_tdate(c.to);

    const auto yf = cdsmodel::year_fraction(from, to, *dcc);
    ASSERT_EQ(yf.has_value(), c.yf_ok) << "yf validity, dcc=" << c.dcc;
    if (c.yf_ok) {
      EXPECT_DOUBLE_EQ(*yf, c.yf) << "yf, dcc=" << c.dcc << " from=" << c.from << " to=" << c.to;
    }

    const auto days = cdsmodel::days_diff(from, to, *dcc);
    ASSERT_EQ(days.has_value(), c.days_ok) << "days validity, dcc=" << c.dcc;
    if (c.days_ok) {
      EXPECT_EQ(*days, c.days) << "days, dcc=" << c.dcc << " from=" << c.from << " to=" << c.to;
    }
  }
}

TEST(DayCountGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kDayCountCases.size(), 800u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(DayCountCodes, RoundTripThroughUpstreamCodes) {
  for (cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
        cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::EffectiveRate}) {
    EXPECT_EQ(cdsmodel::from_upstream_code(cdsmodel::to_upstream_code(dcc)), dcc);
  }
}

TEST(DayCountCodes, RejectsUnknownCodesIncludingTheBadDccSentinel) {
  for (long bad : {-2L, -1L, 0L, 6L, 7L, 9L, 99L}) {
    EXPECT_FALSE(cdsmodel::from_upstream_code(bad).has_value()) << "code " << bad;
  }
}

TEST(DayCountCodes, ActActIsAnAliasNotASeparateConvention) {
  EXPECT_EQ(cdsmodel::kActAct, cdsmodel::DayCount::Act365);
}

TEST(DayCountFraction, KnownValuesAtCompileTime) {
  // Built directly rather than through `make_date`, which returns
  // `Expected<Date>`. GCC 16 cannot constant-evaluate a constexpr function
  // returning a variant whose other alternative is non-trivially
  // destructible when the active one is a class type -- bisected:
  //
  //     constexpr fn -> variant<Date, {int, std::string}> -> get<0>   fails
  //     constexpr fn -> variant<Date, int>                -> get<0>   passes
  //
  // Clang and older GCC accept both, so it is a GCC 16 regression rather
  // than a defect here. `Expected<double>` below is unaffected, because a
  // `double` is not a class type -- which is why every assertion in this
  // test still runs on both compilers rather than being guarded away.
  constexpr cdsmodel::Date y2000{std::chrono::year{2000} / std::chrono::January /
                                 std::chrono::day{1}};
  constexpr cdsmodel::Date y2001{std::chrono::year{2001} / std::chrono::January /
                                 std::chrono::day{1}};

  // 2000 is a leap year: 366 actual days.
  static_assert(*cdsmodel::year_fraction(y2000, y2001, cdsmodel::DayCount::Act365Fixed) ==
                366.0 / 365.0);
  static_assert(*cdsmodel::year_fraction(y2000, y2001, cdsmodel::DayCount::Act360) ==
                366.0 / 360.0);
  // ACT/365 weights those 366 days by 366, giving exactly one year.
  static_assert(*cdsmodel::year_fraction(y2000, y2001, cdsmodel::DayCount::Act365) == 1.0);
  // 30/360 sees a whole year regardless.
  static_assert(*cdsmodel::year_fraction(y2000, y2001, cdsmodel::DayCount::Thirty360) == 1.0);
  SUCCEED();
}

TEST(DayCountFraction, EffectiveRateIsSignOnly) {
  const cdsmodel::Date a = *cdsmodel::make_date(2008, 1, 3);
  const cdsmodel::Date b = *cdsmodel::make_date(2019, 9, 20);
  EXPECT_EQ(*cdsmodel::year_fraction(a, b, cdsmodel::DayCount::EffectiveRate), 1.0);
  EXPECT_EQ(*cdsmodel::year_fraction(b, a, cdsmodel::DayCount::EffectiveRate), -1.0);
  EXPECT_EQ(*cdsmodel::year_fraction(a, a, cdsmodel::DayCount::EffectiveRate), 0.0);
  // It has no day count numerator, only a fraction.
  EXPECT_FALSE(cdsmodel::days_diff(a, b, cdsmodel::DayCount::EffectiveRate).has_value());
}

TEST(DayCountFraction, IsAntisymmetric) {
  const cdsmodel::Date a = *cdsmodel::make_date(2004, 2, 29);
  const cdsmodel::Date b = *cdsmodel::make_date(2011, 7, 31);
  for (cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
        cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360}) {
    EXPECT_DOUBLE_EQ(*cdsmodel::year_fraction(a, b, dcc), -*cdsmodel::year_fraction(b, a, dcc))
        << "dcc=" << cdsmodel::to_upstream_code(dcc);
    EXPECT_EQ(*cdsmodel::days_diff(a, b, dcc), -*cdsmodel::days_diff(b, a, dcc))
        << "dcc=" << cdsmodel::to_upstream_code(dcc);
  }
}

TEST(DayCountDiff, ThirtyVersusThirtyEDifferOnlyOnA31stEnd) {
  // 30/360 collapses a 31st end date only when the start was already a 30th;
  // 30E/360 collapses it unconditionally. That is the entire difference.
  const cdsmodel::Date start_15 = *cdsmodel::make_date(2023, 1, 15);
  const cdsmodel::Date start_30 = *cdsmodel::make_date(2023, 1, 30);
  const cdsmodel::Date end_31 = *cdsmodel::make_date(2023, 3, 31);

  // Start is not a 30th or 31st: the two conventions disagree.
  EXPECT_NE(*cdsmodel::days_diff(start_15, end_31, cdsmodel::DayCount::Thirty360),
            *cdsmodel::days_diff(start_15, end_31, cdsmodel::DayCount::ThirtyE360));
  // Start is a 30th: they agree.
  EXPECT_EQ(*cdsmodel::days_diff(start_30, end_31, cdsmodel::DayCount::Thirty360),
            *cdsmodel::days_diff(start_30, end_31, cdsmodel::DayCount::ThirtyE360));
}

} // namespace
