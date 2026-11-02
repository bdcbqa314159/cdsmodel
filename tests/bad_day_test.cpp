// TIER 1 -- runs anywhere, including CI with no upstream oracle present.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include "golden/bad_day_cases.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <vector>

namespace {

using namespace std::chrono;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<cdsmodel::Date> recorded_holidays() {
  std::vector<cdsmodel::Date> v;
  for (long t : cdsmodel::golden::kBadDayHolidays) {
    v.push_back(cdsmodel::from_tdate(t));
  }
  return v;
}

TEST(BadDayGolden, AdjustmentMatchesRecordedReference) {
  const auto holidays = recorded_holidays();
  for (const auto &c : cdsmodel::golden::kBadDayCases) {
    const auto w = cdsmodel::weekend_from_upstream(c.weekends);
    ASSERT_TRUE(w.has_value()) << "unrepresentable weekend set " << c.weekends;
    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value());
    const auto conv = cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value()) << "unrecognised convention '" << c.conv << "'";

    const auto got = cdsmodel::adjust(cdsmodel::from_tdate(c.tdate), *conv, *cal);
    ASSERT_EQ(got.has_value(), c.ok) << "validity, conv=" << c.conv << " tdate=" << c.tdate;
    if (c.ok) {
      EXPECT_EQ(cdsmodel::to_tdate(*got), c.adjusted)
          << "value, conv=" << c.conv << " tdate=" << c.tdate << " weekends=" << c.weekends;
    }
  }
}

TEST(BadDayGolden, VectorSetIsNotEmpty) { EXPECT_GT(cdsmodel::golden::kBadDayCases.size(), 3000u); }

// --- port semantics needing no oracle ------------------------------------

TEST(BadDayConventions, CodesAreUpstreamsCharacters) {
  static_assert(cdsmodel::to_upstream_code(cdsmodel::BadDayConv::Follow) == 'F');
  static_assert(cdsmodel::to_upstream_code(cdsmodel::BadDayConv::Previous) == 'P');
  static_assert(cdsmodel::to_upstream_code(cdsmodel::BadDayConv::None) == 'N');
  static_assert(cdsmodel::to_upstream_code(cdsmodel::BadDayConv::Modified) == 'M');
  for (cdsmodel::BadDayConv conv : {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
                                    cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Modified}) {
    EXPECT_EQ(cdsmodel::bad_day_conv_from_upstream(cdsmodel::to_upstream_code(conv)), conv);
  }
}

TEST(BadDayConventions, RejectsEveryOtherByte) {
  for (long code = 0; code < 256; ++code) {
    const bool expected = code == 'F' || code == 'P' || code == 'N' || code == 'M';
    EXPECT_EQ(cdsmodel::bad_day_conv_from_upstream(code).has_value(), expected) << "code " << code;
  }
  EXPECT_FALSE(cdsmodel::bad_day_conv_from_upstream('f').has_value()) << "case matters";
  EXPECT_FALSE(cdsmodel::bad_day_conv_from_upstream(-1).has_value());
}

TEST(BadDayAdjust, NoneLeavesTheDateAloneEvenOnAWeekend) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const cdsmodel::Date saturday = d(2024, 6, 1);
  ASSERT_EQ(weekday{saturday}, Saturday);
  EXPECT_EQ(cdsmodel::adjust(saturday, cdsmodel::BadDayConv::None, cal), saturday);
}

TEST(BadDayAdjust, FollowAndPreviousMoveInOppositeDirections) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  const cdsmodel::Date saturday = d(2024, 6, 1);
  EXPECT_EQ(cdsmodel::adjust(saturday, cdsmodel::BadDayConv::Follow, cal), d(2024, 6, 3));
  EXPECT_EQ(cdsmodel::adjust(saturday, cdsmodel::BadDayConv::Previous, cal), d(2024, 5, 31));
  // A business day is returned untouched by every rolling convention.
  const cdsmodel::Date monday = d(2024, 6, 3);
  for (cdsmodel::BadDayConv conv : {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
                                    cdsmodel::BadDayConv::Modified}) {
    EXPECT_EQ(cdsmodel::adjust(monday, conv, cal), monday);
  }
}

