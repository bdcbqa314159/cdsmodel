// TIER 1 -- replays tests/golden/stream_cash_flows_cases.hpp, so a swap leg's
// cash flows are covered in CI, where no oracle is present.
//
// Amounts compare at the ACCUMULATING tier, relative. Two independent reasons
// point there and either would be enough: the amounts are products of sums of
// discounted quantities, and the file crossed a compiler to get here. A ULP
// tolerance is for one computation compared against itself in the same binary,
// which this is not.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/float_rate.hpp>
#include <cdsmodel/stream_cash_flows.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/stream_cash_flows_cases.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

void expect_recorded(double want, double got, const char *what, std::size_t where) {
  EXPECT_NEAR(want, got, kStreamCashFlowsTolerance * std::max(1.0, std::abs(want)))
      << what << " at " << where;
}

cdsmodel::DayCount dcc_of(long code) {
  const cdsmodel::Expected<cdsmodel::DayCount> d = cdsmodel::from_upstream_code(code);
  return d.value_or(cdsmodel::DayCount::Act360);
}

/// Rebuild the schedule a case was recorded from. Its dates are not recorded
/// separately -- the port's own generator produces them, and coupon_schedule
/// is pinned by its own vectors, so recording them twice would only let the
/// two copies disagree.
cdsmodel::Expected<cdsmodel::CouponSchedule>
schedule_of(cdsmodel::Date start, cdsmodel::Date maturity, int months, bool stub_at_end) {
  return cdsmodel::coupon_schedule(
      {.start = start,
       .maturity = maturity,
       .coupon_interval = {months, cdsmodel::PeriodUnit::Months},
       .stub_position = stub_at_end ? cdsmodel::StubPosition::Back : cdsmodel::StubPosition::Front},
      {}, cdsmodel::Calendar::none());
}

cdsmodel::ZeroCurve recorded_curve() {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < kStreamCurvePoints; ++i) {
    points.push_back(
        cdsmodel::RatePoint{cdsmodel::from_tdate(kStreamCurveDates[i]), kStreamCurveRates[i]});
  }
  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(kStreamCurveBaseDate), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  return *curve;
}

TEST(StreamCashFlowsGolden, ReplaysRecordedFixedLegs) {
  ASSERT_GE(kFixedLegCases.size(), 50u);
  int matched = 0;
  for (const FixedLegCase &c : kFixedLegCases) {
    const cdsmodel::Expected<cdsmodel::CouponSchedule> schedule = schedule_of(
        cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.maturity), c.months, c.stub_at_end);
    ASSERT_TRUE(schedule.has_value());
    ASSERT_EQ(static_cast<long>(schedule->stub), c.stub_location)
        << "the schedule the case was recorded from no longer reproduces";

    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
        cdsmodel::fixed_stream_cash_flows(
            {.schedule = *schedule,
             .principal = 1.0e6,
             .pay_day_count = dcc_of(c.day_count),
             .principal_flows = {.subtract_initial = c.subtract_initial != 0,
                                 .add_final = c.add_final != 0},
             .first_coupon_rate = c.first_coupon_rate,
             .final_coupon_rate = c.final_coupon_rate,
             .fixed_rate = c.fixed_rate});
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->size(), c.flow_count);
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const RecordedFlow &want = kStreamFlows[c.first_flow + i];
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].date), want.date) << "fixed date " << i;
      expect_recorded(want.amount, (*got)[i].amount, "fixed amount", i);
    }
    ++matched;
  }
  EXPECT_GT(matched, 250);
}

TEST(StreamCashFlowsGolden, ReplaysRecordedFloatingLegs) {
  ASSERT_GE(kFloatLegCases.size(), 50u);
  const cdsmodel::ZeroCurve curve = recorded_curve();
  int matched = 0;
  int refused = 0;
  for (const FloatLegCase &c : kFloatLegCases) {
    const cdsmodel::Expected<cdsmodel::CouponSchedule> schedule = schedule_of(
        cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.maturity), c.months, c.stub_at_end);
    ASSERT_TRUE(schedule.has_value());

    const cdsmodel::InterpType interp =
        c.interp == 0 ? cdsmodel::InterpType::Linear : cdsmodel::InterpType::FlatForwards;
    const cdsmodel::FloatStream stream{
        .schedule = *schedule,
        .principal = 1.0e6,
        .pay_day_count = dcc_of(c.day_count),
        .principal_flows = {.subtract_initial = c.subtract_initial != 0,
                            .add_final = c.add_final != 0},
        .rate = {.maturity_interval = {c.months, cdsmodel::PeriodUnit::Months},
                 .pay_interval = {c.months, cdsmodel::PeriodUnit::Months},
                 .day_count = cdsmodel::DayCount::Act360,
                 .spot_offset = cdsmodel::calendar_day_offset(c.spot_offset_days),
                 .spread = c.spread,
                 .weight = 1.0}};
    const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
        cdsmodel::float_stream_cash_flows(curve, interp, stream, cdsmodel::BadDayConv::None,
                                          cdsmodel::Calendar::none());
    if (!c.accepted) {
      EXPECT_FALSE(got.has_value());
      ++refused;
      continue;
    }
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->size(), c.flow_count);
    for (std::size_t i = 0; i < c.flow_count; ++i) {
      const RecordedFlow &want = kStreamFlows[c.first_flow + i];
      EXPECT_EQ(cdsmodel::to_tdate((*got)[i].date), want.date) << "float date " << i;
      expect_recorded(want.amount, (*got)[i].amount, "float amount", i);
    }
    ++matched;
  }
  EXPECT_GT(matched, 500);
}

