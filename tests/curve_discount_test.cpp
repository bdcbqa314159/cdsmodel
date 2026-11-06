// TIER 1 -- discounting off a finished curve, without an oracle.
//
// A discount factor has properties that hold whatever the basis: it is 1 at
// the base date, it falls as the date recedes for a positive rate, and the
// present value of a set of flows is the sum of their discounted amounts. Those
// are checkable against arithmetic rather than against upstream, which matters
// here because both implementations could be made to agree on a wrong answer.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include "golden/curve_discount_cases.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::CashFlow;
using cdsmodel::InterpType;
using cdsmodel::RatePoint;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::vector<RatePoint> rising(int count = 5) {
  std::vector<RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(
        RatePoint{*cdsmodel::make_date(2024 + i, 1, 1), 0.02 + 0.004 * static_cast<double>(i)});
  }
  return points;
}

cdsmodel::ZeroCurve curve_at(double basis,
                             cdsmodel::DayCount dcc = cdsmodel::DayCount::Act365Fixed) {
  return *cdsmodel::ZeroCurve::create(d(2024, 1, 1), rising(), basis, dcc);
}

std::vector<double> bases() { return {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis}; }

// =====================================================================
// What a discount factor must satisfy.
// =====================================================================

TEST(CurveDiscount, TheDiscountFactorAtTheBaseDateIsExactlyOne) {
  // No time, no discounting. Exactly 1, not approximately -- any basis, any
  // day count, any interpolation type.
  int checked = 0;
  for (const double basis : bases()) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      const cdsmodel::ZeroCurve curve = curve_at(basis, dcc);
      for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
        const std::optional<double> at_base =
            cdsmodel::discount_date(curve, curve.base_date(), interp);
        ASSERT_TRUE(at_base.has_value()) << basis;
        EXPECT_EQ(*at_base, 1.0) << "basis " << basis << " dcc " << cdsmodel::to_upstream_code(dcc);
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 20) << checked;
}

TEST(CurveDiscount, APositiveCurveDiscountsMonotonically) {
  // Later money is worth less. Nothing in the code enforces it, which is what
  // makes it worth asserting: a sign error in the conversion would pass every
  // comparison against upstream only if upstream had the same error.
  int checked = 0;
  for (const double basis : bases()) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
      std::optional<double> previous;
      for (const cdsmodel::Date at : {d(2024, 1, 1), d(2024, 7, 1), d(2025, 1, 1), d(2026, 1, 1),
                                      d(2027, 6, 1), d(2029, 1, 1)}) {
        const std::optional<double> discount = cdsmodel::discount_date(curve, at, interp);
        ASSERT_TRUE(discount.has_value()) << basis;
        EXPECT_GT(*discount, 0.0) << "basis " << basis;
        EXPECT_LE(*discount, 1.0) << "basis " << basis
                                  << ": a positive rate cannot "
                                     "make money worth more";
        if (previous) {
          EXPECT_LT(*discount, *previous) << "basis " << basis << " at " << cdsmodel::to_tdate(at);
        }
        previous = discount;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 60) << checked;
}

TEST(CurveDiscount, TheTwoRoutesToADiscountFactorAgree) {
  // discount_date interpolates then converts in the curve's terms;
  // zero_price converts to continuous then exponentiates. Two
  // implementations of one quantity, which is the UPSTREAM-17 shape -- so the
  // agreement is a test rather than an assumption.
  //
  // At the ACCUMULATING tier, not the 4-ULP one, and the first version of this
  // test got that wrong. The two routes differ in CONDITIONING rather than
  // only in rounding: zero_price goes through a continuously-compounded rate,
  // cc = basis * log(1 + rate/basis), and at a high compounding frequency that
  // is the log of a number very close to 1. Measured on UPSTREAM, which has
  // the identical property:
  //
  //   basis  1    relative 2.4e-16
  //   basis  12   relative 9.6e-16
  //   basis  52   relative 3.1e-15
  //   basis  365  relative 8.3e-14   <- about 350 ULPs
  //   basis  5000 relative 1.2e-16   <- continuous: no conversion at all
  //
  // So the agreement degrades with frequency and recovers at continuous, where
  // the conversion is the identity. That is a fact about the algebra, not about
  // either implementation, and a ULP bound on it would be asserting that
  // log(1+x) is well conditioned near zero.
  int checked = 0;
  for (const double basis : bases()) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      const cdsmodel::ZeroCurve curve = curve_at(basis, dcc);
      for (const cdsmodel::Date at : {d(2025, 1, 1), d(2026, 7, 1), d(2027, 1, 1), d(2029, 1, 1)}) {
        const std::optional<double> via_discount =
            cdsmodel::discount_date(curve, at, InterpType::FlatForwards);
        ASSERT_TRUE(via_discount.has_value());
        const double via_zero_price = curve.zero_price(at);
        EXPECT_NEAR(*via_discount, via_zero_price,
                    1.0e-12 * std::max(1.0, std::fabs(via_zero_price)))
            << "basis " << basis << " at " << cdsmodel::to_tdate(at);
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 40) << checked;
}

