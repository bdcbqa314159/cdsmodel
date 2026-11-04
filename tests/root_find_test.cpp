// TIER 1 -- the root finder. Port semantics, recorded replay, and the
// sensitivity measurement that decides which tolerance tier applies.

#include <cdsmodel/root_find.hpp>

#include <gtest/gtest.h>

#include "golden/root_find_cases.hpp"
#include "root_find_objectives.hpp"

#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

namespace {

using cdsmodel::root_find_brent;
using cdsmodel::RootFindOptions;
using cdsmodel::testing::has_single_root;
using cdsmodel::testing::kAllObjectives;
using cdsmodel::testing::name_of;
using cdsmodel::testing::Objective;
using cdsmodel::testing::root_resolution;
using cdsmodel::testing::Which;

RootFindOptions cdsone_options() {
  // The only call site in the shipped library, from cdsone.c.
  RootFindOptions opt;
  opt.bound_lo = 0.0;
  opt.bound_hi = 100.0;
  opt.max_iterations = 100;
  opt.guess = 0.01;
  opt.initial_x_step = 0.0001;
  opt.initial_f_derivative = 0.0;
  opt.x_tolerance = 1.0e-8;
  opt.f_tolerance = 1.0e-8;
  return opt;
}

TEST(RootFind, MatchesRecordedRoots) {
  // Roots only. The recorded evaluation COUNT is deliberately not asserted
  // here, and the reason is a correction rather than a simplification.
  //
  // The count was asserted in the first version of this test, on the
  // reasoning that an iterative result needs its path checked because two
  // paths can reach the same root. That reasoning is right in-process, where
  // the tier-2 differential suite does assert it and passes.
  //
  // Across platforms it is wrong. CI measured 146 of 10,296 cases taking a
  // different number of evaluations on Linux and Windows than on macOS --
  // including for pure polynomials with no libm involved at all, `Quadratic`
  // worst at 97 cases. The cause is floating-point contraction: a compiler
  // may fuse `a*b + c` into a single FMA with one rounding instead of two,
  // and `-ffp-contract=fast` is the default. That changes values MID
  // EXPRESSION and by different amounts at different points, which is a
  // strictly stronger disturbance than shifting the objective's return value
  // by one ULP -- and shifting the return value is what
  // OneUlpPerturbationOfTheObjectiveDoesNotAmplify measures. That test is
  // true; it just measures a weaker property than this one needed.
  //
  // So the path is in-process evidence, and the root is the portable claim.
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kRootFindCases) {
    RootFindOptions opt;
    opt.bound_lo = c.bound_lo;
    opt.bound_hi = c.bound_hi;
    opt.max_iterations = c.max_iterations;
    opt.guess = c.guess;
    opt.initial_x_step = c.initial_x_step;
    opt.initial_f_derivative = c.initial_f_derivative;
    opt.x_tolerance = c.x_tolerance;
    opt.f_tolerance = c.f_tolerance;

    Objective obj{kAllObjectives[static_cast<std::size_t>(c.objective)]};
    const std::optional<double> got = root_find_brent(obj, opt);
    const std::string where = std::string(name_of(obj.which)) + " case " + std::to_string(checked);

    ASSERT_EQ(got.has_value(), c.solved) << where;
    // The recorded count is still checked, but as a BAND rather than an
    // equality, so it catches an algorithmic divergence without failing on
    // the platform variation measured above. Worst observed ratio across
    // Linux, Windows and macOS was 1.93x (108 evaluations against 208); the
    // band is 4x plus a constant, which leaves real headroom while still
    // catching a port that iterates an order of magnitude more than upstream.
    EXPECT_GT(obj.calls, 0) << where << " -- the objective was never evaluated";
    EXPECT_LE(obj.calls, 4 * c.calls + 8) << where << " -- far more evaluations than upstream";
    EXPECT_LE(c.calls, 4 * obj.calls + 8) << where << " -- far fewer evaluations than upstream";
    if (c.solved) {
      // Relative 1e-12 for a well-posed objective; widened to the objective's
      // own resolution where it has plateaus, because within a plateau the
      // returned point is a property of the path and not of the function.
      const double tolerance =
          std::max(1.0e-12 * std::max(1.0, std::fabs(c.root)), root_resolution(obj.which));
      EXPECT_NEAR(*got, c.root, tolerance) << where;
    }
    ++checked;
  }
  EXPECT_EQ(checked, static_cast<int>(cdsmodel::golden::kRootFindCases.size()));
  EXPECT_GT(checked, 10000) << "recorded vectors thinner than expected: " << checked;
}

