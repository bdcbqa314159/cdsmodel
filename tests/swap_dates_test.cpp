// TIER 1 -- the swap date set, without an oracle.
//
// Four derived values per maturity, so the properties are about how they
// relate to each other and to the inputs -- which is the part a differential
// test cannot establish, because both sides would shift together.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/swap_dates.hpp>

#include <gtest/gtest.h>

#include "golden/swap_dates_cases.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using cdsmodel::BadDayPair;
using cdsmodel::SwapDate;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<cdsmodel::Date> yearly(int day, int count = 5) {
  std::vector<cdsmodel::Date> out;
  for (int years = 1; years <= count; ++years) {
    const std::optional<cdsmodel::Date> date = cdsmodel::make_date(2024 + years, 1, day);
    if (date) {
      out.push_back(*date);
    }
  }
  return out;
}

// =====================================================================
// How the four values relate.
// =====================================================================

TEST(SwapDates, ThePreviousCouponAlwaysPrecedesTheMaturity) {
  int checked = 0;
  for (const int value_day : {1, 15, 28, 29, 31}) {
    const std::optional<cdsmodel::Date> value_date = cdsmodel::make_date(2024, 1, value_day);
    if (!value_date) {
      continue;
    }
    for (const int maturity_day : {15, 28, 31}) {
      const std::vector<cdsmodel::Date> mats = yearly(maturity_day);
      for (const long freq : {1L, 2L, 4L, 12L}) {
        const std::optional<std::vector<SwapDate>> got =
            cdsmodel::swap_dates_from_adjusted(*value_date, freq, mats, std::nullopt);
        ASSERT_TRUE(got.has_value()) << value_day << " " << freq;
        for (const SwapDate &entry : *got) {
          const std::string where = "vd " + std::to_string(value_day) + " freq " +
                                    std::to_string(freq) + " mat " +
                                    std::to_string(cdsmodel::to_tdate(entry.original));
          EXPECT_LT(entry.previous, entry.original)
              << where << ": the previous coupon must come before the maturity";
          // And by no more than one interval, since it IS the previous one.
          const std::optional<cdsmodel::DateInterval> interval =
              cdsmodel::frequency_to_interval(freq);
          ASSERT_TRUE(interval.has_value());
          const std::optional<cdsmodel::Date> two_back =
              cdsmodel::date_from_date_and_offset(entry.original, *interval, -2);
          ASSERT_TRUE(two_back.has_value());
          EXPECT_GT(entry.previous, *two_back) << where << ": and by less than two intervals";
          ++checked;
        }
      }
    }
  }
  EXPECT_GT(checked, 200) << checked;
}

TEST(SwapDates, WithoutATableOrAConventionEveryDateIsItsOwnAdjustment) {
  // No substitution table and the None convention: nothing can move, so all
  // three dates derived from the input must agree where they should.
  const std::vector<cdsmodel::Date> mats = yearly(15);
  const std::optional<std::vector<SwapDate>> got = cdsmodel::swap_dates_from_original(
      d(2024, 1, 15), 4, mats, std::nullopt, cdsmodel::BadDayConv::None, {});
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), mats.size());
  for (std::size_t i = 0; i < mats.size(); ++i) {
    EXPECT_EQ((*got)[i].original, mats[i]) << i;
    EXPECT_EQ((*got)[i].adjusted, mats[i]) << i << ": None adjusts nothing";
  }
}

TEST(SwapDates, TheTwoConstructorsAgreeWhenNothingNeedsAdjusting) {
  // from_adjusted recovers the original by reverse lookup; from_original
  // adjusts forward. With no table and no adjustment, both must land on the
  // same four values -- which is the cheapest check that they derive the
  // cycle from the same date.
  const std::vector<cdsmodel::Date> mats = yearly(15);
  for (const long freq : {1L, 2L, 4L}) {
    const std::optional<std::vector<SwapDate>> from_adjusted =
        cdsmodel::swap_dates_from_adjusted(d(2024, 1, 15), freq, mats, std::nullopt);
    const std::optional<std::vector<SwapDate>> from_original = cdsmodel::swap_dates_from_original(
        d(2024, 1, 15), freq, mats, std::nullopt, cdsmodel::BadDayConv::None, {});
    ASSERT_TRUE(from_adjusted.has_value());
    ASSERT_TRUE(from_original.has_value());
    EXPECT_EQ(*from_adjusted, *from_original) << "freq " << freq;
  }
}

