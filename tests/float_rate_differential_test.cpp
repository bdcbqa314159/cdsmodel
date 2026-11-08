// TIER 2 -- live differential tests for the floating rate definition.
//
// fltrate.c is one assignment-only function, so there is little arithmetic to
// disagree about. What these tests pin is the FIELD MAPPING and the one place
// it is load-bearing: the spot offset, which `JpmcdsFloatRateSet` builds
// through a macro whose name says days and whose body says calendar days with
// adjustment off. If the port's `calendar_day_offset` ever drifted to business
// days, every forward rate in the swap leg would move, and nothing else in the
// suite would notice.
//
// So the second test does not compare structs at all: it advances a date by
// the recorded spot offset through both implementations, over a sweep that
// includes every weekday, and requires them to agree.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/float_rate.hpp>

#include <gtest/gtest.h>

#include <optional>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "fltrate.h"
#include "ldate.h"
}

namespace {

class FloatRateDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

TEST_F(FloatRateDifferential, FieldMappingMatchesTheSetter) {
  int compared = 0;
  for (const int mat_months : {3, 6, 12, 60}) {
    for (const int pay_months : {0, 3, 6, 12}) {
      for (const cdsmodel::DayCount dcc :
           {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
            cdsmodel::DayCount::Thirty360}) {
        for (const int spot : {0, 1, 2, 5}) {
          for (const double spread : {0.0, 0.0025, -0.001}) {
            for (const double weight : {1.0, 0.5, 2.0}) {
              const cdsmodel::DateInterval mat{mat_months, cdsmodel::PeriodUnit::Months};
              const cdsmodel::DateInterval pay{pay_months, cdsmodel::PeriodUnit::Months};

              TDateInterval raw_mat = raw_interval(mat);
              TDateInterval raw_pay = raw_interval(pay);
              TFloatRate ref{};
              ASSERT_EQ(JpmcdsFloatRateSet(&raw_mat, &raw_pay, cdsmodel::to_upstream_code(dcc),
                                           spot, spread, weight, &ref),
                        SUCCESS)
                  << "the setter cannot fail, so this would be news";

              const cdsmodel::FloatRate got{.maturity_interval = mat,
                                            .pay_interval = pay,
                                            .day_count = dcc,
                                            .spot_offset = cdsmodel::calendar_day_offset(spot),
                                            .spread = spread,
                                            .weight = weight};

              EXPECT_EQ(ref.matInterval.prd, got.maturity_interval.periods);
              EXPECT_EQ(ref.payInterval.prd, got.pay_interval.periods);
              EXPECT_EQ(ref.dayCountConv, cdsmodel::to_upstream_code(got.day_count));
              EXPECT_DOUBLE_EQ(ref.spread, got.spread);
              EXPECT_DOUBLE_EQ(ref.weight, got.weight);

              // The macro's expansion, field by field.
              EXPECT_EQ(ref.spotOffset.interval.prd, got.spot_offset.interval.periods);
              EXPECT_EQ(ref.spotOffset.interval.prd_typ, 'D');
              EXPECT_EQ(got.spot_offset.interval.unit, cdsmodel::PeriodUnit::Days);
              EXPECT_EQ(ref.spotOffset.isBusDays, JPMCDS_DATE_ADJ_TYPE_CALENDAR);
              EXPECT_EQ(got.spot_offset.basis, cdsmodel::OffsetBasis::CalendarPeriod);
              EXPECT_EQ(ref.spotOffset.badDayConv, JPMCDS_BAD_DAY_NONE);
              EXPECT_EQ(got.spot_offset.bad_day, cdsmodel::BadDayConv::None);
              EXPECT_EQ(ref.spotOffset.holidayFile, nullptr);
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 400) << "the sweep did not actually compare anything";
}

// The composition streamcf.c performs: reset date, advanced by the spot
// offset, is the index start date.
TEST_F(FloatRateDifferential, SpotOffsetAdvancesIdenticallyFromEveryWeekday) {
  int compared = 0;
  int landed_on_a_weekend = 0;
  const cdsmodel::Calendar calendar = cdsmodel::Calendar::none();
  TDateInterval raw_mat = raw_interval({6, cdsmodel::PeriodUnit::Months});
  TDateInterval raw_pay = raw_interval({6, cdsmodel::PeriodUnit::Months});

  for (const int spot : {0, 1, 2, 3, 7}) {
    TFloatRate ref{};
    ASSERT_EQ(JpmcdsFloatRateSet(&raw_mat, &raw_pay, JPMCDS_ACT_360, spot, 0.0, 1.0, &ref),
              SUCCESS);
    const cdsmodel::AdjustedOffset offset = cdsmodel::calendar_day_offset(spot);

    // A full week of start dates, so every weekday is a reset at least once.
    for (int day = 11; day <= 24; ++day) {
      const cdsmodel::Date reset = d(2024, 3, day);
      TDate raw_out = 0;
      const bool accepted =
          JpmcdsDtFwdAdj(cdsmodel::to_tdate(reset), &ref.spotOffset, &raw_out) == SUCCESS;
      const cdsmodel::Expected<cdsmodel::Date> got =
          cdsmodel::advance_adjusted(reset, offset, calendar);

      ASSERT_EQ(accepted, got.has_value()) << "spot " << spot << " day " << day;
      if (!accepted) {
        continue;
      }
      EXPECT_EQ(raw_out, cdsmodel::to_tdate(*got)) << "spot " << spot << " day " << day;

      const std::chrono::weekday wd{*got};
      if (wd == std::chrono::Saturday || wd == std::chrono::Sunday) {
        ++landed_on_a_weekend;
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 50);
  // The point of the header note, asserted rather than claimed: this offset
  // does not avoid weekends, because the macro disables adjustment. If this
  // ever reaches zero, calendar_day_offset has drifted to business days.
  EXPECT_GT(landed_on_a_weekend, 0)
      << "the spot offset is avoiding weekends, so it is no longer a calendar offset";
}

// Upstream dereferences all three pointers with no check and returns SUCCESS
// unconditionally. There is nothing to compare for the port, which has no
// setter -- recorded here so the absence is deliberate rather than forgotten.
TEST_F(FloatRateDifferential, TheSetterValidatesNothing) {
  TDateInterval mat = raw_interval({6, cdsmodel::PeriodUnit::Months});
  TDateInterval pay = raw_interval({6, cdsmodel::PeriodUnit::Months});
  TFloatRate ref{};
  // 9999 is not a day count convention JpmcdsDayCountValid accepts, and
  // yearfrac.h is already included by fltrate.c.
  EXPECT_EQ(JpmcdsFloatRateSet(&mat, &pay, 9999, -5, 0.0, 1.0, &ref), SUCCESS);
  EXPECT_EQ(ref.dayCountConv, 9999);
  // A negative spot offset is stored as given, too.
  EXPECT_EQ(ref.spotOffset.interval.prd, -5);
  // The port cannot hold either: DayCount is an enum, so 9999 is not a value,
  // and the offset is only built from calendar_day_offset.
}

} // namespace
