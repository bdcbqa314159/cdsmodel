// TIER 1 -- linear interpolation. Properties first, then replay.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interpolation.hpp>

#include <gtest/gtest.h>

#include "golden/interpolation_cases.hpp"

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using cdsmodel::Limit;
using cdsmodel::linear_interpolate;
using cdsmodel::linear_interpolate_discontinuous;
using cdsmodel::linear_interpolate_many;
using cdsmodel::Metric;

cdsmodel::Metric metric_of(int code) {
  switch (code) {
  case 0:
    return Metric::Direct;
  case 1:
    return Metric::ActualDays;
  case 2:
    return Metric::Bond30_360;
  default:
    return Metric::Bond30E_360;
  }
}

TEST(Interpolation, ReproducesTheEndpointsExactly) {
  // Interpolating AT a node must return that node's value, not something a
  // rounding away from it.
  const std::vector<long> xs{100, 200, 400, 800};
  const std::vector<double> fs{0.01, 0.02, 0.025, 0.03};
  for (std::size_t i = 0; i < xs.size(); ++i) {
    const cdsmodel::Expected<double> got = linear_interpolate(xs, fs, static_cast<double>(xs[i]));
    ASSERT_TRUE(got.has_value()) << i;
    EXPECT_EQ(*got, fs[i]) << "at node " << i;
  }
}

TEST(Interpolation, IsMonotoneBetweenMonotoneNodes) {
  const std::vector<long> xs{0, 100, 200, 300};
  const std::vector<double> fs{1.0, 2.0, 3.0, 4.0};
  std::optional<double> previous;
  for (double x = -50.0; x <= 350.0; x += 7.5) {
    const cdsmodel::Expected<double> got = linear_interpolate(xs, fs, x);
    ASSERT_TRUE(got.has_value()) << x;
    if (previous) {
      EXPECT_GT(*got, *previous) << "at " << x;
    }
    previous = *got;
  }
}

TEST(Interpolation, ExtrapolatesLinearlyBeyondBothEnds) {
  // The bracketing search extrapolates rather than clamping, which upstream
  // relies on -- JpmcdsLinInterpCurve clamps its INPUTS precisely because the
  // interpolator does not.
  const std::vector<long> xs{100, 200};
  const std::vector<double> fs{1.0, 2.0};
  EXPECT_DOUBLE_EQ(*linear_interpolate(xs, fs, 0.0), 0.0);
  EXPECT_DOUBLE_EQ(*linear_interpolate(xs, fs, 300.0), 3.0);
  EXPECT_DOUBLE_EQ(*linear_interpolate(xs, fs, 150.0), 1.5);
}

TEST(Interpolation, ASinglePointCurveIsConstant) {
  const std::vector<long> xs{500};
  const std::vector<double> fs{0.07};
  for (double x : {-1000.0, 0.0, 500.0, 1e6}) {
    const cdsmodel::Expected<double> got = linear_interpolate(xs, fs, x);
    ASSERT_TRUE(got.has_value()) << x;
    EXPECT_EQ(*got, 0.07) << x;
  }
}

TEST(Interpolation, RejectsMismatchedOrUnorderedInput) {
  const std::vector<long> xs{100, 200, 300};
  EXPECT_FALSE(linear_interpolate(xs, std::vector<double>{1.0, 2.0}, 150.0).has_value())
      << "length mismatch";
  const std::vector<long> unordered{100, 300, 200};
  EXPECT_FALSE(linear_interpolate(unordered, std::vector<double>{1.0, 2.0, 3.0}, 150.0).has_value())
      << "the checked search must reject an unordered domain";
  EXPECT_FALSE(linear_interpolate(std::vector<long>{}, std::vector<double>{}, 1.0).has_value())
      << "empty";
  // Repeated abscissae are not strictly increasing, so the continuous routine
  // refuses them -- that is what the discontinuous routine is for.
  const std::vector<long> repeated{100, 200, 200};
  EXPECT_FALSE(linear_interpolate(repeated, std::vector<double>{1.0, 2.0, 3.0}, 150.0).has_value());
}

