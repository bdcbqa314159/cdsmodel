// TIER 2 -- live differential tests for the date layer. Built only when a
// local upstream copy exists.
//
// The first test here is the load-bearing one for the whole project: it
// proves that cdsmodel::Date and upstream's TDate denote the same day. Every
// layer above dates inherits that claim, so it is swept across six centuries
// rather than spot-checked.

#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

extern "C" {
#include "cerror.h"
#include "dateconv.h"
#include "ldate.h"
#include "mdydate.h"
}

namespace {

using namespace std::chrono;

class DateDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

// --- the epoch claim ------------------------------------------------------

TEST_F(DateDifferential, EpochMapsOneToOneAcrossSixCenturies) {
  // Every 13th day from 1601-01-01 to roughly 2200, so the stride lands on
  // every weekday and drifts through every month length and leap rule.
  for (TDate t = 0; t < 219000; t += 13) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    ASSERT_EQ(cdsmodel::to_tdate(d), t) << "round trip broken at TDate " << t;

    TMonthDayYear mdy{};
    ASSERT_EQ(JpmcdsDateToMDY(t, &mdy), SUCCESS) << "upstream rejected TDate " << t;

    const year_month_day ymd = cdsmodel::to_ymd(d);
    ASSERT_EQ(static_cast<int>(ymd.year()), static_cast<int>(mdy.year)) << "year at TDate " << t;
    ASSERT_EQ(static_cast<unsigned>(ymd.month()), static_cast<unsigned>(mdy.month))
        << "month at TDate " << t;
    ASSERT_EQ(static_cast<unsigned>(ymd.day()), static_cast<unsigned>(mdy.day))
        << "day at TDate " << t;
  }
}

TEST_F(DateDifferential, EpochDayItself) {
  TMonthDayYear mdy{};
  ASSERT_EQ(JpmcdsDateToMDY(0, &mdy), SUCCESS);
  EXPECT_EQ(mdy.year, 1601);
  EXPECT_EQ(mdy.month, 1);
  EXPECT_EQ(mdy.day, 1);
  EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::kDateEpoch), 0);
}

// --- construction ---------------------------------------------------------

TEST_F(DateDifferential, MakeDateMatchesUpstreamIncludingRejections) {
  // Deliberately includes invalid days and pre-epoch years: agreeing about
  // what is INVALID matters as much as agreeing about values.
  //
  // Month is swept 0..12, NOT 0..13. month == 13 reaches an out-of-bounds
  // read inside upstream (see UPSTREAM-1 in docs/port-notes.md), so there is
  // no defined behaviour there to compare against. The port's rejection of
  // month 13 is asserted in tests/date_test.cpp instead.
  const std::vector<int> years{1600, 1601, 1602, 1700, 1800, 1900, 1996,
                               2000, 2001, 2008, 2024, 2100, 2200};
  const std::vector<int> days{0, 1, 2, 27, 28, 29, 30, 31, 32};

  for (int y : years) {
    for (int m = 0; m <= 12; ++m) {
      for (int d : days) {
        const auto ported = cdsmodel::make_date(y, m, d);
        const TDate reference = JpmcdsDate(y, m, d);
        const bool reference_ok = reference != cdsmodel::kInvalidTDate;

        ASSERT_EQ(ported.has_value(), reference_ok)
            << "validity disagreement for " << y << "-" << m << "-" << d;
        if (reference_ok) {
          EXPECT_EQ(cdsmodel::to_tdate(*ported), reference)
              << "value disagreement for " << y << "-" << m << "-" << d;
        }
      }
    }
  }
}

TEST_F(DateDifferential, LeapDayHandling) {
  // 1700/1800/1900 are not leap years, 1600/2000 are. The 100/400 rules are
  // where hand-rolled calendars usually break.
  for (int y : {1700, 1800, 1900, 2000, 2024, 2100, 2400}) {
    const auto ported = cdsmodel::make_date(y, 2, 29);
    const TDate reference = JpmcdsDate(y, 2, 29);
    ASSERT_EQ(ported.has_value(), reference != cdsmodel::kInvalidTDate) << "year " << y;
    if (ported.has_value()) {
      EXPECT_EQ(cdsmodel::to_tdate(*ported), reference) << "year " << y;
    }
  }
}

// --- the rest of dateconv.c ----------------------------------------------

TEST_F(DateDifferential, DayOfWeekEncodingMatches) {
  for (TDate t = 0; t < 219000; t += 7919) { // prime stride, hits all residues
    long reference = -1;
    ASSERT_EQ(JpmcdsDayOfWeek(t, &reference), SUCCESS);
    EXPECT_EQ(static_cast<long>(cdsmodel::day_of_week(cdsmodel::from_tdate(t)).c_encoding()),
              reference)
        << "at TDate " << t;
  }
}

TEST_F(DateDifferential, DaysInMonthMatches) {
  for (int y = 1601; y <= 2200; ++y) {
    for (int m = 1; m <= 12; ++m) { // month 13 is an out-of-bounds read upstream
      EXPECT_EQ(cdsmodel::days_in_month(year{y} / month{static_cast<unsigned>(m)}),
                static_cast<unsigned>(JpmcdsDaysInMonth(y, m)))
          << y << "-" << m;
    }
  }
}

TEST_F(DateDifferential, EndOfMonthMatches) {
  for (TDate t = 0; t < 219000; t += 29) {
    TDate reference = 0;
    ASSERT_EQ(JpmcdsDateToEOM(t, &reference), SUCCESS) << "at TDate " << t;
    EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::end_of_month(cdsmodel::from_tdate(t))), reference)
        << "at TDate " << t;
  }
}

TEST_F(DateDifferential, NormalizeMatchesIncludingClampAndReject) {
  for (int y : {1601, 1999, 2000, 2024}) {
    for (int m = -14; m <= 26; ++m) {
      for (int d : {0, 1, 28, 29, 30, 31, 32}) {
        TMonthDayYear mdy{m, d, y};
        const bool reference_ok = JpmcdsNormalizeMDY(&mdy) == SUCCESS;
        const auto ported = cdsmodel::normalize_ymd(y, m, d);

        ASSERT_EQ(ported.has_value(), reference_ok)
            << "validity disagreement for " << y << "-" << m << "-" << d;
        if (reference_ok) {
          EXPECT_EQ(static_cast<int>(ported->year()), static_cast<int>(mdy.year))
              << "year for " << y << "-" << m << "-" << d;
          EXPECT_EQ(static_cast<unsigned>(ported->month()), static_cast<unsigned>(mdy.month))
              << "month for " << y << "-" << m << "-" << d;
          EXPECT_EQ(static_cast<unsigned>(ported->day()), static_cast<unsigned>(mdy.day))
              << "day for " << y << "-" << m << "-" << d;
        }
      }
    }
  }
}

TEST_F(DateDifferential, IsLeapMatchesUpstream) {
  // Note JpmcdsIsLeap takes a DATE, not a year -- it reports whether the
  // year containing that date is a leap year. Easy to misread as taking a
  // year, since a small TDate and a year are both just longs.
  for (int y = 1601; y <= 2400; ++y) {
    const cdsmodel::Date jan1 = *cdsmodel::make_date(y, 1, 1);
    long reference = -1;
    ASSERT_EQ(JpmcdsIsLeap(cdsmodel::to_tdate(jan1), &reference), SUCCESS) << "year " << y;
    EXPECT_EQ(cdsmodel::is_leap(year{y}), reference != 0) << "year " << y;
  }
}

} // namespace
