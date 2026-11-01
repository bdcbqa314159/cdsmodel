// TIER 2 -- live differential tests. Built only when a local upstream copy
// exists (see CMakeLists.txt). Every assertion compares the C++20 port against the
// pristine upstream C compiled into the same binary. The oracle is not a
// golden file that can drift -- it is the ancestor, executing live.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/version.hpp>

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <span>
#include <vector>

extern "C" {
#include "bsearch.h"
#include "cerror.h"
}

namespace {

// Calls upstream and packages its out-params into the port's return type, so
// the two are directly comparable.
std::optional<cdsmodel::Bracket> reference_bracket(std::span<const double> xs, double desired) {
  int lo = 0;
  int hi = 0;
  // const_cast: upstream takes double* but does not write through it.
  double *data = const_cast<double *>(xs.data());
  if (JpmcdsBSearchDoubleFast(desired, data, sizeof(double), static_cast<int>(xs.size()), &lo,
                              &hi) != SUCCESS) {
    return std::nullopt;
  }
  return cdsmodel::Bracket{static_cast<std::size_t>(lo), static_cast<std::size_t>(hi)};
}

std::optional<cdsmodel::Bracket> reference_bracket(std::span<const long> xs, double desired) {
  int lo = 0;
  int hi = 0;
  long *data = const_cast<long *>(xs.data());
  if (JpmcdsBSearchLongFast(desired, data, sizeof(long), static_cast<int>(xs.size()), &lo, &hi) !=
      SUCCESS) {
    return std::nullopt;
  }
  return cdsmodel::Bracket{static_cast<std::size_t>(lo), static_cast<std::size_t>(hi)};
}

template <typename T> void expect_matches_reference(const std::vector<T> &xs, double desired) {
  const std::span<const T> view{xs};
  const auto ported = cdsmodel::bracket(view, desired);
  const auto reference = reference_bracket(view, desired);
  ASSERT_EQ(ported.has_value(), reference.has_value())
      << "success/failure disagreement at desired=" << desired << " n=" << xs.size();
  if (ported.has_value()) {
    EXPECT_EQ(ported->lo, reference->lo) << "lo mismatch at desired=" << desired;
    EXPECT_EQ(ported->hi, reference->hi) << "hi mismatch at desired=" << desired;
  }
}

// Upstream writes to a global error log on failure; silence it for the run.
class BSearchDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

TEST_F(BSearchDifferential, DoublesAcrossAndBeyondTheDomain) {
  const std::vector<double> xs{0.0, 1.0, 2.5, 4.0, 10.0, 11.0, 30.0};
  // Below, on, between, and above every knot -- extrapolation ends included.
  for (double d = -5.0; d <= 35.0; d += 0.25) {
    expect_matches_reference(xs, d);
  }
  for (double knot : xs) {
    expect_matches_reference(xs, knot);
  }
}

TEST_F(BSearchDifferential, LongsBehaveLikeDoubles) {
  const std::vector<long> xs{19000L, 19365L, 19730L, 20095L, 21000L};
  for (double d = 18000.0; d <= 22000.0; d += 37.0) {
    expect_matches_reference(xs, d);
  }
}

TEST_F(BSearchDifferential, SingleElementCollapsesToZeroZero) {
  const std::vector<double> xs{7.0};
  for (double d : {-1.0, 7.0, 99.0}) {
    expect_matches_reference(xs, d);
  }
}

TEST_F(BSearchDifferential, TwoElements) {
  const std::vector<double> xs{1.0, 2.0};
  for (double d : {0.0, 1.0, 1.5, 2.0, 3.0}) {
    expect_matches_reference(xs, d);
  }
}

TEST_F(BSearchDifferential, EmptyIsRejectedByBoth) {
  const std::vector<double> xs{};
  const std::span<const double> view{xs};
  EXPECT_FALSE(cdsmodel::bracket(view, 1.0).has_value());
  int lo = 0;
  int hi = 0;
  EXPECT_NE(JpmcdsBSearchDoubleFast(1.0, nullptr, sizeof(double), 0, &lo, &hi), SUCCESS);
}

TEST_F(BSearchDifferential, LargeSweepAgainstReference) {
  std::vector<double> xs;
  xs.reserve(500);
  for (int i = 0; i < 500; ++i) {
    xs.push_back(static_cast<double>(i) * 1.7 + 0.3);
  }
  for (double d = -10.0; d < 900.0; d += 0.9) {
    expect_matches_reference(xs, d);
  }
}

TEST_F(BSearchDifferential, OrderCheckAgreesWithUpstream) {
  const std::vector<double> ok{1.0, 2.0, 3.0};
  const std::vector<double> equal_neighbours{1.0, 2.0, 2.0, 3.0};

  EXPECT_TRUE(cdsmodel::is_strictly_increasing(std::span<const double>{ok}));
  EXPECT_FALSE(cdsmodel::is_strictly_increasing(std::span<const double>{equal_neighbours}));

  EXPECT_EQ(JpmcdsCheckDoubleArrayOrder(const_cast<char *>("t"), const_cast<double *>(ok.data()),
                                        sizeof(double), static_cast<int>(ok.size())),
            SUCCESS);
  EXPECT_NE(JpmcdsCheckDoubleArrayOrder(const_cast<char *>("t"),
                                        const_cast<double *>(equal_neighbours.data()),
                                        sizeof(double), static_cast<int>(equal_neighbours.size())),
            SUCCESS);
}

TEST_F(BSearchDifferential, RunOfEqualValuesMatchesUpstream) {
  // The one site where the port bounds a loop upstream left unbounded. If
  // these agree, the bound is behaviour-preserving on valid input.
  const std::vector<double> xs{1.0, 2.0, 2.0, 2.0, 3.0, 4.0};
  for (double d = 0.0; d <= 5.0; d += 0.25) {
    expect_matches_reference(xs, d);
  }
}

} // namespace
