// TIER 2 -- live differential tests for schedule generation.
//
// These functions differ from each other in one or two lines each, and the
// differences are all at the ends of the list: which boundary date is included,
// which is replaced, which end the grid is counted from. So the sweep is over
// the thing that decides those -- stub direction crossed with intervals that
// divide the span and intervals that do not -- rather than over a handful of
// schedules that happen to be realistic.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_schedule.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "date_sup.h"
#include "datelist.h"
#include "dtlist.h"
#include "ldate.h"
}

namespace {

class DateScheduleDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// RAII wrapper so a failing assertion does not leak a list.
struct Ref {
  TDateList *dl = nullptr;
  explicit Ref(TDateList *owned) : dl(owned) {}
  ~Ref() { JpmcdsFreeDateList(dl); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return dl != nullptr; }
};

std::vector<cdsmodel::Date> as_dates(const TDateList *dl) {
  std::vector<cdsmodel::Date> out;
  for (int i = 0; i < dl->fNumItems; ++i) {
    out.push_back(cdsmodel::from_tdate(dl->fArray[i]));
  }
  return out;
}

TDateInterval upstream_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

/// Intervals that divide a three-year span, and intervals that do not.
std::vector<cdsmodel::DateInterval> intervals() {
  using cdsmodel::DateInterval;
  using cdsmodel::PeriodUnit;
  return {
      DateInterval{1, PeriodUnit::Months},  DateInterval{2, PeriodUnit::Months},
      DateInterval{3, PeriodUnit::Months},  DateInterval{4, PeriodUnit::Months},
      DateInterval{5, PeriodUnit::Months},  DateInterval{6, PeriodUnit::Months},
      DateInterval{7, PeriodUnit::Months},  DateInterval{12, PeriodUnit::Months},
      DateInterval{13, PeriodUnit::Months}, DateInterval{30, PeriodUnit::Days},
      DateInterval{91, PeriodUnit::Days},   DateInterval{182, PeriodUnit::Days},
  };
}

/// Start/maturity pairs: exact month boundaries, month ends, leap days, and a
/// span short enough that the grid is a single interval.
std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> spans() {
  return {
      {d(2024, 1, 15), d(2027, 1, 15)}, {d(2024, 1, 31), d(2027, 1, 31)},
      {d(2024, 2, 29), d(2027, 2, 28)}, {d(2024, 1, 15), d(2024, 4, 15)},
      {d(2024, 1, 15), d(2024, 2, 20)}, {d(2024, 3, 1), d(2029, 11, 30)},
      {d(2024, 1, 15), d(2024, 1, 15)},
  };
}

TEST_F(DateScheduleDifferential, DateListMatchesOverEveryStubAndInterval) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const cdsmodel::DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        TDateInterval raw = upstream_interval(iv);
        const Ref ref(JpmcdsNewDateList(cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity),
                                        &raw, stub_at_end ? TRUE : FALSE));
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
            cdsmodel::date_list(start, maturity, iv, stub_at_end);
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + ".." +
                                  std::to_string(cdsmodel::to_tdate(maturity)) + " iv " +
                                  std::to_string(iv.periods) +
                                  (iv.unit == cdsmodel::PeriodUnit::Months ? "M" : "D") +
                                  " stubAtEnd " + (stub_at_end ? "1" : "0");
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
        if (got) {
          EXPECT_EQ(*got, as_dates(ref.dl)) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 150) << compared;
}

TEST_F(DateScheduleDifferential, DateListExtendedMatchesOverEveryStubAndInterval) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const cdsmodel::DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        TDateInterval raw = upstream_interval(iv);
        const Ref ref(JpmcdsNewDateListExtended(cdsmodel::to_tdate(start),
                                                cdsmodel::to_tdate(maturity), &raw,
                                                stub_at_end ? TRUE : FALSE));
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
            cdsmodel::date_list_extended(start, maturity, iv, stub_at_end);
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + ".." +
                                  std::to_string(cdsmodel::to_tdate(maturity)) + " iv " +
                                  std::to_string(iv.periods) + " stubAtEnd " +
                                  (stub_at_end ? "1" : "0");
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
        if (got) {
          EXPECT_EQ(*got, as_dates(ref.dl)) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 150) << compared;
}

