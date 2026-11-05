#pragma once

// Port of upstream interpc.c -- interpolating a rate off a curve under a
// chosen interpolation type, honouring the curve's OWN basis and day count.
//
// Not to be confused with `ZeroCurve::zero_rate`, which replaces
// `JpmcdsZeroRate` and hardcodes continuous compounding on ACT/365F. These are
// different functions and I checked that before writing either: this one
// reads `curve->fBasis` and `curve->fDayCountConv` and dispatches on an
// `interpType` argument. `JpmcdsDiscountDate` in tcurve.c -- the reason this
// file is being ported now -- needs this one, not the other.
//
// Two exported functions and three statics:
//
//     JpmcdsInterpRate         -> interp_rate        (a TDate wrapper)
//     JpmcdsInterpRateExact    -> interp_rate_exact  (a double date)
//     InterpRateFlatFwds       -> detail, reachable
//     InterpRateDiscountFactors-> detail, NOT reachable -- see below
//     InterpRateFind           -> bsearch's bracket
//
// ## What the port cannot reach, and why that is not an omission
//
// `ZeroCurve::create` refuses a basis outside {continuous} union [1, 365] and
// a day count other than ACT/365F or ACT/360. That was a deliberate decision
// three modules ago -- UPSTREAM-15, where upstream's own validating
// constructor accepted curves that `JpmcdsZeroPrice` then priced as NaN.
//
// Two consequences here, both of which remove code rather than leave it
// untested:
//
//   * `InterpRateDiscountFactors` exists for `fBasis == JPMCDS_DISCOUNT_FACTOR`
//     (-2), which the port cannot construct. It has no port at all.
//   * `InterpRateFlatFwds` branches on five day counts; three of them
//     (30/360, 30E/360, ACT/365) are unreachable for the same reason, so the
//     port carries only the ACT/360 and ACT/365F arms.
//
// That is worth stating explicitly because "the port is missing a branch" and
// "the port cannot represent the input that reaches it" look identical in a
// diff. The differential suite asserts the second: the curves those branches
// need are refused at construction, on both sides for the day counts and on
// the port's side alone for the basis.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace cdsmodel {

