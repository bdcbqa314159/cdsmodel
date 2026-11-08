// TIER 2 -- live differential tests for the day count string parser.
//
// A substring-matching parser cannot be verified by its documented inputs:
// the interesting behaviour is what it does with everything else. So in
// addition to the canonical names, this enumerates every 1-, 2- and 3-
// character string over the alphabet that drives upstream's branches
// ("A B E F N D 3 0 6 /"), which is 1,110 strings exercising every
// combination its strstr chain can see.

#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "ldate.h"
#include "yearfrac.h"
}

namespace {

class DayCountStringDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Expected<long> reference_parse(const std::string &s) {
  long code = 0;
  std::string mutable_copy = s; // upstream takes char*, not const char*
  if (JpmcdsStringToDayCountConv(mutable_copy.data(), &code) != SUCCESS) {
    return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
  }
  return code;
}

void expect_parse_matches(const std::string &s) {
  const cdsmodel::Expected<long> reference = reference_parse(s);
  const cdsmodel::Expected<long> ported = cdsmodel::parse_day_count_code(s);

  ASSERT_EQ(ported.has_value(), reference.has_value())
      << "validity disagreement for \"" << s << "\"";
  if (reference.has_value()) {
    EXPECT_EQ(*ported, *reference) << "code disagreement for \"" << s << "\"";
  }
}

std::vector<std::string> canonical_and_awkward_strings() {
  return {
      // the documented names
      "Actual/365",
      "Actual/365F",
      "Actual/360",
      "30/360",
      "30E/360",
      "Effective",
      "Default",
      // case variants -- the parser uppercases
      "ACTUAL/365",
      "actual/365",
      "aCtUaL/365f",
      "effective",
      "EFFECTIVE",
      "default",
      "NONE",
      "none",
      "None",
      // Act/Act, which shares ACT/365's code
      "Act/Act",
      "ACT/ACT",
      "A/A",
      // abbreviations upstream's own comment advertises
      "A",
      "ACT/360",
      "ACT/365",
      "ACT/365F",
      "B30/360",
      "B30E/360",
      // whitespace: leading is stripped, internal is NOT
      " ACT/360",
      "   ACT/360",
      "\tACT/360",
      "ACT /360",
      "ACT / 360",
      "ACT/360 ",
      " 30/360 ",
      // empty and whitespace-only
      "",
      " ",
      "   ",
      "\t\n",
      // strings that parse by accident -- preserved leniency
      "GARBAGE365",
      "A360",
      "A365",
      "B360",
      "30360",
      "XA360X",
      "FA365",
      "EFFORT",
      // strings that contain 'A' but match nothing inside that branch, so they
      // fall out of the whole chain (this is how DEFAULT still works)
      "BANANA",
      "DEFAULTS",
      "ADEFAULT",
      "NONEX",
      "XNONE",
      // the documented illegal form
      "30/Actual",
      "30/ACTUAL",
      // single characters that drive individual branches
      "B",
      "E",
      "F",
      "N",
      "D",
      "3",
      "0",
      "6",
      "/",
      // over the 32-character truncation limit, with the deciding token
      // beyond the cut so truncation changes the answer
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA365",
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA365",
      "                                 ACT/360",
      "30/360AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
      // embedded NUL-adjacent and punctuation noise
      "ACT-360",
      "ACT_365",
      "30 / 360",
      "-1",
      "0",
      "8",
  };
}

TEST_F(DayCountStringDifferential, CanonicalAndAwkwardStringsMatch) {
  for (const std::string &s : canonical_and_awkward_strings()) {
    expect_parse_matches(s);
  }
}

TEST_F(DayCountStringDifferential, EveryShortStringOverTheDecidingAlphabetMatches) {
  const std::string alphabet = "ABEFND306/";
  for (char a : alphabet) {
    expect_parse_matches(std::string{a});
    for (char b : alphabet) {
      expect_parse_matches(std::string{a, b});
      for (char c : alphabet) {
        expect_parse_matches(std::string{a, b, c});
      }
    }
  }
}

TEST_F(DayCountStringDifferential, LowercaseAlphabetBehavesIdenticallyToUppercase) {
  const std::string alphabet = "abefnd306/";
  for (char a : alphabet) {
    for (char b : alphabet) {
      expect_parse_matches(std::string{a, b});
    }
  }
}

TEST_F(DayCountStringDifferential, FormatMatchesForEveryCodeIncludingInvalidOnes) {
  for (long code = -6; code <= 14; ++code) {
    const char *reference = JpmcdsFormatDayCountConv(code);
    ASSERT_NE(reference, nullptr) << "code " << code;
    EXPECT_EQ(cdsmodel::format_day_count_code(code), std::string_view{reference})
        << "code " << code;
  }
}

TEST_F(DayCountStringDifferential, ValidityPredicateMatchesJpmcdsDayCountValid) {
  // from_upstream_code succeeding is exactly upstream's JpmcdsDayCountValid.
  for (long code = -6; code <= 14; ++code) {
    const bool reference = JpmcdsDayCountValid(nullptr, code) == SUCCESS;
    EXPECT_EQ(cdsmodel::from_upstream_code(code).has_value(), reference) << "code " << code;
  }
}

TEST_F(DayCountStringDifferential, RoundTripFormatThenParseForCalculableConventions) {
  for (cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
        cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::EffectiveRate}) {
    const std::string name{cdsmodel::to_string(dcc)};
    expect_parse_matches(name);
    EXPECT_EQ(cdsmodel::parse_day_count(name), dcc) << "name \"" << name << "\"";
  }
}

TEST_F(DayCountStringDifferential, NoneDoesNotRoundTripUpstreamEither) {
  // "NONE" parses to code 0, which upstream's formatter has no case for.
  // Asserted against upstream so this records its behaviour, not just ours.
  const cdsmodel::Expected<long> parsed = reference_parse("NONE");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, 0);
  EXPECT_STREQ(JpmcdsFormatDayCountConv(0), "unrecognized");
  EXPECT_EQ(cdsmodel::parse_day_count_code("NONE"), 0L);
  EXPECT_EQ(cdsmodel::format_day_count_code(0), "unrecognized");
}

} // namespace