TEST_F(DateScheduleDifferential, PayDatesMatch) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const cdsmodel::DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        TDateInterval raw = upstream_interval(iv);
        const Ref ref(JpmcdsNewPayDates(cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity),
                                        &raw, stub_at_end ? TRUE : FALSE));
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
            cdsmodel::pay_dates(start, maturity, iv, stub_at_end);
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref))
            << cdsmodel::to_tdate(start) << " iv " << iv.periods;
        if (got) {
          EXPECT_EQ(*got, as_dates(ref.dl)) << cdsmodel::to_tdate(start) << " iv " << iv.periods;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 150) << compared;
}

TEST_F(DateScheduleDifferential, TheRollVariantMatchesIncludingWhereItIgnoresTheRollDate) {
  int compared = 0;
  const cdsmodel::Date rolls[] = {d(2024, 1, 15), d(2023, 7, 1), d(2024, 2, 20), d(2020, 3, 31)};
  for (const auto &[start, maturity] : spans()) {
    for (const cdsmodel::DateInterval iv : intervals()) {
      for (const cdsmodel::Date roll : rolls) {
        for (const bool stub_at_end : {false, true}) {
          TDateInterval raw = upstream_interval(iv);
          const Ref ref(JpmcdsNewDateListExtendedRoll(
              cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), cdsmodel::to_tdate(roll),
              &raw, stub_at_end ? TRUE : FALSE));
          const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
              cdsmodel::date_list_extended_roll(start, maturity, roll, iv, stub_at_end);
          const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " roll " +
                                    std::to_string(cdsmodel::to_tdate(roll)) + " iv " +
                                    std::to_string(iv.periods) + " stubAtEnd " +
                                    (stub_at_end ? "1" : "0");
          ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
          if (got) {
            EXPECT_EQ(*got, as_dates(ref.dl)) << where;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 600) << compared;
}

TEST_F(DateScheduleDifferential, NoRollDateMatchesUpstreamsZeroSentinel) {
  // Upstream spends TDate 0 as "no roll date". The port uses nullopt, so this
  // is the one place the two encodings have to be shown equivalent -- and 0 is
  // 1601-01-01, a date a caller could legitimately mean.
  for (const auto &[start, maturity] : spans()) {
    for (const cdsmodel::DateInterval iv : intervals()) {
      for (const bool stub_at_end : {false, true}) {
        TDateInterval raw = upstream_interval(iv);
        const Ref ref(JpmcdsNewDateListExtendedRoll(cdsmodel::to_tdate(start),
                                                    cdsmodel::to_tdate(maturity), 0L, &raw,
                                                    stub_at_end ? TRUE : FALSE));
        const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
            cdsmodel::date_list_extended_roll(start, maturity, std::nullopt, iv, stub_at_end);
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref));
        if (got) {
          EXPECT_EQ(*got, as_dates(ref.dl));
        }
      }
    }
  }
}

TEST_F(DateScheduleDifferential, BusinessDayAdjustmentMatchesForEveryConvention) {
  // Upstream resolves a holiday FILE per date; the port is handed calendars.
  // "NONE" is the only name both sides agree on without a file on disk, so
  // that is what this compares -- weekends only, which is the default
  // calendar either way.
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> schedule = cdsmodel::date_list(
      d(2024, 1, 6), d(2026, 1, 6), cdsmodel::DateInterval{1, cdsmodel::PeriodUnit::Months}, true);
  ASSERT_TRUE(schedule.has_value());
  // Upstream's built-in "NONE": no holiday dates, standard weekends.
  const cdsmodel::Calendar weekends = cdsmodel::Calendar::none();
  const std::vector<cdsmodel::Calendar> calendars{weekends};

  int compared = 0;
  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified, cdsmodel::BadDayConv::None}) {
    Ref ref(JpmcdsNewEmptyDateList(static_cast<int>(schedule->size())));
    ASSERT_TRUE(ref);
    for (std::size_t i = 0; i < schedule->size(); ++i) {
      ref.dl->fArray[i] = cdsmodel::to_tdate((*schedule)[i]);
    }
    const int rc = JpmcdsDateListBusDayAdj(ref.dl, cdsmodel::to_upstream_code(conv),
                                           const_cast<char *>("NONE"));
    const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
        cdsmodel::adjust_date_list(*schedule, conv, calendars);
    ASSERT_EQ(got.has_value(), rc == SUCCESS) << "conv " << static_cast<char>(conv);
    if (got) {
      EXPECT_EQ(*got, as_dates(ref.dl)) << "conv " << static_cast<char>(conv);
    }
    ++compared;
  }
  EXPECT_EQ(compared, 4);
}

