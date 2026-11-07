// TIER 2 -- live differential tests for the date-string parser.
//
// A parser's domain is strings, so the sweep is over SHAPES: the four
// documented formats, the delimiters the documentation omits, two-digit-year
// pivots, the rejected year band, trailing junk, and malformed input of every
// kind I could think of. Everything is compared on acceptance AND on the date
// produced, because a parser that accepts the right set and returns the wrong
// day is the failure mode that matters.
//
// One input is NOT swept here: it triggers UPSTREAM-38, a stack buffer
// overflow in upstream's month-name helper. It has its own DISABLED test.

#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "ldate.h"
}

namespace {

class DateFromStringDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

/// Compare both sides on one input, returning whether upstream accepted it.
bool compare(const std::string &input) {
  std::string mutable_copy = input; // upstream takes char* and uses strtok
  TDate ref = -999;
  const bool ref_ok = JpmcdsStringToDate(mutable_copy.data(), &ref) == SUCCESS;
  const std::optional<cdsmodel::Date> got = cdsmodel::date_from_string(input);

  EXPECT_EQ(ref_ok, got.has_value()) << "acceptance differs for \"" << input << "\"";
  if (ref_ok && got) {
    EXPECT_EQ(ref, cdsmodel::to_tdate(*got)) << "date differs for \"" << input << "\"";
  }
  return ref_ok;
}

TEST_F(DateFromStringDifferential, MatchesAcrossEveryShapeICouldThinkOf) {
  const std::vector<std::string> inputs{
      // the four documented formats
      "20240315",
      "03/15/2024",
      "Mar-15-2024",
      "15-Mar-2024",
      // the delimiter set is "-/. ", which the documentation does not say
      "03.15.2024",
      "03 15 2024",
      "03-15-2024",
      "3/15/2024",
      // mixed delimiters, which upstream's strtok does not mind
      "03/15-2024",
      "03.15/2024",
      // month names: case, length, every month
      "jan-15-2024",
      "JAN-15-2024",
      "Jan-15-2024",
      "jAn-15-2024",
      "January-15-2024",
      "ja-15-2024",
      "j-15-2024",
      "janu-15-2024",
      "feb-29-2024",
      "feb-29-2023",
      "dec-31-2024",
      "xyz-15-2024",
      // two names, which the isMonth guard refuses
      "Jan-Feb-2024",
      // two-digit years and the crossover at 60
      "1/1/59",
      "1/1/60",
      "1/1/99",
      "1/1/00",
      "1/1/01",
      // the rejected band, and its edges
      "1/1/99",
      "1/1/100",
      "1/1/150",
      "1/1/1599",
      "1/1/1600",
      "1/1/1601",
      "1/1/1602",
      // trailing tokens, which upstream silently ignores
      "1-1-2024-GARBAGE",
      "1/1/2024/99/99",
      "1-1-2024-",
      // too few tokens
      "1-1",
      "1",
      "",
      "-",
      "---",
      "1--",
      // the undelimited form needs exactly eight characters
      "2024031",
      "202403150",
      "0240315",
      "00240315",
      "20240000",
      "20241315",
      "20240230",
      "20240229",
      "20230229",
      "20241131",
      "20241130",
      // zero components in the undelimited form
      "00001015",
      "20240015",
      "20240300",
      // non-numeric where a number is wanted
      "aa/15/2024",
      "03/aa/2024",
      "abcdefgh", // "03/15/aaaa" diverges: UPSTREAM-39
      // whitespace, which is a delimiter here
      " 03 15 2024 ",
      "03  15  2024",
      // day and month out of range
      "13/15/2024",
      "03/32/2024",
      "00/15/2024",
      "03/00/2024",
      // negative-looking input
      "-3/15/2024",
      "3/-15/2024",
      // the epoch and its neighbourhood
      "16010101",
      "16001231",
      "1-1-1601",
  };

  int accepted = 0;
  int rejected = 0;
  for (const std::string &input : inputs) {
    if (compare(input)) {
      ++accepted;
    } else {
      ++rejected;
    }
  }
  // Both outcomes must occur in quantity, or the sweep is only testing one
  // branch of the parser.
  EXPECT_GT(accepted, 25) << "almost nothing was accepted";
  EXPECT_GT(rejected, 25) << "almost nothing was rejected";
}

// Every day of a few years through all four documented formats, which is the
// part a shape-based sweep cannot cover: the arithmetic.
TEST_F(DateFromStringDifferential, MatchesForEveryDayAcrossAllFourFormats) {
  constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int compared = 0;
  for (const int year : {2023, 2024, 2100, 2000, 1999}) {
    for (int month = 1; month <= 12; ++month) {
      for (int day = 1; day <= 31; ++day) {
        if (!cdsmodel::make_date(year, month, day)) {
          continue; // 31 April and friends; the invalid-input sweep covers those
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%04d%02d%02d", year, month, day);
        compare(buf);
        std::snprintf(buf, sizeof buf, "%02d/%02d/%04d", month, day, year);
        compare(buf);
        std::snprintf(buf, sizeof buf, "%s-%02d-%04d", kMonths[month - 1], day, year);
        compare(buf);
        std::snprintf(buf, sizeof buf, "%02d-%s-%04d", day, kMonths[month - 1], year);
        compare(buf);
        compared += 4;
      }
    }
  }
  EXPECT_GT(compared, 7000) << "the day-by-day sweep did not run";
}

// The 31-character bound is real -- `strncpy` truncates rather than
// overflowing -- and the security-relevant consequence is that a long input
// is ACCEPTED, not refused. This is the contrast with UPSTREAM-4, an
// unbounded copy 225 lines earlier in the same file.
TEST_F(DateFromStringDifferential, LongInputIsTruncatedNotRejected) {
  const std::string padded = "03/15/2024" + std::string(40, 'X');
  std::string copy = padded;
  TDate ref = -999;
  EXPECT_EQ(JpmcdsStringToDate(copy.data(), &ref), SUCCESS)
      << "upstream stopped accepting over-long input";
  EXPECT_EQ(ref, cdsmodel::to_tdate(*cdsmodel::make_date(2024, 3, 15)));
  // Why acceptance rather than agreement is asserted: this input survives
  // only because of the `atol` junk tolerance below, so the port refuses it.
  // Truncation on its own has no observable case -- every input whose answer
  // the 31-byte cut would change runs through `atol` on a 27-digit token,
  // which overflows a signed long. Measured: "1/1/" + 27 twos yields
  // 1601-01-01, an answer produced by undefined behaviour. Not pinned.
  EXPECT_FALSE(cdsmodel::date_from_string(padded).has_value());
}

// ------------------------------------------------------------ UPSTREAM-39

// Both sides measured on the same inputs, asserting the DIVERGENCE rather
// than agreement: upstream accepts an unparseable year, the port refuses it.
// Every expected year here was MEASURED, not predicted -- two of my
// predictions were wrong, including "99bogus", which lands in 1999 and not
// 2099 because 99 is above the pivot and so takes the +1900 branch.
TEST_F(DateFromStringDifferential, Upstream39UnparseableYearIsAcceptedUpstreamRefusedHere) {
  struct Case {
    const char *input;
    int year; // what atol's silent 0-or-prefix produces
    int month;
    int day;
  };
  const Case cases[]{
      // No digits at all: atol returns 0, indistinguishable from a parsed
      // zero, and 0 is below CROSSOVER_YEAR -- so the year is INVENTED.
      {"03/15/aaaa", 2000, 3, 15},
      {"15-Mar-twenty", 2000, 3, 15},
      {"1-1-x2024", 2000, 1, 1},
      // Leading digits, then junk: atol stops at the non-digit, silently.
      {"03/15/2024XX", 2024, 3, 15},
      {"03/15/99bogus", 1999, 3, 15},
  };

  for (const Case &c : cases) {
    std::string copy = c.input;
    TDate ref = -999;
    ASSERT_EQ(JpmcdsStringToDate(copy.data(), &ref), SUCCESS)
        << "upstream now refuses \"" << c.input << "\"; UPSTREAM-39 may be fixed";
    EXPECT_EQ(ref, cdsmodel::to_tdate(*cdsmodel::make_date(c.year, c.month, c.day)))
        << "upstream's invented year changed for \"" << c.input << "\"";
    EXPECT_FALSE(cdsmodel::date_from_string(c.input).has_value())
        << "the port accepted the unparseable year in \"" << c.input << "\"";
  }

  // The contrast that makes this an inconsistency and not a convention: the
  // month and day fields, parsed by the same function a few lines earlier,
  // DO detect an unparseable token and refuse.
  for (const char *input : {"aa/15/2024", "03/aa/2024"}) {
    std::string copy = input;
    TDate ref = -999;
    EXPECT_NE(JpmcdsStringToDate(copy.data(), &ref), SUCCESS)
        << input << ": month/day no longer refuse, so the inconsistency is gone";
  }
}

// ------------------------------------------------------------ UPSTREAM-38

// The precondition, measured without touching the overflow: the month-name
// helper only ever matches a three-character token, so every shorter one is
// refused -- and refused only AFTER three bytes have been lowercased in place.
TEST_F(DateFromStringDifferential, Upstream38ShortMonthTokensAreRefusedAfterTheWrite) {
  // Safe positions: these tokens sit near the start of the buffer, so the
  // third byte is still inside it.
  for (const char *input : {"j-15-2024", "ja-15-2024", "janu-15-2024"}) {
    std::string copy = input;
    TDate ref = -999;
    EXPECT_NE(JpmcdsStringToDate(copy.data(), &ref), SUCCESS)
        << input << " was accepted as a month name";
    EXPECT_FALSE(cdsmodel::date_from_string(input).has_value()) << input;
  }
  // The port checks the length BEFORE touching a character, so there is no
  // position at which a short token is unsafe.
  EXPECT_FALSE(cdsmodel::date_from_string(std::string(29, '1') + "-a").has_value());
}

TEST_F(DateFromStringDifferential, DISABLED_Upstream38ShortTokenAtTheBufferEndOverflows) {
  // 29 digits keep %ld satisfied for the first token, pushing the second --
  // the one NStringToMonth sees -- to offset 30 of a 32-byte buffer. Its
  // unconditional `cp[2] = tolower(cp[2])` then touches dateCopy[32].
  // ASan: stack-buffer-overflow at convert.c:395. Never enabled.
  std::string input = std::string(29, '1') + "-a";
  TDate out = -999;
  JpmcdsStringToDate(input.data(), &out);
}

} // namespace
