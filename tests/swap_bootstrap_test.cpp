// TIER 1 -- replays tests/golden/swap_bootstrap_cases.hpp, so the swap
// bootstrap is covered in CI, where no oracle is present.
//
// The recorded rates are ROOTS. They are bounded by the solver's stopping
// criterion, not by rounding, so they compare at the SOLVER tier -- looser
// than every other golden file here, and deliberately. Demanding 1e-12 of a
// root would be demanding that two implementations iterate identically.
//
// Which is why this file does not stop at comparing numbers. The second test
// asserts the property the solve EXISTS to establish: every benchmark must
// re-price to par off the curve built from it. That is an oracle, not a
// tolerance -- it would catch a port that reproduced the recorded rates and
// still built a curve that prices nothing correctly.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/swap_bootstrap_flows.hpp>
#include <cdsmodel/swap_dates.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include "golden/swap_bootstrap_cases.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

std::vector<cdsmodel::Date> benchmark_dates(int strip) {
  std::vector<cdsmodel::Date> out;
  for (std::size_t i = 0; i < kSwapBootstrapBenchmarks; ++i) {
    out.push_back(
        cdsmodel::from_tdate(strip == 0 ? kSwapBenchmarkDates0[i] : kSwapBenchmarkDates1[i]));
  }
  return out;
}

std::vector<double> benchmark_rates(int strip) {
  std::vector<double> out;
  for (std::size_t i = 0; i < kSwapBootstrapBenchmarks; ++i) {
    out.push_back(strip == 0 ? kSwapBenchmarkRates0[i] : kSwapBenchmarkRates1[i]);
  }
  return out;
}

std::optional<cdsmodel::ZeroCurveBuilder> seeded_builder() {
  std::optional<cdsmodel::ZeroCurveBuilder> curve = cdsmodel::ZeroCurveBuilder::create(
      cdsmodel::from_tdate(kSwapBootstrapValueDate), 8, 1L, cdsmodel::DayCount::Act365Fixed);
  if (!curve) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < kSwapBootstrapStubPoints; ++i) {
    if (!curve->add_rate(cdsmodel::from_tdate(kSwapBootstrapStubDates[i]),
                         kSwapBootstrapStubRates[i])) {
      return std::nullopt;
    }
  }
  return curve;
}

std::optional<cdsmodel::ZeroCurve> discount_curve() {
  std::vector<cdsmodel::RatePoint> points;
  for (std::size_t i = 0; i < kSwapBootstrapDiscountPoints; ++i) {
    points.push_back(cdsmodel::RatePoint{cdsmodel::from_tdate(kSwapBootstrapDiscountDates[i]),
                                         kSwapBootstrapDiscountRates[i]});
  }
  return cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(kSwapBootstrapValueDate), points, 1.0,
                                     cdsmodel::DayCount::Act365Fixed);
}

cdsmodel::SwapConventions conventions_of(const SwapBootstrapCase &c) {
  const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
  const std::optional<cdsmodel::BadDayConv> conv = cdsmodel::bad_day_conv_from_upstream(c.conv);
  return cdsmodel::SwapConventions{.fixed_frequency = c.frequency,
                                   .float_frequency = c.frequency,
                                   .fixed_day_count = dcc.value_or(cdsmodel::DayCount::Act360),
                                   .float_day_count = dcc.value_or(cdsmodel::DayCount::Act360),
                                   .interp = c.interp == 0 ? cdsmodel::InterpType::Linear
                                                           : cdsmodel::InterpType::FlatForwards,
                                   .bad_day = conv.value_or(cdsmodel::BadDayConv::None)};
}

TEST(SwapBootstrapGolden, ReplaysRecordedCurves) {
  ASSERT_GE(kSwapBootstrapCases.size(), 50u);
  const std::optional<cdsmodel::ZeroCurve> disc = discount_curve();
  ASSERT_TRUE(disc.has_value());

  int matched = 0;
  for (const SwapBootstrapCase &c : kSwapBootstrapCases) {
    std::optional<cdsmodel::ZeroCurveBuilder> curve = seeded_builder();
    ASSERT_TRUE(curve.has_value());

    const bool ok = cdsmodel::add_swaps(
        *curve, c.with_discount_curve ? disc : std::nullopt, benchmark_dates(c.strip),
        benchmark_rates(c.strip), conventions_of(c), std::nullopt, cdsmodel::Calendar::none());
    ASSERT_EQ(ok, c.accepted) << "strip " << c.strip << " freq " << c.frequency;
    if (!c.accepted) {
      continue;
    }

    const std::span<const cdsmodel::ZeroCurveBuilder::Point> points = curve->points();
    ASSERT_EQ(points.size(), c.point_count);
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedCurvePoint &want = kSwapBootstrapPoints[c.first_point + i];
      EXPECT_EQ(cdsmodel::to_tdate(points[i].date), want.date) << "point " << i;
      EXPECT_NEAR(want.rate, points[i].rate, kSwapBootstrapRateTolerance) << "rate " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 80);
}

