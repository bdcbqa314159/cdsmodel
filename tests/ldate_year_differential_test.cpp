// TIER 2 -- live differential tests for ldate.c's four year helpers.
//
// All four are pure functions of a date, so the sweep is over DATES rather
// than over flags: every day of several full leap cycles, plus the century
// boundaries where the Gregorian rule's two exceptions interact.
//
// These four were the last unclaimed functions in ldate.c, and they were
// unclaimed for a reason worth recording: they exist to serve
// `JpmcdsDayCountFraction`'s ACT/ACT branch, which walks whole years, and the
// port's `year_fraction` intersects the period with each calendar year
// instead. So nothing in the port needs them -- but all four are declared in
// `ldate.h`, which makes them public API.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <gtest/gtest.h>

#include <optional>

extern "C" {
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
}

namespace {

class LdateYearDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

TEST_F(LdateYearDifferential, AllFourAgreeOverEveryDayOfSeveralLeapCycles) {
  int compared = 0;
  int leap_days_seen = 0;
  // 1896-2104 covers 1900 (divisible by 100, not a leap year), 2000
  // (divisible by 400, IS one) and 2100 (not one again) -- the three cases
  // that distinguish the Gregorian rule from "every fourth year".
  const cdsmodel::Date first = *cdsmodel::make_date(1896, 1, 1);
  const cdsmodel::Date last = *cdsmodel::make_date(2104, 12, 31);

  for (cdsmodel::Date d = first; d <= last; d += std::chrono::days{1}) {
    const long raw = cdsmodel::to_tdate(d);

    int ref_year = -1;
    ASSERT_EQ(JpmcdsYear(raw, &ref_year), SUCCESS) << raw;
    EXPECT_EQ(ref_year, cdsmodel::year_of(d)) << raw;

    long ref_leap = -1;
    ASSERT_EQ(JpmcdsIsLeap(raw, &ref_leap), SUCCESS) << raw;
    EXPECT_EQ(ref_leap == 1, cdsmodel::is_leap_year(d)) << raw;

    TDate ref_start = 0;
    ASSERT_EQ(JpmcdsYearStart(raw, &ref_start), SUCCESS) << raw;
    EXPECT_EQ(ref_start, cdsmodel::to_tdate(cdsmodel::year_start(d))) << raw;

    const std::chrono::year_month_day ymd = cdsmodel::to_ymd(d);
    if (ymd.month() == std::chrono::February && ymd.day() == std::chrono::day{29}) {
      ++leap_days_seen;
    }
    ++compared;
  }

  EXPECT_GT(compared, 76000) << "the sweep did not cover the intended range";
  // 1896..2104 inclusive: 53 multiples of four ((2104-1896)/4 + 1), minus
  // 1900 and 2100, which are divisible by 100 and not by 400. I first wrote 50
  // here by miscounting the multiples; the sweep said 51 and the sweep was
  // right.
  EXPECT_EQ(leap_days_seen, 51) << "the leap days in range changed";
}

TEST_F(LdateYearDifferential, DaysLeftThisYearAgreesAcrossEveryConvention) {
  int compared = 0;
  int differing_by_convention = 0;
  // A year's worth of dates is enough: the function's answer depends on the
  // date within its year and on the convention, not on the year itself --
  // except across a leap boundary, so two years are swept.
  const cdsmodel::Date first = *cdsmodel::make_date(2024, 1, 1);
  const cdsmodel::Date last = *cdsmodel::make_date(2025, 12, 31);

  for (cdsmodel::Date d = first; d <= last; d += std::chrono::days{1}) {
    const long raw = cdsmodel::to_tdate(d);
    long seen_act = -1;
    long seen_thirty = -1;
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
          cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360}) {
      long ref = -999;
      const bool accepted =
          JpmcdsDaysLeftThisYear(raw, static_cast<int>(cdsmodel::to_upstream_code(dcc)), &ref) ==
          SUCCESS;
      const std::optional<long> got = cdsmodel::days_left_this_year(d, dcc);
      ASSERT_EQ(accepted, got.has_value()) << raw;
      if (!accepted) {
        continue;
      }
      EXPECT_EQ(ref, *got) << raw << " dcc " << cdsmodel::to_upstream_code(dcc);
      if (dcc == cdsmodel::DayCount::Act365Fixed) {
        seen_act = ref;
      }
      if (dcc == cdsmodel::DayCount::Thirty360) {
        seen_thirty = ref;
      }
      ++compared;
    }
    if (seen_act != seen_thirty) {
      ++differing_by_convention;
    }
  }
  EXPECT_GT(compared, 2500);
  // The point of the name's dishonesty: the answer is a DAY COUNT, not a count
  // of days, so ACT and 30/360 disagree for most of the year.
  EXPECT_GT(differing_by_convention, 300)
      << "ACT and 30/360 agreed everywhere, so the convention is being ignored";
}

