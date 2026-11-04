// TIER 2 -- live differential tests for the root finder.
//
// This suite checks something the others do not, and has to.
//
// Every module ported so far evaluates a formula. A one-ULP difference in an
// intermediate stays a one-ULP difference in the answer, so comparing the
// answer is a complete test. A root finder is different: its arithmetic feeds
// comparisons that choose the next iterate. A difference far too small to see
// can flip a branch, change the iteration path, and move the answer by orders
// of magnitude more than the difference that caused it.
//
// So agreement on the root is NOT sufficient evidence here -- two different
// paths can land on the same root by luck. These tests also count objective
// evaluations and require THAT to match. An identical root reached in a
// different number of steps would mean the transcription diverged somewhere
// the tolerance happened to absorb.
//
// Both sides call the same C++ objective functions, so the function values
// are identical by construction and the only thing under test is the search.

#include <cdsmodel/root_find.hpp>

#include "root_find_objectives.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "cgeneral.h"
#include "rtbrent.h"
}

namespace {

using cdsmodel::testing::name_of;
using cdsmodel::testing::Objective;
using cdsmodel::testing::Which;

class RootFindDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

/// The same objective, reached through upstream's function-pointer signature.
extern "C" int trampoline(double x, void *data, double *out) {
  Objective *obj = static_cast<Objective *>(data);
  const std::optional<double> value = (*obj)(x);
  if (!value) {
    return FAILURE;
  }
  *out = *value;
  return SUCCESS;
}

struct Outcome {
  std::optional<double> root;
  int calls = 0;
};

Outcome reference_run(Which which, const cdsmodel::RootFindOptions &opt) {
  Objective obj{which};
  double solution = 0.0;
  const int rc = JpmcdsRootFindBrent(
      trampoline, &obj, opt.bound_lo, opt.bound_hi, opt.max_iterations, opt.guess,
      opt.initial_x_step, opt.initial_f_derivative, opt.x_tolerance, opt.f_tolerance, &solution);
  if (rc != SUCCESS) {
    return {std::nullopt, obj.calls};
  }
  return {solution, obj.calls};
}

Outcome port_run(Which which, const cdsmodel::RootFindOptions &opt) {
  Objective obj{which};
  const std::optional<double> root = cdsmodel::root_find_brent(obj, opt);
  return {root, obj.calls};
}

std::string label(Which w, const cdsmodel::RootFindOptions &o) {
  return std::string(name_of(w)) + " [" + std::to_string(o.bound_lo) + "," +
         std::to_string(o.bound_hi) + "] guess=" + std::to_string(o.guess) +
         " step=" + std::to_string(o.initial_x_step) +
         " deriv=" + std::to_string(o.initial_f_derivative) +
         " xacc=" + std::to_string(o.x_tolerance) + " facc=" + std::to_string(o.f_tolerance) +
         " iters=" + std::to_string(o.max_iterations);
}

TEST_F(RootFindDifferential, RootAndIterationPathMatchAcrossTheOptionSpace) {
  // The shared list, so an objective added there is swept here without edit.
  const std::vector<Which> objectives(std::begin(cdsmodel::testing::kAllObjectives),
                                      std::end(cdsmodel::testing::kAllObjectives));

  struct Bounds {
    double lo, hi;
  };
  const std::vector<Bounds> brackets{{0.0, 100.0}, {-10.0, 10.0}, {0.0, 3.0},
                                     {1.0, 2.0},   {-1.0, 5.0},   {0.0, 1.0}};
  const std::vector<double> guesses{0.0, 0.01, 0.5, 1.0, 1.5};
  const std::vector<double> steps{0.0, 0.0001, 0.1, -0.25};
  const std::vector<double> derivs{0.0, 1.0, -2.0};
  const std::vector<double> accs{1.0e-8, 1.0e-12, 0.0};

  int compared = 0;
  int both_failed = 0;
  for (Which which : objectives) {
    for (const Bounds &b : brackets) {
      for (double guess : guesses) {
        if (guess < b.lo || guess > b.hi) {
          continue; // both sides reject; covered explicitly elsewhere
        }
        for (double step : steps) {
          for (double deriv : derivs) {
            for (double acc : accs) {
              cdsmodel::RootFindOptions opt;
              opt.bound_lo = b.lo;
              opt.bound_hi = b.hi;
              opt.max_iterations = 100;
              opt.guess = guess;
              opt.initial_x_step = step;
              opt.initial_f_derivative = deriv;
              opt.x_tolerance = acc;
              opt.f_tolerance = acc;

              const Outcome expected = reference_run(which, opt);
              const Outcome got = port_run(which, opt);

              ASSERT_EQ(got.root.has_value(), expected.root.has_value()) << label(which, opt);
              // The iteration path, not just the answer. This is the check
              // that distinguishes a faithful transcription from a lucky one.
              EXPECT_EQ(got.calls, expected.calls)
                  << label(which, opt) << " -- same answer, different path";
              if (!expected.root) {
                ++both_failed;
                continue;
              }
              EXPECT_EQ(*got.root, *expected.root) << label(which, opt);
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 2000) << "sweep thinner than expected: " << compared;
  // Failures are part of the contract here, not an accident: NoRoot and
  // FailsAbove exist to exercise them.
  EXPECT_GT(both_failed, 100) << "rejection paths barely exercised: " << both_failed;
}

TEST_F(RootFindDifferential, RejectedInputsAreRejectedByBoth) {
  const std::vector<cdsmodel::RootFindOptions> bad{
      {.bound_lo = 1.0, .bound_hi = 1.0, .guess = 1.0},  // bounds equal
      {.bound_lo = 2.0, .bound_hi = 1.0, .guess = 1.5},  // bounds reversed
      {.bound_lo = 0.0, .bound_hi = 1.0, .guess = -0.5}, // guess below
      {.bound_lo = 0.0, .bound_hi = 1.0, .guess = 1.5},  // guess above
  };
  for (const cdsmodel::RootFindOptions &opt : bad) {
    cdsmodel::RootFindOptions o = opt;
    o.max_iterations = 100;
    o.x_tolerance = 1.0e-8;
    o.f_tolerance = 1.0e-8;
    EXPECT_FALSE(reference_run(Which::Linear, o).root.has_value()) << label(Which::Linear, o);
    EXPECT_FALSE(port_run(Which::Linear, o).root.has_value()) << label(Which::Linear, o);
  }
}

TEST_F(RootFindDifferential, NonPositiveIterationCountsAgree) {
  // Upstream's two phases disagree about what a non-positive count means:
  // the secant loop runs zero times and reports "not bracketed", while the
  // Brent loop runs zero times and reports FAILURE. Whatever falls out of
  // that, the port must fall out the same way.
  for (int iters : {-5, 0, 1, 2, 3}) {
    for (Which which : {Which::Linear, Which::Cubic, Which::CdsMonotone}) {
      cdsmodel::RootFindOptions opt;
      opt.bound_lo = 0.0;
      opt.bound_hi = 100.0;
      opt.max_iterations = iters;
      opt.guess = 0.01;
      opt.initial_x_step = 0.0001;
      opt.x_tolerance = 1.0e-8;
      opt.f_tolerance = 1.0e-8;
      const Outcome expected = reference_run(which, opt);
      const Outcome got = port_run(which, opt);
      ASSERT_EQ(got.root.has_value(), expected.root.has_value())
          << name_of(which) << " iters=" << iters;
      EXPECT_EQ(got.calls, expected.calls) << name_of(which) << " iters=" << iters;
      if (expected.root) {
        EXPECT_EQ(*got.root, *expected.root) << name_of(which) << " iters=" << iters;
      }
    }
  }
}

TEST_F(RootFindDifferential, TheCdsoneCallSiteExactly) {
  // The options cdsone.c passes, which is the only call site in the shipped
  // library: bounds [0,100], 100 iterations, guess 1%, step 1bp, no
  // derivative, both tolerances 1e-8.
  cdsmodel::RootFindOptions opt;
  opt.bound_lo = 0.0;
  opt.bound_hi = 100.0;
  opt.max_iterations = 100;
  opt.guess = 0.01;
  opt.initial_x_step = 0.0001;
  opt.initial_f_derivative = 0.0;
  opt.x_tolerance = 1.0e-8;
  opt.f_tolerance = 1.0e-8;

  for (Which which : {Which::CdsMonotone, Which::Linear, Which::Exponential, Which::Quadratic,
                      Which::SteepTanh}) {
    const Outcome expected = reference_run(which, opt);
    const Outcome got = port_run(which, opt);
    ASSERT_EQ(got.root.has_value(), expected.root.has_value()) << name_of(which);
    EXPECT_EQ(got.calls, expected.calls) << name_of(which);
    if (expected.root) {
      EXPECT_EQ(*got.root, *expected.root) << name_of(which);
    }
  }
}

} // namespace
