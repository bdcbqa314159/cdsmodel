// TIER 1 -- replays tests/golden/date_from_string_cases.hpp, so the parser is
// covered in CI, where there is no oracle.
//
// Dates are integers, so the comparison is exact: a recorded TDate either
// matches or it does not, and crossing a compiler cannot perturb it. The
// accumulating tolerance tier has nothing to apply to here.

#include "golden/date_from_string_cases.hpp"

#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <optional>

namespace {

TEST(DateFromStringGolden, MatchesEveryRecordedCase) {
  int accepted = 0;
  int refused = 0;
  for (const cdsmodel::golden::DateFromStringCase &c : cdsmodel::golden::kDateFromStringCases) {
    const cdsmodel::Expected<cdsmodel::Date> got = cdsmodel::date_from_string(c.input);
    ASSERT_EQ(c.accepted, got.has_value()) << "acceptance differs for \"" << c.input << "\"";
    if (c.accepted) {
      ++accepted;
      EXPECT_EQ(c.tdate, cdsmodel::to_tdate(*got)) << "date differs for \"" << c.input << "\"";
    } else {
      ++refused;
    }
  }
  // The counts are computable from the file, so they are asserted rather than
  // bounded: a regenerated file that quietly lost its rejected cases would
  // otherwise still pass. These are the generator's own reported totals.
  EXPECT_EQ(accepted, 7342);
  EXPECT_EQ(refused, 170);
}

// A positive control. A sweep that only ever confirms agreement measures the
// fixture, not the parser -- so one recorded date is perturbed by a day and
// the comparison must notice.
TEST(DateFromStringGolden, TheComparisonWouldNoticeAWrongDate) {
  const cdsmodel::golden::DateFromStringCase *first_accepted = nullptr;
  for (const cdsmodel::golden::DateFromStringCase &c : cdsmodel::golden::kDateFromStringCases) {
    if (c.accepted) {
      first_accepted = &c;
      break;
    }
  }
  ASSERT_NE(first_accepted, nullptr) << "the file records no accepted case at all";
  const cdsmodel::Expected<cdsmodel::Date> got = cdsmodel::date_from_string(first_accepted->input);
  ASSERT_TRUE(got.has_value());
  EXPECT_NE(first_accepted->tdate + 1, cdsmodel::to_tdate(*got));
}

} // namespace
