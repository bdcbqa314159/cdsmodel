#pragma once

// Port of upstream cxbsearch.c -- locating a value in a sorted array and
// reporting the whole run of equal elements. One function.
//
//   JpmcdsBinarySearchLong -> match_range
//
// Distinct from bsearch.hpp (`bsearch.inc`), which brackets a value between
// two indices for interpolation. This one answers a different question: how
// many copies of the value are there, and where do they start. The two in-
// library callers use it to find today's date in a schedule, where a repeated
// date is the case that matters.
//
// ## Three outputs become one range
//
// Upstream returns `exact`, `loBound` and `hiBound` through pointers, with a
// documented invariant tying them together:
//
//     ** The idea behind these return values is that hiBound-loBound-1 should
//     ** give the number of exact matches in all cases, and that everything in
//     ** the range [loBound+1, hiBound-1] inclusive should be an exact match.
//
// That invariant is the whole answer, so the port returns it directly: `first`
// is `loBound + 1` and `count` is `hiBound - loBound - 1`. The arithmetic
// becomes the type's rather than something each caller re-derives, and
// `hiBound - loBound - 1 == count` stops being a documented promise and
// becomes a tautology.
//
// The sentinels go with it. Upstream's `loBound == -1` and
// `hiBound == arraySize` are not "absent" -- they are one position past each
// end, and they are load-bearing for the subtraction. Expressed as a half-open
// range there is nothing to signal.
//
// ## Deliberate difference: UPSTREAM-37
//
// The main path guards both optional outputs against NULL and the three
// early-return paths do not. See the note on `match_range`.
//
// ## Two documented facts, one of them wrong
//
// The parameter comment says of `hiBound`:
//
//     long   *hiBound)   /* (O) Index of first value strictly greater than xDesired,
//                           -1 if no such value exists. */
//
// while the function comment, 20 lines above, says
//
//     ** is returned for exact and loBound, and arraySize for hiBound.
//
// The code returns `arraySize`. So the parameter comment is simply wrong, and
// the two comments contradict each other within one file. Measured: seeking 99
// in `{10,20,20,30,40}` gives `hiBound == 5`, not −1. The port has no such
// value to be wrong about.
//
// ## `exact` is not the first match
//
// Upstream documents `exact` as "the index of A value which is equal", and the
// indefinite article is doing real work: the code picks whichever of the two
// bracketing indices matches, which need not be the first of a run. MEASURED,
// because it is the kind of thing that reads as pedantry until it is checked:
//
//     {10,10,20,30} seek 10  ->  exact == 1, while the first match is 0
//
// `match_range` returns the first, which is a stronger guarantee and one a
// caller can actually rely on. The differential test therefore asserts
// upstream's `exact` lies within the port's range rather than equalling its
// start -- comparing them directly would be asserting something upstream never
// promised.
//
// ## The typo
//
//     JpmcdsErrMsg("%s: x array not in increasing order.n", routine);
//
// `.n` for `.\n`. Not filed: a log line that does not end is a cosmetic defect
// with no behavioural consequence, and this register is for things that change
// an answer or touch memory. Recorded here because somebody reading the two
// files side by side will notice, and the reason for the omission should be
// written down rather than inferred.

#include <cstddef>
#include <optional>
#include <span>

namespace cdsmodel {

/// The run of elements equal to a sought value, as a half-open range.
///
/// `count == 0` means no match, and `first` is then where one would be
/// inserted -- so `first` is meaningful either way and needs no sentinel.
struct MatchRange {
  std::size_t first = 0;
  std::size_t count = 0;

  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }

  /// The index of the first equal element, if there is one.
  [[nodiscard]] constexpr std::optional<std::size_t> exact() const noexcept {
    return count > 0 ? std::optional<std::size_t>{first} : std::nullopt;
  }

  friend constexpr bool operator==(const MatchRange &, const MatchRange &) = default;
};

