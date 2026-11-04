#pragma once

// Port of upstream rtbrent.c -- the one-dimensional root finder the model
// uses to invert a price into a spread.
//
// This is the first module where an arithmetic difference can AMPLIFY rather
// than carry. Everything ported so far evaluates a formula once: a one-ULP
// difference stays one ULP. Here the arithmetic feeds comparisons that choose
// the next iterate, so a difference too small to see can change the branch
// taken, which changes the iteration path, which changes the answer by far
// more than the original difference. Bit-exactness in-process is therefore
// necessary but no longer sufficient evidence, and this header is transcribed
// with that in mind: every expression keeps its exact written form and every
// comparison its exact operands.
//
// Three things are deliberately NOT tidied, because each one decides a branch:
//
//   * `initial_f_derivative == 0.0` and `initial_x_step == 0.0` are exact
//     floating-point comparisons against zero. They select Newton-vs-step and
//     default-vs-given, so making them tolerant would change which path runs.
//   * the interpolation-suitability test `f3*f31 < ratio*f2*f21` is written in
//     that order, including the `ratio` that upstream computes before it has
//     checked the denominators it divides by.
//   * upstream's ABS macro is `((x) > 0 ? (x) : -(x))`, which returns NEGATIVE
//     zero for an input of zero. Every use is inside a comparison, where
//     -0.0 compares equal to +0.0, so `std::fabs` is behaviourally identical.
//     The difference is real and happens not to matter; it is recorded so the
//     substitution is a checked decision rather than an assumption.
//
// The algorithm is secant first, then the bounds, then Brent's inverse
// quadratic with a bisection fallback. The structure matters: upstream reaches
// Brent only once two of three points bracket a root, and it is willing to
// evaluate the objective at both bounds to get there.

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <optional>

namespace cdsmodel {

/// A function whose root is wanted. Returns nullopt where it cannot be
/// evaluated, which upstream signals with a FAILURE return and an out-param.
template <class F>
concept Objective = requires(F &f, double x) {
  { f(x) } -> std::convertible_to<std::optional<double>>;
};

/// Tuning for `root_find_brent`. Upstream passes these as eight positional
/// arguments, six of them `double`, in an order no caller can check.
///
/// The defaults are upstream's own behaviour where it has any: a zero
/// `initial_x_step` means "one percent of the bracket", and a zero
/// `initial_f_derivative` means "no derivative known, take a step instead".
struct RootFindOptions {
  double bound_lo = 0.0;
  double bound_hi = 0.0;
  int max_iterations = 100;
  double guess = 0.0;

  /// First step away from the guess. Zero means one percent of the bracket.
  double initial_x_step = 0.0;

  /// Derivative at the guess, for a Newton first step. Zero means unknown.
  double initial_f_derivative = 0.0;