TEST(RootFind, EveryReturnedRootSatisfiesTheSolversOwnContract) {
  // The strongest check in this file, and the only one that needs neither an
  // oracle nor a recorded number: whatever the search returns, the objective
  // must actually be within `f_tolerance` of zero there.
  //
  // This is what the recorded roots were really standing in for. A recorded
  // root says "macOS got this number"; this says "the returned point solves
  // the equation to the accuracy the caller asked for", which is the property
  // a caller depends on and is identical on every platform.
  //
  // Every acceptance branch in root_find_brent returns a point satisfying
  // `f == 0.0` or `|f| <= f_tolerance`, so this holds by construction -- and
  // asserting it means a future edit that returns an unconverged point fails
  // here rather than silently shipping.
  int verified = 0;
  for (const auto &c : cdsmodel::golden::kRootFindCases) {
    if (!c.solved) {
      continue;
    }
    RootFindOptions opt;
    opt.bound_lo = c.bound_lo;
    opt.bound_hi = c.bound_hi;
    opt.max_iterations = c.max_iterations;
    opt.guess = c.guess;
    opt.initial_x_step = c.initial_x_step;
    opt.initial_f_derivative = c.initial_f_derivative;
    opt.x_tolerance = c.x_tolerance;
    opt.f_tolerance = c.f_tolerance;

    const Which which = kAllObjectives[static_cast<std::size_t>(c.objective)];
    Objective obj{which};
    const std::optional<double> root = root_find_brent(obj, opt);
    if (!root) {
      continue; // a platform that did not converge is covered by the test above
    }
    Objective probe{which};
    const std::optional<double> value = probe(*root);
    ASSERT_TRUE(value.has_value()) << name_of(which) << " root is outside its own domain";
    EXPECT_LE(std::fabs(*value), opt.f_tolerance)
        << name_of(which) << " returned " << *root << " where f = " << *value
        << ", outside the requested f_tolerance of " << opt.f_tolerance;
    // And it must lie inside the bracket it was asked to search.
    EXPECT_GE(*root, opt.bound_lo) << name_of(which);
    EXPECT_LE(*root, opt.bound_hi) << name_of(which);
    ++verified;
  }
  EXPECT_GT(verified, 5000) << "too few solved cases verified: " << verified;
}

