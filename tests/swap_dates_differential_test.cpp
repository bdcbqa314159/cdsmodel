// TIER 2 -- live differential tests for the swap date set.
//
// Four parallel arrays upstream, so the comparison is per index across all
// four at once -- an adjusted date that matches while its `previous` does not
// is the failure this module is shaped to produce.
//
// The sweep is over the two things that decide the answers: the day of month
// (the on-cycle heuristic keys on it) and whether a substitution table is
// present, absent, or present-but-empty. Those three are genuinely different
// inputs upstream, which is why the port takes an optional span.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/swap_dates.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
#include "zcswdate.h"
}

namespace {

class SwapDatesDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// An upstream TBadDayList over a port map, plus the null/empty distinction.
struct RefList {
  std::vector<TDate> bad;
  std::vector<TDate> good;
  TBadDayList list{};

  explicit RefList(std::span<const cdsmodel::BadDayPair> map) {
    for (const cdsmodel::BadDayPair &pair : map) {
      bad.push_back(cdsmodel::to_tdate(pair.bad));
      good.push_back(cdsmodel::to_tdate(pair.good));
    }
    list.count = static_cast<int>(map.size());
    list.maxAllowed = static_cast<int>(map.size());
    list.badDay = bad.empty() ? nullptr : bad.data();
    list.goodDay = good.empty() ? nullptr : good.data();
  }
};

struct Ref {
  TSwapDates *sd = nullptr;
  explicit Ref(TSwapDates *owned) : sd(owned) {}
  ~Ref() { JpmcdsSwapDatesFree(sd); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return sd != nullptr; }
};

/// Compare all four arrays at every index.
void expect_same(const Ref &ref, const std::vector<cdsmodel::SwapDate> &got,
                 const std::string &where) {
  ASSERT_TRUE(static_cast<bool>(ref)) << where;
  ASSERT_EQ(got.size(), static_cast<std::size_t>(ref.sd->numDates)) << where;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const std::string at = where + " i=" + std::to_string(i);
    EXPECT_EQ(cdsmodel::to_tdate(got[i].adjusted), ref.sd->adjusted[i]) << at << " adjusted";
    EXPECT_EQ(cdsmodel::to_tdate(got[i].original), ref.sd->original[i]) << at << " original";
    EXPECT_EQ(cdsmodel::to_tdate(got[i].previous), ref.sd->previous[i]) << at << " previous";
    EXPECT_EQ(got[i].on_cycle, ref.sd->onCycle[i] == TRUE) << at << " onCycle";
  }
}

std::vector<cdsmodel::Date> maturities(int day) {
  std::vector<cdsmodel::Date> out;
  for (int years = 1; years <= 5; ++years) {
    const cdsmodel::Expected<cdsmodel::Date> date = cdsmodel::make_date(2024 + years, 1, day);
    if (date) {
      out.push_back(*date);
    }
  }
  return out;
}

