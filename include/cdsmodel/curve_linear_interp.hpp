#pragma once

// Port of upstream linterpc.c -- re-expressing a zero curve on a different set
// of dates by linear interpolation. One function.
//
//   JpmcdsLinInterpCurve -> linear_interp_curve
//
// Distinct from interp_curve.hpp (`interpc.c`), which interpolates a RATE out
// of a curve. This produces a whole new curve, on dates the caller chooses,
// and is the only place in the library that passes a day-count metric to the
// interpolator rather than NULL -- a case interpolation.hpp was written to
// support and that nothing had exercised until now.
//
// ## No caller inside the library
//
// `JpmcdsLinInterpCurve` appears exactly once outside its own file:
//
//     TCurveInterpFunc JpmcdsLinInterpCurve;      /* interp.h:65 */
//
// which is a declaration against a function-pointer typedef, not a call. So
// this is an exported function whose only possible callers are external ones --
// the same situation as `JpmcdsGenerateUnadjDates` (UPSTREAM-30), and it has
// the same consequence: the arguments nobody inside the library passes are the
// arguments nothing inside the library validates.
//
// ## Deliberate difference: UPSTREAM-36
//
// A zero-point input curve makes the clamping loop read `fArray[-1]` and
// `fArray[0]` through a NULL pointer. See the note below.
//
// ## The third annotated dead parameter
//
//     void *interpData                    /* (I) Interpolation data-not used */
//
// Annotated at the declaration this time, where `zerocurve.c`'s `fwdLength`
// was annotated at the call site. Five files now carry the parameters of
// features that were removed; two of them say so.
//
// ## Clamping, not extrapolating
//
// Upstream's comment is accurate and the behaviour is worth stating precisely:
//
//     /* Make sure that we don't extrapolate. Before the first date
//      * in oldCurve, we just use the first rate in oldCurve. After
//      * the last date in oldCurve, we just use the last rate in oldCurve.
//      */
//
// The clamp applies to the date the interpolation is PERFORMED at, while the
// date STORED in the new curve is the one the caller asked for. So a desired
// date beyond the old curve's end comes back carrying the old curve's final
// rate at its own date -- a flat extension, not an extrapolation. Two
// different dates per point, and conflating them would silently change the
// output curve's dates.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Replaces `JpmcdsLinInterpCurve`: a new curve carrying `old_curve`'s rates
/// interpolated onto `desired_dates`, in `old_curve`'s own basis and day
/// count.
///
/// The interpolation metric comes from the old curve's day count convention,
/// which is upstream's `JpmcdsDayCountToMetricFunc`. A convention outside the
/// five that function knows is refused, as upstream refuses it.
///
/// `interpData` is not a parameter: upstream's is annotated
/// `/* Interpolation data-not used */`.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-36. The clamping loop reads both ends of
/// the old curve with no check that it has any points:
///
/// ```c
/// modDates[idx] = MIN(desiredDates->fArray[idx],
///                     oldCurve->fArray[oldCurve->fNumItems-1].fDate);
/// modDates[idx] = MAX(modDates[idx],
///                     oldCurve->fArray[0].fDate);
/// ```
///
/// `JpmcdsNewTCurve(baseDate, 0, ...)` is accepted -- only a NEGATIVE count is
/// refused -- and gives `fNumItems == 0` with `fArray == NULL`, so those two
/// lines become `NULL[-1]` and `NULL[0]`. Confirmed: UBSan reports "applying
/// non-zero offset 18446744073709551600 to null pointer" at linterpc.c:65
/// (that is -16, one `TRatePt`), ASan a SEGV on `0xfffffffffffffff0`, and the
/// process aborts. `zerocurve.c` builds a zero-point curve as its first act,
/// so such a curve is not a hypothetical shape.
///
/// This is the FOURTH `fArray[fNumItems-1]`-without-a-guard in the register,
/// after UPSTREAM-21 (a read), UPSTREAM-29 (the schedule that enables one) and
/// UPSTREAM-32 (a read-modify-write). At four occurrences it stops being four
/// accidents and becomes an idiom the codebase trusts: the author of each site
/// knew the count was positive, and none of them said so in a way a compiler
/// or a reader could check.
///
/// The port cannot express the input: `ZeroCurve` is non-empty by
/// construction, so `points().front()` and `points().back()` are total.
[[nodiscard]] inline std::optional<ZeroCurve>
linear_interp_curve(std::span<const Date> desired_dates, const ZeroCurve &old_curve) {
  if (desired_dates.empty()) {
    // Upstream reaches NEW_ARRAY(TDate, 0), which is JpmcdsMallocSafe(0), and
    // that returns NULL by design -- so it refuses this too, as an allocation
    // error rather than as an empty request.
    return std::nullopt;
  }
  const std::optional<Metric> metric = metric_for_day_count(old_curve.day_count());
  if (!metric) {
    return std::nullopt;
  }

  const std::span<const RatePoint> points = old_curve.points();
  const Date first = points.front().date;
  const Date last = points.back().date;

  std::vector<long> xs;
  std::vector<double> fs;
  xs.reserve(points.size());
  fs.reserve(points.size());
  for (const RatePoint &point : points) {
    xs.push_back(to_tdate(point.date));
    fs.push_back(point.rate);
  }

  // The clamp is on the date interpolated AT; the date stored is the one asked
  // for. Keeping them in separate vectors makes that impossible to conflate.
  std::vector<long> clamped;
  clamped.reserve(desired_dates.size());
  for (const Date date : desired_dates) {
    clamped.push_back(to_tdate(std::clamp(date, first, last)));
  }

  const std::optional<std::vector<double>> rates =
      linear_interpolate_many(xs, fs, clamped, *metric);
  if (!rates) {
    return std::nullopt;
  }

  std::vector<RatePoint> result;
  result.reserve(desired_dates.size());
  for (std::size_t i = 0; i < desired_dates.size(); ++i) {
    result.push_back(RatePoint{desired_dates[i], (*rates)[i]});
  }
  return ZeroCurve::create(old_curve.base_date(), result, old_curve.basis(), old_curve.day_count());
}

} // namespace cdsmodel