TEST(CurveDiscount, ADiscountFactorRoundTripsThroughItsOwnRate) {
  // The conversion is invertible, so interpolating a rate and discounting it
  // must undo. This catches a basis or day count applied inconsistently
  // between the two halves of discount_date.
  for (const double basis : bases()) {
    const cdsmodel::ZeroCurve curve = curve_at(basis);
    for (const cdsmodel::Date at : {d(2025, 1, 1), d(2026, 7, 1), d(2029, 1, 1)}) {
      const std::optional<double> discount =
          cdsmodel::discount_date(curve, at, InterpType::FlatForwards);
      const std::optional<double> rate = cdsmodel::interp_rate(curve, at, InterpType::FlatForwards);
      ASSERT_TRUE(discount.has_value());
      ASSERT_TRUE(rate.has_value());
      const std::optional<double> years =
          cdsmodel::year_fraction(curve.base_date(), at, curve.day_count());
      ASSERT_TRUE(years.has_value());
      const std::optional<double> back =
          cdsmodel::discount_to_rate_year_frac(*discount, *years, basis);
      ASSERT_TRUE(back.has_value()) << basis;
      EXPECT_NEAR(*back, *rate, 1.0e-12 * std::max(1.0, std::fabs(*rate)))
          << "basis " << basis << " at " << cdsmodel::to_tdate(at);
    }
  }
}

// =====================================================================
// The present value.
// =====================================================================

TEST(CurveDiscount, APresentValueIsTheSumOfItsDiscountedFlows) {
  // Stated as the definition, and checked bit-exactly in the same order the
  // implementation accumulates. Not a tolerance: if these differ at all, the
  // order differs.
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  const std::vector<CashFlow> flows{
      {d(2025, 1, 1), 0.05}, {d(2026, 1, 1), 0.05}, {d(2027, 1, 1), 0.05}, {d(2029, 1, 1), 1.05}};
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    double expected = 0.0;
    for (const CashFlow &flow : flows) {
      const std::optional<double> discount = cdsmodel::discount_date(curve, flow.date, interp);
      ASSERT_TRUE(discount.has_value());
      expected += *discount * flow.amount;
    }
    const std::optional<double> pv = cdsmodel::cash_flow_pv(curve, flows, interp);
    ASSERT_TRUE(pv.has_value());
    EXPECT_EQ(*pv, expected) << static_cast<int>(interp);
  }
}

TEST(CurveDiscount, APresentValueScalesLinearlyAndAddsAcrossSubsets) {
  const cdsmodel::ZeroCurve curve = curve_at(2.0);
  const std::vector<CashFlow> flows{
      {d(2025, 1, 1), 0.05}, {d(2026, 1, 1), 0.05}, {d(2027, 1, 1), 1.05}};
  std::vector<CashFlow> doubled = flows;
  for (CashFlow &flow : doubled) {
    flow.amount *= 2.0;
  }
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    const std::optional<double> single = cdsmodel::cash_flow_pv(curve, flows, interp);
    const std::optional<double> twice = cdsmodel::cash_flow_pv(curve, doubled, interp);
    ASSERT_TRUE(single.has_value());
    ASSERT_TRUE(twice.has_value());
    EXPECT_NEAR(*twice, 2.0 * *single, 1.0e-14 * std::fabs(*single));

    // Splitting the run splits the value. Accumulating, so the relative tier.
    const std::optional<double> head =
        cdsmodel::cash_flow_pv(curve, std::span{flows}.first(1), interp);
    const std::optional<double> tail =
        cdsmodel::cash_flow_pv(curve, std::span{flows}.subspan(1), interp);
    ASSERT_TRUE(head.has_value());
    ASSERT_TRUE(tail.has_value());
    EXPECT_NEAR(*head + *tail, *single, 1.0e-12 * std::fabs(*single));
  }
}

TEST(CurveDiscount, NoFlowsIsWorthNothingAndIsNotAnError) {
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  for (const InterpType interp : {InterpType::Linear, InterpType::FlatForwards}) {
    const std::optional<double> pv = cdsmodel::cash_flow_pv(curve, {}, interp);
    ASSERT_TRUE(pv.has_value());
    EXPECT_EQ(*pv, 0.0);
  }
}

