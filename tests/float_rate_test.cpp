// TIER 1 -- the floating rate definition, verified with no oracle present.
//
// THERE IS NO tests/golden/float_rate_cases.hpp, and that is deliberate.
//
// fltrate.c contains no arithmetic. Its one function assigns six values and
// expands a macro, so the only runtime behaviour a recorded vector could
// capture is what `JpmcdsDtFwdAdj` does with the resulting spot offset -- and
// that is already recorded, 400 cases of it, as `kFwdAdjustCases` in
// tests/golden/date_sup_cases.hpp, replayed by date_sup_test.cpp. A second
// vector set over the same function would be a copy whose only new content is
// the three constants the macro writes.
//
// Those three constants are what this file pins instead, at compile time
// where possible. The failure this guards against is concrete: if
// `calendar_day_offset` ever drifted to business days, every forward rate in a
// floating leg would move, and the differential suite would catch it only on a
// machine with the oracle.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/float_rate.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace {

using cdsmodel::calendar_day_offset;

// The macro's expansion, as a compile-time fact rather than a test that could
// be skipped or filtered out.
static_assert(calendar_day_offset(2).interval.periods == 2);
static_assert(calendar_day_offset(2).interval.unit == cdsmodel::PeriodUnit::Days);
static_assert(calendar_day_offset(2).basis == cdsmodel::OffsetBasis::CalendarPeriod,
              "JPMCDS_SET_ADJ_INTERVAL_DAYS sets isBusDays to "
              "JPMCDS_DATE_ADJ_TYPE_CALENDAR -- despite the macro's name, a spot "
              "offset is CALENDAR days");
static_assert(calendar_day_offset(2).bad_day == cdsmodel::BadDayConv::None,
              "the macro disables bad-day adjustment, so the spot date is not "
              "moved off a weekend");
static_assert(calendar_day_offset(0).interval.periods == 0);
static_assert(calendar_day_offset(-5).interval.periods == -5,
              "upstream stores a negative offset as given; nothing rejects it");

// The documented order: spread is added AFTER the weight multiplies.
static_assert(cdsmodel::FloatRate{}.weight == 1.0);
static_assert(cdsmodel::FloatRate{}.spread == 0.0);

TEST(FloatRateGolden, ACalendarSpotOffsetDoesNotAvoidWeekends) {
  const cdsmodel::Calendar calendar = cdsmodel::Calendar::none();
  // Friday 2024-03-15 plus two calendar days is Sunday the 17th. A business
  // offset would give Tuesday the 19th. This is the whole distinction.
  const cdsmodel::Expected<cdsmodel::Date> spot = cdsmodel::advance_adjusted(
      *cdsmodel::make_date(2024, 3, 15), calendar_day_offset(2), calendar);
  ASSERT_TRUE(spot.has_value());
  EXPECT_EQ(*spot, *cdsmodel::make_date(2024, 3, 17));
  EXPECT_EQ(std::chrono::weekday{*spot}, std::chrono::Sunday);
}

TEST(FloatRateGolden, ASpotOffsetOfZeroIsTheResetDate) {
  const cdsmodel::Calendar calendar = cdsmodel::Calendar::none();
  for (int day = 11; day <= 24; ++day) {
    const cdsmodel::Date reset = *cdsmodel::make_date(2024, 3, day);
    const cdsmodel::Expected<cdsmodel::Date> spot =
        cdsmodel::advance_adjusted(reset, calendar_day_offset(0), calendar);
    ASSERT_TRUE(spot.has_value());
    EXPECT_EQ(*spot, reset) << "day " << day;
  }
}

// Every weekday as a reset, so the offset is exercised across the weekend
// boundary in both directions.
TEST(FloatRateGolden, EveryWeekdayAdvancesByExactlyTheCalendarCount) {
  const cdsmodel::Calendar calendar = cdsmodel::Calendar::none();
  int checked = 0;
  for (const int days : {0, 1, 2, 3, 7, 30}) {
    for (int day = 1; day <= 31; ++day) {
      const cdsmodel::Date reset = *cdsmodel::make_date(2024, 3, day);
      const cdsmodel::Expected<cdsmodel::Date> spot =
          cdsmodel::advance_adjusted(reset, calendar_day_offset(days), calendar);
      ASSERT_TRUE(spot.has_value()) << "day " << day << " + " << days;
      EXPECT_EQ((*spot - reset).count(), days) << "day " << day << " + " << days;
      ++checked;
    }
  }
  EXPECT_GT(checked, 150);
}

// A FloatRate is an aggregate, so a caller can only get the fields wrong by
// naming them. Upstream's positional setter took matInterval and payInterval
// adjacently, both TDateInterval*, so transposing them compiled silently.
TEST(FloatRateGolden, TheIntervalsAreDistinguishableByName) {
  const cdsmodel::FloatRate rate{.maturity_interval = {60, cdsmodel::PeriodUnit::Months},
                                 .pay_interval = {6, cdsmodel::PeriodUnit::Months},
                                 .day_count = cdsmodel::DayCount::Act360,
                                 .spot_offset = calendar_day_offset(2),
                                 .spread = 0.0025,
                                 .weight = 1.0};
  EXPECT_EQ(rate.maturity_interval.periods, 60);
  EXPECT_EQ(rate.pay_interval.periods, 6);
  EXPECT_NE(rate.maturity_interval, rate.pay_interval);

  // Simple LIBOR, per upstream's documentation: pay interval equal to the
  // maturity interval means a zero-coupon rate.
  const cdsmodel::FloatRate libor{.maturity_interval = {3, cdsmodel::PeriodUnit::Months},
                                  .pay_interval = {3, cdsmodel::PeriodUnit::Months},
                                  .spot_offset = calendar_day_offset(2)};
  EXPECT_EQ(libor.maturity_interval, libor.pay_interval);
}

} // namespace
