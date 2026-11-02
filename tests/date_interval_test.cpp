// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>

#include "golden/date_interval_cases.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace {

cdsmodel::PeriodUnit unit_from(int u) {
  return u == 1 ? cdsmodel::PeriodUnit::Months : cdsmodel::PeriodUnit::Days;
}

TEST(DateIntervalGolden, ConstructionMatchesRecordedReference) {
  for (const auto &c : cdsmodel::golden::kIntervalMakeCases) {
    const auto got = cdsmodel::make_date_interval(c.periods, c.type);
    ASSERT_EQ(got.has_value(), c.ok) << "validity for " << c.periods << c.type;
    if (c.ok) {
      EXPECT_EQ(got->periods, c.out_periods) << "periods for " << c.periods << c.type;
      EXPECT_EQ(got->unit, unit_from(c.out_unit)) << "unit for " << c.periods << c.type;
    }
  }
}

TEST(DateIntervalGolden, ParsingMatchesRecordedReference) {
  for (const auto &c : cdsmodel::golden::kIntervalParseCases) {
    const auto got = cdsmodel::parse_date_interval(c.input);
    ASSERT_EQ(got.has_value(), c.ok) << "validity for \"" << c.input << "\"";
    if (c.ok) {
      EXPECT_EQ(got->periods, c.periods) << "periods for \"" << c.input << "\"";
      EXPECT_EQ(got->unit, unit_from(c.unit)) << "unit for \"" << c.input << "\"";
    }
  }
}

TEST(DateIntervalGolden, AdvanceMatchesRecordedReference) {
  for (const auto &c : cdsmodel::golden::kIntervalAdvanceCases) {
    const cdsmodel::Date start = cdsmodel::from_tdate(c.start);
    const cdsmodel::DateInterval iv{c.periods, unit_from(c.unit)};
    const auto got = cdsmodel::advance(start, iv);
    ASSERT_EQ(got.has_value(), c.ok)
        << "validity, start=" << c.start << " n=" << c.periods << " unit=" << c.unit;
    if (c.ok) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.result)
          << "value, start=" << c.start << " n=" << c.periods << " unit=" << c.unit;
    }
  }
}

TEST(DateIntervalGolden, FormattingMatchesRecordedReference) {
  for (const auto &c : cdsmodel::golden::kIntervalFormatCases) {
    const cdsmodel::DateInterval iv{c.periods, unit_from(c.unit)};
    EXPECT_EQ(cdsmodel::format_date_interval(iv), c.text)
        << "periods=" << c.periods << " unit=" << c.unit;
  }
}