TEST_F(DateScheduleDifferential, Upstream24_TheRollDateIsInertInExactlyTheDocumentedCase) {
  // Header: "starting from roll-date we see if we can count forward an
  // integral number of intervals to the maturity date. If this is so then we
  // can generate our datelist by counting forward from the rolldate."
  //
  // The code tests start->maturity instead, so the roll date is consulted
  // when there is NO stub and dropped when there is one.
  TDateInterval quarterly =
      upstream_interval(cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months});
  const cdsmodel::Date roll = d(2024, 1, 15);
  const cdsmodel::Date maturity = d(2027, 1, 15); // twelve quarters from roll
  const cdsmodel::Date start = d(2024, 2, 20);    // mid-period

  // The premise, from upstream's own counter.
  int intervals_from_roll = 0;
  int extra_from_roll = 0;
  ASSERT_EQ(JpmcdsCountDates(cdsmodel::to_tdate(roll), cdsmodel::to_tdate(maturity), &quarterly,
                             &intervals_from_roll, &extra_from_roll),
            SUCCESS);
  EXPECT_EQ(extra_from_roll, 0) << "roll -> maturity is integral, the documented case";

  int intervals_from_start = 0;
  int extra_from_start = 0;
  ASSERT_EQ(JpmcdsCountDates(cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), &quarterly,
                             &intervals_from_start, &extra_from_start),
            SUCCESS);
  EXPECT_NE(extra_from_start, 0) << "start -> maturity has a stub";

  // And the roll date makes no difference at all.
  const Ref with_roll(JpmcdsNewDateListExtendedRoll(cdsmodel::to_tdate(start),
                                                    cdsmodel::to_tdate(maturity),
                                                    cdsmodel::to_tdate(roll), &quarterly, FALSE));
  const Ref without(JpmcdsNewDateListExtendedRoll(
      cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), 0L, &quarterly, FALSE));
  ASSERT_TRUE(with_roll);
  ASSERT_TRUE(without);
  EXPECT_EQ(as_dates(with_roll.dl), as_dates(without.dl))
      << "the roll date changed nothing in the case it exists for";

  // Where it DOES take effect, start->maturity is integral -- no stub to fix --
  // and it introduces one, pushing the list past the maturity.
  const cdsmodel::Date aligned_start = d(2024, 4, 20);
  const cdsmodel::Date aligned_maturity = d(2027, 4, 20);
  const Ref effective(JpmcdsNewDateListExtendedRoll(cdsmodel::to_tdate(aligned_start),
                                                    cdsmodel::to_tdate(aligned_maturity),
                                                    cdsmodel::to_tdate(roll), &quarterly, FALSE));
  const Ref ignored(JpmcdsNewDateListExtendedRoll(cdsmodel::to_tdate(aligned_start),
                                                  cdsmodel::to_tdate(aligned_maturity), 0L,
                                                  &quarterly, FALSE));
  ASSERT_TRUE(effective);
  ASSERT_TRUE(ignored);
  EXPECT_NE(as_dates(effective.dl), as_dates(ignored.dl))
      << "here the roll date matters, which is the case it is not needed in";
  EXPECT_GT(effective.dl->fArray[effective.dl->fNumItems - 1], cdsmodel::to_tdate(aligned_maturity))
      << "and it pushes the schedule past maturity";
  EXPECT_LT(effective.dl->fArray[0], cdsmodel::to_tdate(aligned_start))
      << "and begins before the start";

  // The port reproduces all of it -- deliberately. See the header.
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> ported_inert =
      cdsmodel::date_list_extended_roll(
          start, maturity, roll, cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months}, false);
  ASSERT_TRUE(ported_inert.has_value());
  EXPECT_EQ(*ported_inert, as_dates(with_roll.dl));
  const cdsmodel::Expected<std::vector<cdsmodel::Date>> ported_effective =
      cdsmodel::date_list_extended_roll(aligned_start, aligned_maturity, roll,
                                        cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months},
                                        false);
  ASSERT_TRUE(ported_effective.has_value());
  EXPECT_EQ(*ported_effective, as_dates(effective.dl));
}

