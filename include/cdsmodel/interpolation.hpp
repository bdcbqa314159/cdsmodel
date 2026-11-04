#pragma once

// Port of upstream lintrp1.c + lintrp1.inc (the template, expanded once for
// long abscissae) plus the three x-axis metric functions from yearfrac.c.
//
// The second preprocessor-as-template in the distribution after bsearch.inc,
// and like that one it becomes an ordinary function here -- upstream
// instantiates it exactly once, for `long`, so there is nothing to
// generalise over and a template would be speculative.
//
// Two structural notes.
//
// Upstream threads byte STRIDES through every routine -- `xskip`, `fskip`,
// `dskip`, `oskip` -- so that it can walk the interleaved `TRatePt` array of
// a TCurve as two logical arrays. Spans replace that: a caller with
// interleaved data gathers first. The strides are the reason upstream's x and
// f arrays are `char *` internally, casting back on every access.
//
// UPSTREAM-13 lives in the discontinuous routine and is the reason the port's
// `bsearch.hpp` has no order-checking global. See
// `linear_interpolate_discontinuous`.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// How distance along the x axis is measured before interpolating.
///
/// Upstream passes a `TMetricDoubleFunc` function pointer, with NULL meaning
/// "use the default", and the two forms are ALMOST the same thing. The null
/// branch computes `(desired - x_lo) / (x_hi - x_lo)`; the metric branch
/// computes `metric(desired, x_lo) / metric(x_hi, x_lo)`, and
/// `actualDaysDiff(a, b)` is `b - a`. Numerator and denominator are both
/// negated, negation is exact, so the quotient is bit-identical.
///
/// Except when the denominator is ZERO, because `-0.0` is not `0.0`. A
/// zero-width bracket -- reachable in the discontinuous routine when a
/// repeated abscissa sits at the end of the curve and the search extrapolates
/// -- gives `100/0 = +inf` down the null path and `-100/0 = -inf` down the
/// metric path. Measured on `x = {100, 200, 300, 300}`:
///
/// ```
/// desired    mfunc=NULL     mfunc=ACT/365F agree?
/// 400        inf            -inf           *** NO ***
/// ```
///
/// Both are infinities at a point where the function is genuinely undefined,
/// so neither is wrong and this is recorded as a quirk rather than a defect.
/// But they are not interchangeable, and both forms are used in the library --
/// `interpc.c` passes NULL, `linterpc.c` passes a day-count metric -- so the
/// port keeps them distinct. The first draft of this header collapsed them and
/// the differential suite caught it immediately.
enum class Metric {
  /// Upstream's `mfunc == NULL`: the fraction computed directly as
  /// `(desired - x_lo) / (x_hi - x_lo)`. The default, as it is upstream.
  Direct,
  /// `actualDaysDiff`: a plain difference, evaluated in the metric-function
  /// argument order. Selected by `JpmcdsDayCountToMetricFunc` for ACT/365F,
  /// ACT/365 and ACT/360 -- all three, since a scale factor cancels in the
  /// ratio. Differs from `Direct` only on a zero-width bracket.
  ActualDays,
  /// `bondDaysDiff`: 30/360 day difference.
  Bond30_360,
  /// `bondEDaysDiff`: 30E/360 day difference.
  Bond30E_360,
};

/// The metric a day count convention selects. Replaces
/// `JpmcdsDayCountToMetricFunc`.
///
/// Returns nullopt for the conventions upstream rejects, which is every one
/// outside the five listed. Note that the three ACT variants all map to the
/// same metric: the ratio is scale-invariant, so dividing by 360 or 365 would
/// cancel.
[[nodiscard]] constexpr std::optional<Metric> metric_for_day_count(DayCount dcc) noexcept {
  switch (dcc) {
  case DayCount::Act365Fixed:
  case DayCount::Act365:
  case DayCount::Act360:
    return Metric::ActualDays;
  case DayCount::Thirty360:
    return Metric::Bond30_360;
  case DayCount::ThirtyE360:
    return Metric::Bond30E_360;
  case DayCount::EffectiveRate:
    return std::nullopt; // upstream: "Day count convention unknown"
  }
  return std::nullopt;
}