  double x_tolerance = 0.0;
  double f_tolerance = 0.0;
};

namespace detail {

/// Upstream's ONE_PERCENT.
inline constexpr double kBracketFraction = 0.01;

/// What the secant phase concluded. Upstream returns this through two
/// `TBoolean` out-params plus the status, a three-way encoding its own comment
/// has to tabulate to explain.
struct SecantOutcome {
  enum class Status { NotBracketed, Bracketed, Found, ObjectiveFailed };
  Status status = Status::NotBracketed;
  double solution = 0.0;
};

/// Upstream's `secantMethod`. Runs the secant iteration until it finds the
/// root, brackets it, steps outside the bounds, or runs out of iterations.
template <Objective F>
SecantOutcome secant_method(F &f, const RootFindOptions &opt, std::array<double, 3> &x,
                            std::array<double, 3> &y) {
  // `int j = numIterations; while (j--)` -- so a non-positive count runs the
  // body zero times and reports "not bracketed" rather than failing. The
  // Brent phase treats the same input as a failure; the inconsistency is
  // upstream's and is preserved.
  int j = opt.max_iterations;
  while (j-- != 0) {
    // Order the pair so index 0 holds the smaller function value.
    if (std::fabs(y[0]) > std::fabs(y[2])) {
      std::swap(x[0], x[2]);
      std::swap(y[0], y[2]);
    }

    double dx = 0.0;
    if (std::fabs(y[0] - y[2]) <= opt.f_tolerance) {
      // The secant denominator is too small to divide by, so `f_tolerance`
      // stands in for it as a floor on the magnitude. The two branches exist
      // to carry the sign the denominator would have had; both reduce to
      // dx = y0 * (x2 - x0) / (+/-f_tolerance).
      if (y[0] - y[2] > 0) {
        dx = -y[0] * (x[0] - x[2]) / opt.f_tolerance;
      } else {
        dx = y[0] * (x[0] - x[2]) / opt.f_tolerance;
      }
    } else {
      dx = (x[2] - x[0]) * y[0] / (y[0] - y[2]);
    }
    x[1] = x[0] + dx;

    if (x[1] < opt.bound_lo || x[1] > opt.bound_hi) {
      // Note that y[1] is NOT evaluated here. Upstream leaves it whatever it
      // was, and the caller overwrites both before using them.
      return {SecantOutcome::Status::NotBracketed, 0.0};
    }

    const std::optional<double> value = f(x[1]);
    if (!value) {
      return {SecantOutcome::Status::ObjectiveFailed, 0.0};
    }
    y[1] = *value;

    if (y[1] == 0.0 ||
        (std::fabs(y[1]) <= opt.f_tolerance && std::fabs(x[1] - x[0]) <= opt.x_tolerance)) {
      return {SecantOutcome::Status::Found, x[1]};
    }

    if ((y[0] < 0 && y[1] < 0 && y[2] < 0) || (y[0] > 0 && y[1] > 0 && y[2] > 0)) {
      // All three on the same side: keep the two smallest and iterate.
      if (std::fabs(y[0]) > std::fabs(y[1])) {
        x[2] = x[0];
        y[2] = y[0];
        x[0] = x[1];
        y[0] = y[1];
      } else {
        x[2] = x[1];
        y[2] = y[1];
      }
      continue;
    }
    // A sign change exists among the three. Arrange it so y[0]*y[2] < 0.
    if (y[0] * y[2] > 0) {
      if (x[1] < x[0]) {
        std::swap(x[0], x[1]);
        std::swap(y[0], y[1]);
      } else {
        std::swap(x[1], x[2]);
        std::swap(y[1], y[2]);
      }
    }
    return {SecantOutcome::Status::Bracketed, 0.0};
  }
  return {SecantOutcome::Status::NotBracketed, 0.0};
}

/// Upstream's `brentMethod`: inverse quadratic interpolation through three
/// points, falling back to bisection when the interpolation is unsuitable.
///
/// Returns nullopt both when the objective fails and when the iteration count
/// is exhausted -- upstream returns FAILURE for both, and non-convergence
/// genuinely has no answer to report.
template <Objective F>
std::optional<double> brent_method(F &f, const RootFindOptions &opt,
                                   const std::array<double, 3> &x_in,
                                   const std::array<double, 3> &y_in) {
  double x1 = x_in[0];
  double x2 = x_in[1];
  double x3 = x_in[2];
  double f1 = y_in[0];
  double f2 = y_in[1];
  double f3 = y_in[2];

  for (int j = 1; j <= opt.max_iterations; ++j) {
    // Keep f1 and f2 of opposite sign, f2 and f3 of the same sign.
    if (f2 * f1 > 0.0) {
      std::swap(x1, x3);
      std::swap(f1, f3);
    }
    const double f21 = f2 - f1;
    const double f32 = f3 - f2;
    const double f31 = f3 - f1;
    const double x21 = x2 - x1;
    const double x31 = x3 - x1;

    // `ratio` divides by (x2-x1) before anything has established it is
    // non-zero, and x2 is a midpoint of x1 and x3, so x2 == x1 whenever
    // x1 == x3. The quotient is then an infinity or a NaN. That is defined
    // IEEE behaviour rather than undefined C, and the comparison it feeds is
    // false for a NaN, after which `f21 == 0.` catches the same situation and
    // selects bisection. The order is load-bearing and is preserved: the
    // division is not hoisted, guarded, or reordered.
    const double ratio = (x3 - x1) / (x2 - x1);
    if (f3 * f31 < ratio * f2 * f21 || f21 == 0. || f31 == 0. || f32 == 0.) {
      // Unsuitable for interpolation -- bisect instead. Upstream's comment
      // explains why the zero tests are exact rather than near-zero: an
      // objective truncated to a fixed number of digits really did produce an
      // exactly equal pair of values in one case they hit.
      x3 = x2;
      f3 = f2;
    } else {
      const double xm =
          x1 - (f1 / f21) * x21 + ((f1 * f2) / (f31 * f32)) * x31 - ((f1 * f2) / (f21 * f32)) * x21;
      const std::optional<double> fm_value = f(xm);
      if (!fm_value) {
        return std::nullopt;
      }
      const double fm = *fm_value;
      if (fm == 0.0 ||
          (std::fabs(fm) <= opt.f_tolerance && std::fabs(xm - x1) <= opt.x_tolerance)) {
        return xm;
      }
      if (fm * f1 < 0.0) {
        x3 = xm;
        f3 = fm;
      } else {
        x1 = xm;
        f1 = fm;
        x3 = x2;
        f3 = f2;
      }
    }
    x2 = 0.5 * (x1 + x3);
    const std::optional<double> f2_value = f(x2);
    if (!f2_value) {
      return std::nullopt;
    }
    f2 = *f2_value;
    if (f2 == 0.0 || (std::fabs(f2) <= opt.f_tolerance && std::fabs(x2 - x1) <= opt.x_tolerance)) {
      return x2;
    }
  }
  return std::nullopt; // iterations exhausted
}

} // namespace detail

/// Solve `f(x) == 0` on `[bound_lo, bound_hi]`. Replaces `JpmcdsRootFindBrent`.
///
/// Secant from the guess first; if that neither finds nor brackets a root, try
/// the bounds themselves; once two points bracket, finish with Brent.
///
/// Returns nullopt where upstream returns FAILURE: bounds the wrong way round,
/// a guess outside them, an objective that cannot be evaluated at a point the
/// search needs, function values at both bounds that imply no root, or the
/// iteration count exhausted inside Brent.
///
/// One acceptance test is worth knowing about before trusting a result, and it
/// is preserved exactly as written:
///
///     if (y0 == 0.0 || (ABS(y0) <= facc && (ABS(boundLo-x0) <= xacc ||
///                                           ABS(boundHi-x0) <= xacc)))
///
/// The guess is accepted when its function value is within `f_tolerance` AND
/// the guess happens to lie within `x_tolerance` of EITHER BOUND. Proximity to
/// a bound is not evidence about a root; it is standing in for the
/// "did the last two iterates converge" test that the later checks use and
/// that cannot be applied when there is no previous iterate. The consequence
/// is that acceptance of the initial guess depends on where the caller put the
/// bounds. It only ever accepts a point already inside `f_tolerance`, so it
/// cannot return something the caller called unacceptable -- which is why this
/// is documented as a quirk rather than recorded as a defect.
template <Objective F>
[[nodiscard]] std::optional<double> root_find_brent(F &&f, const RootFindOptions &opt) {
  std::array<double, 3> x{};
  std::array<double, 3> y{};
  x[0] = opt.guess;

  if (opt.bound_lo >= opt.bound_hi) {
    return std::nullopt;
  }
  if (x[0] < opt.bound_lo || x[0] > opt.bound_hi) {
    return std::nullopt;
  }

  const std::optional<double> at_guess = f(x[0]);
  if (!at_guess) {
    return std::nullopt;
  }
  y[0] = *at_guess;

  if (y[0] == 0.0 ||
      (std::fabs(y[0]) <= opt.f_tolerance && (std::fabs(opt.bound_lo - x[0]) <= opt.x_tolerance ||
                                              std::fabs(opt.bound_hi - x[0]) <= opt.x_tolerance))) {
    return x[0];
  }

  const double bound_spread = opt.bound_hi - opt.bound_lo;
  double initial_x_step = opt.initial_x_step;
  if (initial_x_step == 0.0) {
    initial_x_step = detail::kBracketFraction * bound_spread;
  }

  if (opt.initial_f_derivative == 0) {
    x[2] = x[0] + initial_x_step;
  } else {
    x[2] = x[0] - y[0] / opt.initial_f_derivative;
  }

  if (x[2] < opt.bound_lo || x[2] > opt.bound_hi) {
    x[2] = x[0] - initial_x_step; // try the other direction
    if (x[2] < opt.bound_lo) {
      x[2] = opt.bound_lo;
    }
    if (x[2] > opt.bound_hi) {
      x[2] = opt.bound_hi;
    }
    if (x[2] == x[0]) {
      // The two points must differ, so step in off the bound we landed on.
      if (x[2] == opt.bound_lo) {
        x[2] = opt.bound_lo + detail::kBracketFraction * bound_spread;
      } else {
        x[2] = opt.bound_hi - detail::kBracketFraction * bound_spread;
      }
    }
  }

  const std::optional<double> at_second = f(x[2]);
  if (!at_second) {
    return std::nullopt;
  }
  y[2] = *at_second;

  if (y[2] == 0.0 ||
      (std::fabs(y[2]) <= opt.f_tolerance && std::fabs(x[2] - x[0]) <= opt.x_tolerance)) {
    return x[2];
  }

  bool bracketed = false;
  {
    const detail::SecantOutcome outcome = detail::secant_method(f, opt, x, y);
    switch (outcome.status) {
    case detail::SecantOutcome::Status::ObjectiveFailed:
      return std::nullopt;
    case detail::SecantOutcome::Status::Found:
      return outcome.solution;
    case detail::SecantOutcome::Status::Bracketed:
      bracketed = true;
      break;
    case detail::SecantOutcome::Status::NotBracketed:
      break;
    }
  }

  if (!bracketed) {
    // Secant did not bracket. Try the bounds, lower first.
    const std::optional<double> at_lo = f(opt.bound_lo);
    if (!at_lo) {
      return std::nullopt;
    }
    const double f_lo = *at_lo;
    if (f_lo == 0.0 ||
        (std::fabs(f_lo) <= opt.f_tolerance && std::fabs(opt.bound_lo - x[0]) <= opt.x_tolerance)) {
      return opt.bound_lo;
    }

    if (y[0] * f_lo < 0) {
      // Bracketed between the lower bound and the guess; order them.
      x[2] = x[0];
      x[0] = opt.bound_lo;
      y[2] = y[0];
      y[0] = f_lo;
    } else {
      const std::optional<double> at_hi = f(opt.bound_hi);
      if (!at_hi) {
        return std::nullopt;
      }
      const double f_hi = *at_hi;
      if (f_hi == 0.0 || (std::fabs(f_hi) <= opt.f_tolerance &&
                          std::fabs(opt.bound_hi - x[0]) <= opt.x_tolerance)) {
        return opt.bound_hi;
      }
      if (y[0] * f_hi < 0) {
        x[2] = opt.bound_hi;
        y[2] = f_hi;
      } else {
        return std::nullopt; // values at both bounds imply no root
      }
    }

    // Two points bracket the root; Brent needs a third. Take the midpoint.
    x[1] = 0.5 * (x[0] + x[2]);
    const std::optional<double> at_mid = f(x[1]);
    if (!at_mid) {
      return std::nullopt;
    }
    y[1] = *at_mid;
    if (y[1] == 0.0 ||
        (std::fabs(y[1]) <= opt.f_tolerance && std::fabs(x[1] - x[0]) <= opt.x_tolerance)) {
      return x[1];
    }
  }

  return detail::brent_method(f, opt, x, y);
}

} // namespace cdsmodel
