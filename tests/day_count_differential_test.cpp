// TIER 2 -- live differential tests for the day count conventions.
//
// The ACT/365 case matters most: the port computes the leap-weighted
// denominator by intersecting the period with each calendar year, where
// upstream walks years by adding 365 or 366 to a running date. The two are
// only equivalent if upstream's walk never drifts, which is asserted here
// across a wide sweep rather than argued in a comment.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

extern "C" {
#include "cerror.h"
#include "ldate.h"
}

namespace {

constexpr cdsmodel::DayCount kAll[] = {
    cdsmodel::DayCount::Act365,     cdsmodel::DayCount::Act365Fixed,
    cdsmodel::DayCount::Act360,     cdsmodel::DayCount::Thirty360,
    cdsmodel::DayCount::ThirtyE360, cdsmodel::DayCount::EffectiveRate};

class DayCountDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

void expect_year_fraction_matches(cdsmodel::Date from, cdsmodel::Date to, cdsmodel::DayCount dcc) {
  double reference = 0.0;
  const bool reference_ok =
      JpmcdsDayCountFraction(cdsmodel::to_tdate(from), cdsmodel::to_tdate(to),
                             cdsmodel::to_upstream_code(dcc), &reference) == SUCCESS;
  const auto ported = cdsmodel::year_fraction(from, to, dcc);

  ASSERT_EQ(ported.has_value(), reference_ok)
      << "validity disagreement, dcc=" << cdsmodel::to_upstream_code(dcc)
      << " from=" << cdsmodel::to_tdate(from) << " to=" << cdsmodel::to_tdate(to);
  if (reference_ok) {
    // Both sides accumulate the same doubles in the same order for every
    // convention except ACT/365, where the summation order differs. Exact
    // equality is the right bar: a relative tolerance would hide a genuine
    // divergence in the leap weighting.
    EXPECT_DOUBLE_EQ(*ported, reference)
        << "dcc=" << cdsmodel::to_upstream_code(dcc) << " from=" << cdsmodel::to_tdate(from)
        << " to=" << cdsmodel::to_tdate(to);
  }
}

void expect_days_diff_matches(cdsmodel::Date from, cdsmodel::Date to, cdsmodel::DayCount dcc) {
  long reference = 0;
  const bool reference_ok = JpmcdsDaysDiff(cdsmodel::to_tdate(from), cdsmodel::to_tdate(to),
                                           cdsmodel::to_upstream_code(dcc), &reference) == SUCCESS;
  const auto ported = cdsmodel::days_diff(from, to, dcc);

  ASSERT_EQ(ported.has_value(), reference_ok)
      << "validity disagreement, dcc=" << cdsmodel::to_upstream_code(dcc);
  if (reference_ok) {
    EXPECT_EQ(*ported, reference) << "dcc=" << cdsmodel::to_upstream_code(dcc)
                                  << " from=" << cdsmodel::to_tdate(from)
                                  << " to=" << cdsmodel::to_tdate(to);
  }
}

// Dates chosen to hit the cases these conventions actually disagree on:
// month ends, the 30th and 31st, leap days, year boundaries, and periods
// spanning one, two and many years.
std::vector<cdsmodel::Date> interesting_dates() {
  std::vector<cdsmodel::Date> v;
  for (auto [y, m, d] : std::initializer_list<std::array<int, 3>>{
           {1999, 1, 1},   {1999, 1, 31}, {1999, 2, 28},  {1999, 3, 30},  {1999, 3, 31},
           {1999, 12, 31}, {2000, 1, 1},  {2000, 2, 28},  {2000, 2, 29},  {2000, 3, 1},
           {2000, 4, 30},  {2000, 5, 31}, {2000, 12, 31}, {2001, 1, 1},   {2003, 7, 15},
           {2004, 2, 29},  {2008, 1, 3},  {2008, 6, 20},  {2012, 10, 31}, {2020, 2, 29},
           {2023, 8, 30},  {2023, 8, 31}, {2024, 2, 29},  {2027, 11, 30}}) {
    v.push_back(*cdsmodel::make_date(y, m, d));
  }
  return v;
}

TEST_F(DayCountDifferential, YearFractionMatchesForEveryConventionAndDatePair) {
  const auto dates = interesting_dates();
  for (cdsmodel::Date a : dates) {
    for (cdsmodel::Date b : dates) { // both orders, so the sign path is covered
      for (cdsmodel::DayCount dcc : kAll) {
        expect_year_fraction_matches(a, b, dcc);
      }
    }
  }
}

TEST_F(DayCountDifferential, DaysDiffMatchesForEveryConventionAndDatePair) {
  const auto dates = interesting_dates();
  for (cdsmodel::Date a : dates) {
    for (cdsmodel::Date b : dates) {
      for (cdsmodel::DayCount dcc : kAll) {
        expect_days_diff_matches(a, b, dcc);
      }
    }
  }
}

TEST_F(DayCountDifferential, ActActLeapWeightingOverLongPeriods) {
  // Periods spanning many years are where a drifting year-walk would show up.
  const cdsmodel::Date base = *cdsmodel::make_date(1996, 2, 29);
  for (int years = 0; years <= 40; ++years) {
    for (int offset : {0, 1, 59, 60, 181, 364}) {
      const cdsmodel::Date to = base + std::chrono::days{years * 365 + offset};
      expect_year_fraction_matches(base, to, cdsmodel::DayCount::Act365);
    }
  }
}

TEST_F(DayCountDifferential, ActActEveryStartDayOfAFourYearWindow) {
  // Every start date across a leap cycle, each measured to a fixed end.
  const cdsmodel::Date end = *cdsmodel::make_date(2005, 6, 15);
  const cdsmodel::Date first = *cdsmodel::make_date(2000, 1, 1);
  for (int i = 0; i < 1461; ++i) {
    expect_year_fraction_matches(first + std::chrono::days{i}, end, cdsmodel::DayCount::Act365);
  }
}

TEST_F(DayCountDifferential, InvalidConventionCodesRejectedByBoth) {
  const cdsmodel::Date a = *cdsmodel::make_date(2008, 1, 3);
  const cdsmodel::Date b = *cdsmodel::make_date(2013, 6, 20);
  for (long bad : {-2L, 0L, 6L, 7L, 9L, 100L}) {
    EXPECT_FALSE(cdsmodel::from_upstream_code(bad).has_value()) << "code " << bad;
    double unused = 0.0;
    EXPECT_NE(JpmcdsDayCountFraction(cdsmodel::to_tdate(a), cdsmodel::to_tdate(b), bad, &unused),
              SUCCESS)
        << "upstream accepted code " << bad;
  }
}

TEST_F(DayCountDifferential, ActActAliasIsTheSameCodeUpstream) {
  // Documented conflation: ACT/ACT and ACT/365 are one code, so upstream
  // cannot distinguish them. If upstream ever splits them, this fails.
  EXPECT_EQ(cdsmodel::to_upstream_code(cdsmodel::kActAct), JPMCDS_ACT_ACT);
  EXPECT_EQ(JPMCDS_ACT_ACT, JPMCDS_ACT_365);
}

} // namespace