// The specific case the name gets wrong, found by measuring rather than by
// predicting.
//
// I expected 31 December to be the clean illustration: one day left under ACT,
// zero under 30/360. It is not -- 30/360 USA adjusts D1 from 31 to 30, so the
// difference from 31 December to 1 January is 360 - 330 - 29 = 1 under both
// conventions, and the two AGREE exactly where I expected them to diverge.
//
// 1 January is the clean case: 365 real days to the next new year against
// 30/360's 360.
TEST_F(LdateYearDifferential, TheConventionChangesTheAnswerAwayFromTheBoundary) {
  const cdsmodel::Date jan1 = *cdsmodel::make_date(2025, 1, 1);
  const long raw = cdsmodel::to_tdate(jan1);

  long ref_act = -999;
  ASSERT_EQ(JpmcdsDaysLeftThisYear(raw, JPMCDS_ACT_365F, &ref_act), SUCCESS);
  EXPECT_EQ(ref_act, 365) << "2025 is not a leap year, so ACT sees 365 days";

  long ref_thirty = -999;
  ASSERT_EQ(JpmcdsDaysLeftThisYear(raw, JPMCDS_B30_360, &ref_thirty), SUCCESS);
  EXPECT_EQ(ref_thirty, 360) << "30/360 sees twelve thirty-day months";

  EXPECT_EQ(cdsmodel::days_left_this_year(jan1, cdsmodel::DayCount::Act365Fixed),
            std::optional<long>{365});
  EXPECT_EQ(cdsmodel::days_left_this_year(jan1, cdsmodel::DayCount::Thirty360),
            std::optional<long>{360});

  // And the boundary where they coincide, recorded because it is where I
  // expected them to differ.
  const cdsmodel::Date nye = *cdsmodel::make_date(2024, 12, 31);
  EXPECT_EQ(cdsmodel::days_left_this_year(nye, cdsmodel::DayCount::Act365Fixed),
            std::optional<long>{1});
  EXPECT_EQ(cdsmodel::days_left_this_year(nye, cdsmodel::DayCount::Thirty360),
            std::optional<long>{1})
      << "30/360 adjusts D1 from 31 to 30, which makes this agree with ACT";

  // A leap year has one more ACT day and the same 30/360 count.
  const cdsmodel::Date leap_jan1 = *cdsmodel::make_date(2024, 1, 1);
  EXPECT_EQ(cdsmodel::days_left_this_year(leap_jan1, cdsmodel::DayCount::Act365Fixed),
            std::optional<long>{366});
  EXPECT_EQ(cdsmodel::days_left_this_year(leap_jan1, cdsmodel::DayCount::Thirty360),
            std::optional<long>{360});
}

// JpmcdsIsLeap takes a DATE and the JPMCDS_IS_LEAP macro takes a YEAR; the
// function routes through JpmcdsDateToMDY to get what the macro wants. The
// port expresses that as a composition, and the two must agree.
TEST_F(LdateYearDifferential, TheDateFormAndTheYearFormAgree) {
  for (int y = 1896; y <= 2104; ++y) {
    const cdsmodel::Date jan1 = *cdsmodel::make_date(y, 1, 1);
    long ref = -1;
    ASSERT_EQ(JpmcdsIsLeap(cdsmodel::to_tdate(jan1), &ref), SUCCESS) << y;
    EXPECT_EQ(ref == 1, cdsmodel::is_leap_year(jan1)) << y;
    EXPECT_EQ(cdsmodel::is_leap_year(jan1), cdsmodel::is_leap(std::chrono::year{y})) << y;
  }
}

} // namespace
