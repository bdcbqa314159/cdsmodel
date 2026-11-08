// TIER 2 -- live differential tests for the interval/date supplements.
//
// Two jobs beyond the usual agreement sweep.
//
// First, a normalisation argument has to be CHECKED rather than asserted.
// `make_date_interval` collapses every unit letter to days or months at
// construction, so `interval_to_years` has two branches where upstream has
// eleven. The claim is that normalisation divides numerator and denominator by
// the same factor and so the correctly-rounded quotient is unchanged -- 'Q'
// with 5 periods becomes 15 months, and 15/12 equals 5/4 exactly. That is an
// argument; the sweep below is the evidence.
//
// Second, UPSTREAM-12: 'U' computes the right answer and then falls through a
// missing `break` into the error branch. Asserted as a disagreement so an
// upstream fix becomes visible.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "convert.h"
#include "date_sup.h"
#include "dateadj.h"
#include "dateconv.h"
#include "ldate.h"
}

namespace {

class DateSupDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  void TearDown() override { JpmcdsHolidayEmptyCache(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Upstream's interval, built the way a caller builds it.
///
/// This matters more than it looks. `JpmcdsMakeDateInterval` normalises the
/// exotic letters -- 'A' and 'Y' to 12 months, 'S' to 6, 'Q' to 3, 'W' to 7
/// days, **'U' to 28 days** -- which is precisely what the port's
/// `make_date_interval` does. A raw `TDateInterval{n, 'U', 0}` is a different
/// animal: `JpmcdsDtFwdAny` refuses it and `JpmcdsDateIntervalToYears`
/// mishandles it (UPSTREAM-12).
///
/// The first version of this file compared the port's normalised interval
/// against hand-built raw structs and reported three failures. The port was
/// right; the comparison was not like for like. Hand-built structs now appear
/// only in the UPSTREAM-12 test, where they are the point.
cdsmodel::Expected<TDateInterval> reference_interval(int periods, char letter) {
  TDateInterval iv{};
  if (JpmcdsMakeDateInterval(periods, letter, &iv) != SUCCESS) {
    return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
  }
  return iv;
}

/// Every unit letter upstream's JpmcdsDateIntervalToYears handles, with the
/// ones the port's parser accepts flagged.
struct Unit {
  char letter;
  bool port_accepts;
};
constexpr Unit kUnits[]{{'A', true},  {'Y', true},  {'S', true},  {'Q', true},  {'M', true},
                        {'W', true},  {'D', true},  {'U', true},  {'I', false}, {'K', false},
                        {'L', false}, {'E', false}, {'F', false}, {'G', false}, {'H', false},
                        {'J', false}, {'T', false}};

TEST_F(DateSupDifferential, IntervalToYearsSurvivesNormalisation) {
  // The evidence for the normalisation argument. For every letter the port
  // accepts, the normalised interval must give bit-identically what upstream
  // computes from the raw letter.
  int compared = 0;
  for (const Unit &u : kUnits) {
    if (!u.port_accepts) {
      continue;
    }
    for (int periods : {1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 24, 37, 100, 365, 1000}) {
      const cdsmodel::Expected<TDateInterval> built = reference_interval(periods, u.letter);
      ASSERT_TRUE(built.has_value()) << periods << u.letter;
      TDateInterval raw = *built;
      double expected = 0.0;
      const bool ref_ok = JpmcdsDateIntervalToYears(&raw, &expected) == SUCCESS;

      const cdsmodel::Expected<cdsmodel::DateInterval> iv =
          cdsmodel::make_date_interval(periods, u.letter);
      ASSERT_TRUE(iv.has_value()) << periods << u.letter;
      const double got = cdsmodel::interval_to_years(*iv);

      // Built through the constructor, 'U' arrives as days and works, so
      // every accepted letter must agree. UPSTREAM-12 is exercised separately.
      ASSERT_TRUE(ref_ok) << periods << u.letter;
      EXPECT_EQ(got, expected) << periods << u.letter << ": normalisation changed the quotient";
      ++compared;
    }
  }
  EXPECT_GT(compared, 90) << "sweep thinner than expected: " << compared;
}

TEST_F(DateSupDifferential, Upstream12_LunarMonthComputesCorrectlyThenFails) {
  // Isolated so the finding is legible, and REACHABLE ONLY through a
  // hand-built TDateInterval -- JpmcdsMakeDateInterval turns 'U' into 28
  // days, after which the 'D' branch handles it correctly. Upstream itself
  // contemplates hand-built intervals: JpmcdsDtFwdAny keeps code for A, S and
  // Q with the comment "left here for people who have set up their own
  // TDateIntervals without calling JpmcdsMakeDateInterval". It extends that
  // courtesy to 'W' and not to 'U'.
  //
  // A genuinely unknown letter leaves the output untouched; 'U' writes 28/365
  // and still returns FAILURE. That difference is what proves the computation
  // ran and only the break is gone.
  TDateInterval lunar{1, 'U', 0};
  double years = -999.0;
  EXPECT_NE(JpmcdsDateIntervalToYears(&lunar, &years), SUCCESS);
  EXPECT_DOUBLE_EQ(years, 28.0 / 365.0) << "'U' did not compute its value before failing";

  TDateInterval unknown{1, 'Z', 0};
  double untouched = -999.0;
  EXPECT_NE(JpmcdsDateIntervalToYears(&unknown, &untouched), SUCCESS);
  EXPECT_EQ(untouched, -999.0) << "an unknown letter should not write anything";

  // And it propagates: frequency and date counting both consume it.
  double freq = 0.0;
  EXPECT_NE(JpmcdsDateIntervalToFreq(&lunar, &freq), SUCCESS);

  // The port has no such case: 'U' normalises to 28 days and works.
  const cdsmodel::Expected<cdsmodel::DateInterval> iv = cdsmodel::make_date_interval(1, 'U');
  ASSERT_TRUE(iv.has_value());
  EXPECT_EQ(iv->unit, cdsmodel::PeriodUnit::Days);
  EXPECT_EQ(iv->periods, 28);
  EXPECT_DOUBLE_EQ(cdsmodel::interval_to_years(*iv), 28.0 / 365.0);
  EXPECT_TRUE(cdsmodel::interval_to_frequency(*iv).has_value());
}

TEST_F(DateSupDifferential, FrequencyConversionsMatchIncludingTheIntegerTruncation) {
  for (long freq = -2; freq <= 15; ++freq) {
    TDateInterval expected{};
    const bool ref_ok = JpmcdsFreq2TDateInterval(freq, &expected) == SUCCESS;
    const cdsmodel::Expected<cdsmodel::DateInterval> got = cdsmodel::frequency_to_interval(freq);
    ASSERT_EQ(got.has_value(), ref_ok) << freq;
    if (!ref_ok) {
      continue;
    }
    EXPECT_EQ(got->periods, expected.prd) << freq;
    EXPECT_EQ(got->unit, cdsmodel::PeriodUnit::Months) << freq;
    EXPECT_EQ(expected.prd_typ, 'M') << freq;
  }
  // The truncation, pinned: 12/5 is 2 in integer arithmetic, and 7 through 11
  // all collapse to 1.
  EXPECT_EQ(cdsmodel::frequency_to_interval(5)->periods, 2);
  for (long freq : {7, 8, 9, 10, 11}) {
    EXPECT_EQ(cdsmodel::frequency_to_interval(freq)->periods, 1) << freq;
  }
}

TEST_F(DateSupDifferential, IntervalToFrequencyMatches) {
  int compared = 0;
  for (const Unit &u : kUnits) {
    if (!u.port_accepts) {
      continue;
    }
    for (int periods : {1, 2, 3, 6, 12, 24}) {
      const cdsmodel::Expected<TDateInterval> built = reference_interval(periods, u.letter);
      ASSERT_TRUE(built.has_value());
      TDateInterval raw = *built;
      double expected = 0.0;
      const bool ref_ok = JpmcdsDateIntervalToFreq(&raw, &expected) == SUCCESS;
      const cdsmodel::Expected<cdsmodel::DateInterval> iv =
          cdsmodel::make_date_interval(periods, u.letter);
      ASSERT_TRUE(iv.has_value());
      const cdsmodel::Expected<double> got = cdsmodel::interval_to_frequency(*iv);
      ASSERT_EQ(got.has_value(), ref_ok) << periods << u.letter;
      if (ref_ok) {
        EXPECT_EQ(*got, expected) << periods << u.letter;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 30) << "sweep thinner than expected: " << compared;
}

TEST_F(DateSupDifferential, DateFromDateAndOffsetMatches) {
  int compared = 0;
  for (const Unit &u : kUnits) {
    if (!u.port_accepts) {
      continue;
    }
    for (int periods : {1, 3, 6}) {
      const cdsmodel::Expected<cdsmodel::DateInterval> iv =
          cdsmodel::make_date_interval(periods, u.letter);
      ASSERT_TRUE(iv.has_value());
      TDateInterval raw = *reference_interval(periods, u.letter);
      for (cdsmodel::Date start :
           {d(2024, 1, 31), d(2024, 2, 29), d(2024, 6, 15), d(1999, 12, 31), d(2030, 3, 1)}) {
        for (int index : {-40, -7, -1, 0, 1, 5, 40, 200}) {
          TDate expected = 0;
          const bool ref_ok = JpmcdsDateFromDateAndOffset(cdsmodel::to_tdate(start), &raw, index,
                                                          &expected) == SUCCESS;
          const cdsmodel::Expected<cdsmodel::Date> got =
              cdsmodel::date_from_date_and_offset(start, *iv, index);
          const std::string where = std::to_string(periods) + u.letter + " from " +
                                    std::to_string(cdsmodel::to_tdate(start)) + " x " +
                                    std::to_string(index);
          ASSERT_EQ(got.has_value(), ref_ok) << where;
          if (ref_ok) {
            EXPECT_EQ(cdsmodel::to_tdate(*got), expected) << where;
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 500) << "sweep thinner than expected: " << compared;
}

TEST_F(DateSupDifferential, MakeDateArrayMatches) {
  for (const Unit &u : kUnits) {
    if (!u.port_accepts) {
      continue;
    }
    const cdsmodel::Expected<cdsmodel::DateInterval> iv = cdsmodel::make_date_interval(1, u.letter);
    ASSERT_TRUE(iv.has_value());
    TDateInterval raw = *reference_interval(1, u.letter);
    for (int start_index : {0, 1, 3}) {
      for (int count : {0, 1, 5, 20}) {
        std::vector<TDate> expected(static_cast<std::size_t>(count == 0 ? 1 : count));
        const bool ref_ok = JpmcdsMakeTDateArray(cdsmodel::to_tdate(d(2024, 1, 31)), &raw,
                                                 start_index, 1, count, expected.data()) == SUCCESS;
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
            cdsmodel::make_date_array(d(2024, 1, 31), *iv, start_index, count);
        ASSERT_EQ(got.has_value(), ref_ok) << u.letter << " start " << start_index;
        if (!ref_ok) {
          continue;
        }
        ASSERT_EQ(static_cast<int>(got->size()), count);
        for (int i = 0; i < count; ++i) {
          EXPECT_EQ(cdsmodel::to_tdate((*got)[static_cast<std::size_t>(i)]),
                    expected[static_cast<std::size_t>(i)])
              << u.letter << " i=" << i;
        }
      }
    }
  }
}

TEST_F(DateSupDifferential, CountDatesMatchesAcrossSpansAndIntervals) {
  int compared = 0;
  int rejected_by_both = 0;
  for (const Unit &u : kUnits) {
    if (!u.port_accepts) {
      continue;
    }
    for (int periods : {1, 2, 3, 6, -1, -3}) {
      const cdsmodel::Expected<cdsmodel::DateInterval> iv =
          cdsmodel::make_date_interval(periods, u.letter);
      if (!iv) {
        continue;
      }
      TDateInterval raw = *reference_interval(periods, u.letter);
      for (cdsmodel::Date from : {d(2024, 1, 31), d(2020, 2, 29), d(2024, 6, 15)}) {
        for (cdsmodel::Date to :
             {d(2024, 1, 31), d(2024, 7, 15), d(2029, 6, 20), d(2019, 3, 1), d(2044, 1, 1)}) {
          int ref_n = 0;
          int ref_extra = 0;
          const bool ref_ok = JpmcdsCountDates(cdsmodel::to_tdate(from), cdsmodel::to_tdate(to),
                                               &raw, &ref_n, &ref_extra) == SUCCESS;
          const cdsmodel::Expected<cdsmodel::DateCount> got = cdsmodel::count_dates(from, to, *iv);
          const std::string where = std::to_string(periods) + u.letter + " " +
                                    std::to_string(cdsmodel::to_tdate(from)) + " -> " +
                                    std::to_string(cdsmodel::to_tdate(to));
          ASSERT_EQ(got.has_value(), ref_ok) << where;
          if (!ref_ok) {
            ++rejected_by_both;
            continue;
          }
          EXPECT_EQ(got->intervals, ref_n) << where;
          EXPECT_EQ(got->extra_days, ref_extra) << where;
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 150) << "sweep thinner than expected: " << compared;
  EXPECT_GT(rejected_by_both, 10) << "the rejection paths were barely exercised";
}

TEST_F(DateSupDifferential, DateFwdThenAdjustMatches) {
  const cdsmodel::Calendar cal = *cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard);
  int compared = 0;
  for (cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified}) {
    for (const Unit &u : kUnits) {
      if (!u.port_accepts) {
        continue;
      }
      const cdsmodel::Expected<cdsmodel::DateInterval> iv =
          cdsmodel::make_date_interval(2, u.letter);
      ASSERT_TRUE(iv.has_value());
      TDateInterval raw = *reference_interval(2, u.letter);
      for (cdsmodel::Date start :
           {d(2024, 1, 31), d(2024, 3, 23), d(2024, 11, 29), d(2024, 6, 15)}) {
        TDate expected = 0;
        const bool ref_ok =
            JpmcdsDateFwdThenAdjust(cdsmodel::to_tdate(start), &raw,
                                    static_cast<long>(static_cast<char>(conv)),
                                    const_cast<char *>("NONE"), &expected) == SUCCESS;
        const cdsmodel::Expected<cdsmodel::Date> got =
            cdsmodel::date_fwd_then_adjust(start, *iv, conv, cal);
        ASSERT_EQ(got.has_value(), ref_ok) << u.letter << " " << static_cast<char>(conv);
        if (ref_ok) {
          EXPECT_EQ(cdsmodel::to_tdate(*got), expected)
              << u.letter << " " << static_cast<char>(conv) << " from "
              << cdsmodel::to_tdate(start);
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 100) << "sweep thinner than expected: " << compared;
}

TEST_F(DateSupDifferential, AdvanceAdjustedMatchesOnEveryBasis) {
  const cdsmodel::Calendar cal = *cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard);
  struct Basis {
    cdsmodel::OffsetBasis port;
    int upstream;
    char letter; // the unit letter that selects this path upstream
  };
  const Basis bases[]{{cdsmodel::OffsetBasis::BusinessDays, JPMCDS_DATE_ADJ_TYPE_BUSINESS, 'D'},
                      {cdsmodel::OffsetBasis::BusinessPeriod, JPMCDS_DATE_ADJ_TYPE_BUSINESS, 'M'},
                      {cdsmodel::OffsetBasis::CalendarPeriod, JPMCDS_DATE_ADJ_TYPE_CALENDAR, 'M'},
                      {cdsmodel::OffsetBasis::CalendarPeriod, JPMCDS_DATE_ADJ_TYPE_CALENDAR, 'D'},
                      {cdsmodel::OffsetBasis::Weekdays, JPMCDS_DATE_ADJ_TYPE_WEEKDAY, 'D'}};

  int compared = 0;
  for (const Basis &b : bases) {
    for (int periods : {1, 2, 5, -2}) {
      for (cdsmodel::BadDayConv conv : {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
                                        cdsmodel::BadDayConv::Modified}) {
        for (cdsmodel::Date start :
             {d(2024, 1, 31), d(2024, 3, 22), d(2024, 2, 29), d(2024, 6, 15)}) {
          TDateAdjIntvl raw{};
          raw.interval.prd = periods;
          raw.interval.prd_typ = b.letter;
          raw.interval.flag = 0;
          raw.isBusDays = b.upstream;
          raw.holidayFile = const_cast<char *>("NONE");
          raw.badDayConv = static_cast<long>(static_cast<char>(conv));
          TDate expected = 0;
          const bool ref_ok = JpmcdsDtFwdAdj(cdsmodel::to_tdate(start), &raw, &expected) == SUCCESS;

          const cdsmodel::Expected<cdsmodel::DateInterval> iv =
              cdsmodel::make_date_interval(periods, b.letter);
          ASSERT_TRUE(iv.has_value());
          const cdsmodel::AdjustedOffset offset{*iv, b.port, conv};
          const cdsmodel::Expected<cdsmodel::Date> got =
              cdsmodel::advance_adjusted(start, offset, cal);

          const std::string where = std::to_string(periods) + b.letter + " basis " +
                                    std::to_string(static_cast<int>(b.port)) + " conv " +
                                    std::string(1, static_cast<char>(conv)) + " from " +
                                    std::to_string(cdsmodel::to_tdate(start));
          ASSERT_EQ(got.has_value(), ref_ok) << where;
          if (ref_ok) {
            EXPECT_EQ(cdsmodel::to_tdate(*got), expected) << where;
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 150) << "sweep thinner than expected: " << compared;
}

} // namespace
