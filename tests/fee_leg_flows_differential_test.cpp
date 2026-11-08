// TIER 2 -- live differential tests for the fee leg's non-contingent flows.
//
// These are the cash flows a protection buyer pays if there is no default: the
// schedule, not a price. No discounting and no survival probability, so unlike
// most of this suite the comparison is of products rather than of sums, and
// ULPs are the right tier.
//
// Both functions carried a deferral note for most of the project saying they
// needed cashflow.c. That stopped being true several slices ago and nothing
// noticed until `port_progress.py` grew a mentioned-but-not-claimed check.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cds.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cmemory.h"
#include "convert.h"
#include "cx.h"
#include "feeleg.h"
#include "ldate.h"
#include "macros.h"
#include "stub.h"
}

namespace {

class FeeLegFlowsDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct RefCFL {
  TCashFlowList *cfl = nullptr;
  ~RefCFL() { JpmcdsFreeCFL(cfl); }
  RefCFL() = default;
  explicit RefCFL(TCashFlowList *owned) : cfl(owned) {}
  RefCFL(const RefCFL &) = delete;
  RefCFL &operator=(const RefCFL &) = delete;
  explicit operator bool() const { return cfl != nullptr; }
};

struct RefFl {
  TFeeLeg *p = nullptr;
  ~RefFl() { JpmcdsFeeLegFree(p); }
  RefFl() = default;
  explicit RefFl(TFeeLeg *owned) : p(owned) {}
  RefFl(const RefFl &) = delete;
  RefFl &operator=(const RefFl &) = delete;
  explicit operator bool() const { return p != nullptr; }
};

void expect_flows_match(const TCashFlowList *ref, const std::vector<cdsmodel::CashFlow> &got,
                        const char *what) {
  ASSERT_EQ(ref->fNumItems, static_cast<int>(got.size())) << what;
  for (int i = 0; i < ref->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->fArray[i].fDate, cdsmodel::to_tdate(got[k].date)) << what << " date " << i;
    // yearFrac * rate * notional -- two multiplies, nothing accumulated, and
    // one computation against itself in the same binary. ULPs.
    EXPECT_NEAR(ref->fArray[i].fAmount, got[k].amount,
                4.0 * std::max(1.0, std::abs(ref->fArray[i].fAmount)) *
                    std::numeric_limits<double>::epsilon())
        << what << " amount " << i;
  }
}

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