namespace detail {

/// Flat-forward interpolation on a curve in its own terms. Replaces the
/// file-local `InterpRateFlatFwds`.
///
/// The shape is: convert both bracketing rates to discount factors, raise
/// their ratio to the fraction of the interval covered, convert back. Which
/// is the same idea as `ZeroCurveBuilder::interpolate`'s flat-forwards arm and
/// a genuinely independent implementation of it -- a thing I noted two slices
/// ago as worth comparing directly, and the comparison is now a test.
///
/// Three early returns before any arithmetic, all reproduced:
///
///   * an exact hit on the lower bracket, OR a lower bracket date that is
///     already past the desired date, returns the lower rate. The second half
///     is how "do not extrapolate backwards" is implemented down here, in
///     addition to the clamp in the caller.
///   * an exact hit on the upper bracket returns the upper rate.
///   * **the base date falling between the brackets** returns one side or the
///     other, flat. Upstream's comment explains why: *"This ensures that
///     basedate will be associated with a 'kink' in the forwards, and
///     discount(base) == 1.0"*. Without it the interpolated discount factor at
///     the base date would not be exactly 1, and every price would inherit
///     that error.
///
/// `desired` is a double because upstream's exact variant admits fractional
/// dates for tree models. For ACT/360 and ACT/365F the day-count difference is
/// a plain subtraction, so a fraction is meaningful; the other conventions
/// reject it, and those conventions cannot reach here anyway.
[[nodiscard]] inline std::optional<double> interp_flat_forwards(const ZeroCurve &curve,
                                                                double desired) {
  const std::span<const RatePoint> points = curve.points();
  std::vector<TDate> dates;
  dates.reserve(points.size());
  for (const RatePoint &point : points) {
    dates.push_back(to_tdate(point.date));
  }
  const std::optional<Bracket> at = bracket(std::span<const TDate>{dates}, desired);
  if (!at) {
    return std::nullopt;
  }
  const TDate date_lo = dates[at->lo];
  const TDate date_hi = dates[at->hi];
  const double rate_lo = points[at->lo].rate;
  const double rate_hi = points[at->hi].rate;

  if (static_cast<double>(date_lo) == desired || static_cast<double>(date_lo) > desired) {
    return rate_lo;
  }
  if (static_cast<double>(date_hi) == desired) {
    return rate_hi;
  }
  // The base date between the brackets: flat on each side of it, so that the
  // discount factor at the base date is exactly 1.
  const TDate base = to_tdate(curve.base_date());
  if (date_lo <= base && base <= date_hi) {
    return desired < static_cast<double>(base) ? rate_lo : rate_hi;
  }

  const std::optional<double> disc_lo =
      rate_to_discount(rate_lo, curve.base_date(), from_tdate(date_lo), curve.day_count(),
                       static_cast<long>(curve.basis()));
  const std::optional<double> disc_hi =
      rate_to_discount(rate_hi, curve.base_date(), from_tdate(date_hi), curve.day_count(),
                       static_cast<long>(curve.basis()));
  if (!disc_lo || !disc_hi || *disc_lo == 0.0) {
    return std::nullopt;
  }

  // ACT/360 and ACT/365F measure the gap as a plain date difference, which is
  // what makes a fractional date meaningful. The three conventions that need
  // JpmcdsDaysDiff cannot reach here -- ZeroCurve::create refuses them.
  const double span = static_cast<double>(date_hi - date_lo);
  const double covered = desired - static_cast<double>(date_lo);
  if (detail::is_almost_zero(span) || span < 0.0) {
    return std::nullopt;
  }

  const double disc_desired = *disc_lo * std::pow(*disc_hi / *disc_lo, covered / span);

  const TDate desired_tdate = static_cast<TDate>(desired);
  const bool fractional = !detail::is_almost_zero(desired - static_cast<double>(desired_tdate));
  if (fractional) {
    // Upstream computes the year fraction by hand here rather than from two
    // dates, because one end is not a date.
    const double denominator = curve.day_count() == DayCount::Act360 ? 360.0 : 365.0;
    const double year_frac =
        (desired - static_cast<double>(to_tdate(curve.base_date()))) / denominator;
    return discount_to_rate_year_frac(disc_desired, year_frac, curve.basis());
  }
  return discount_to_rate(disc_desired, curve.base_date(), from_tdate(desired_tdate),
                          curve.day_count(), static_cast<long>(curve.basis()));
}

} // namespace detail