TEST(DateIntervalGolden, VectorSetsAreNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kIntervalMakeCases.size(), 300u);
  EXPECT_GT(cdsmodel::golden::kIntervalAdvanceCases.size(), 300u);
  EXPECT_GT(cdsmodel::golden::kIntervalParseCases.size(), 30u);
  EXPECT_GT(cdsmodel::golden::kIntervalFormatCases.size(), 30u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(DateIntervalMake, NormalisesToDaysOrMonthsAtCompileTime) {
  using cdsmodel::DateInterval;
  using cdsmodel::PeriodUnit;
  static_assert(*cdsmodel::make_date_interval(1, 'Y') == DateInterval{12, PeriodUnit::Months});
  static_assert(*cdsmodel::make_date_interval(1, 'A') == DateInterval{12, PeriodUnit::Months});
  static_assert(*cdsmodel::make_date_interval(1, 'S') == DateInterval{6, PeriodUnit::Months});
  static_assert(*cdsmodel::make_date_interval(1, 'Q') == DateInterval{3, PeriodUnit::Months});
  static_assert(*cdsmodel::make_date_interval(5, 'M') == DateInterval{5, PeriodUnit::Months});
  static_assert(*cdsmodel::make_date_interval(2, 'W') == DateInterval{14, PeriodUnit::Days});
  static_assert(*cdsmodel::make_date_interval(1, 'U') == DateInterval{28, PeriodUnit::Days});
  static_assert(*cdsmodel::make_date_interval(3, 'D') == DateInterval{3, PeriodUnit::Days});
  SUCCEED();
}

TEST(DateIntervalMake, IsCaseInsensitiveIncludingY) {
  // Upstream's constructor is case-insensitive here; its date ADVANCE is not
  // (UPSTREAM-3). Normalising at construction is what makes the port immune.
  for (auto [lower, upper] : {std::pair{'y', 'Y'},
                              {'a', 'A'},
                              {'s', 'S'},
                              {'q', 'Q'},
                              {'m', 'M'},
                              {'w', 'W'},
                              {'d', 'D'},
                              {'u', 'U'}}) {
    EXPECT_EQ(cdsmodel::make_date_interval(1, lower), cdsmodel::make_date_interval(1, upper))
        << "'" << lower << "' vs '" << upper << "'";
  }
}

TEST(DateIntervalMake, RejectsConstructOnlyAndInvalidTypes) {
  // F E B G H I J K L T V are accepted by upstream's constructor but cannot
  // be advanced by, so the port declines them. X Z etc. are simply invalid.
  for (char t : {'F', 'E', 'B', 'G', 'H', 'I', 'J', 'K', 'L', 'T', 'V', 'X', 'Z', '0', '/'}) {
    EXPECT_FALSE(cdsmodel::make_date_interval(1, t).has_value()) << "type '" << t << "'";
  }
}

TEST(DateIntervalParse, DefaultsToOnePeriodWhenNoDigits) {
  using cdsmodel::DateInterval;
  using cdsmodel::PeriodUnit;
  static_assert(*cdsmodel::parse_date_interval("M") == DateInterval{1, PeriodUnit::Months});
  static_assert(*cdsmodel::parse_date_interval("Y") == DateInterval{12, PeriodUnit::Months});
  static_assert(*cdsmodel::parse_date_interval("D") == DateInterval{1, PeriodUnit::Days});
  SUCCEED();
}

TEST(DateIntervalParse, HandlesSigns) {
  using cdsmodel::DateInterval;
  using cdsmodel::PeriodUnit;
  EXPECT_EQ(cdsmodel::parse_date_interval("-6M"), (DateInterval{-6, PeriodUnit::Months}));
  EXPECT_EQ(cdsmodel::parse_date_interval("+3M"), (DateInterval{3, PeriodUnit::Months}));
  EXPECT_EQ(cdsmodel::parse_date_interval("-1Y"), (DateInterval{-12, PeriodUnit::Months}));
}

TEST(DateIntervalParse, RejectsDigitRunsThatWouldOverflowUpstreamsBuffer) {
  // UPSTREAM-4: upstream copies digits into a 32-byte stack buffer with no
  // bounds check, so this input is a stack-buffer-overflow there. The port
  // rejects it instead. This is the one place the port does NOT reproduce
  // upstream behaviour, and it is deliberate.
  EXPECT_FALSE(cdsmodel::parse_date_interval(std::string(40, '9') + "M").has_value());
  EXPECT_FALSE(cdsmodel::parse_date_interval(std::string(32, '1') + "D").has_value());
  // Just inside the limit still parses.
  EXPECT_TRUE(cdsmodel::parse_date_interval("1D").has_value());
}

TEST(DateIntervalParse, RejectsDigitRunsThatWouldOverflowAnInt) {
  // Found by auditing the port under UBSan, NOT by differential testing:
  // upstream's atoi overflows too, so both sides produced wrapped garbage and
  // "agreed". Upstream parses "9999999999M" as 1410065407 months; a 20-digit
  // run comes back as prd=-1, which is also its FAILURE value.
  //
  // The port rejects instead. Recording upstream's answers here would enshrine
  // undefined behaviour as the specification, so these cases are deliberately
  // absent from the golden vectors.
  EXPECT_TRUE(cdsmodel::parse_date_interval("2147483647M").has_value());
  EXPECT_EQ(cdsmodel::parse_date_interval("2147483647M")->periods, 2147483647);

  EXPECT_FALSE(cdsmodel::parse_date_interval("2147483648M").has_value());
  EXPECT_FALSE(cdsmodel::parse_date_interval("9999999999M").has_value());
  EXPECT_FALSE(cdsmodel::parse_date_interval("99999999999999999999M").has_value());
  // The sign is applied after accumulation, so the negative bound matches.
  EXPECT_FALSE(cdsmodel::parse_date_interval("-9999999999M").has_value());

  // Leading zeros do not count against the magnitude, only the digit budget.
  EXPECT_TRUE(cdsmodel::parse_date_interval("0000000001M").has_value());
  EXPECT_EQ(cdsmodel::parse_date_interval("0000000001M")->periods, 1);
}

TEST(DateIntervalParse, OverflowRejectionIsExactAtTheBoundary) {
  // One below, at, and one above INT_MAX, so the check cannot be off by one.
  EXPECT_EQ(cdsmodel::parse_date_interval("2147483646M")->periods, 2147483646);
  EXPECT_EQ(cdsmodel::parse_date_interval("2147483647M")->periods, 2147483647);
  EXPECT_FALSE(cdsmodel::parse_date_interval("2147483648M").has_value());
}

TEST(DateIntervalParse, RejectsMissingOrMisplacedPeriodType) {
  for (std::string_view s : {"", "1", "12", "-", "+", "1X", "X", "1 Y", " 1Y", "Y1"}) {
    const auto got = cdsmodel::parse_date_interval(s);
    if (s == "Y1") {
      // "Y1" parses: no digits, so periods defaults to 1 and 'Y' is the type.
      // The trailing "1" is simply never examined, as upstream also ignores it.
      EXPECT_TRUE(got.has_value()) << "\"" << s << "\"";
    } else {
      EXPECT_FALSE(got.has_value()) << "\"" << s << "\"";
    }
  }
}

TEST(DateIntervalFormat, MonthsCollapseToTheLargestExactUnit) {
  using cdsmodel::DateInterval;
  using cdsmodel::PeriodUnit;
  EXPECT_EQ(cdsmodel::format_date_interval({12, PeriodUnit::Months}), "1A");
  EXPECT_EQ(cdsmodel::format_date_interval({24, PeriodUnit::Months}), "2A");
  EXPECT_EQ(cdsmodel::format_date_interval({18, PeriodUnit::Months}), "3S");
  EXPECT_EQ(cdsmodel::format_date_interval({6, PeriodUnit::Months}), "1S");
  EXPECT_EQ(cdsmodel::format_date_interval({3, PeriodUnit::Months}), "1Q");
  EXPECT_EQ(cdsmodel::format_date_interval({5, PeriodUnit::Months}), "5M");
}

TEST(DateIntervalFormat, DaysNeverCollapseToWeeks) {
  // UPSTREAM-5: upstream's week branch is dead code (missing break), so 14
  // days formats as "14D". Reproduced, because it is observable output.
  using cdsmodel::PeriodUnit;
  EXPECT_EQ(cdsmodel::format_date_interval({7, PeriodUnit::Days}), "7D");
  EXPECT_EQ(cdsmodel::format_date_interval({14, PeriodUnit::Days}), "14D");
  EXPECT_EQ(cdsmodel::format_date_interval({1, PeriodUnit::Days}), "1D");
}

TEST(DateIntervalAdvance, MonthArithmeticClampsTheDay) {
  const cdsmodel::Date jan31 = *cdsmodel::make_date(2023, 1, 31);
  const auto feb = cdsmodel::advance(jan31, {1, cdsmodel::PeriodUnit::Months});
  ASSERT_TRUE(feb.has_value());
  EXPECT_EQ(cdsmodel::to_ymd(*feb), cdsmodel::to_ymd(*cdsmodel::make_date(2023, 2, 28)));

  const cdsmodel::Date jan31_leap = *cdsmodel::make_date(2024, 1, 31);
  const auto feb_leap = cdsmodel::advance(jan31_leap, {1, cdsmodel::PeriodUnit::Months});
  ASSERT_TRUE(feb_leap.has_value());
  EXPECT_EQ(cdsmodel::to_ymd(*feb_leap), cdsmodel::to_ymd(*cdsmodel::make_date(2024, 2, 29)));
}

TEST(DateIntervalAdvance, ClampingIsNotInvertible) {
  // A consequence of clamping worth stating: advancing forward then back does
  // not return the original date. Schedule generation must therefore always
  // roll from a fixed anchor, never iteratively. This bites real schedule code.
  const cdsmodel::Date jan31 = *cdsmodel::make_date(2023, 1, 31);
  const auto forward = cdsmodel::advance(jan31, {1, cdsmodel::PeriodUnit::Months});
  ASSERT_TRUE(forward.has_value());
  const auto back = cdsmodel::advance(*forward, {-1, cdsmodel::PeriodUnit::Months});
  ASSERT_TRUE(back.has_value());
  EXPECT_NE(cdsmodel::to_tdate(*back), cdsmodel::to_tdate(jan31));
  EXPECT_EQ(cdsmodel::to_ymd(*back), cdsmodel::to_ymd(*cdsmodel::make_date(2023, 1, 28)));
}

TEST(DateIntervalAdvance, RejectsTheEpochDay) {
  // Upstream requires startDate > 0, so TDate 0 cannot be advanced.
  EXPECT_FALSE(
      cdsmodel::advance(cdsmodel::kDateEpoch, {1, cdsmodel::PeriodUnit::Months}).has_value());
  EXPECT_TRUE(cdsmodel::advance(cdsmodel::kDateEpoch + std::chrono::days{1},
                                {1, cdsmodel::PeriodUnit::Months})
                  .has_value());
}

} // namespace