TEST(SwapDates, AnOnCycleMaturityIsAWholeNumberOfIntervalsFromTheValueDate) {
  // The flag's meaning, asserted against count_dates rather than against
  // upstream. Note the one-way direction: on_cycle true implies an exact
  // count, but a false flag does NOT imply the converse -- that is
  // UPSTREAM-27, and the next test is the other half.
  int on_cycle_seen = 0;
  for (const int day : {1, 15, 28}) {
    const std::optional<cdsmodel::Date> value_date = cdsmodel::make_date(2024, 1, day);
    ASSERT_TRUE(value_date.has_value());
    for (const long freq : {1L, 2L, 4L, 12L}) {
      const std::optional<cdsmodel::DateInterval> interval = cdsmodel::frequency_to_interval(freq);
      ASSERT_TRUE(interval.has_value());
      const std::vector<cdsmodel::Date> mats = yearly(day);
      const std::optional<std::vector<SwapDate>> got =
          cdsmodel::swap_dates_from_adjusted(*value_date, freq, mats, std::nullopt);
      ASSERT_TRUE(got.has_value());
      for (const SwapDate &entry : *got) {
        if (!entry.on_cycle) {
          continue;
        }
        ++on_cycle_seen;
        const std::optional<cdsmodel::DateCount> counted =
            cdsmodel::count_dates(*value_date, entry.original, *interval);
        ASSERT_TRUE(counted.has_value());
        EXPECT_EQ(counted->extra_days, 0)
            << "on_cycle claimed for a maturity that is not a whole number of intervals away";
        // And the previous coupon is then one interval short of the maturity,
        // counted from the value date.
        const std::optional<cdsmodel::Date> expected =
            cdsmodel::date_from_date_and_offset(*value_date, *interval, counted->intervals - 1);
        ASSERT_TRUE(expected.has_value());
        EXPECT_EQ(entry.previous, *expected);
      }
    }
  }
  EXPECT_GT(on_cycle_seen, 20) << on_cycle_seen;
}

TEST(SwapDates, AFalseFlagDoesNotImplyOffCycle) {
  // The other half, and the reason the property above is one-way. A 31st of
  // the month maturity an exact number of intervals from a 31st value date is
  // reported off cycle -- UPSTREAM-27, reproduced deliberately.
  const cdsmodel::Date value_date = d(2024, 1, 31);
  const cdsmodel::Date maturity = d(2027, 1, 31);
  const cdsmodel::DateInterval quarterly{3, cdsmodel::PeriodUnit::Months};
  const std::optional<cdsmodel::DateCount> counted =
      cdsmodel::count_dates(value_date, maturity, quarterly);
  ASSERT_TRUE(counted.has_value());
  ASSERT_EQ(counted->extra_days, 0) << "the premise: genuinely on cycle";

  const std::optional<std::vector<SwapDate>> got =
      cdsmodel::swap_dates_from_adjusted(value_date, 4, {{maturity}}, std::nullopt);
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got->front().on_cycle)
      << "the day-of-month heuristic suppresses it, and the port reproduces that";

  // Below the 29th the same shape is reported on cycle, which is what makes
  // the heuristic a day-of-month rule rather than a schedule one.
  const std::optional<std::vector<SwapDate>> inside =
      cdsmodel::swap_dates_from_adjusted(d(2024, 1, 28), 4, {{d(2027, 1, 28)}}, std::nullopt);
  ASSERT_TRUE(inside.has_value());
  EXPECT_TRUE(inside->front().on_cycle);
}

// =====================================================================
// The substitution table, and the optional that carries its absence.
// =====================================================================

TEST(SwapDates, NoTableAndAnEmptyTableAreDifferentInputs) {
  // The measurement that decided the signature. nullopt means "use the
  // convention"; an empty span means "use this table, which says nothing", and
  // the second adjusts nothing at all.
  const cdsmodel::Date saturday = d(2024, 1, 6);
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

  const std::optional<cdsmodel::Date> by_convention =
      cdsmodel::zc_adjust_date(saturday, std::nullopt, cdsmodel::BadDayConv::Follow, calendars);
  ASSERT_TRUE(by_convention.has_value());
  EXPECT_NE(*by_convention, saturday);
  EXPECT_TRUE(calendars.front().is_business_day(*by_convention));

  const std::optional<cdsmodel::Date> by_empty_table = cdsmodel::zc_adjust_date(
      saturday, std::span<const BadDayPair>{}, cdsmodel::BadDayConv::Follow, calendars);
  ASSERT_TRUE(by_empty_table.has_value());
  EXPECT_EQ(*by_empty_table, saturday) << "an empty table adjusts nothing";
  EXPECT_FALSE(calendars.front().is_business_day(*by_empty_table))
      << "so the result need not be a business day -- UPSTREAM-26";
}