// --------------------------------------------- properties, no oracle needed

// The interp type is read on the float path and must be: two interpolation
// schemes off the same curve should not agree on every flow. Guards against a
// port that silently ignores the parameter, which the recorded vectors alone
// would not catch if the generator had ignored it too.
TEST(StreamCashFlowsGolden, InterpolationTypeChangesFloatingAmounts) {
  const cdsmodel::ZeroCurve curve = recorded_curve();
  const cdsmodel::Expected<cdsmodel::CouponSchedule> schedule = schedule_of(
      cdsmodel::from_tdate(kStreamCurveBaseDate), *cdsmodel::make_date(2029, 4, 15), 6, false);
  ASSERT_TRUE(schedule.has_value());
  const cdsmodel::FloatStream stream{
      .schedule = *schedule,
      .principal = 1.0e6,
      .pay_day_count = cdsmodel::DayCount::Act360,
      .rate = {.maturity_interval = {6, cdsmodel::PeriodUnit::Months},
               .pay_interval = {6, cdsmodel::PeriodUnit::Months},
               .day_count = cdsmodel::DayCount::Act360}};

  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> linear =
      cdsmodel::float_stream_cash_flows(curve, cdsmodel::InterpType::Linear, stream,
                                        cdsmodel::BadDayConv::None, cdsmodel::Calendar::none());
  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> flat =
      cdsmodel::float_stream_cash_flows(curve, cdsmodel::InterpType::FlatForwards, stream,
                                        cdsmodel::BadDayConv::None, cdsmodel::Calendar::none());
  ASSERT_TRUE(linear.has_value());
  ASSERT_TRUE(flat.has_value());
  ASSERT_EQ(linear->size(), flat->size());
  int differing = 0;
  for (std::size_t i = 0; i < linear->size(); ++i) {
    EXPECT_EQ((*linear)[i].date, (*flat)[i].date) << i;
    if ((*linear)[i].amount != (*flat)[i].amount) {
      ++differing;
    }
  }
  EXPECT_GT(differing, 0) << "the interpolation type is being ignored";
}

// One flow per period, plus one if the initial principal is paid out.
TEST(StreamCashFlowsGolden, FlowCountFollowsThePrincipalFlags) {
  const cdsmodel::Expected<cdsmodel::CouponSchedule> schedule =
      schedule_of(*cdsmodel::make_date(2024, 1, 15), *cdsmodel::make_date(2029, 4, 15), 6, false);
  ASSERT_TRUE(schedule.has_value());
  const std::size_t periods = schedule->periods.size();
  ASSERT_GT(periods, 1u);

  for (const bool subtract_initial : {false, true}) {
    for (const bool add_final : {false, true}) {
      const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
          cdsmodel::fixed_stream_cash_flows(
              {.schedule = *schedule,
               .principal = 1.0e6,
               .pay_day_count = cdsmodel::DayCount::Act360,
               .principal_flows = {.subtract_initial = subtract_initial, .add_final = add_final},
               .fixed_rate = 0.04});
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(got->size(), periods + (subtract_initial ? 1u : 0u));
      if (subtract_initial) {
        EXPECT_EQ(got->front().amount, -1.0e6);
        EXPECT_EQ(got->front().date, schedule->periods.front().accrual_start);
      }
      if (add_final) {
        // The final flow carries the principal on top of its coupon.
        EXPECT_GT(got->back().amount, 1.0e6);
      }
    }
  }
}

// A zero principal and an absurd fixed rate are both refused, which is
// upstream's CheckFixedStreamDetails -- minus the value date it never read.
TEST(StreamCashFlowsGolden, AMalformedFixedStreamIsRefused) {
  const cdsmodel::Expected<cdsmodel::CouponSchedule> schedule =
      schedule_of(*cdsmodel::make_date(2024, 1, 15), *cdsmodel::make_date(2029, 1, 15), 6, false);
  ASSERT_TRUE(schedule.has_value());

  EXPECT_FALSE(cdsmodel::make_fixed_stream({.schedule = *schedule,
                                            .principal = 0.0,
                                            .pay_day_count = cdsmodel::DayCount::Act360,
                                            .fixed_rate = 0.04})
                   .has_value());
  EXPECT_FALSE(cdsmodel::make_fixed_stream({.schedule = *schedule,
                                            .principal = 1.0e6,
                                            .pay_day_count = cdsmodel::DayCount::Act360,
                                            .fixed_rate = -1.5})
                   .has_value());
  EXPECT_FALSE(cdsmodel::make_fixed_stream({.schedule = *schedule,
                                            .principal = 1.0e6,
                                            .pay_day_count = cdsmodel::DayCount::Act360,
                                            .fixed_rate = cdsmodel::kMaxStreamRate + 1.0})
                   .has_value());
  EXPECT_TRUE(cdsmodel::make_fixed_stream({.schedule = *schedule,
                                           .principal = 1.0e6,
                                           .pay_day_count = cdsmodel::DayCount::Act360,
                                           .fixed_rate = 0.04})
                  .has_value());
}

} // namespace