/// Interpolate a rate off a curve at a possibly-fractional date. Replaces
/// `JpmcdsInterpRateExact`.
///
/// ## UPSTREAM-28 -- a process-wide flag left disabled on the success paths
///
/// Upstream opens by disabling the binary search's order checking, saving the
/// previous value to restore later:
///
/// ```c
/// /*
/// ** Since we are using the general interpolation functions, but we
/// ** know that the TCurve should have been constructed properly,
/// ** we will turn off order checking in the binary search routines.
/// **
/// ** This might well produce a dramatic improvement in all the ALIB
/// ** zero curve analytics!
/// */
/// TBoolean checkOrder = JpmcdsBSearchCheckOrder (FALSE);
/// ```
///
/// and restores it at the `done:` label:
///
/// ```c
/// done:
///     /* Re-set the global check order flag. */
///     JpmcdsBSearchCheckOrder (checkOrder);
/// ```
///
/// Between those two points sit **two `return SUCCESS` statements** -- the
/// clamps for a date past the end of the curve and a date before its start:
///
/// ```c
/// if (desiredDate > curve->fArray[curveLen-1].fDate)
/// {
///     *rate = curve->fArray[curveLen-1].fRate;
///     return SUCCESS;                       /* bypasses done: */
/// }
/// else if (desiredDate < curve->fArray[0].fDate)
/// {
///     *rate = curve->fArray[0].fRate;
///     return SUCCESS;                       /* bypasses done: */
/// }
/// ```
///
/// So every extrapolation request leaves order checking **off, process-wide
/// and permanently**. Measured:
///
/// ```
/// after calling with...                        order checking
/// (initial state)                              ON
/// a date inside the curve                      ON
/// a date AFTER the curve (extrapolation)       OFF  <- LEAKED
/// a date BEFORE the curve (extrapolation)      OFF  <- LEAKED
/// an empty curve (an error path)               ON
/// ```
///
/// This is UPSTREAM-13's global and UPSTREAM-13's failure mode, in a different
/// function and with the reachability inverted. There the leak was on two
/// **error** paths in `lintrp1.inc`; here it is on two **success** paths, and
/// extrapolating off the end of a curve is ordinary use rather than a fault.
/// Note the last row above: the genuine error path restores correctly, which
/// is the opposite of U13.
///
/// And the consequence is demonstrable in two calls:
///
/// ```
/// order checking ON : JpmcdsBSearchDouble(unsorted) -> FAILURE
/// after one extrapolation: same call               -> SUCCESS  <- no longer validated
/// ```
///
/// An unsorted array that the checked search correctly rejected now passes.
/// The precondition those functions exist to verify is simply no longer
/// verified, anywhere in the process, for the rest of its life.
///
/// **Cannot occur in the port**, and for a structural reason rather than a
/// careful one: there is no global. `bracket` and `bracket_checked` are
/// separate functions and the caller picks, which is the decision recorded in
/// bsearch.hpp when UPSTREAM-13 was filed. A flag that does not exist cannot
/// be left in the wrong state, so this is the second defect that one decision
/// has pre-empted.
///
/// ## The clamps themselves
///
/// Both are reproduced. A date beyond the last point returns the last rate and
/// a date before the first returns the first -- flat extrapolation in both
/// directions, "regardless of what the interpolation type is". The guard is
/// skipped entirely for a discount-factor curve, which the port cannot build.
[[nodiscard]] inline std::optional<double> interp_rate_exact(const ZeroCurve &curve, double desired,
                                                             InterpType interp) {
  const std::span<const RatePoint> points = curve.points();
  if (points.empty()) {
    return std::nullopt; // unreachable: ZeroCurve::create refuses an empty curve
  }

  // Flat extrapolation at both ends, whatever the interpolation type.
  if (desired > static_cast<double>(to_tdate(points.back().date))) {
    return points.back().rate;
  }
  if (desired < static_cast<double>(to_tdate(points.front().date))) {
    return points.front().rate;
  }

  switch (interp) {
  case InterpType::FlatForwards:
    return detail::interp_flat_forwards(curve, desired);
  case InterpType::Linear: {
    std::vector<long> dates;
    std::vector<double> rates;
    dates.reserve(points.size());
    rates.reserve(points.size());
    for (const RatePoint &point : points) {
      dates.push_back(static_cast<long>(to_tdate(point.date)));
      rates.push_back(point.rate);
    }
    // Upstream passes a NULL metric function, which interpolation.hpp calls
    // Metric::Direct and keeps distinct from ActualDays -- see UPSTREAM-13's
    // neighbour, where the two differ on a zero-width bracket.
    return linear_interpolate(dates, rates, desired, Metric::Direct);
  }
  }
  return std::nullopt;
}

/// `interp_rate_exact` at a whole date. Replaces `JpmcdsInterpRate`, which is
/// a one-line cast wrapper around the exact form.
[[nodiscard]] inline std::optional<double> interp_rate(const ZeroCurve &curve, Date date,
                                                       InterpType interp) {
  return interp_rate_exact(curve, static_cast<double>(to_tdate(date)), interp);
}

} // namespace cdsmodel