TEST(Interpolation, TheCheckedSearchNeverStopsChecking) {
  // The port's answer to UPSTREAM-13: upstream's order validation lives in a
  // mutable global that one failed call can leave disabled for the life of
  // the process. Here it is a different function, so repetition cannot change
  // the outcome.
  const std::vector<long> unordered{10, 50, 20, 90};
  const std::vector<double> fs{1.0, 2.0, 3.0, 4.0};
  for (int attempt = 0; attempt < 5; ++attempt) {
    EXPECT_FALSE(cdsmodel::bracket_checked(std::span<const long>{unordered}, 30.0).has_value())
        << "attempt " << attempt;
    EXPECT_FALSE(linear_interpolate(unordered, fs, 30.0).has_value()) << "attempt " << attempt;
    // And the unchecked form still works, every time, by being a different
    // function rather than a different global state.
    EXPECT_TRUE(cdsmodel::bracket(std::span<const long>{unordered}, 30.0).has_value())
        << "attempt " << attempt;
  }
}

TEST(Interpolation, DiscontinuityTakesTheRequestedLimit) {
  const std::vector<long> xs{100, 200, 200, 300};
  const std::vector<double> fs{1.0, 2.0, 5.0, 6.0};
  EXPECT_EQ(*linear_interpolate_discontinuous(xs, fs, 200.0, Limit::Left), 2.0);
  EXPECT_EQ(*linear_interpolate_discontinuous(xs, fs, 200.0, Limit::Right), 5.0);
  // Away from the jump, the limit is irrelevant.
  EXPECT_EQ(*linear_interpolate_discontinuous(xs, fs, 150.0, Limit::Left),
            *linear_interpolate_discontinuous(xs, fs, 150.0, Limit::Right));
}

TEST(Interpolation, ThreeSharedAbscissaeTakeTheMiddleValue) {
  // Upstream's rule, which is odd and observable: with three or more entries
  // at the same x, neither limit is used -- the middle one is.
  const std::vector<long> xs{100, 200, 200, 200, 300};
  const std::vector<double> fs{1.0, 2.0, 3.0, 5.0, 6.0};
  for (Limit limit : {Limit::Left, Limit::Right}) {
    const cdsmodel::Expected<double> got = linear_interpolate_discontinuous(xs, fs, 200.0, limit);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 3.0) << "expected the middle of f[1..3], regardless of the limit";
  }
}

TEST(Interpolation, DiscontinuousAcceptsRepeatedButNotDecreasingAbscissae) {
  const std::vector<double> fs{1.0, 2.0, 3.0};
  EXPECT_TRUE(
      linear_interpolate_discontinuous(std::vector<long>{100, 200, 200}, fs, 150.0, Limit::Left)
          .has_value());
  EXPECT_FALSE(
      linear_interpolate_discontinuous(std::vector<long>{100, 300, 200}, fs, 150.0, Limit::Left)
          .has_value());
}

TEST(Interpolation, DirectAndActualDaysDifferOnlyOnAZeroWidthBracket) {
  // Agreement on a normal curve...
  const std::vector<long> xs{100, 200, 400};
  const std::vector<double> fs{1.0, 2.0, 3.0};
  for (double x : {-50.0, 150.0, 250.0, 500.0}) {
    EXPECT_EQ(*linear_interpolate(xs, fs, x, Metric::Direct),
              *linear_interpolate(xs, fs, x, Metric::ActualDays))
        << x;
  }
  // ...and opposite infinities when the bracket collapses.
  const std::vector<long> flat{100, 200, 300, 300};
  const std::vector<double> ff{1.0, 2.0, 3.0, 9.0};
  const cdsmodel::Expected<double> direct =
      linear_interpolate_discontinuous(flat, ff, 400.0, Limit::Right, Metric::Direct);
  const cdsmodel::Expected<double> actual =
      linear_interpolate_discontinuous(flat, ff, 400.0, Limit::Right, Metric::ActualDays);
  ASSERT_TRUE(direct.has_value());
  ASSERT_TRUE(actual.has_value());
  EXPECT_TRUE(std::isinf(*direct));
  EXPECT_TRUE(std::isinf(*actual));
  EXPECT_GT(*direct, 0.0);
  EXPECT_LT(*actual, 0.0);
}