TEST(RootFind, OneUlpPerturbationOfTheReturnValueDoesNotAmplify) {
  // The measurement this module exists to settle. Everything ported before
  // this evaluates a formula, so a one-ULP difference stays one ULP. A root
  // finder feeds its arithmetic into comparisons that choose the next
  // iterate, so in principle a sub-ULP difference can flip a branch and move
  // the answer arbitrarily. That would mean cross-platform replay needs a
  // looser tolerance than the relative 1e-12 used for accumulating values.
  //
  // Measured here rather than assumed, on the platform actually running: for
  // every single-rooted objective, perturbing the objective's return value by
  // exactly one ULP in either direction leaves the root within relative
  // 1e-12. The 1e-8 convergence tolerance acts as a quantiser that absorbs
  // perturbations far below it.
  //
  // IMPORTANT -- what this does NOT establish, learned the hard way.
  //
  // Shifting the objective's RETURN VALUE by one ULP is a weaker disturbance
  // than a real cross-platform difference, which changes values mid
  // expression (floating-point contraction can fuse `a*b + c` into a single
  // FMA with one rounding instead of two) and by different amounts at
  // different points. CI found 146 of 10,296 recorded cases taking a
  // different number of evaluations on Linux and Windows -- including pure
  // polynomials -- while this test passed on all three platforms.
  //
  // So this is a true statement about a narrower property than it first
  // appears to cover. It is kept because the property is worth pinning, and
  // because the gap between it and what CI measured is the whole lesson: a
  // single-platform sensitivity probe is not evidence of cross-platform
  // reproducibility. The three-OS matrix is.
  const RootFindOptions opt = cdsone_options();
  int measured = 0;
  for (Which which : kAllObjectives) {
    if (!has_single_root(which)) {
      continue; // see MultipleRootsArePathDependent
    }
    Objective base{which};
    const std::optional<double> unperturbed = root_find_brent(base, opt);
    if (!unperturbed) {
      continue; // NoRoot and FailsAbove are not solvable from this guess
    }
    for (int direction : {-1, 1}) {
      Objective shifted{which, direction};
      const std::optional<double> root = root_find_brent(shifted, opt);
      ASSERT_TRUE(root.has_value())
          << name_of(which) << " perturbed by " << direction << " ULP stopped converging";
      const double tolerance = 1.0e-12 * std::max(1.0, std::fabs(*unperturbed));
      EXPECT_NEAR(*root, *unperturbed, tolerance)
          << name_of(which) << " moved by " << std::fabs(*root - *unperturbed)
          << " under a one-ULP perturbation";
    }
    ++measured;
  }
  EXPECT_GE(measured, 6) << "too few objectives actually measured: " << measured;
}

TEST(RootFind, MultipleRootsArePathDependent) {
  // The honest counterpart to the test above, and not a defect.
  //
  // `TwoRoots` is zero at x = 0 and again near x = 3.75, and the cdsone call
  // site puts a bound at exactly 0. Unperturbed, the search accepts x = 0 via
  // the exact `y == 0.0` test. One ULP disqualifies it and the search finds
  // the other root instead -- a change of 3.75, from a perturbation of 1e-17.
  //
  // That is correct behaviour for a function with two roots: the finder
  // promises A root, not a particular one. It is recorded as an executable
  // property because "the root finder is robust to one ULP" is true only for
  // single-rooted objectives, and the unqualified claim would be false.
  const RootFindOptions opt = cdsone_options();
  Objective base{Which::TwoRoots};
  const std::optional<double> unperturbed = root_find_brent(base, opt);
  ASSERT_TRUE(unperturbed.has_value());
  EXPECT_EQ(*unperturbed, 0.0) << "expected the root sitting on the lower bound";

  Objective shifted{Which::TwoRoots, 1};
  const std::optional<double> moved = root_find_brent(shifted, opt);
  ASSERT_TRUE(moved.has_value());
  EXPECT_GT(std::fabs(*moved - *unperturbed), 1.0)
      << "expected the search to find the far root once x=0 was disqualified";
  // And whichever it returns really is a root of the perturbed function.
  Objective probe{Which::TwoRoots, 1};
  const std::optional<double> value = probe(*moved);
  ASSERT_TRUE(value.has_value());
  EXPECT_LT(std::fabs(*value), 1.0e-8) << "returned a point that is not a root";
}

TEST(RootFind, SolvesEachObjectiveToItsKnownRoot) {
  // Independent of upstream and of the recorded vectors: analytic roots.
  const RootFindOptions opt = cdsone_options();
  struct Known {
    Which which;
    double root;
  };
  const Known known[]{{Which::Linear, 1.5},
                      {Which::Quadratic, std::numbers::sqrt2},
                      {Which::Exponential, std::log(7.0)},
                      {Which::SteepTanh, 1.25},
                      {Which::CdsMonotone, 2.0 / 3.0}};
  for (const Known &k : known) {
    Objective obj{k.which};
    const std::optional<double> got = root_find_brent(obj, opt);
    ASSERT_TRUE(got.has_value()) << name_of(k.which);
    // The finder's own contract is |f| <= f_tolerance, which near a steep
    // root is much tighter in x than near a flat one. 1e-6 covers both.
    EXPECT_NEAR(*got, k.root, 1.0e-6) << name_of(k.which);
  }
}