// The leg-level function, on legs built by hand so the schedule is not also
// under test.
TEST_F(FeeLegFlowsDifferential, FeeLegFlowsMatchesOnHandBuiltLegs) {
  int compared = 0;
  const std::vector<std::vector<cdsmodel::FeePayment>> schedules{
      {},
      {{d(2024, 1, 15), d(2024, 4, 15), d(2024, 4, 15)}},
      {{d(2024, 1, 15), d(2024, 4, 15), d(2024, 4, 15)},
       {d(2024, 4, 15), d(2024, 7, 15), d(2024, 7, 15)},
       {d(2024, 7, 15), d(2025, 1, 15), d(2025, 1, 15)}},
      // A pay date later than the accrual end, which is what a pay offset
      // produces and which the amount must not depend on.
      {{d(2024, 1, 15), d(2024, 4, 15), d(2024, 4, 20)}},
      // Zero-length accrual: the amount must be zero, not an error.
      {{d(2024, 1, 15), d(2024, 1, 15), d(2024, 4, 15)}},
      // Across a leap day.
      {{d(2024, 2, 15), d(2024, 3, 15), d(2024, 3, 15)}},
  };

  for (const auto &schedule : schedules) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
          cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360}) {
      for (const double notional : {1.0, 1.0e7, -5.0e6}) {
        for (const double coupon : {0.0, 0.01, -0.005}) {
          std::vector<TDate> starts;
          std::vector<TDate> ends;
          std::vector<TDate> pays;
          for (const cdsmodel::FeePayment &p : schedule) {
            starts.push_back(cdsmodel::to_tdate(p.accrual_start));
            ends.push_back(cdsmodel::to_tdate(p.accrual_end));
            pays.push_back(cdsmodel::to_tdate(p.pay));
          }
          RefFl leg(JpmcdsFeeLegMakeEmpty(static_cast<int>(schedule.size())));
          ASSERT_TRUE(static_cast<bool>(leg));
          for (std::size_t i = 0; i < schedule.size(); ++i) {
            leg.p->accStartDates[i] = starts[i];
            leg.p->accEndDates[i] = ends[i];
            leg.p->payDates[i] = pays[i];
          }
          leg.p->notional = notional;
          leg.p->couponRate = coupon;
          leg.p->dcc = cdsmodel::to_upstream_code(dcc);
          leg.p->accrualPayConv = ACCRUAL_PAY_ALL;
          leg.p->obsStartOfDay = TRUE;

          RefCFL ref(JpmcdsFeeLegFlows(leg.p));

          const cdsmodel::FeeLeg port = cdsmodel::make_fee_leg(schedule, notional, coupon, dcc,
                                                               cdsmodel::AccrualPayment::All, true);
          const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
              cdsmodel::fee_leg_flows(port);

          ASSERT_EQ(static_cast<bool>(ref), got.has_value()) << "acceptance differs";
          if (ref) {
            expect_flows_match(ref.cfl, *got, "fee_leg_flows");
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 150) << "the sweep compared almost nothing";
}

// The amount depends on the ACCRUAL dates and the date on the PAY date, which
// a port could easily conflate while still producing plausible numbers.
TEST_F(FeeLegFlowsDifferential, TheAmountUsesAccrualDatesAndTheDateUsesThePayDate) {
  const std::vector<cdsmodel::FeePayment> schedule{
      {d(2024, 1, 15), d(2024, 4, 15), d(2024, 5, 20)}};
  const cdsmodel::FeeLeg leg = cdsmodel::make_fee_leg(
      schedule, 1.0e6, 0.01, cdsmodel::DayCount::Act360, cdsmodel::AccrualPayment::All, true);
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got = cdsmodel::fee_leg_flows(leg);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 1u);

  EXPECT_EQ(got->front().date, d(2024, 5, 20)) << "the flow is dated at the PAY date";
  // 91 days from 15 January to 15 April, ACT/360.
  const cdsmodel::Expected<double> year_frac =
      cdsmodel::year_fraction(d(2024, 1, 15), d(2024, 4, 15), cdsmodel::DayCount::Act360);
  ASSERT_TRUE(year_frac.has_value());
  EXPECT_DOUBLE_EQ(got->front().amount, *year_frac * 0.01 * 1.0e6);
  // And the accrual really is the shorter span, not the span to the pay date.
  const cdsmodel::Expected<double> to_pay =
      cdsmodel::year_fraction(d(2024, 1, 15), d(2024, 5, 20), cdsmodel::DayCount::Act360);
  ASSERT_TRUE(to_pay.has_value());
  EXPECT_NE(got->front().amount, *to_pay * 0.01 * 1.0e6)
      << "the amount was computed to the pay date";
}

// The CDS-level entry point, where the schedule IS under test.
TEST_F(FeeLegFlowsDifferential, CdsFeeLegFlowsMatchesAcrossSchedulesAndConventions) {
  int compared = 0;
  int refused_by_both = 0;
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

  for (const auto &[start, end] : std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>>{
           {d(2024, 3, 20), d(2029, 6, 20)}, // on cycle for 3M
           {d(2024, 3, 20), d(2029, 7, 11)}, // off cycle: a stub
           {d(2024, 2, 29), d(2025, 2, 28)}, // leap day
           {d(2024, 3, 20), d(2024, 6, 20)}, // one period
           {d(2024, 3, 20), d(2024, 3, 20)}, // zero length
       }) {
    for (const int months : {3, 6, 12}) {
      for (const cdsmodel::DayCount dcc :
           {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Thirty360}) {
        for (const cdsmodel::BadDayConv conv :
             {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
          TDateInterval raw = raw_interval({months, cdsmodel::PeriodUnit::Months});
          TStubMethod stub{};
          ASSERT_EQ(JpmcdsStringToStubMethod(const_cast<char *>("f/s"), &stub), SUCCESS);

          RefCFL ref(JpmcdsCdsFeeLegFlows(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), &raw,
                                          &stub, 1.0e7, 0.01, cdsmodel::to_upstream_code(dcc),
                                          cdsmodel::to_upstream_code(conv),
                                          const_cast<char *>("NONE")));

          const cdsmodel::Cds cds{.start = start,
                                  .end = end,
                                  .coupon_rate = 0.01,
                                  .coupon_interval = {months, cdsmodel::PeriodUnit::Months},
                                  .day_count = dcc,
                                  .bad_day = conv};
          const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
              cdsmodel::cds_fee_leg_flows(cds, 1.0e7, calendars);

          ASSERT_EQ(static_cast<bool>(ref), got.has_value())
              << "acceptance differs, months " << months;
          if (!ref) {
            ++refused_by_both;
            continue;
          }
          expect_flows_match(ref.cfl, *got, "cds_fee_leg_flows");
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 40) << "the CDS-level sweep compared almost nothing";
}

// `pay_accrual_on_default` cannot change these flows -- they are what happens
// when there is NO default. Asserted because the field is on the leg and a
// reader could reasonably expect it to matter.
TEST_F(FeeLegFlowsDifferential, TheAccrualConventionCannotChangeNonContingentFlows) {
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  cdsmodel::Cds cds{.start = d(2024, 3, 20),
                    .end = d(2029, 6, 20),
                    .coupon_rate = 0.01,
                    .day_count = cdsmodel::DayCount::Act360};

  cds.pay_accrual_on_default = true;
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> with =
      cdsmodel::cds_fee_leg_flows(cds, 1.0e7, calendars);
  cds.pay_accrual_on_default = false;
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> without =
      cdsmodel::cds_fee_leg_flows(cds, 1.0e7, calendars);

  ASSERT_TRUE(with.has_value());
  ASSERT_TRUE(without.has_value());
  EXPECT_EQ(*with, *without) << "the accrual-on-default convention changed flows that "
                                "by definition assume no default";
}

} // namespace
