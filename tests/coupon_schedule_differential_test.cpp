// TIER 2 -- live differential tests for the coupon-date schedule.
//
// `JpmcdsNewCouponDatesSwap` takes sixteen parameters, so an exhaustive sweep
// is not available. The axes swept here are the ones that change the shape of
// the answer rather than shift it: stub position and length, in-arrears,
// whether the last accrual date is adjusted, the three bad-day conventions,
// the two day offsets, and spans that do and do not divide evenly by the
// coupon interval. Roll dates and the full-first-coupon date get their own
// sweeps, since each one changes which branch generates the cycle.
//
// Two tests assert a DIVERGENCE rather than a match -- the deliberate
// differences UPSTREAM-29 and UPSTREAM-30. They are written as divergences on
// purpose: if upstream is ever fixed, they fail and say so.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "dtlist.h"
#include "ldate.h"
#include "schedule.h"
}

namespace {

class CouponScheduleDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Owns an upstream schedule so a failing assertion cannot leak it.
struct Ref {
  TCouponDateList *list = nullptr;
  explicit Ref(TCouponDateList *owned) : list(owned) {}
  ~Ref() { JpmcdsFreeCouponDateList(list); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return list != nullptr; }
};

struct RefDates {
  TDateList *list = nullptr;
  ~RefDates() { JpmcdsFreeDateList(list); }
  RefDates() = default;
  RefDates(const RefDates &) = delete;
  RefDates &operator=(const RefDates &) = delete;
};

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

std::vector<cdsmodel::CouponDates> as_periods(const TCouponDateList *list) {
  std::vector<cdsmodel::CouponDates> out;
  for (int i = 0; i < list->fNumItems; ++i) {
    out.push_back(cdsmodel::CouponDates{cdsmodel::from_tdate(list->fArray[i].resetDate),
                                        cdsmodel::from_tdate(list->fArray[i].payDate),
                                        cdsmodel::from_tdate(list->fArray[i].accrueStartDate),
                                        cdsmodel::from_tdate(list->fArray[i].accrueEndDate)});
  }
  return out;
}

/// Calls both sides and asserts they agree -- on the schedule when both
/// produce one, and on the refusal when neither does.
void compare(const cdsmodel::CouponScheduleTerms &terms,
             const cdsmodel::CouponConventions &conventions, int &compared, int &refused) {
  TDateInterval raw = raw_interval(terms.coupon_interval);
  const auto *first_roll = std::get_if<cdsmodel::FirstRollDate>(&terms.roll_anchor);
  const auto *last_roll = std::get_if<cdsmodel::LastRollDate>(&terms.roll_anchor);

  const Ref ref(JpmcdsNewCouponDatesSwap(
      cdsmodel::to_tdate(terms.start), cdsmodel::to_tdate(terms.maturity), &raw,
      conventions.adjust_last_accrual_date ? TRUE : FALSE, conventions.in_arrears ? TRUE : FALSE,
      conventions.pay_offset_days, conventions.reset_offset_days,
      terms.stub_position == cdsmodel::StubPosition::Back ? TRUE : FALSE,
      terms.stub_length == cdsmodel::StubLength::Long ? TRUE : FALSE,
      first_roll != nullptr ? cdsmodel::to_tdate(first_roll->date) : 0,
      last_roll != nullptr ? cdsmodel::to_tdate(last_roll->date) : 0,
      terms.full_first_coupon_date ? cdsmodel::to_tdate(*terms.full_first_coupon_date) : 0,
      cdsmodel::to_upstream_code(conventions.pay), cdsmodel::to_upstream_code(conventions.accrual),
      cdsmodel::to_upstream_code(conventions.reset), const_cast<char *>("NONE")));

  const cdsmodel::Expected<cdsmodel::CouponSchedule> got =
      cdsmodel::coupon_schedule(terms, conventions, cdsmodel::Calendar::none());

  if (!ref) {
    EXPECT_FALSE(got.has_value()) << "port accepted what upstream refused";
    ++refused;
    return;
  }
  // The zero-period schedule is UPSTREAM-29, covered by its own test.
  if (ref.list->fNumItems == 0) {
    EXPECT_FALSE(got.has_value()) << "port returned a zero-period schedule";
    ++refused;
    return;
  }
  ASSERT_TRUE(got.has_value()) << "port refused what upstream accepted";
  EXPECT_EQ(as_periods(ref.list), got->periods);
  EXPECT_EQ(ref.list->stubLocation, static_cast<long>(got->stub));
  ++compared;
}

const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> &spans() {
  static const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> v{
      {d(2024, 1, 15), d(2029, 1, 15)}, // divides evenly by 3M, 6M, 12M
      {d(2024, 2, 20), d(2027, 5, 10)}, // divides by nothing tidy
      {d(2024, 1, 31), d(2026, 7, 31)}, // month ends, where clamping shows
      {d(2024, 2, 29), d(2027, 2, 28)}, // the leap-day span
      {d(2024, 3, 15), d(2024, 9, 15)}, // one interval exactly at 6M
      {d(2024, 3, 16), d(2024, 3, 18)}, // shorter than one interval
      {d(2024, 3, 16), d(2024, 3, 17)}, // a weekend, which collapses on adjust
  };
  return v;
}

TEST_F(CouponScheduleDifferential, MatchesAcrossStubsArrearsAndConventions) {
  int compared = 0;
  int refused = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int periods : {1, 3, 6, 12}) {
      for (const cdsmodel::StubPosition position :
           {cdsmodel::StubPosition::Front, cdsmodel::StubPosition::Back}) {
        for (const cdsmodel::StubLength length :
             {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
          for (const bool in_arrears : {false, true}) {
            for (const bool adjust_last : {false, true}) {
              for (const cdsmodel::BadDayConv conv :
                   {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
                    cdsmodel::BadDayConv::Previous, cdsmodel::BadDayConv::Modified}) {
                compare({.start = start,
                         .maturity = maturity,
                         .coupon_interval = {periods, cdsmodel::PeriodUnit::Months},
                         .stub_position = position,
                         .stub_length = length},
                        {.pay = conv,
                         .accrual = conv,
                         .reset = conv,
                         .adjust_last_accrual_date = adjust_last,
                         .in_arrears = in_arrears},
                        compared, refused);
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 1000) << "the sweep did not actually compare schedules";
  EXPECT_GT(refused, 0) << "no input was refused, so the refusal path is untested";
}

TEST_F(CouponScheduleDifferential, MatchesAcrossMixedConventionsAndOffsets) {
  int compared = 0;
  int refused = 0;
  // The three conventions differing from each other, which is what a real
  // contract looks like and what a positional signature gets wrong.
  for (const auto &[start, maturity] : spans()) {
    for (const int pay_offset : {-2, 0, 1, 5}) {
      for (const int reset_offset : {0, 2}) {
        compare({.start = start,
                 .maturity = maturity,
                 .coupon_interval = {6, cdsmodel::PeriodUnit::Months}},
                {.pay = cdsmodel::BadDayConv::Modified,
                 .accrual = cdsmodel::BadDayConv::Follow,
                 .reset = cdsmodel::BadDayConv::Previous,
                 .pay_offset_days = pay_offset,
                 .reset_offset_days = reset_offset},
                compared, refused);
      }
    }
  }
  EXPECT_GT(compared, 20);
}

TEST_F(CouponScheduleDifferential, MatchesAcrossRollDates) {
  int compared = 0;
  int refused = 0;
  const cdsmodel::Date start = d(2024, 1, 15);
  const cdsmodel::Date maturity = d(2028, 7, 20);
  for (const int month : {1, 2, 4, 7, 12, 30, 48}) {
    const cdsmodel::Expected<cdsmodel::Date> roll =
        cdsmodel::advance(start, {month, cdsmodel::PeriodUnit::Months});
    ASSERT_TRUE(roll.has_value());
    for (const bool use_first : {false, true}) {
      for (const cdsmodel::StubLength length :
           {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
        cdsmodel::RollAnchor anchor;
        if (use_first) {
          anchor = cdsmodel::FirstRollDate{*roll};
        } else {
          anchor = cdsmodel::LastRollDate{*roll};
        }
        compare({.start = start,
                 .maturity = maturity,
                 .coupon_interval = {6, cdsmodel::PeriodUnit::Months},
                 .stub_length = length,
                 .roll_anchor = anchor},
                {.pay = cdsmodel::BadDayConv::Follow,
                 .accrual = cdsmodel::BadDayConv::Follow,
                 .reset = cdsmodel::BadDayConv::Follow},
                compared, refused);
      }
    }
  }
  EXPECT_GT(compared, 20);
}

TEST_F(CouponScheduleDifferential, MatchesAcrossFullFirstCouponDates) {
  int compared = 0;
  int refused = 0;
  const cdsmodel::Date start = d(2024, 3, 15);
  for (const auto maturity : {d(2027, 1, 15), d(2027, 3, 15), d(2026, 11, 20)}) {
    for (const int months_back : {1, 3, 6, 12}) {
      const cdsmodel::Expected<cdsmodel::Date> ffc =
          cdsmodel::advance(start, {-months_back, cdsmodel::PeriodUnit::Months});
      ASSERT_TRUE(ffc.has_value());
      for (const cdsmodel::StubPosition position :
           {cdsmodel::StubPosition::Front, cdsmodel::StubPosition::Back}) {
        for (const bool in_arrears : {false, true}) {
          compare({.start = start,
                   .maturity = maturity,
                   .coupon_interval = {6, cdsmodel::PeriodUnit::Months},
                   .stub_position = position,
                   .full_first_coupon_date = *ffc},
                  {.pay = cdsmodel::BadDayConv::Follow,
                   .accrual = cdsmodel::BadDayConv::Follow,
                   .reset = cdsmodel::BadDayConv::Follow,
                   .in_arrears = in_arrears},
                  compared, refused);
        }
      }
    }
  }
  EXPECT_GT(compared, 20);
}

// The override lands after the resets are computed, so the first period can
// carry a reset later than its own accrual start. Asserted directly, because
// an implementation that applied the override first would still pass the
// sweep above on every period but the zeroth of a front-stub schedule.
TEST_F(CouponScheduleDifferential, FullFirstCouponLeavesResetAfterAccrualStart) {
  const cdsmodel::Date start = d(2024, 3, 15);
  const cdsmodel::Expected<cdsmodel::CouponSchedule> got =
      cdsmodel::coupon_schedule({.start = start,
                                 .maturity = d(2027, 1, 15),
                                 .coupon_interval = {6, cdsmodel::PeriodUnit::Months},
                                 .stub_position = cdsmodel::StubPosition::Front,
                                 .full_first_coupon_date = d(2023, 9, 15)},
                                {}, cdsmodel::Calendar::none());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->stub, cdsmodel::StubLocation::Front);
  EXPECT_EQ(got->periods.front().accrual_start, d(2023, 9, 15));
  EXPECT_EQ(got->periods.front().reset, start);
  EXPECT_GT(got->periods.front().reset, got->periods.front().accrual_start);
  EXPECT_TRUE(cdsmodel::is_well_formed(*got)) << "upstream's validator accepts this";
}

// UPSTREAM-29. Upstream returns a non-NULL schedule with zero periods, which
// its own JpmcdsCouponDateListCheck rejects, and which zcswap.c then indexes
// at fNumItems-1 == -1.
TEST_F(CouponScheduleDifferential, Upstream29ZeroPeriodSchedule) {
  const cdsmodel::Date date = d(2024, 3, 15);
  TDateInterval raw = raw_interval({6, cdsmodel::PeriodUnit::Months});
  for (const bool stub_at_end : {false, true}) {
    const Ref ref(JpmcdsNewCouponDatesSwap(cdsmodel::to_tdate(date), cdsmodel::to_tdate(date), &raw,
                                           TRUE, FALSE, 0, 0, stub_at_end ? TRUE : FALSE, FALSE, 0,
                                           0, 0, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                                           JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")));
    ASSERT_TRUE(static_cast<bool>(ref)) << "upstream refused; UPSTREAM-29 may be fixed";
    EXPECT_EQ(ref.list->fNumItems, 0);
    EXPECT_EQ(JpmcdsCouponDateListCheck(ref.list), FAILURE)
        << "upstream's own validator now accepts its own output";
    // The stub location also survives: at i == 0 upstream maps FRONT to NONE
    // but leaves BACK alone, so stubAtEnd decides whether a schedule with no
    // periods at all still claims a stub.
    EXPECT_EQ(ref.list->stubLocation, stub_at_end ? JPMCDS_BACK_STUB : JPMCDS_NO_STUB);

    EXPECT_FALSE(
        cdsmodel::coupon_schedule({.start = date,
                                   .maturity = date,
                                   .coupon_interval = {6, cdsmodel::PeriodUnit::Months},
                                   .stub_position = stub_at_end ? cdsmodel::StubPosition::Back
                                                                : cdsmodel::StubPosition::Front},
                                  {}, cdsmodel::Calendar::none())
            .has_value());
  }
}

// UPSTREAM-30. JpmcdsGenerateUnadjDates sizes its scratch array from the span
// startDate..matDate but steps the loop from the roll date. Exported, and it
// validates nothing. Not executed: the overflowing call is left DISABLED
// because running it is undefined behaviour. The port's refusal is asserted.
TEST_F(CouponScheduleDifferential, Upstream30RollDateOutsideSpanIsRefused) {
  const cdsmodel::CouponScheduleTerms terms{.start = d(2024, 1, 1),
                                            .maturity = d(2024, 7, 1),
                                            .coupon_interval = {1, cdsmodel::PeriodUnit::Months},
                                            .roll_anchor = cdsmodel::FirstRollDate{d(1990, 1, 1)}};
  EXPECT_FALSE(cdsmodel::generate_unadjusted_dates(terms).has_value());
  EXPECT_FALSE(cdsmodel::coupon_schedule(terms, {}, cdsmodel::Calendar::none()).has_value());
}

TEST_F(CouponScheduleDifferential, DISABLED_Upstream30Overflows) {
  TDateInterval raw = raw_interval({1, cdsmodel::PeriodUnit::Months});
  RefDates out;
  long stub = 0;
  // Writes past the end of a heap buffer. ASan-confirmed; see
  // docs/defect-report.md. Never enabled.
  JpmcdsGenerateUnadjDates(cdsmodel::to_tdate(d(2024, 1, 1)), cdsmodel::to_tdate(d(2024, 7, 1)),
                           &raw, FALSE, FALSE, cdsmodel::to_tdate(d(1990, 1, 1)), 0, &out.list,
                           &stub);
}

// The unadjusted cycle on its own, where upstream and the port agree and no
// calendar is involved.
TEST_F(CouponScheduleDifferential, UnadjustedDatesMatch) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int periods : {1, 3, 6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        for (const cdsmodel::StubLength length :
             {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
          TDateInterval raw = raw_interval({periods, cdsmodel::PeriodUnit::Months});
          RefDates out;
          long stub = 0;
          const int rc = JpmcdsGenerateUnadjDates(
              cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), &raw,
              stub_at_end ? TRUE : FALSE, length == cdsmodel::StubLength::Long ? TRUE : FALSE, 0, 0,
              &out.list, &stub);

          const cdsmodel::Expected<cdsmodel::UnadjustedDates> got =
              cdsmodel::generate_unadjusted_dates(
                  {.start = start,
                   .maturity = maturity,
                   .coupon_interval = {periods, cdsmodel::PeriodUnit::Months},
                   .stub_position =
                       stub_at_end ? cdsmodel::StubPosition::Back : cdsmodel::StubPosition::Front,
                   .stub_length = length});

          if (rc != SUCCESS) {
            EXPECT_FALSE(got.has_value());
            continue;
          }
          ASSERT_TRUE(got.has_value());
          ASSERT_EQ(out.list->fNumItems, static_cast<int>(got->dates.size()));
          for (int i = 0; i < out.list->fNumItems; ++i) {
            EXPECT_EQ(cdsmodel::from_tdate(out.list->fArray[i]),
                      got->dates[static_cast<std::size_t>(i)])
                << "date " << i;
          }
          EXPECT_EQ(stub, static_cast<long>(got->stub));
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 50);
}

} // namespace