TEST(RootFind, Rejected) {
  Objective obj{Which::Linear};
  RootFindOptions opt = cdsone_options();

  opt.bound_lo = 1.0;
  opt.bound_hi = 1.0;
  opt.guess = 1.0;
  EXPECT_FALSE(root_find_brent(obj, opt).has_value()) << "equal bounds";

  opt.bound_lo = 2.0;
  opt.bound_hi = 1.0;
  opt.guess = 1.5;
  EXPECT_FALSE(root_find_brent(obj, opt).has_value()) << "reversed bounds";

  opt = cdsone_options();
  opt.guess = -1.0;
  EXPECT_FALSE(root_find_brent(obj, opt).has_value()) << "guess below the lower bound";
  opt.guess = 101.0;
  EXPECT_FALSE(root_find_brent(obj, opt).has_value()) << "guess above the upper bound";

  // No root in the bracket: upstream reports the bound values and fails.
  Objective none{Which::NoRoot};
  EXPECT_FALSE(root_find_brent(none, cdsone_options()).has_value());
}

TEST(RootFind, AnObjectiveThatCannotBeEvaluatedPropagatesFailure) {
  // FailsAbove returns nullopt above x = 3. With a bracket reaching 100 the
  // search must step there, and the failure must surface rather than being
  // treated as a value.
  Objective obj{Which::FailsAbove};
  RootFindOptions opt = cdsone_options();
  opt.guess = 50.0;
  EXPECT_FALSE(root_find_brent(obj, opt).has_value());
  EXPECT_GT(obj.calls, 0) << "the objective was never called";
}

TEST(RootFind, GuessAcceptanceDependsOnProximityToABound) {
  // The quirk documented on root_find_brent, as an executable fact.
  //
  // The initial guess is accepted when |f(guess)| <= f_tolerance AND the
  // guess is within x_tolerance of EITHER bound. Proximity to a bound is not
  // evidence about a root; it stands in for the converged-iterates test that
  // cannot run when there is no previous iterate. The consequence is that
  // accepting the guess depends on where the caller put the bounds.
  //
  // f(x) = 2x - 3, so f(1.5) = 0 exactly and the guess is taken regardless.
  // Offset the guess slightly so |f| is small but non-zero, and the bound
  // position becomes the deciding factor.
  Objective obj{Which::Linear};
  RootFindOptions wide;
  wide.bound_lo = -100.0;
  wide.bound_hi = 100.0;
  wide.max_iterations = 100;
  wide.guess = 1.5 + 1.0e-9; // f = 2e-9, inside a 1e-8 f_tolerance
  wide.initial_x_step = 0.1;
  wide.x_tolerance = 1.0e-6;
  wide.f_tolerance = 1.0e-8;

  RootFindOptions tight = wide;
  tight.bound_lo = wide.guess - 1.0e-9; // now within x_tolerance of a bound
  tight.bound_hi = wide.guess + 10.0;

  Objective a{Which::Linear};
  Objective b{Which::Linear};
  const std::optional<double> from_wide = root_find_brent(a, wide);
  const std::optional<double> from_tight = root_find_brent(b, tight);
  ASSERT_TRUE(from_wide.has_value());
  ASSERT_TRUE(from_tight.has_value());

  // Near a bound, the guess is returned as-is after a single evaluation.
  EXPECT_EQ(*from_tight, tight.guess);
  EXPECT_EQ(b.calls, 1);
  // Far from both bounds, the same guess is rejected and the search runs on.
  EXPECT_GT(a.calls, 1) << "the wide-bracket case short-circuited too";
}

} // namespace