TEST(BadDayAdjust, ModifiedTurnsAroundRatherThanLeaveTheMonth) {
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();
  // 2024-06-30 is a Sunday, so Follow lands in July and Modified must not.
  const cdsmodel::Date sunday = d(2024, 6, 30);
  ASSERT_EQ(weekday{sunday}, Sunday);
  EXPECT_EQ(cdsmodel::adjust(sunday, cdsmodel::BadDayConv::Follow, cal), d(2024, 7, 1));
  EXPECT_EQ(cdsmodel::adjust(sunday, cdsmodel::BadDayConv::Modified, cal), d(2024, 6, 28));

  // Mid-month, Modified and Follow agree.
  const cdsmodel::Date saturday = d(2024, 6, 15);
  EXPECT_EQ(cdsmodel::adjust(saturday, cdsmodel::BadDayConv::Modified, cal),
            cdsmodel::adjust(saturday, cdsmodel::BadDayConv::Follow, cal));
}

TEST(BadDayAdjust, MultipleCalendarsYieldADayThatIsGoodInAllOfThem) {
  const auto us = cdsmodel::Calendar::create(std::vector{d(2024, 7, 4), d(2024, 7, 5)},
                                             cdsmodel::Weekend::Standard);
  const auto uk =
      cdsmodel::Calendar::create(std::vector{d(2024, 7, 8)}, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(us.has_value());
  ASSERT_TRUE(uk.has_value());
  const std::array<cdsmodel::Calendar, 2> both{*us, *uk};

  // 4 Jul is a Thursday. US closed Thu and Fri, weekend Sat/Sun, UK closed
  // Mon the 8th -- so the first day good in both is Tuesday the 9th.
  const auto got = cdsmodel::adjust(d(2024, 7, 4), cdsmodel::BadDayConv::Follow,
                                    std::span<const cdsmodel::Calendar>{both});
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, d(2024, 7, 9));
  for (const cdsmodel::Calendar &cal : both) {
    EXPECT_TRUE(cal.is_business_day(*got));
  }
}

TEST(BadDayAdjust, RejectsAnEmptyCalendarSet) {
  // Upstream rejects a null holiday list array; None still short-circuits.
  EXPECT_FALSE(cdsmodel::adjust(d(2024, 6, 1), cdsmodel::BadDayConv::Follow,
                                std::span<const cdsmodel::Calendar>{})
                   .has_value());
  EXPECT_EQ(cdsmodel::adjust(d(2024, 6, 1), cdsmodel::BadDayConv::None,
                             std::span<const cdsmodel::Calendar>{}),
            d(2024, 6, 1));
}

TEST(BadDayAdjust, AllWeekendCalendarDeclinesInsteadOfLooping) {
  // UPSTREAM-6: upstream accepts this calendar and then never returns. The
  // port returns nullopt. If this test ever hangs, the bound regressed.
  const auto cal = cdsmodel::Calendar::create({}, *cdsmodel::weekend_from_upstream(0x7F));
  ASSERT_TRUE(cal.has_value());
  EXPECT_FALSE(cal->allows_business_days());
  for (cdsmodel::BadDayConv conv : {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
                                    cdsmodel::BadDayConv::Modified}) {
    EXPECT_FALSE(cdsmodel::adjust(d(2024, 6, 3), conv, *cal).has_value())
        << "conv=" << static_cast<char>(conv);
  }
  // None does not roll, so it still answers.
  EXPECT_EQ(cdsmodel::adjust(d(2024, 6, 3), cdsmodel::BadDayConv::None, *cal), d(2024, 6, 3));
}

TEST(BadDayAdjust, TerminatesWithOnlyOneBusinessWeekdayAndManyHolidays) {
  // The case that broke the first version of the loop bound: with 0x7E only
  // Monday trades, so each holiday costs a full week to skip, not a day.
  std::vector<cdsmodel::Date> mondays;
  cdsmodel::Date m = d(2024, 1, 1); // a Monday
  ASSERT_EQ(weekday{m}, Monday);
  for (int i = 0; i < 20; ++i) {
    mondays.push_back(m);
    m += days{7};
  }
  const auto cal = cdsmodel::Calendar::create(mondays, *cdsmodel::weekend_from_upstream(0x7E));
  ASSERT_TRUE(cal.has_value());
  ASSERT_EQ(cal->holidays().size(), 20u) << "every Monday should survive construction";

  // Twenty consecutive trading Mondays are holidays, so Follow must roll 21
  // weeks. A one-day-per-holiday bound would give up long before.
  const auto got = cdsmodel::adjust(d(2024, 1, 1), cdsmodel::BadDayConv::Follow, *cal);
  ASSERT_TRUE(got.has_value()) << "the loop bound is too tight again";
  EXPECT_EQ(*got, d(2024, 1, 1) + days{7 * 20});
  EXPECT_TRUE(cal->is_business_day(*got));
}

} // namespace