// The oracle a tolerance cannot express. A solved curve is only correct if the
// instruments it was solved from price back to par on it.
//
// Only the floating-at-par cases are checked, because only there is the
// benchmark a cash-flow list this test can re-price directly; the explicit
// floating path needs the whole stream machinery and is covered by the
// differential suite.
TEST(SwapBootstrapGolden, EveryBenchmarkRepricesToParOnItsOwnCurve) {
  int checked = 0;
  for (const SwapBootstrapCase &c : kSwapBootstrapCases) {
    if (c.with_discount_curve != 0 || !c.accepted) {
      continue;
    }
    std::optional<cdsmodel::ZeroCurveBuilder> curve = seeded_builder();
    ASSERT_TRUE(curve.has_value());
    const cdsmodel::SwapConventions conventions = conventions_of(c);
    const std::vector<cdsmodel::Date> dates = benchmark_dates(c.strip);
    const std::vector<double> rates = benchmark_rates(c.strip);
    ASSERT_TRUE(cdsmodel::add_swaps(*curve, std::nullopt, dates, rates, conventions, std::nullopt,
                                    cdsmodel::Calendar::none()));

    const std::optional<cdsmodel::ZeroCurve> built = curve->to_zero_curve();
    ASSERT_TRUE(built.has_value());
    const std::optional<cdsmodel::DateInterval> interval =
        cdsmodel::frequency_to_interval(conventions.fixed_frequency);
    ASSERT_TRUE(interval.has_value());
    const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

    // The stub position has to be derived the way add_swaps derives it --
    // on-cycle means counting forward, so the stub is at the end; otherwise
    // is_end_stub decides. Hardcoding `true` here passed every on-cycle
    // benchmark and failed every off-cycle one, which is how this oracle
    // earned its place: it was reporting a real mismatch between what the
    // bootstrap priced and what this test re-priced.
    const std::optional<std::vector<cdsmodel::SwapDate>> swap_dates =
        cdsmodel::swap_dates_from_original(curve->value_date(), conventions.fixed_frequency, dates,
                                           std::nullopt, conventions.bad_day, calendars);
    ASSERT_TRUE(swap_dates.has_value());
    ASSERT_EQ(swap_dates->size(), dates.size());

    for (std::size_t i = 0; i < dates.size(); ++i) {
      bool end_stub = true;
      if (!(*swap_dates)[i].on_cycle) {
        const std::optional<bool> detected = cdsmodel::is_end_stub(
            curve->value_date(), (*swap_dates)[i].original, *interval, conventions.stub_position);
        ASSERT_TRUE(detected.has_value());
        end_stub = *detected;
      }
      const std::optional<std::vector<cdsmodel::CashFlow>> flows = cdsmodel::swap_cash_flows(
          curve->value_date(), (*swap_dates)[i].original, end_stub, rates[i], *interval,
          conventions.fixed_day_count, {.conv = conventions.bad_day}, calendars);
      ASSERT_TRUE(flows.has_value()) << "benchmark " << i;
      const std::optional<double> pv = cdsmodel::cash_flow_pv(*built, *flows, conventions.interp);
      ASSERT_TRUE(pv.has_value()) << "benchmark " << i;
      // Par: the bond-equivalent benchmark is worth 1.0.
      EXPECT_NEAR(*pv, 1.0, kSwapBootstrapParTolerance)
          << "benchmark " << i << " does not re-price to par on the curve built from it"
          << " (strip " << c.strip << ", freq " << c.frequency << ")";
      ++checked;
    }
  }
  EXPECT_GT(checked, 80) << "the par-reprice oracle checked almost nothing";
}

// Rates are finite and the curve is strictly increasing in date -- properties
// a solve can violate without any recorded number noticing.
TEST(SwapBootstrapGolden, EveryCurveIsWellFormed) {
  int checked = 0;
  for (const SwapBootstrapCase &c : kSwapBootstrapCases) {
    if (!c.accepted) {
      continue;
    }
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedCurvePoint &p = kSwapBootstrapPoints[c.first_point + i];
      EXPECT_TRUE(std::isfinite(p.rate)) << "point " << i;
      EXPECT_GT(p.rate, -1.0) << "a rate at or below -100% has no discount factor";
      if (i > 0) {
        EXPECT_GT(p.date, kSwapBootstrapPoints[c.first_point + i - 1].date)
            << "dates are not strictly increasing at " << i;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 500);
}

// UPSTREAM-33, pinned without the oracle: the port solves for the point it
// added even when that point is not the curve's latest, which is the input
// that defeats upstream's `&zc->rate[zc->numItems-1]`.
TEST(SwapBootstrapGolden, Upstream33ABenchmarkInsideTheCurveStillSolves) {
  const std::optional<cdsmodel::ZeroCurve> disc = discount_curve();
  ASSERT_TRUE(disc.has_value());
  std::optional<cdsmodel::ZeroCurveBuilder> curve = seeded_builder();
  ASSERT_TRUE(curve.has_value());
  // A point well beyond the benchmark, so the added point lands in the middle.
  ASSERT_TRUE(curve->add_rate(*cdsmodel::make_date(2035, 1, 15), 0.040));

  EXPECT_TRUE(cdsmodel::value_fixed_float_swap(
      *curve, *disc, 1.0, *cdsmodel::make_date(2029, 1, 15), 0.045,
      {.interp = cdsmodel::InterpType::FlatForwards}, cdsmodel::Calendar::none()))
      << "the port stopped solving a benchmark inside the curve";

  bool found = false;
  for (const cdsmodel::ZeroCurveBuilder::Point &p : curve->points()) {
    if (p.date == *cdsmodel::make_date(2035, 1, 15)) {
      EXPECT_DOUBLE_EQ(p.rate, 0.040) << "the pre-existing far point was overwritten";
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

} // namespace