/// Distance from `from` to `to` under a metric. Argument order follows
/// upstream's helpers, where `actualDaysDiff(start, end)` is `end - start`.
///
/// ## UPSTREAM-14 -- the bond metrics return stack garbage before the epoch
///
/// Upstream's helpers discard the status of `JpmcdsDaysDiff` over an
/// uninitialised local:
///
/// ```c
/// static double bondDaysDiff(double startDate, double endDate)
/// {
///     long days;                                        /* uninitialised */
///     (void)JpmcdsDaysDiff((TDate)startDate, (TDate)endDate,
///                          JPMCDS_B30_360, &days);
///     return ((double)days);                            /* garbage if it failed */
/// }
/// ```
///
/// An earlier draft of this comment said the call "cannot fail: the
/// convention is a compile-time constant and both are calculable". That was
/// wrong, and the golden replay disproved it within the hour.
/// `JpmcdsDaysDiff` fails for a TDate before the 1601 epoch and does NOT
/// write its out parameter, so the metric returns whatever was on the stack.
/// Measured, and the values are NOT stable -- which is the point. Two
/// different binaries built from the same sources gave
/// `metric(-50, 100) = 6101050560` and `= 4`, and interpolating at -50 on a
/// curve running 0.01 to 0.02 gave **-667794.1351** and **-619018.2639**.
/// Both reported SUCCESS. Whatever is in that stack slot is what you get, so
/// quoting a single figure would misrepresent it.
///
/// It is an uninitialised read, so AddressSanitizer cannot see it --
/// MemorySanitizer would. And it was the generator's full cross-product of
/// abscissae against metrics that found it, not the differential sweeps,
/// which used realistic dates for each metric.
///
/// The port checks the status it is given, so a failure becomes nullopt. In
/// practice the branch rarely fires: `std::chrono` has no 1601 epoch, so a
/// pre-epoch abscissa is just a date in 1600 and the 30/360 difference is
/// well defined. The port therefore returns a sensible extrapolation where
/// upstream returns garbage -- a divergence recorded with the defect.
[[nodiscard]] inline std::optional<double> metric_distance(Metric metric, double from,
                                                           double to) noexcept {
  if (metric == Metric::Direct || metric == Metric::ActualDays) {
    return to - from;
  }
  // Upstream casts the doubles straight to TDate. Reject what is not
  // representable instead of truncating silently.
  if (!is_representable_tdate(static_cast<TDate>(from)) ||
      !is_representable_tdate(static_cast<TDate>(to))) {
    return std::nullopt;
  }
  const DayCount dcc = metric == Metric::Bond30_360 ? DayCount::Thirty360 : DayCount::ThirtyE360;
  const std::optional<long> days =
      days_diff(from_tdate(static_cast<TDate>(from)), from_tdate(static_cast<TDate>(to)), dcc);
  if (!days) {
    return std::nullopt;
  }
  return static_cast<double>(*days);
}

namespace detail {

/// The shared tail of all three routines: the fraction of the way from `lo`
/// to `hi` that `desired` sits, under `metric`.
///
/// Expression order preserved: upstream computes
/// `metric(desired, x_lo) / metric(x_hi, x_lo)` and then
/// `f_lo + fraction * (f_hi - f_lo)`.
[[nodiscard]] inline std::optional<double>
interpolate_between(double desired, long x_lo, long x_hi, double f_lo, double f_hi, Metric metric) {
  double fraction = 0.0;
  if (metric == Metric::Direct) {
    // Upstream's null branch, in its own order. NOT the same as routing
    // ActualDays through the metric form: see the note on `Metric`.
    fraction = (desired - static_cast<double>(x_lo)) / static_cast<double>(x_hi - x_lo);
  } else {
    const std::optional<double> numerator =
        metric_distance(metric, desired, static_cast<double>(x_lo));
    const std::optional<double> denominator =
        metric_distance(metric, static_cast<double>(x_hi), static_cast<double>(x_lo));
    if (!numerator || !denominator) {
      return std::nullopt;
    }
    fraction = *numerator / *denominator;
  }
  return f_lo + fraction * (f_hi - f_lo);
}

} // namespace detail

/// Linear interpolation at one point. Replaces `JpmcdsLinInterpLongPoint1`.
///
/// `xs` must be strictly increasing and the same length as `fs`. Extrapolates
/// beyond either end, because the bracketing search does: below the first
/// point it brackets with the first pair, above the last with the last pair.
/// That is upstream's behaviour and callers who need clamping do it
/// themselves -- `JpmcdsLinInterpCurve` is the one in-library example, and it
/// clamps its inputs before calling.
[[nodiscard]] inline std::optional<double> linear_interpolate(std::span<const long> xs,
                                                              std::span<const double> fs,
                                                              double desired,
                                                              Metric metric = Metric::Direct) {
  if (xs.size() != fs.size()) {
    return std::nullopt;
  }
  const std::optional<Bracket> at = bracket_checked(xs, desired);
  if (!at) {
    return std::nullopt;
  }
  if (at->lo == at->hi) {
    return fs[static_cast<std::size_t>(at->lo)]; // a single-point curve
  }
  return detail::interpolate_between(
      desired, xs[static_cast<std::size_t>(at->lo)], xs[static_cast<std::size_t>(at->hi)],
      fs[static_cast<std::size_t>(at->lo)], fs[static_cast<std::size_t>(at->hi)], metric);
}

