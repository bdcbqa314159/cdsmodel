// Emits tests/golden/coupon_schedule_cases.hpp.
//
// Records coupon schedules built by JpmcdsNewCouponDatesSwap. Every recorded
// quantity is a date or a small integer, so the replay compares EXACTLY --
// there is no tolerance in this header and there should not be one.
//
// Schedules are variable length, so the periods live in one flat pool and each
// case carries an offset and a count into it. That is a deliberate exception
// to the no-parallel-arrays rule: a generated constexpr table cannot hold a
// vector, and the replay reconstructs the schedule before comparing.
//
// The sweep is narrower than the tier-2 differential sweep on purpose. Tier 1
// exists so CI has coverage with no oracle present, not to re-run the live
// comparison; the spans are kept short enough that the pool stays readable.
//
// UPSTREAM-29's zero-period schedules ARE recorded, as cases with
// `accepted == true` and `period_count == 0`. The replay asserts the port
// refuses them, so the deliberate difference is pinned without the oracle.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_coupon_schedule_golden
//   ./build/debug/gen_coupon_schedule_golden > tests/golden/coupon_schedule_cases.hpp

#include <cstdio>
#include <string>
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

struct Inputs {
  TDate start = 0;
  TDate maturity = 0;
  int periods = 6;
  bool stub_at_end = false;
  bool long_stub = false;
  bool in_arrears = false;
  bool adjust_last = true;
  long conv = JPMCDS_BAD_DAY_NONE;
  int pay_offset = 0;
  int reset_offset = 0;
  TDate first_roll = 0;
  TDate last_roll = 0;
  TDate full_first_coupon = 0;
};

TDate ymd(long y, long m, long d) {
  TMonthDayYear mdy;
  mdy.year = y;
  mdy.month = m;
  mdy.day = d;
  TDate out = 0;
  JpmcdsMDYToDate(&mdy, &out);
  return out;
}

std::vector<std::string> g_cases;
std::vector<std::string> g_periods;