TEST(SwapDates, ATableTakesPrecedenceOverTheConventionEvenWhereItSaysNothing) {
  const cdsmodel::Date listed = d(2024, 1, 6);
  const cdsmodel::Date unlisted = d(2024, 1, 13);
  const cdsmodel::Date monday = d(2024, 1, 8);
  const std::vector<BadDayPair> table{{listed, monday}};
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  const std::span<const BadDayPair> as_span{table};

  EXPECT_EQ(cdsmodel::zc_adjust_date(listed, as_span, cdsmodel::BadDayConv::Follow, calendars),
            std::optional{monday});
  EXPECT_EQ(cdsmodel::zc_adjust_date(unlisted, as_span, cdsmodel::BadDayConv::Follow, calendars),
            std::optional{unlisted})
      << "absent from the table, so untouched -- the convention is not consulted";
  // A date that needs no adjustment is unaffected either way.
  EXPECT_EQ(cdsmodel::zc_adjust_date(monday, as_span, cdsmodel::BadDayConv::Follow, calendars),
            std::optional{monday});
}

TEST(SwapDates, TheOriginalIsRecoveredByReverseLookup) {
  const cdsmodel::Date adjusted = d(2025, 1, 15);
  const cdsmodel::Date unadjusted = d(2025, 1, 13);
  const std::vector<BadDayPair> table{{unadjusted, adjusted}};
  const std::optional<std::vector<SwapDate>> got = cdsmodel::swap_dates_from_adjusted(
      d(2024, 1, 15), 4, {{adjusted}}, std::span<const BadDayPair>{table});
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 1u);
  EXPECT_EQ(got->front().adjusted, adjusted);
  EXPECT_EQ(got->front().original, unadjusted) << "recovered through good_to_bad";
  // Without the table the original is the adjusted date itself.
  const std::optional<std::vector<SwapDate>> plain =
      cdsmodel::swap_dates_from_adjusted(d(2024, 1, 15), 4, {{adjusted}}, std::nullopt);
  ASSERT_TRUE(plain.has_value());
  EXPECT_EQ(plain->front().original, adjusted);
}

TEST(SwapDates, AnInvalidFrequencyIsRefusedAndAnEmptyInputIsNot) {
  const std::vector<cdsmodel::Date> mats = yearly(15);
  for (const long freq : {-1L, 0L, 13L, 1000L}) {
    EXPECT_FALSE(
        cdsmodel::swap_dates_from_adjusted(d(2024, 1, 15), freq, mats, std::nullopt).has_value())
        << freq;
  }
  // No maturities is an empty result, not an error -- there is nothing to
  // derive and nothing wrong with asking.
  const std::optional<std::vector<SwapDate>> none =
      cdsmodel::swap_dates_from_adjusted(d(2024, 1, 15), 4, {}, std::nullopt);
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->empty());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(SwapDates, MatchesRecordedSwapDateSets) {
  int checked = 0;
  int on_cycle_seen = 0;
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  for (const auto &c : cdsmodel::golden::kSwapDatesCases) {
    std::vector<cdsmodel::Date> input;
    for (std::size_t i = 0; i < c.n; ++i) {
      input.push_back(cdsmodel::from_tdate(c.input[i]));
    }
    const cdsmodel::Date value_date = cdsmodel::from_tdate(c.value_date);
    std::optional<std::vector<SwapDate>> got;
    if (c.source == 0) {
      got = cdsmodel::swap_dates_from_adjusted(value_date, c.freq, input, std::nullopt);
    } else {
      const std::optional<cdsmodel::BadDayConv> conv =
          cdsmodel::bad_day_conv_from_upstream(c.bad_day);
      ASSERT_TRUE(conv.has_value()) << c.bad_day;
      got = cdsmodel::swap_dates_from_original(value_date, c.freq, input, std::nullopt, *conv,
                                               calendars);
    }
    const std::string where = "src " + std::to_string(c.source) + " vd " +
                              std::to_string(c.value_date) + " freq " + std::to_string(c.freq) +
                              " conv " + std::to_string(c.bad_day);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (!got) {
      continue;
    }
    ASSERT_EQ(got->size(), c.n) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      const std::string at = where + " i=" + std::to_string(i);
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].adjusted), c.adjusted[i]) << at << " adjusted";
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].original), c.original[i]) << at << " original";
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].previous), c.previous[i]) << at << " previous";
      EXPECT_EQ((*got)[i].on_cycle, c.on_cycle[i]) << at << " on_cycle";
      if (c.on_cycle[i]) {
        ++on_cycle_seen;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 2000) << checked;
  EXPECT_GT(on_cycle_seen, 20)
      << "the sweep must contain on-cycle cases, not just suppressed ones: " << on_cycle_seen;
}

} // namespace