TEST(Interpolation, MetricForDayCountMatchesUpstreamsDispatch) {
  using cdsmodel::DayCount;
  EXPECT_EQ(cdsmodel::metric_for_day_count(DayCount::Act365Fixed), Metric::ActualDays);
  EXPECT_EQ(cdsmodel::metric_for_day_count(DayCount::Act365), Metric::ActualDays);
  EXPECT_EQ(cdsmodel::metric_for_day_count(DayCount::Act360), Metric::ActualDays);
  EXPECT_EQ(cdsmodel::metric_for_day_count(DayCount::Thirty360), Metric::Bond30_360);
  EXPECT_EQ(cdsmodel::metric_for_day_count(DayCount::ThirtyE360), Metric::Bond30E_360);
  EXPECT_FALSE(cdsmodel::metric_for_day_count(DayCount::EffectiveRate).has_value());
}

TEST(Interpolation, ManyIsTheLoopOverOne) {
  const std::vector<long> xs{100, 200, 400, 800};
  const std::vector<double> fs{0.01, 0.02, 0.025, 0.03};
  const std::vector<long> desired{50, 100, 150, 300, 700, 900};
  const cdsmodel::Expected<std::vector<double>> many = linear_interpolate_many(xs, fs, desired);
  ASSERT_TRUE(many.has_value());
  ASSERT_EQ(many->size(), desired.size());
  for (std::size_t i = 0; i < desired.size(); ++i) {
    EXPECT_EQ((*many)[i], *linear_interpolate(xs, fs, static_cast<double>(desired[i])))
        << "i=" << i;
  }
}

// =====================================================================
// Replay.
// =====================================================================

TEST(Interpolation, MatchesRecordedPointResults) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kInterpCases) {
    const std::vector<long> xs(c.xs.begin(), c.xs.begin() + static_cast<std::ptrdiff_t>(c.n));
    const std::vector<double> fs(c.fs.begin(), c.fs.begin() + static_cast<std::ptrdiff_t>(c.n));
    const cdsmodel::Expected<double> got =
        linear_interpolate(xs, fs, c.desired, metric_of(c.metric));
    const std::string where = "n=" + std::to_string(c.n) + " at " + std::to_string(c.desired) +
                              " metric " + std::to_string(c.metric);
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (c.solved) {
      // Non-accumulating: one subtraction, one division, one multiply-add.
      // 4 ULPs is the cross-platform tier for that shape.
      if (std::isfinite(c.value)) {
        EXPECT_NEAR(*got, c.value,
                    4.0 * std::max(1.0, std::fabs(c.value)) *
                        std::numeric_limits<double>::epsilon())
            << where;
      } else {
        EXPECT_EQ(*got, c.value) << where;
      }
    }
    ++checked;
  }
  EXPECT_GT(checked, 250) << checked;
}

TEST(Interpolation, MatchesRecordedDiscontinuousResults) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kDiscontinuousCases) {
    const std::vector<long> xs(c.xs.begin(), c.xs.begin() + static_cast<std::ptrdiff_t>(c.n));
    const std::vector<double> fs(c.fs.begin(), c.fs.begin() + static_cast<std::ptrdiff_t>(c.n));
    const cdsmodel::Expected<double> got = linear_interpolate_discontinuous(
        xs, fs, c.desired, c.right_limit ? Limit::Right : Limit::Left, metric_of(c.metric));
    const std::string where = "n=" + std::to_string(c.n) + " at " + std::to_string(c.desired) +
                              " metric " + std::to_string(c.metric) +
                              (c.right_limit ? " right" : " left");
    ASSERT_EQ(got.has_value(), c.solved) << where;
    if (c.solved) {
      if (std::isfinite(c.value)) {
        EXPECT_NEAR(*got, c.value,
                    4.0 * std::max(1.0, std::fabs(c.value)) *
                        std::numeric_limits<double>::epsilon())
            << where;
      } else {
        EXPECT_EQ(*got, c.value) << where << " (infinite values must match in sign)";
      }
    }
    ++checked;
  }
  EXPECT_GT(checked, 250) << checked;
}

} // namespace