TEST_F(DateScheduleDifferential, PassingTheStartDateAsTheRollDateIsNotAlwaysANoOp) {
  // cashflow.c:230 passes startDate AS the roll date. The obvious reading is
  // that this makes the mechanism inert: count_dates(roll, start) is trivially
  // integral, so firstDate becomes startDate.
  //
  // It is not inert, and three hand-picked probes said it was. What actually
  // happens is that the roll branch ends in
  // `NewDateListExtended(first, maturity, interval, TRUE)` -- generating
  // FORWARD from the start -- where rollDate = 0 generates BACKWARD from
  // maturity. Those agree only where month arithmetic does not clamp:
  // 29 Feb + 12M = 28 Feb, but 28 Feb - 12M = 28 Feb, not 29 Feb.
  //
  // Measured over every integral-span combination of start and maturity
  // day-of-month: 7 of 378 differ, and every one of them starts on 29
  // February. That is asserted as a property rather than as the count, so a
  // broader mechanism would fail this rather than slip through.
  int integral_spans = 0;
  int differing = 0;
  for (int month = 1; month <= 12; ++month) {
    for (int start_day = 27; start_day <= 31; ++start_day) {
      const cdsmodel::Expected<cdsmodel::Date> start = cdsmodel::make_date(2024, month, start_day);
      if (!start) {
        continue;
      }
      for (int maturity_day = 27; maturity_day <= 31; ++maturity_day) {
        const cdsmodel::Expected<cdsmodel::Date> maturity =
            cdsmodel::make_date(2027, month, maturity_day);
        if (!maturity) {
          continue;
        }
        for (int periods = 1; periods <= 12; ++periods) {
          const cdsmodel::DateInterval iv{periods, cdsmodel::PeriodUnit::Months};
          TDateInterval raw = upstream_interval(iv);
          int intervals = 0;
          int extra = 0;
          if (JpmcdsCountDates(cdsmodel::to_tdate(*start), cdsmodel::to_tdate(*maturity), &raw,
                               &intervals, &extra) != SUCCESS ||
              extra != 0) {
            continue; // the roll branch is only reachable on an integral span
          }
          ++integral_spans;

          const Ref as_called(JpmcdsNewDateListExtendedRoll(
              cdsmodel::to_tdate(*start), cdsmodel::to_tdate(*maturity), cdsmodel::to_tdate(*start),
              &raw, FALSE));
          const Ref no_roll(JpmcdsNewDateListExtendedRoll(
              cdsmodel::to_tdate(*start), cdsmodel::to_tdate(*maturity), 0L, &raw, FALSE));
          ASSERT_TRUE(as_called);
          ASSERT_TRUE(no_roll);

          // Whatever upstream does, the port must do.
          const cdsmodel::Expected<std::vector<cdsmodel::Date>> ported =
              cdsmodel::date_list_extended_roll(*start, *maturity, *start, iv, false);
          ASSERT_TRUE(ported.has_value());
          const std::string where = std::to_string(month) + "/" + std::to_string(start_day) +
                                    " -> " + std::to_string(maturity_day) + " iv " +
                                    std::to_string(periods);
          EXPECT_EQ(*ported, as_dates(as_called.dl)) << where;

          if (as_dates(as_called.dl) != as_dates(no_roll.dl)) {
            ++differing;
            // Every difference must come from a clamped month end. February
            // 29 is the only start that can be clamped by a month step.
            EXPECT_EQ(month, 2) << where << ": an unexpected shape differs";
            EXPECT_EQ(start_day, 29) << where << ": an unexpected shape differs";
          }
        }
      }
    }
  }
  EXPECT_GT(integral_spans, 300) << integral_spans;
  EXPECT_GT(differing, 0) << "the mechanism is NOT inert, which is the point of this test";
}

} // namespace
