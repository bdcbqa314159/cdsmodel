// TIER 2 -- live differential tests for the bad day conventions.
//
// Note what is NOT swept here: a calendar whose weekend set covers all seven
// days. Upstream accepts such a calendar and then never returns from an
// adjustment (UPSTREAM-6), so there is no behaviour to compare against. The
// port's handling of it is asserted in tests/bad_day_test.cpp instead.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <array>
#include <vector>

extern "C" {
#include "buscache.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
}

namespace {

class BadDayDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  void TearDown() override { JpmcdsHolidayEmptyCache(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Upstream calendar from the same inputs the port receives. Caller frees.
THolidayList *reference_calendar(const std::vector<cdsmodel::Date> &holidays, long weekends) {
  static std::vector<TDate> tdates;
  tdates.clear();
  for (cdsmodel::Date h : holidays) {
    tdates.push_back(cdsmodel::to_tdate(h));
  }
  TDateList dl{};
  dl.fNumItems = static_cast<int>(tdates.size());
  dl.fArray = tdates.empty() ? nullptr : tdates.data();
  return JpmcdsHolidayListNewGeneral(tdates.empty() ? nullptr : &dl, weekends);
}

// Weekend sets that all leave at least one business weekday, so upstream
// terminates. 0x7F is deliberately absent.
const long kWeekendSets[] = {0x00, 0x60, 0x40, 0x30, 0x01, 0x20, 0x7E};

std::vector<cdsmodel::Date> holiday_set() {
  return {d(2023, 12, 25), d(2023, 12, 26), d(2024, 1, 1), d(2024, 3, 29), d(2024, 4, 1),
          d(2024, 5, 6),   d(2024, 5, 27),  d(2024, 7, 4), d(2024, 8, 26), d(2024, 11, 28),
          d(2024, 12, 25), d(2024, 12, 26), d(2025, 1, 1)};
}

constexpr cdsmodel::BadDayConv kConvs[] = {
    cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous, cdsmodel::BadDayConv::None,
    cdsmodel::BadDayConv::Modified};

TEST_F(BadDayDifferential, AdjustMatchesOverAFullYearForEveryConventionAndWeekendSet) {
  const auto holidays = holiday_set();
  for (long w : kWeekendSets) {
    THolidayList *hl = reference_calendar(holidays, w);
    ASSERT_NE(hl, nullptr) << "weekends=0x" << std::hex << w;
    const auto cal = cdsmodel::Calendar::create(holidays, *cdsmodel::weekend_from_upstream(w));
    ASSERT_TRUE(cal.has_value());

    for (int i = 0; i < 420; ++i) {
      const cdsmodel::Date day = d(2023, 12, 1) + std::chrono::days{i};
      const TDate t = cdsmodel::to_tdate(day);
      for (cdsmodel::BadDayConv conv : kConvs) {
        TDate out = 0;
        const bool reference_ok =
            JpmcdsHolidayListBusinessDay(t, cdsmodel::to_upstream_code(conv), hl, &out) == SUCCESS;
        const auto ported = cdsmodel::adjust(day, conv, *cal);

        ASSERT_EQ(ported.has_value(), reference_ok)
            << "validity, conv=" << static_cast<char>(conv) << " tdate=" << t << " weekends=0x"
            << std::hex << w;
        if (reference_ok) {
          EXPECT_EQ(cdsmodel::to_tdate(*ported), out)
              << "value, conv=" << static_cast<char>(conv) << " tdate=" << t << " weekends=0x"
              << std::hex << w;
        }
      }
    }
    JpmcdsHolidayListDelete(hl);
  }
}

TEST_F(BadDayDifferential, ModifiedDiffersFromFollowOnlyAtMonthEnds) {
  // The interesting dates for Modified are the last business days of months.
  // Sweeping every day of two years guarantees we hit the cases where
  // following leaves the month and Modified has to turn around.
  const auto holidays = holiday_set();
  THolidayList *hl = reference_calendar(holidays, 0x60);
  ASSERT_NE(hl, nullptr);
  const auto cal = cdsmodel::Calendar::create(holidays, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(cal.has_value());

  int divergences = 0;
  for (int i = 0; i < 730; ++i) {
    const cdsmodel::Date day = d(2023, 12, 1) + std::chrono::days{i};
    TDate ref_follow = 0;
    TDate ref_modified = 0;
    ASSERT_EQ(JpmcdsHolidayListBusinessDay(cdsmodel::to_tdate(day), 'F', hl, &ref_follow), SUCCESS);
    ASSERT_EQ(JpmcdsHolidayListBusinessDay(cdsmodel::to_tdate(day), 'M', hl, &ref_modified),
              SUCCESS);
    if (ref_follow != ref_modified) {
      ++divergences;
    }
    EXPECT_EQ(cdsmodel::to_tdate(*cdsmodel::adjust(day, cdsmodel::BadDayConv::Follow, *cal)),
              ref_follow);
    EXPECT_EQ(cdsmodel::to_tdate(*cdsmodel::adjust(day, cdsmodel::BadDayConv::Modified, *cal)),
              ref_modified);
  }
  // If the two never diverged, this test is proving nothing about Modified.
  EXPECT_GT(divergences, 0) << "Follow and Modified agreed on every date in two years, "
                               "so the month-end turnaround was never exercised";
  JpmcdsHolidayListDelete(hl);
}

TEST_F(BadDayDifferential, MultipleCalendarsMatchUpstreamsRestartLoop) {
  // Two markets with disjoint holidays: the result must be a business day in
  // both, which is the whole point of the multi-list variant.
  const std::vector<cdsmodel::Date> us{d(2024, 7, 4), d(2024, 11, 28), d(2024, 12, 25)};
  const std::vector<cdsmodel::Date> uk{d(2024, 5, 6), d(2024, 8, 26), d(2024, 12, 26)};

  THolidayList *hl_us = reference_calendar(us, 0x60);
  ASSERT_NE(hl_us, nullptr);
  // reference_calendar reuses one static buffer, so build the second after
  // upstream has copied the first.
  static std::vector<TDate> uk_dates;
  uk_dates.clear();
  for (cdsmodel::Date h : uk) {
    uk_dates.push_back(cdsmodel::to_tdate(h));
  }
  TDateList uk_dl{};
  uk_dl.fNumItems = static_cast<int>(uk_dates.size());
  uk_dl.fArray = uk_dates.data();
  THolidayList *hl_uk = JpmcdsHolidayListNewGeneral(&uk_dl, 0x60);
  ASSERT_NE(hl_uk, nullptr);

  THolidayList *lists[2] = {hl_us, hl_uk};
  const std::array<cdsmodel::Calendar, 2> cals{
      *cdsmodel::Calendar::create(us, cdsmodel::Weekend::Standard),
      *cdsmodel::Calendar::create(uk, cdsmodel::Weekend::Standard)};

  for (int i = 0; i < 400; ++i) {
    const cdsmodel::Date day = d(2024, 1, 1) + std::chrono::days{i};
    for (cdsmodel::BadDayConv conv : kConvs) {
      TDate out = 0;
      const bool reference_ok = JpmcdsMultiHolidayListBusinessDay(cdsmodel::to_tdate(day),
                                                                  cdsmodel::to_upstream_code(conv),
                                                                  2, lists, &out) == SUCCESS;
      const auto ported = cdsmodel::adjust(day, conv, std::span<const cdsmodel::Calendar>{cals});
      ASSERT_EQ(ported.has_value(), reference_ok) << "validity, conv=" << static_cast<char>(conv);
      if (reference_ok) {
        EXPECT_EQ(cdsmodel::to_tdate(*ported), out)
            << "value, conv=" << static_cast<char>(conv) << " tdate=" << cdsmodel::to_tdate(day);
      }
    }
  }
  JpmcdsHolidayListDelete(hl_us);
  JpmcdsHolidayListDelete(hl_uk);
}

TEST_F(BadDayDifferential, ConventionValidityMatchesJpmcdsBadDayConvValid) {
  for (long code = 0; code < 256; ++code) {
    const bool reference = JpmcdsBadDayConvValid(nullptr, code) == SUCCESS;
    EXPECT_EQ(cdsmodel::bad_day_conv_from_upstream(code).has_value(), reference)
        << "code " << code << " ('" << static_cast<char>(code) << "')";
  }
}

TEST_F(BadDayDifferential, Upstream6_AllWeekendCalendarIsAcceptedByUpstream) {
  // Records half of UPSTREAM-6: the calendar upstream accepts. The other half
  // -- that adjusting against it never returns -- cannot be asserted in a
  // test, because the test would never finish. See docs/port-notes.md.
  THolidayList *hl = JpmcdsHolidayListNewGeneral(nullptr, 0x7F);
  EXPECT_NE(hl, nullptr) << "upstream now rejects an all-weekend calendar; "
                            "UPSTREAM-6 may be fixed and the port's guard reconsidered";
  if (hl != nullptr) {
    JpmcdsHolidayListDelete(hl);
  }
  // The port accepts it too, and then declines to roll rather than hanging.
  const auto cal = cdsmodel::Calendar::create({}, *cdsmodel::weekend_from_upstream(0x7F));
  ASSERT_TRUE(cal.has_value());
  EXPECT_FALSE(cal->allows_business_days());
  EXPECT_FALSE(cdsmodel::adjust(d(2024, 6, 3), cdsmodel::BadDayConv::Follow, *cal).has_value());
}

} // namespace