TEST_F(SwapDatesDifferential, FromAdjustedMatchesAcrossDaysOfMonthAndFrequencies) {
  int compared = 0;
  for (const int value_day : {1, 15, 28, 29, 30, 31}) {
    const cdsmodel::Expected<cdsmodel::Date> value_date = cdsmodel::make_date(2024, 1, value_day);
    if (!value_date) {
      continue;
    }
    for (const int maturity_day : {15, 28, 29, 30, 31}) {
      const std::vector<cdsmodel::Date> mats = maturities(maturity_day);
      if (mats.empty()) {
        continue;
      }
      std::vector<TDate> raw;
      for (const cdsmodel::Date date : mats) {
        raw.push_back(cdsmodel::to_tdate(date));
      }
      for (const long freq : {1L, 2L, 4L, 12L}) {
        const Ref ref(JpmcdsSwapDatesNewFromAdjusted(cdsmodel::to_tdate(*value_date),
                                                     static_cast<int>(freq), raw.data(),
                                                     static_cast<int>(raw.size()), nullptr));
        const cdsmodel::Expected<std::vector<cdsmodel::SwapDate>> got =
            cdsmodel::swap_dates_from_adjusted(*value_date, freq, mats, std::nullopt);
        const std::string where = "vd " + std::to_string(value_day) + " mat " +
                                  std::to_string(maturity_day) + " freq " + std::to_string(freq);
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
        if (got) {
          expect_same(ref, *got, where);
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 80) << compared;
}

TEST_F(SwapDatesDifferential, FromAdjustedMatchesWithASubstitutionTable) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const std::vector<cdsmodel::Date> mats = maturities(15);
  std::vector<TDate> raw;
  for (const cdsmodel::Date date : mats) {
    raw.push_back(cdsmodel::to_tdate(date));
  }
  // A table that maps some of the maturities back to other dates.
  const std::vector<cdsmodel::BadDayPair> table{{d(2025, 1, 13), d(2025, 1, 15)},
                                                {d(2027, 1, 16), d(2027, 1, 15)}};
  RefList list(table);
  int compared = 0;
  for (const long freq : {1L, 2L, 4L}) {
    const Ref ref(JpmcdsSwapDatesNewFromAdjusted(cdsmodel::to_tdate(value_date),
                                                 static_cast<int>(freq), raw.data(),
                                                 static_cast<int>(raw.size()), &list.list));
    const cdsmodel::Expected<std::vector<cdsmodel::SwapDate>> got =
        cdsmodel::swap_dates_from_adjusted(value_date, freq, mats,
                                           std::span<const cdsmodel::BadDayPair>{table});
    const std::string where = "freq " + std::to_string(freq);
    ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
    if (got) {
      expect_same(ref, *got, where);
    }
    ++compared;
  }
  EXPECT_EQ(compared, 3);
}

TEST_F(SwapDatesDifferential, FromOriginalMatchesUnderEveryConvention) {
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  int compared = 0;
  for (const int day : {6, 15, 28, 31}) {
    const cdsmodel::Expected<cdsmodel::Date> value_date = cdsmodel::make_date(2024, 1, day);
    if (!value_date) {
      continue;
    }
    const std::vector<cdsmodel::Date> mats = maturities(day);
    std::vector<TDate> raw;
    for (const cdsmodel::Date date : mats) {
      raw.push_back(cdsmodel::to_tdate(date));
    }
    for (const cdsmodel::BadDayConv conv :
         {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
          cdsmodel::BadDayConv::Modified, cdsmodel::BadDayConv::None}) {
      for (const long freq : {1L, 4L}) {
        const Ref ref(JpmcdsSwapDatesNewFromOriginal(
            cdsmodel::to_tdate(*value_date), static_cast<int>(freq), raw.data(),
            static_cast<int>(raw.size()), nullptr, cdsmodel::to_upstream_code(conv),
            const_cast<char *>("NONE")));
        const cdsmodel::Expected<std::vector<cdsmodel::SwapDate>> got =
            cdsmodel::swap_dates_from_original(*value_date, freq, mats, std::nullopt, conv,
                                               calendars);
        const std::string where = "day " + std::to_string(day) + " conv " +
                                  std::string{static_cast<char>(conv)} + " freq " +
                                  std::to_string(freq);
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
        if (got) {
          expect_same(ref, *got, where);
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 25) << compared;
}

TEST_F(SwapDatesDifferential, AnInvalidFrequencyIsRejectedByBoth) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const std::vector<cdsmodel::Date> mats = maturities(15);
  std::vector<TDate> raw;
  for (const cdsmodel::Date date : mats) {
    raw.push_back(cdsmodel::to_tdate(date));
  }
  for (const long freq : {-1L, 0L, 13L, 100L}) {
    const Ref ref(JpmcdsSwapDatesNewFromAdjusted(cdsmodel::to_tdate(value_date),
                                                 static_cast<int>(freq), raw.data(),
                                                 static_cast<int>(raw.size()), nullptr));
    EXPECT_FALSE(static_cast<bool>(ref)) << freq;
    EXPECT_FALSE(
        cdsmodel::swap_dates_from_adjusted(value_date, freq, mats, std::nullopt).has_value())
        << freq;
  }
}

// =====================================================================
// The two divergences.
// =====================================================================

TEST_F(SwapDatesDifferential, Upstream26_ASubstitutionTableDisablesTheConvention) {
  // JpmcdsZCAdjustDate branches on the PRESENCE of a list, so a date the list
  // does not mention comes back unadjusted even though a convention was
  // supplied -- in a function documented as making a date "fall on a business
  // day".
  const cdsmodel::Date listed_saturday = d(2024, 1, 6);
  const cdsmodel::Date other_saturday = d(2024, 1, 13);
  const cdsmodel::Date monday = d(2024, 1, 8);
  const std::vector<cdsmodel::BadDayPair> table{{listed_saturday, monday}};
  RefList list(table);
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

  TDate out = 0;
  // With no list, the convention moves both Saturdays.
  ASSERT_EQ(JpmcdsZCAdjustDate(cdsmodel::to_tdate(other_saturday), nullptr, 'F',
                               const_cast<char *>("NONE"), &out),
            SUCCESS);
  const TDate moved_by_convention = out;
  EXPECT_NE(moved_by_convention, cdsmodel::to_tdate(other_saturday));

  // With the list, the unlisted Saturday is returned untouched.
  ASSERT_EQ(JpmcdsZCAdjustDate(cdsmodel::to_tdate(other_saturday), &list.list, 'F',
                               const_cast<char *>("NONE"), &out),
            SUCCESS);
  EXPECT_EQ(out, cdsmodel::to_tdate(other_saturday))
      << "still a Saturday, though a convention was supplied";
  EXPECT_FALSE(calendars.front().is_business_day(cdsmodel::from_tdate(out)))
      << "and not a business day, which is what the function claims to deliver";

  // The port reproduces both, which is what the optional span is for.
  EXPECT_EQ(cdsmodel::zc_adjust_date(other_saturday, std::nullopt, cdsmodel::BadDayConv::Follow,
                                     calendars),
            cdsmodel::from_tdate(moved_by_convention));
  EXPECT_EQ(cdsmodel::zc_adjust_date(other_saturday, std::span<const cdsmodel::BadDayPair>{table},
                                     cdsmodel::BadDayConv::Follow, calendars),
            other_saturday);
}

TEST_F(SwapDatesDifferential, Upstream26_AnEmptyTableIsNotTheSameAsNoTable) {
  // The branch is on the pointer, so an empty-but-present list adjusts
  // nothing. This is the measurement that decided the port's signature: a
  // plain span could not tell the two inputs apart.
  const cdsmodel::Date saturday = d(2024, 1, 6);
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  const std::vector<cdsmodel::BadDayPair> empty{};
  RefList none(empty);

  TDate with_convention = 0;
  TDate with_empty_list = 0;
  ASSERT_EQ(JpmcdsZCAdjustDate(cdsmodel::to_tdate(saturday), nullptr, 'F',
                               const_cast<char *>("NONE"), &with_convention),
            SUCCESS);
  ASSERT_EQ(JpmcdsZCAdjustDate(cdsmodel::to_tdate(saturday), &none.list, 'F',
                               const_cast<char *>("NONE"), &with_empty_list),
            SUCCESS);
  EXPECT_NE(with_convention, with_empty_list) << "the two inputs give different answers";
  EXPECT_EQ(with_empty_list, cdsmodel::to_tdate(saturday));

  // nullopt and an empty span must therefore differ in the port too.
  EXPECT_EQ(
      cdsmodel::zc_adjust_date(saturday, std::nullopt, cdsmodel::BadDayConv::Follow, calendars),
      cdsmodel::from_tdate(with_convention));
  EXPECT_EQ(cdsmodel::zc_adjust_date(saturday, std::span<const cdsmodel::BadDayPair>{},
                                     cdsmodel::BadDayConv::Follow, calendars),
            saturday);
}

TEST_F(SwapDatesDifferential, Upstream27_MonthEndDatesAreDeclaredOffCycleWhenTheyAreNot) {
  // The heuristic: "We assume we can only be on cycle if date is not on or
  // after the 29th of the month." JpmcdsCountDates disagrees.
  int suppressed = 0;
  int differing = 0;
  for (const long freq : {1L, 2L, 4L, 12L}) {
    const cdsmodel::Expected<cdsmodel::DateInterval> interval =
        cdsmodel::frequency_to_interval(freq);
    ASSERT_TRUE(interval.has_value()) << freq;
    for (int month = 1; month <= 12; ++month) {
      for (int value_day = 26; value_day <= 31; ++value_day) {
        const cdsmodel::Expected<cdsmodel::Date> value_date =
            cdsmodel::make_date(2024, month, value_day);
        // The value date's day and the maturity's are swept INDEPENDENTLY.
        // Holding them equal hides the whole finding: the cases where the
        // previous coupon date moves are 29 February against a 28 February
        // maturity, and 2024-02-29 -> 2029-02-29 is not a date at all.
        for (int years = 1; years <= 5; ++years) {
          for (int maturity_day = 26; maturity_day <= 31; ++maturity_day) {
            const cdsmodel::Expected<cdsmodel::Date> maturity =
                cdsmodel::make_date(2024 + years, month, maturity_day);
            if (!value_date || !maturity) {
              continue;
            }
            const cdsmodel::Expected<cdsmodel::DateCount> counted =
                cdsmodel::count_dates(*value_date, *maturity, *interval);
            if (!counted || counted->extra_days != 0) {
              continue; // not genuinely on cycle
            }
            const TDate raw = cdsmodel::to_tdate(*maturity);
            const Ref ref(JpmcdsSwapDatesNewFromAdjusted(cdsmodel::to_tdate(*value_date),
                                                         static_cast<int>(freq),
                                                         const_cast<TDate *>(&raw), 1, nullptr));
            ASSERT_TRUE(static_cast<bool>(ref));
            if (ref.sd->onCycle[0] == TRUE) {
              continue; // the heuristic agreed
            }
            ++suppressed;
            // It claims off cycle for a date CountDates calls on cycle. Does
            // that change the previous coupon date?
            const cdsmodel::Expected<cdsmodel::Date> forward =
                cdsmodel::date_from_date_and_offset(*value_date, *interval, counted->intervals - 1);
            const cdsmodel::Expected<cdsmodel::Date> backward =
                cdsmodel::date_from_date_and_offset(*maturity, *interval, -1);
            ASSERT_TRUE(forward.has_value());
            ASSERT_TRUE(backward.has_value());
            if (*forward != *backward) {
              ++differing;
              // Every one of these has a 29 February value date -- asserted as
              // a property, so a wider mechanism fails here rather than hiding.
              EXPECT_EQ(month, 2) << "an unexpected shape differs";
              EXPECT_EQ(value_day, 29) << "an unexpected shape differs";
            }
            // And the port reproduces upstream's choice either way.
            const cdsmodel::Expected<std::vector<cdsmodel::SwapDate>> got =
                cdsmodel::swap_dates_from_adjusted(*value_date, freq, {{*maturity}}, std::nullopt);
            ASSERT_TRUE(got.has_value());
            EXPECT_FALSE(got->front().on_cycle);
            EXPECT_EQ(cdsmodel::to_tdate(got->front().previous), ref.sd->previous[0]);
          }
        }
      }
    }
  }
  EXPECT_GT(suppressed, 400) << "genuinely on-cycle dates called off cycle: " << suppressed;
  EXPECT_GT(differing, 0) << "and the previous coupon date differs in some of them: " << differing;
}

} // namespace