/// Replaces `JpmcdsBinarySearchLong`.
///
/// `xs` must be sorted ascending. Upstream says so and deliberately does not
/// check -- *"this would in fact defeat the purpose of having a fast binary
/// search if you are going to slow it down by doing a linear check of the
/// inputs"* -- and returns `FAILURE` only when its iteration cap runs out,
/// which is how an unsorted array announces itself. Reproduced: no linear
/// precondition check, and nullopt where the cap expires.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-37. Upstream's main path carefully guards
/// its two optional outputs:
///
/// ```c
/// if (loBound != NULL) { ... }
/// if (hiBound != NULL) { ... }
/// ```
///
/// which tells a caller that NULL is a supported argument -- the guard IS the
/// documentation. The three early-return paths then write through them
/// unconditionally:
///
/// ```c
/// if (xDesired < ELEMENT(0))
/// {
///     *exact   = -1;
///     *loBound = -1;      /* no guard */
///     *hiBound = 0;       /* no guard */
///     return SUCCESS;
/// }
/// ```
///
/// so a value outside the array's range, or an array of one element, stores
/// through NULL. Confirmed: UBSan reports "store to null pointer of type
/// 'long'" at cxbsearch.c:84, ASan a SEGV on address 0, and the process
/// aborts. Both in-library callers pass non-NULL for all three, so it is
/// reachable only through the public header -- which is also the only place
/// anyone would read the guard and conclude NULL was allowed.
///
/// This one is not a missing check. It is an INCONSISTENT check, forty lines
/// apart in one function, and that is worse: a missing check is a precondition
/// the caller can be told about, while a check on one path and not another is
/// an invitation the code does not honour.
///
/// Returning a value removes the question. There are no output pointers, so
/// there is nothing to guard on any path.
template <typename T, typename R, typename Proj>
[[nodiscard]] constexpr std::optional<MatchRange> match_range(T desired, std::span<const R> items,
                                                              Proj key) noexcept {
  if (items.empty()) {
    return std::nullopt; // upstream's REQUIRE(arraySize > 0)
  }
  const std::size_t size = items.size();
  const auto at = [&](std::size_t i) { return key(items[i]); };

  // Out of range in either direction: no match, and the insertion point is an
  // end. Upstream's two early returns, which are where it forgets its guards.
  if (desired < at(0)) {
    return MatchRange{0, 0};
  }
  if (at(size - 1) < desired) {
    return MatchRange{size, 0};
  }
  if (size == 1) {
    // Upstream asserts `xDesired == ELEMENT(0)` here, which the two checks
    // above guarantee -- and which vanishes under NDEBUG, so the assert is
    // documentation rather than protection.
    return MatchRange{0, 1};
  }

  // Upstream's bounded loop. The cap is `arraySize + 1` iterations, and
  // exhausting it is how an unsorted array is detected -- in principle. See
  // the header note: across an exhaustive sweep of short unsorted arrays it
  // never fired once.
  std::ptrdiff_t lo = 0;
  std::ptrdiff_t hi = static_cast<std::ptrdiff_t>(size) - 2;
  std::ptrdiff_t mid = 0;
  bool bracketed = false;
  for (std::size_t count = size + 1; count > 0; --count) {
    mid = (hi + lo) / 2;
    if (desired < at(static_cast<std::size_t>(mid))) {
      hi = mid - 1;
    } else if (at(static_cast<std::size_t>(mid + 1)) < desired) {
      lo = mid + 1;
    } else {
      bracketed = true;
      break;
    }
  }
  if (!bracketed) {
    return std::nullopt; // upstream: "x array not in increasing order"
  }

  // Walk out of the run of equal values in both directions. Upstream does this
  // only when the corresponding output pointer is non-NULL; here both are
  // always needed, because the range is the answer.
  std::ptrdiff_t below = mid;
  while (below >= 0 && !(at(static_cast<std::size_t>(below)) < desired)) {
    --below;
  }
  std::ptrdiff_t above = mid + 1;
  while (above < static_cast<std::ptrdiff_t>(size) &&
         !(desired < at(static_cast<std::size_t>(above)))) {
    ++above;
  }
  return MatchRange{static_cast<std::size_t>(below + 1),
                    static_cast<std::size_t>(above - below - 1)};
}

/// The contiguous case: the array holds the keys themselves.
///
/// Upstream reaches a field inside an array of structs with a byte `skip`
/// stride, which is how both its callers use it. The projection overload above
/// is that stride, expressed as a function of the element rather than as a
/// distance in bytes -- so a caller cannot get it wrong by passing the size of
/// the wrong type.
template <typename T>
[[nodiscard]] constexpr std::optional<MatchRange> match_range(T desired,
                                                              std::span<const T> xs) noexcept {
  return match_range(desired, xs, [](const T &x) { return x; });
}

} // namespace cdsmodel