void record(const Inputs &in) {
  TDateInterval iv;
  JpmcdsMakeDateInterval(in.periods, 'M', &iv);
  TCouponDateList *s = JpmcdsNewCouponDatesSwap(
      in.start, in.maturity, &iv, in.adjust_last ? TRUE : FALSE, in.in_arrears ? TRUE : FALSE,
      in.pay_offset, in.reset_offset, in.stub_at_end ? TRUE : FALSE, in.long_stub ? TRUE : FALSE,
      in.first_roll, in.last_roll, in.full_first_coupon, in.conv, in.conv, in.conv, (char *)"NONE");

  const std::size_t offset = g_periods.size();
  long stub = 0;
  std::size_t count = 0;
  if (s != nullptr) {
    stub = s->stubLocation;
    count = static_cast<std::size_t>(s->fNumItems);
    for (int i = 0; i < s->fNumItems; ++i) {
      char row[120];
      std::snprintf(row, sizeof row, "    {%ld, %ld, %ld, %ld},", (long)s->fArray[i].resetDate,
                    (long)s->fArray[i].payDate, (long)s->fArray[i].accrueStartDate,
                    (long)s->fArray[i].accrueEndDate);
      g_periods.emplace_back(row);
    }
  }

  char row[320];
  std::snprintf(
      row, sizeof row,
      "    {%ld, %ld, %d, %s, %s, %s, %s, %ld, %d, %d, %ld, %ld, %ld, %s, %ld, %zu, %zu},",
      (long)in.start, (long)in.maturity, in.periods, in.stub_at_end ? "true" : "false",
      in.long_stub ? "true" : "false", in.in_arrears ? "true" : "false",
      in.adjust_last ? "true" : "false", in.conv, in.pay_offset, in.reset_offset,
      (long)in.first_roll, (long)in.last_roll, (long)in.full_first_coupon,
      s != nullptr ? "true" : "false", stub, offset, count);
  g_cases.emplace_back(row);
  JpmcdsFreeCouponDateList(s);
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const TDate spans[][2] = {
      {ymd(2024, 1, 15), ymd(2027, 1, 15)}, // divides evenly
      {ymd(2024, 2, 20), ymd(2026, 5, 10)}, // divides by nothing tidy
      {ymd(2024, 1, 31), ymd(2026, 7, 31)}, // month ends
      {ymd(2024, 2, 29), ymd(2026, 2, 28)}, // leap day
  };
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW, JPMCDS_BAD_DAY_MODIFIED};

  for (const auto &span : spans) {
    for (const int periods : {6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        for (const bool long_stub : {false, true}) {
          for (const bool in_arrears : {false, true}) {
            for (const bool adjust_last : {false, true}) {
              for (const long conv : convs) {
                record({.start = span[0],
                        .maturity = span[1],
                        .periods = periods,
                        .stub_at_end = stub_at_end,
                        .long_stub = long_stub,
                        .in_arrears = in_arrears,
                        .adjust_last = adjust_last,
                        .conv = conv});
              }
            }
          }
        }
      }
    }
  }

  // Day offsets, with the three conventions differing from each other.
  for (const int pay_offset : {-2, 0, 1, 5}) {
    for (const int reset_offset : {0, 2}) {
      record({.start = ymd(2024, 1, 15),
              .maturity = ymd(2026, 7, 20),
              .pay_offset = pay_offset,
              .reset_offset = reset_offset,
              .first_roll = 0});
    }
  }

  // Roll dates, both anchors, which select the two branches that seed the
  // cycle from a date other than the start or the maturity.
  for (const int months : {1, 4, 7, 12, 24}) {
    TDateInterval iv;
    JpmcdsMakeDateInterval(months, 'M', &iv);
    TDate roll = 0;
    JpmcdsDtFwdAny(ymd(2024, 1, 15), &iv, &roll);
    for (const bool long_stub : {false, true}) {
      record({.start = ymd(2024, 1, 15),
              .maturity = ymd(2027, 7, 20),
              .long_stub = long_stub,
              .conv = JPMCDS_BAD_DAY_FOLLOW,
              .first_roll = roll});
      record({.start = ymd(2024, 1, 15),
              .maturity = ymd(2027, 7, 20),
              .long_stub = long_stub,
              .conv = JPMCDS_BAD_DAY_FOLLOW,
              .last_roll = roll});
    }
  }

  // Full first coupon date, where the override lands after the resets.
  for (const int months_back : {1, 3, 6, 12}) {
    TDateInterval iv;
    JpmcdsMakeDateInterval(-months_back, 'M', &iv);
    TDate ffc = 0;
    JpmcdsDtFwdAny(ymd(2024, 3, 15), &iv, &ffc);
    for (const bool stub_at_end : {false, true}) {
      for (const bool in_arrears : {false, true}) {
        record({.start = ymd(2024, 3, 15),
                .maturity = ymd(2027, 1, 15),
                .stub_at_end = stub_at_end,
                .in_arrears = in_arrears,
                .conv = JPMCDS_BAD_DAY_FOLLOW,
                .full_first_coupon = ffc});
      }
    }
  }

  // UPSTREAM-29: start == maturity, and a span short enough that bad-day
  // adjustment collapses it. Both record as accepted with zero periods.
  for (const bool stub_at_end : {false, true}) {
    record({.start = ymd(2024, 3, 15), .maturity = ymd(2024, 3, 15), .stub_at_end = stub_at_end});
    record({.start = ymd(2024, 3, 16), // Saturday
            .maturity = ymd(2024, 3, 17),
            .stub_at_end = stub_at_end,
            .conv = JPMCDS_BAD_DAY_FOLLOW});
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Recorded coupon schedules from JpmcdsNewCouponDatesSwap. Dates and small
// integers only, so the replay compares EXACTLY -- no tolerance here.
//
// The periods of every schedule live in one flat pool; each case carries an
// offset and a count into it.
//
// A case with `accepted == true` and `period_count == 0` is UPSTREAM-29: a
// schedule upstream returns and its own validator rejects. The port refuses
// those, and the replay asserts the refusal.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_coupon_schedule_golden
//   ./build/debug/gen_coupon_schedule_golden > tests/golden/coupon_schedule_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// One period of a recorded schedule.
struct RecordedCouponPeriod {
  TDate reset;
  TDate pay;
  TDate accrual_start;
  TDate accrual_end;
};

/// One call, its arguments and its answer. The three TDate fields use
/// upstream's `0 means unsupplied` encoding; the replay converts.
struct CouponScheduleCase {
  TDate start;
  TDate maturity;
  int periods;
  bool stub_at_end;
  bool long_stub;
  bool in_arrears;
  bool adjust_last;
  long conv;
  int pay_offset;
  int reset_offset;
  TDate first_roll;
  TDate last_roll;
  TDate full_first_coupon;
  bool accepted;
  long stub_location;
  std::size_t first_period;
  std::size_t period_count;
};

)");

  std::printf("inline constexpr std::array<RecordedCouponPeriod, %zu> kCouponSchedulePeriods{{\n",
              g_periods.size());
  for (const std::string &r : g_periods) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<CouponScheduleCase, %zu> kCouponScheduleCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