// =====================================================================
// first_date and dates.
// =====================================================================

TEST(CurveDiscount, TheFirstDateIsTheMinimumNotTheBaseDate) {
  // Upstream takes MIN(baseDate, points[0]), so a curve whose first point
  // precedes its base date reports the point -- which is the straddled case
  // the flat-forwards interpolator has a branch for.
  const std::vector<RatePoint> points{{d(2024, 1, 1), 0.03}, {d(2027, 1, 1), 0.05}};
  for (const cdsmodel::Date base : {d(2023, 1, 1), d(2024, 1, 1), d(2025, 6, 1)}) {
    const std::optional<cdsmodel::ZeroCurve> curve =
        cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(curve.has_value()) << cdsmodel::to_tdate(base);
    EXPECT_EQ(curve->first_date(), std::min(base, points.front().date))
        << "base " << cdsmodel::to_tdate(base);
    EXPECT_LE(curve->first_date(), base);
    EXPECT_LE(curve->first_date(), points.front().date);
  }
}

TEST(CurveDiscount, TheDatesAreThePointDatesInOrder) {
  const cdsmodel::ZeroCurve curve = curve_at(1.0);
  const std::vector<cdsmodel::Date> dates = curve.dates();
  ASSERT_EQ(dates.size(), curve.points().size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(dates[i], curve.points()[i].date) << i;
  }
  EXPECT_TRUE(std::ranges::is_sorted(dates, std::ranges::less{}))
      << "strictly increasing, as create requires";
}

// =====================================================================
// Replay.
// =====================================================================

TEST(CurveDiscount, MatchesRecordedDiscountFactors) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kDiscountDateCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value()) << c.day_count;
    std::vector<RatePoint> points;
    for (std::size_t i = 0; i < c.n; ++i) {
      points.push_back(RatePoint{cdsmodel::from_tdate(c.dates[i]), c.rates[i]});
    }
    const std::optional<cdsmodel::ZeroCurve> curve =
        cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(c.base_date), points, c.basis, *dcc);
    ASSERT_TRUE(curve.has_value()) << "basis " << c.basis;
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    const std::optional<double> got =
        cdsmodel::discount_date(*curve, cdsmodel::from_tdate(c.at), interp);
    const std::string where = "base " + std::to_string(c.base_date) + " basis " +
                              std::to_string(c.basis) + " dcc " + std::to_string(c.day_count) +
                              " interp " + std::to_string(c.interp) + " at " + std::to_string(c.at);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      EXPECT_NEAR(*got, c.discount,
                  4.0 * std::max(1.0, std::fabs(c.discount)) *
                      std::numeric_limits<double>::epsilon())
          << where;
      // The recording also carries the other route, so the cross-check is
      // replayed without the oracle present.
      EXPECT_NEAR(curve->zero_price(cdsmodel::from_tdate(c.at)), c.zero_price,
                  4.0 * std::max(1.0, std::fabs(c.zero_price)) *
                      std::numeric_limits<double>::epsilon())
          << where << " zero_price";
    }
    EXPECT_EQ(cdsmodel::to_tdate(curve->first_date()), c.first_date) << where;
    ++checked;
  }
  EXPECT_GT(checked, 250) << checked;
}

TEST(CurveDiscount, MatchesRecordedPresentValues) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kCashFlowPvCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value()) << c.day_count;
    std::vector<RatePoint> points;
    for (std::size_t i = 0; i < c.n_points; ++i) {
      points.push_back(RatePoint{cdsmodel::from_tdate(c.dates[i]), c.rates[i]});
    }
    const std::optional<cdsmodel::ZeroCurve> curve =
        cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(c.base_date), points, c.basis, *dcc);
    ASSERT_TRUE(curve.has_value());
    std::vector<CashFlow> flows;
    for (std::size_t i = 0; i < c.n_flows; ++i) {
      flows.push_back(CashFlow{cdsmodel::from_tdate(c.flow_dates[i]), c.flow_amounts[i]});
    }
    const InterpType interp = c.interp == 0 ? InterpType::Linear : InterpType::FlatForwards;
    const std::optional<double> got = cdsmodel::cash_flow_pv(*curve, flows, interp);
    const std::string where = "basis " + std::to_string(c.basis) + " dcc " +
                              std::to_string(c.day_count) + " interp " + std::to_string(c.interp) +
                              " flows " + std::to_string(c.n_flows);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      // The amounts span 1e-6 to 1e12, so this is an order check as much as a
      // value check -- a reassociated sum would miss by far more than this.
      EXPECT_NEAR(*got, c.pv, 1.0e-12 * std::max(1.0, std::fabs(c.pv))) << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 30) << checked;
}

} // namespace