/// Linear interpolation at many points. Replaces
/// `JpmcdsLinInterpLongArray1`, which is a loop over the point routine and
/// nothing more.
///
/// The desired abscissae need not be ordered.
[[nodiscard]] inline std::optional<std::vector<double>>
linear_interpolate_many(std::span<const long> xs, std::span<const double> fs,
                        std::span<const long> desired, Metric metric = Metric::Direct) {
  std::vector<double> out;
  out.reserve(desired.size());
  for (long x : desired) {
    const std::optional<double> value = linear_interpolate(xs, fs, static_cast<double>(x), metric);
    if (!value) {
      return std::nullopt;
    }
    out.push_back(*value);
  }
  return out;
}

/// Which side of a discontinuity to take when `desired` lands exactly on a
/// repeated abscissa.
enum class Limit { Left, Right };

/// Linear interpolation over a curve that may contain discontinuities.
/// Replaces `JpmcdsLinInterpLongPoint1Discontinuous`.
///
/// A discontinuity is expressed by REPEATING an x value with different f
/// values, so `xs` here must be non-decreasing rather than strictly
/// increasing. Landing exactly on a repeated x takes the left or right limit
/// per `limit` -- except where three or more entries share the x value, when
/// upstream takes the MIDDLE one. That is preserved; it is odd, and it is
/// observable.
///
/// ## UPSTREAM-13 -- a leaked global that disables validation process-wide
///
/// Upstream cannot express "search without checking order" per call, because
/// that choice lives in a mutable global: `bSearchCheckOrder_g`, toggled by
/// `JpmcdsBSearchCheckOrder` and consulted by both `JpmcdsBSearchDouble` and
/// `JpmcdsBSearchLong`. So this routine clears it, does its own
/// non-decreasing check, searches, and restores it:
///
/// ```c
/// checkOrder = JpmcdsBSearchCheckOrder(FALSE);
/// if (checkOrder) { ... if (out of order) return FAILURE; ... }   /* leak */
/// if (SEARCH_ROUTINE_NAME(...) == FAILURE) goto done;             /* leak */
/// JpmcdsBSearchCheckOrder(checkOrder);                            /* only here */
/// ```
///
/// Both error paths leave before the restore. One failed call therefore
/// disables order validation for **every** binary search in the process, for
/// the rest of its life -- including inside the zero curve and the whole CDS
/// pricing path. Measured:
///
/// ```
/// baseline:                      JpmcdsBSearchLong(unordered) -> FAILURE (validated)
/// one failed call ...            -> FAILURE
/// after that single failed call: JpmcdsBSearchLong(unordered) -> SUCCESS
/// current global state:          FALSE (leaked)
/// ```
///
/// Severity is medium rather than high, and the reason is reachability rather
/// than consequence. The function has no caller inside the library and no
/// declaration in any public header -- though it IS an exported symbol and it
/// IS in the XLL's surface, so a spreadsheet can reach it. Once tripped the
/// damage is process-wide, but producing a wrong number then needs a later
/// unsorted array, and curves built through `JpmcdsMakeTCurve` are validated
/// on construction.
///
/// The port cannot express it. `bsearch.hpp` offers `bracket` and
/// `bracket_checked` as two functions and has no global at all -- a choice
/// made on taste in the first slice of this port, whose comment then read
/// "process-wide state that silently changed the behaviour of every caller".
/// UPSTREAM-13 is that sentence turning out to be literally true.
[[nodiscard]] inline std::optional<double>
linear_interpolate_discontinuous(std::span<const long> xs, std::span<const double> fs,
                                 double desired, Limit limit, Metric metric = Metric::Direct) {
  if (xs.size() != fs.size() || xs.empty()) {
    return std::nullopt;
  }
  // Non-decreasing, not strictly increasing: equal neighbours ARE the
  // discontinuities. Upstream's own loop tests `x[i] > x[i+1]`.
  for (std::size_t i = 0; i + 1 < xs.size(); ++i) {
    if (xs[i] > xs[i + 1]) {
      return std::nullopt;
    }
  }
  // Unchecked search, because repeated abscissae are legal here. Upstream
  // reaches the same place by clearing a global; here it is a different
  // function.
  const std::optional<Bracket> at = bracket(xs, desired);
  if (!at) {
    return std::nullopt;
  }

  auto lo = static_cast<std::size_t>(at->lo);
  auto hi = static_cast<std::size_t>(at->hi);
  if (at->lo == at->hi) {
    return fs[lo]; // a single-point curve
  }
  if (desired != static_cast<double>(xs[lo])) {
    return detail::interpolate_between(desired, xs[lo], xs[hi], fs[lo], fs[hi], metric);
  }

  // Landed exactly on an abscissa. Widen to the full run of equal values,
  // ignoring the `hi` the search produced.
  while (lo > 0 && xs[lo] == xs[lo - 1]) {
    --lo;
  }
  for (hi = lo; hi + 1 < xs.size() && xs[hi + 1] == xs[lo];) {
    ++hi;
  }
  if (hi - lo >= 2) {
    return fs[(hi + lo) / 2]; // three or more share the x value: take the middle
  }
  return limit == Limit::Right ? fs[hi] : fs[lo];
}

} // namespace cdsmodel
