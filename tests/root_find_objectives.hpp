#pragma once

// Objectives for the root finder, shared by three consumers: the tier-2
// differential suite, the tier-1 replay suite, and the golden generator.
//
// Shared rather than duplicated on purpose. A recorded root is only
// meaningful if the function that produced it is byte-identical to the one
// replaying it, and three copies of a switch statement would drift.
//
// They are chosen to reach branches, not to be realistic: a quantised output
// so `f21`/`f31`/`f32` hit exactly zero and force bisection, a near-flat
// region, a kink, a function that cannot be evaluated above a threshold, and
// one with two roots where a one-ULP change decides which you get.

#include <cdsmodel/expected.hpp>

#include <cmath>
#include <limits>
#include <optional>

namespace cdsmodel::testing {

enum class Which {
  Linear,       ///< one root, trivially solvable
  Cubic,        ///< one real root in the brackets used
  Quadratic,    ///< smooth curvature the interpolation should like
  Exponential,  ///< transcendental: libm is inside the loop
  SteepTanh,    ///< nearly flat away from the root, very steep at it
  Quantised,    ///< output truncated to 6 digits -- forces exact f-ties
  FlatThenKink, ///< derivative zero on one side of the root
  NoRoot,       ///< same sign at both bounds
  FailsAbove,   ///< cannot be evaluated above a threshold
  CdsMonotone,  ///< monotone and saturating, the shape of the real problem
  TwoRoots,     ///< roots at 0 AND ~3.75; x=0 also sits on a bound
};

inline constexpr Which kAllObjectives[] = {Which::Linear,       Which::Cubic,     Which::Quadratic,
                                           Which::Exponential,  Which::SteepTanh, Which::Quantised,
                                           Which::FlatThenKink, Which::NoRoot,    Which::FailsAbove,
                                           Which::CdsMonotone,  Which::TwoRoots};

/// A counting objective. `perturb` shifts every returned value by exactly one
/// ULP, which is the proxy for a cross-platform libm difference.
struct Objective {
  Which which;
  int perturb = 0;
  int calls = 0;

  cdsmodel::Expected<double> operator()(double x) {
    ++calls;
    double v = 0.0;
    switch (which) {
    case Which::Linear:
      v = 2.0 * x - 3.0;
      break;
    case Which::Cubic:
      v = x * x * x - 2.0 * x - 5.0;
      break;
    case Which::Quadratic:
      v = x * x - 2.0;
      break;
    case Which::Exponential:
      v = std::exp(x) - 7.0;
      break;
    case Which::SteepTanh:
      v = std::tanh(50.0 * (x - 1.25));
      break;
    case Which::Quantised:
      v = std::trunc((x * x - 2.0) * 1.0e6) / 1.0e6;
      break;
    case Which::FlatThenKink:
      v = x < 1.5 ? -1.0 : (x - 1.5);
      break;
    case Which::NoRoot:
      v = x * x + 1.0;
      break;
    case Which::FailsAbove:
      if (x > 3.0) {
        return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument,
                              "the objective is undefined at x = " + cdsmodel::detail::num(x));
      }
      v = x - 2.0;
      break;
    case Which::CdsMonotone:
      v = 0.02 * x / (1.0 + 0.5 * x) - 0.01;
      break;
    case Which::TwoRoots:
      v = 0.045 * (1.0 - std::exp(-4.0 * x)) - 0.012 * x;
      break;
    }
    if (perturb > 0) {
      v = std::nextafter(v, std::numeric_limits<double>::infinity());
    } else if (perturb < 0) {
      v = std::nextafter(v, -std::numeric_limits<double>::infinity());
    }
    return v;
  }
};

inline const char *name_of(Which w) {
  switch (w) {
  case Which::Linear:
    return "Linear";
  case Which::Cubic:
    return "Cubic";
  case Which::Quadratic:
    return "Quadratic";
  case Which::Exponential:
    return "Exponential";
  case Which::SteepTanh:
    return "SteepTanh";
  case Which::Quantised:
    return "Quantised";
  case Which::FlatThenKink:
    return "FlatThenKink";
  case Which::NoRoot:
    return "NoRoot";
  case Which::FailsAbove:
    return "FailsAbove";
  case Which::CdsMonotone:
    return "CdsMonotone";
  case Which::TwoRoots:
    return "TwoRoots";
  }
  return "?";
}

/// Objectives with exactly one root in every bracket used, so a recorded root
/// is a property of the function rather than of the search path. `TwoRoots`
/// is excluded by construction -- see RootFind.MultipleRootsArePathDependent.
[[nodiscard]] inline bool has_single_root(Which w) { return w != Which::TwoRoots; }

/// How precisely the root is DETERMINED by the objective, in x.
///
/// This is not a solver tolerance. `Quantised` truncates its output to 1e-6,
/// so near its root -- where the derivative is about 2.83 -- every x within
/// roughly 1e-6/2.83 = 3.5e-7 produces an identical function value. The
/// function is flat in steps, and any point on the step satisfies
/// `|f| <= f_tolerance` equally. Which point a search lands on is a property
/// of the path, not of the function, so two correct implementations can and
/// do disagree within a plateau.
///
/// Measured in CI: Linux and Windows differ from macOS by up to 6.3e-9 on
/// this objective -- comfortably inside one plateau, and not a defect in
/// either. Every smooth objective agrees to relative 1e-12 on all three.
///
/// Returning 0 means "determined as precisely as the arithmetic allows", and
/// the caller should use the ordinary accumulating-float tolerance.
[[nodiscard]] inline double root_resolution(Which w) {
  return w == Which::Quantised ? 1.0e-6 : 0.0;
}

} // namespace cdsmodel::testing
