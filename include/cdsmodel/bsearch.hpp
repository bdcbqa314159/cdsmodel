#pragma once

// Port of upstream bsearch.c + bsearch.inc.
//
// Upstream generated two copies of this algorithm by #including bsearch.inc
// twice with X_TYPE #defined to double and long -- the C preprocessor used as
// a template engine. One C++20 template replaces both copies.
//
// IMPORTANT: this is a *bracketing* search for interpolation, NOT a membership
// search. std::lower_bound is the wrong tool -- see docs/port-notes.md for the
// three boundary behaviours that would silently change if you used it.

#include <cdsmodel/expected.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>

namespace cdsmodel {

/// Indices of the pair of array elements surrounding a desired value.
/// Both equal 0 for a single-element array.
struct Bracket {
  std::size_t lo;
  std::size_t hi;

  friend constexpr bool operator==(const Bracket &, const Bracket &) = default;
};

/// True when every element is strictly less than its successor.
///
/// Replaces upstream JpmcdsCheckDoubleArrayOrder / JpmcdsCheckLongArrayOrder
/// (25 lines of pointer arithmetic each). Upstream rejects equal neighbours,
/// so the comparison is >=, not >.
template <std::totally_ordered T>
[[nodiscard]] constexpr bool is_strictly_increasing(std::span<const T> xs) noexcept {
  return std::ranges::adjacent_find(xs, std::greater_equal<T>{}) == xs.end();
}

/// Bracket `desired` within ordered `xs`, assuming `xs` is strictly increasing.
///
/// Equivalent to upstream JpmcdsBSearchDoubleFast / JpmcdsBSearchLongFast with
/// skip == sizeof(T). Returns nullopt where upstream returns FAILURE.
///
/// Deliberately preserved upstream behaviours:
///   - extrapolation: desired <= xs.front() yields {0, 1}
///   - extrapolation: desired >= xs.back()  yields {n-2, n-1}
///   - single element yields {0, 0}
///   - a run of equal values advances hi past the run
///
/// ponytail: upstream switched on skip to replace an index multiply with a
/// shift. A contiguous std::span indexes directly, so that whole branch is
/// gone -- the compiler does better than the 2009 hand-optimisation.
template <std::totally_ordered T>
[[nodiscard]] constexpr Expected<Bracket> bracket(std::span<const T> xs, double desired) noexcept {
  const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(xs.size());
  if (n < 1) {
    return fail(ErrorKind::bad_argument, "a bracket needs at least 1 point, got " + detail::num(n));
  }
  if (n == 1) {
    return Bracket{0, 0};
  }
  if (desired <= static_cast<double>(xs.front())) {
    return Bracket{0, 1};
  }
  if (desired >= static_cast<double>(xs.back())) {
    return Bracket{static_cast<std::size_t>(n - 2), static_cast<std::size_t>(n - 1)};
  }

  // Signed indices on purpose: upstream lets `hi` go to mid-1 and relies on
  // int arithmetic. With std::size_t that underflows to a huge value and the
  // next xs[mid] is an out-of-bounds read.
  std::ptrdiff_t lo = 0;
  std::ptrdiff_t hi = n - 2;
  std::ptrdiff_t mid = 0;

  // The counter is upstream's guard against non-monotone input sending the
  // search into a cycle. Exhausting it means the precondition was violated.
  for (std::ptrdiff_t guard = n + 1; guard > 0; --guard) {
    mid = (hi + lo) / 2;
    if (mid < 0 || mid + 1 >= n) {
      return fail(ErrorKind::bad_argument, "the computed bracket index is outside the array");
    }
    if (desired < static_cast<double>(xs[static_cast<std::size_t>(mid)])) {
      hi = mid - 1;
    } else if (desired > static_cast<double>(xs[static_cast<std::size_t>(mid + 1)])) {
      lo = mid + 1;
    } else {
      // Walk past any run of identical values, as upstream does. Upstream
      // argues it cannot run off the end because xs.back() != desired; the
      // bound here makes that structural instead of an argument in a comment.
      std::ptrdiff_t end = mid + 1;
      while (end + 1 < n &&
             xs[static_cast<std::size_t>(mid)] == xs[static_cast<std::size_t>(end)]) {
        ++end;
      }
      return Bracket{static_cast<std::size_t>(mid), static_cast<std::size_t>(end)};
    }
  }
  return fail(ErrorKind::bad_argument, "no bracket found; the domain array may not be ordered");
}

/// Bracket `desired`, verifying the ordering precondition first.
///
/// Equivalent to upstream JpmcdsBSearchDouble / JpmcdsBSearchLong. Upstream
/// chose between checked and unchecked via a mutable global flag
/// (bSearchCheckOrder_g, toggled by JpmcdsBSearchCheckOrder) -- process-wide
/// state that silently changed the behaviour of every caller. Here the caller
/// picks the function and the global is gone.
///
/// So `JpmcdsBSearchCheckOrder` has no port and never will: it existed only to
/// toggle that global, and a function whose entire purpose is to mutate
/// process-wide state has nothing to replace it with. Recorded here as a
/// deliberate non-port rather than left as an omission -- see UPSTREAM-13,
/// which is the defect that global caused.
template <std::totally_ordered T>
[[nodiscard]] constexpr Expected<Bracket> bracket_checked(std::span<const T> xs,
                                                          double desired) noexcept {
  if (!is_strictly_increasing(xs)) {
    return fail(ErrorKind::bad_argument, "the domain array is not strictly increasing");
  }
  return bracket(xs, desired);
}

} // namespace cdsmodel
