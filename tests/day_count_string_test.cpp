// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>

#include "golden/day_count_string_cases.hpp"

#include <gtest/gtest.h>

namespace {

TEST(DayCountStringGolden, ParsingMatchesRecordedReference) {
  for (const cdsmodel::golden::ParseCase &c : cdsmodel::golden::kParseCases) {
    const auto got = cdsmodel::parse_day_count_code(c.input);
    ASSERT_EQ(got.has_value(), c.ok) << "validity for \"" << c.input << "\"";
    if (c.ok) {
      EXPECT_EQ(*got, c.code) << "code for \"" << c.input << "\"";
    }
  }
}

TEST(DayCountStringGolden, FormattingMatchesRecordedReference) {
  for (const cdsmodel::golden::FormatCase &c : cdsmodel::golden::kFormatCases) {
    EXPECT_EQ(cdsmodel::format_day_count_code(c.code), c.text) << "code " << c.code;
  }
}

TEST(DayCountStringGolden, VectorSetIsNotEmpty) {
  EXPECT_GT(cdsmodel::golden::kParseCases.size(), 1000u);
  EXPECT_GT(cdsmodel::golden::kFormatCases.size(), 15u);
}

// --- port semantics needing no oracle ------------------------------------

TEST(DayCountStringParse, CanonicalNamesAtCompileTime) {
  static_assert(cdsmodel::parse_day_count("Actual/365") == cdsmodel::DayCount::Act365);
  static_assert(cdsmodel::parse_day_count("Actual/365F") == cdsmodel::DayCount::Act365Fixed);
  static_assert(cdsmodel::parse_day_count("Actual/360") == cdsmodel::DayCount::Act360);
  static_assert(cdsmodel::parse_day_count("30/360") == cdsmodel::DayCount::Thirty360);
  static_assert(cdsmodel::parse_day_count("30E/360") == cdsmodel::DayCount::ThirtyE360);
  static_assert(cdsmodel::parse_day_count("Effective") == cdsmodel::DayCount::EffectiveRate);
  // Act/Act shares Act/365's code upstream, so it parses to the same value.
  static_assert(cdsmodel::parse_day_count("Act/Act") == cdsmodel::DayCount::Act365);
  SUCCEED();
}

TEST(DayCountStringParse, IsCaseInsensitiveAndStripsLeadingWhitespace) {
  for (std::string_view s : {"act/360", "ACT/360", "Act/360", "  act/360", "\tACT/360"}) {
    EXPECT_EQ(cdsmodel::parse_day_count(s), cdsmodel::DayCount::Act360) << s;
  }
}

TEST(DayCountStringParse, KeepsInternalWhitespaceAndStillParses) {
  // Upstream's preprocessing strips only LEADING whitespace. "ACT / 360"
  // keeps its spaces; it parses solely because the search is by substring.
  EXPECT_EQ(cdsmodel::parse_day_count("ACT / 360"), cdsmodel::DayCount::Act360);
}

TEST(DayCountStringParse, LeniencyIsPreservedNotTightened) {
  // Upstream accepts these. Rejecting them would be a breaking change, not a
  // fix, so the port accepts them too. If a strict parser is ever added, it
  // goes alongside rather than replacing this.
  EXPECT_EQ(cdsmodel::parse_day_count("GARBAGE365"), cdsmodel::DayCount::Act365);
  EXPECT_EQ(cdsmodel::parse_day_count("A360"), cdsmodel::DayCount::Act360);
  EXPECT_EQ(cdsmodel::parse_day_count("B360"), cdsmodel::DayCount::Thirty360);
}

TEST(DayCountStringParse, NonCalculableKeywordsParseButAreNotConventions) {
  // "DEFAULT" and "NONE" parse to codes upstream refuses to calculate with.
  EXPECT_EQ(cdsmodel::parse_day_count_code("DEFAULT"), cdsmodel::kDayCountDefaultCode);
  EXPECT_EQ(cdsmodel::parse_day_count_code("NONE"), cdsmodel::kDayCountNoneCode);
  EXPECT_FALSE(cdsmodel::parse_day_count("DEFAULT").has_value());
  EXPECT_FALSE(cdsmodel::parse_day_count("NONE").has_value());
}

TEST(DayCountStringParse, DefaultReachesTheKeywordCheckDespiteContainingAnA) {
  // "DEFAULT" enters the 'A' branch, matches none of its inner tests, and
  // falls out of the whole if/else chain to the keyword comparison. If that
  // control flow were "fixed" into a clean else-chain, this would break.
  EXPECT_TRUE(cdsmodel::parse_day_count_code("DEFAULT").has_value());
  EXPECT_EQ(cdsmodel::format_day_count_code(*cdsmodel::parse_day_count_code("DEFAULT")), "Default");
}

TEST(DayCountStringParse, TruncatesAtThirtyTwoCharacters) {
  // 'A' x29 + "365" is exactly 32 characters, so "365" survives the cut.
  EXPECT_EQ(cdsmodel::parse_day_count("AAAAAAAAAAAAAAAAAAAAAAAAAAAAA365"),
            cdsmodel::DayCount::Act365);
  // 'A' x30 + "365" is 33, so the cut lands mid-token leaving "...A36": the
  // deciding substring is destroyed and the parse fails. One character of
  // input changes the answer from ACT/365 to rejected.
  EXPECT_FALSE(cdsmodel::parse_day_count_code("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA365").has_value());
}

TEST(DayCountStringParse, RejectsEmptyAndUnrecognised) {
  for (std::string_view s : {"", " ", "\t\n", "XYZ", "BANANA", "DEFAULTS"}) {
    EXPECT_FALSE(cdsmodel::parse_day_count_code(s).has_value()) << "\"" << s << "\"";
  }
}

TEST(DayCountStringFormat, NoneDoesNotRoundTrip) {
  // Documented asymmetry: "NONE" parses to 0, which has no formatter case.
  EXPECT_EQ(cdsmodel::parse_day_count_code("NONE"), 0L);
  EXPECT_EQ(cdsmodel::format_day_count_code(0), "unrecognized");
}

TEST(DayCountStringFormat, EveryConventionRoundTrips) {
  for (cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
        cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::EffectiveRate}) {
    EXPECT_EQ(cdsmodel::parse_day_count(cdsmodel::to_string(dcc)), dcc) << cdsmodel::to_string(dcc);
  }
}

} // namespace
