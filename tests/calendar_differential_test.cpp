// TIER 2 -- live differential tests for the holiday calendar.
//
// Upstream's calendars live in a process-wide cache reached by name, so these
// tests build THolidayList values directly via JpmcdsHolidayListNewGeneral
// and never touch JpmcdsHolidayListFromCache. That is deliberate: the cache
// is the thing the port removes, and exercising it here would leak into every
// other test in the binary through the global it mutates.

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

class CalendarDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  void TearDown() override {
    // Keep the global cache out of the other tests in this binary.
    JpmcdsHolidayEmptyCache();
  }
};

/// Build upstream's THolidayList from the same inputs the port receives.
/// Returns nullptr where upstream rejects, as the port returns nullopt.
THolidayList *reference_calendar(const std::vector<cdsmodel::Date> &holidays,
                                 cdsmodel::Weekend weekends) {
  std::vector<TDate> tdates;
  tdates.reserve(holidays.size());
  for (cdsmodel::Date d : holidays) {
    tdates.push_back(cdsmodel::to_tdate(d));
  }
  TDateList dl{};
  dl.fNumItems = static_cast<int>(tdates.size());
  dl.fArray = tdates.empty() ? nullptr : tdates.data();
  // JpmcdsHolidayListNewGeneral copies the list, so a stack TDateList is fine.
  return JpmcdsHolidayListNewGeneral(tdates.empty() ? nullptr : &dl,
                                     cdsmodel::to_upstream_weekends(weekends));
}

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

const cdsmodel::Weekend kWeekendSets[] = {
    cdsmodel::Weekend::None,
    cdsmodel::Weekend::Standard,
    cdsmodel::Weekend::Sunday,
    cdsmodel::Weekend::Friday | cdsmodel::Weekend::Saturday, // Gulf convention
    cdsmodel::Weekend::Monday,
    cdsmodel::Weekend::Saturday,
};

// --- weekend classification ---------------------------------------------

TEST_F(CalendarDifferential, WeekendBitsAgreeForEverySetAndEveryWeekday) {
  // Sweeps 28 consecutive days against every weekend set, so each weekday is
  // tested four times under each mask.
  for (cdsmodel::Weekend w : kWeekendSets) {
    THolidayList *hl = reference_calendar({}, w);
    ASSERT_NE(hl, nullptr);
    const auto cal = cdsmodel::Calendar::create({}, w);
    ASSERT_TRUE(cal.has_value());

    for (int i = 0; i < 28; ++i) {
      const cdsmodel::Date day = d(2024, 1, 1) + std::chrono::days{i};
      const TDate t = cdsmodel::to_tdate(day);
      const bool reference = JPMCDS_IS_WEEKEND(t, hl->weekends) != 0;
      EXPECT_EQ(cal->is_weekend(day), reference)
          << "weekends=" << cdsmodel::to_upstream_weekends(w) << " tdate=" << t;
    }
    JpmcdsHolidayListDelete(hl);
  }
}

// --- construction, including the two behaviours worth pinning -----------

TEST_F(CalendarDifferential, RejectsNonIncreasingListsExactlyAsUpstreamDoes) {
  const std::vector<std::vector<cdsmodel::Date>> inputs{
      {},                                               // empty: accepted
      {d(2024, 3, 12)},                                 // single: accepted
      {d(2024, 3, 12), d(2024, 3, 13)},                 // increasing
      {d(2024, 3, 13), d(2024, 3, 12)},                 // decreasing
      {d(2024, 3, 12), d(2024, 3, 12)},                 // duplicate
      {d(2024, 3, 12), d(2024, 3, 14), d(2024, 3, 13)}, // out of order
      {cdsmodel::kDateEpoch},                           // TDate 0
      {cdsmodel::kDateEpoch + std::chrono::days{1}},    // TDate 1
  };
  for (const auto &in : inputs) {
    THolidayList *hl = reference_calendar(in, cdsmodel::Weekend::Standard);
    const auto cal = cdsmodel::Calendar::create(in, cdsmodel::Weekend::Standard);
    EXPECT_EQ(cal.has_value(), hl != nullptr)
        << "validity disagreement for a list of " << in.size() << " date(s)";
    if (hl != nullptr) {
      JpmcdsHolidayListDelete(hl);
    }
  }
}

TEST_F(CalendarDifferential, WeekendHolidaysAreStrippedFromTheStoredList) {
  // 2024-03-16 is a Saturday, 2024-03-17 a Sunday, the rest weekdays.
  const std::vector<cdsmodel::Date> holidays{d(2024, 3, 15), d(2024, 3, 16), d(2024, 3, 17),
                                             d(2024, 3, 18)};
  for (cdsmodel::Weekend w : kWeekendSets) {
    THolidayList *hl = reference_calendar(holidays, w);
    ASSERT_NE(hl, nullptr) << "weekends=" << cdsmodel::to_upstream_weekends(w);
    const auto cal = cdsmodel::Calendar::create(holidays, w);
    ASSERT_TRUE(cal.has_value());

    ASSERT_EQ(static_cast<int>(cal->holidays().size()), hl->dateList->fNumItems)
        << "stored holiday count, weekends=" << cdsmodel::to_upstream_weekends(w);
    for (int i = 0; i < hl->dateList->fNumItems; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(cal->holidays()[static_cast<std::size_t>(i)]),
                hl->dateList->fArray[i])
          << "stored holiday " << i;
    }
    JpmcdsHolidayListDelete(hl);
  }
}

// --- predicates ----------------------------------------------------------

TEST_F(CalendarDifferential, IsHolidayAndIsBusinessDayAgreeOverAFullYear) {
  // A plausible calendar: fixed and moving holidays, some landing on weekends.
  const std::vector<cdsmodel::Date> holidays{
      d(2024, 1, 1),   d(2024, 1, 15),  d(2024, 2, 19), d(2024, 3, 29),  d(2024, 5, 27),
      d(2024, 6, 19),  d(2024, 7, 4),   d(2024, 9, 2),  d(2024, 11, 28), d(2024, 11, 29),
      d(2024, 12, 24), d(2024, 12, 25), d(2024, 12, 31)};

  for (cdsmodel::Weekend w : kWeekendSets) {
    THolidayList *hl = reference_calendar(holidays, w);
    ASSERT_NE(hl, nullptr);
    const auto cal = cdsmodel::Calendar::create(holidays, w);
    ASSERT_TRUE(cal.has_value());

    for (int i = 0; i < 400; ++i) {
      const cdsmodel::Date day = d(2023, 12, 1) + std::chrono::days{i};
      const TDate t = cdsmodel::to_tdate(day);

      TBoolean ref_holiday = FALSE;
      ASSERT_EQ(JpmcdsHolidayListIsHoliday(t, hl, &ref_holiday), SUCCESS);
      EXPECT_EQ(cal->is_holiday(day), ref_holiday != FALSE)
          << "is_holiday, tdate=" << t << " weekends=" << cdsmodel::to_upstream_weekends(w);

      TBoolean ref_business = FALSE;
      ASSERT_EQ(JpmcdsHolidayListIsBusinessDay(t, hl, &ref_business), SUCCESS);
      EXPECT_EQ(cal->is_business_day(day), ref_business != FALSE)
          << "is_business_day, tdate=" << t << " weekends=" << cdsmodel::to_upstream_weekends(w);
    }
    JpmcdsHolidayListDelete(hl);
  }
}

TEST_F(CalendarDifferential, BuiltInCalendarsMatchUpstreamsCachedOnes) {
  // Upstream creates "NONE" and "NO_WEEKENDS" lazily inside holidayFind.
  // This is the one test that touches the cache, which is why TearDown
  // empties it.
  THolidayList *none = JpmcdsHolidayListFromCache(const_cast<char *>("NONE"));
  ASSERT_NE(none, nullptr);
  EXPECT_EQ(none->weekends, cdsmodel::to_upstream_weekends(cdsmodel::Weekend::Standard));
  EXPECT_EQ(none->dateList->fNumItems, 0);
  EXPECT_EQ(cdsmodel::Calendar::none().weekends(), cdsmodel::Weekend::Standard);
  EXPECT_TRUE(cdsmodel::Calendar::none().holidays().empty());

  THolidayList *nw = JpmcdsHolidayListFromCache(const_cast<char *>("NO_WEEKENDS"));
  ASSERT_NE(nw, nullptr);
  EXPECT_EQ(nw->weekends, cdsmodel::to_upstream_weekends(cdsmodel::Weekend::None));
  EXPECT_EQ(cdsmodel::Calendar::no_weekends().weekends(), cdsmodel::Weekend::None);

  // Every day is a business day under NO_WEEKENDS, both sides.
  for (int i = 0; i < 14; ++i) {
    const cdsmodel::Date day = d(2024, 6, 1) + std::chrono::days{i};
    TBoolean ref = FALSE;
    ASSERT_EQ(JpmcdsHolidayListIsBusinessDay(cdsmodel::to_tdate(day), nw, &ref), SUCCESS);
    EXPECT_TRUE(ref != FALSE);
    EXPECT_TRUE(cdsmodel::Calendar::no_weekends().is_business_day(day));
  }
}

TEST_F(CalendarDifferential, NoneMeansNoHolidaysNotNoNonBusinessDays) {
  // The built-in named "NONE" still has Saturday and Sunday as weekends.
  // Worth asserting against upstream, because the name says otherwise.
  THolidayList *none = JpmcdsHolidayListFromCache(const_cast<char *>("NONE"));
  ASSERT_NE(none, nullptr);
  const cdsmodel::Date saturday = d(2024, 6, 1);
  ASSERT_EQ(std::chrono::weekday{saturday}, std::chrono::Saturday);

  TBoolean ref = TRUE;
  ASSERT_EQ(JpmcdsHolidayListIsBusinessDay(cdsmodel::to_tdate(saturday), none, &ref), SUCCESS);
  EXPECT_FALSE(ref != FALSE) << "upstream NONE treats Saturday as a business day";
  EXPECT_FALSE(cdsmodel::Calendar::none().is_business_day(saturday));
}

} // namespace
