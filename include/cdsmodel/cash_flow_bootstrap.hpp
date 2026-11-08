#pragma once

// Port of upstream zcall.c -- the cash-flow third: `JpmcdsZCAddCashFlowList`,
// `JpmcdsZCAddCFLPoints`, `JpmcdsJpmcdsZCPresentValueCFL` and the static
// `JpmcdsObjFunctionRate` they share.
//
// This is where a curve stops being a container and becomes a bootstrap: an
// instrument's price goes in, and a zero rate that reproduces it comes out.
// Everything before this slice could be checked against a formula; this has to
// be checked against a fixed point -- price the instrument off the curve you
// built from it and you must get the price back.
//
// Separate header rather than more methods on `ZeroCurveBuilder`, because the
// dependency runs one way: bootstrapping needs curves AND cash flows, while
// neither of those needs bootstrapping.
//
// `JpmcdsObjFunctionRate` is a file-local function plus a 10-field
// `TObjectiveData` struct that exists only to carry its arguments past a
// `void*`. Six of those ten fields are set to NULL or 0.0 at this call site
// with the comment "ditto" -- they belong to the OTHER caller of the same
// objective type, in zcswap.c. A lambda captures what it needs and the struct
// has nothing left to describe.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Upstream's bootstrap bounds and solver tuning, from the top of zcall.c.
///
/// `kRateXTolerance` is `DBL_MAX` upstream, which is not a tolerance but a way
/// of switching the x-test off: convergence is decided by the f-tolerance
/// alone. Kept, because it decides how many digits of the answer mean
/// anything -- a root accepted on `|f| < 1e-10` is pinned to the precision of
/// the price, not of the rate.
inline constexpr double kMinZeroRate = -10000.00;
inline constexpr double kMaxZeroRate = 10000.00;
inline constexpr int kMaxBootstrapIterations = 50;
inline constexpr double kInitialRateStep = 1e-8;
inline constexpr double kRateXTolerance = std::numeric_limits<double>::max();
inline constexpr double kRateFTolerance = 1e-10;

/// Upstream's fallback guess when the curve cannot supply one.
inline constexpr double kFallbackRateGuess = 0.06;

/// Present value of a run of cash flows off a curve under construction.
/// Replaces `JpmcdsJpmcdsZCPresentValueCFL` -- the doubled prefix is in the
/// exported symbol name and is permanent.
///
/// Upstream takes the whole list plus `iLo` and `iHi`, and spends its first
/// three branches checking that `0 <= iLo <= iHi < fNumItems`. A span IS the
/// subrange, so those checks have nothing to check and the caller cannot
/// express the error.
///
/// Worth noting what this function does well, because it is the counterexample
/// to UPSTREAM-21 next door: it validates every index it is given, in a file
/// where its neighbour dereferences `fArray[-1]`.
///
/// The exact-date branch is the only place in upstream that READS the cached
/// discount array rather than recomputing from the rate:
///
/// ```c
/// if (j<zc->numItems && zc->date[j]==date)
///     pv = zc->discount[j]*amt;
/// ```
///
/// It is also why that cache has to be right. The port keeps the shortcut --
/// it is not an optimisation here so much as the definition of consistency:
/// if the cached discount and a fresh computation could differ, this function
/// and `present_value` would disagree about the same cash flow. The builder's
/// invariant is what makes them identical.
///
/// `j` advances monotonically across the whole run and is never reset, so the
/// shortcut assumes the flows are date-ordered. Out-of-order flows do not
/// break the answer -- the match simply fails and the slow path produces the
/// same number -- so this is one of the few places upstream is accidentally
/// robust rather than accidentally wrong.
[[nodiscard]] inline Expected<double> present_value_cash_flows(const ZeroCurveBuilder &curve,
                                                               std::span<const CashFlow> flows,
                                                               InterpType interp) {
  double sum = 0.0;
  std::size_t j = 0;
  for (const CashFlow &flow : flows) {
    const std::span<const ZeroCurveBuilder::Point> points = curve.points();
    while (j < points.size() && points[j].date < flow.date) {
      ++j;
    }
    if (j < points.size() && points[j].date == flow.date) {
      sum += points[j].discount * flow.amount; // the cached discount
    } else {
      const Expected<double> pv = curve.present_value(flow.amount, flow.date, interp);
      if (!pv) {
        return unexpected(pv.error());
      }
      sum += *pv;
    }
  }
  return sum;
}

/// Add a point for every cash flow date after `last_date` that the curve does
/// not already carry. Replaces `JpmcdsZCAddCFLPoints`.
///
/// Every rate is interpolated BEFORE any point is inserted, because inserting
/// changes what later interpolations would return. Upstream's comment says so
/// and the port keeps the two-phase shape for that reason.
///
/// ## UPSTREAM-23 -- a sign test that silently drops points
///
/// Upstream filters what it inserts:
///
/// ```c
/// if (rate >= 0.0)
/// {
///     dates[n] = d;
///     rates[n++] = rate;
/// }
/// ```
///
/// No `else`, no message, no status. A cash flow date whose interpolated rate
/// is negative is simply not added -- in a function whose own comment is
/// *"Ensure all uncovered cash flow dates in curve ... This will make sure
/// they can be priced appropriately by the curve."*
///
/// Measured on a zero-coupon bond over a curve at −0.5%, varying only the
/// price:
///
/// ```
/// price=0.97  solved rate +0.0044   dates placed 2 of 5
/// price=1.00  solved rate −2.2e-13  dates placed 1 of 5
/// price=1.06  solved rate −0.0083   dates placed 1 of 5
/// ```
///
/// The cliff is at exactly zero: a solved rate of −2e-13 drops four points
/// that a rate of +2e-13 would have kept. The curve's SHAPE depends on the
/// sign of a number that is, at that magnitude, noise.
///
/// Negative zero rates are not exotic -- EUR, CHF and JPY curves were negative
/// for most of a decade after 2014 -- so this is reachable from ordinary
/// market data rather than from a pathological input.
///
/// **What it does not do, having tried to show that it does.** I expected the
/// dropped points to break the bootstrap's fixed point, and they do not. Under
/// linear interpolation AND under flat forwards, with 1 of 5 dates placed, the
/// instrument still reprices to its input within the solver's f-tolerance.
/// The reason is that inserting a point at its own interpolated rate is
/// idempotent for both schemes: linear interpolation puts it on the line, and
/// flat forwards is log-linear in the discount factor, which puts it on that
/// curve. Splitting an interval at a point already on the path leaves the path
/// alone.
///
/// So the severity is low and the finding is about data, not prices: a caller
/// who inspects the finished curve sees a different number of points depending
/// on the sign of the rates, for no documented reason. The port reproduces no
/// filter at all -- every date that is missing gets its interpolated rate --
/// which is what the comment describes.
[[nodiscard]] inline bool add_cash_flow_points(ZeroCurveBuilder &curve,
                                               std::span<const CashFlow> flows, Date last_date,
                                               InterpType interp) {
  struct Pending {
    Date date;
    double rate;
  };
  std::vector<Pending> pending;
  pending.reserve(flows.size());

  for (const CashFlow &flow : flows) {
    if (flow.date <= last_date || curve.find_exact(flow.date)) {
      continue;
    }
    const Expected<double> rate = curve.interpolate(flow.date, interp);
    if (!rate) {
      return false;
    }
    // UPSTREAM-23: upstream adds this only when *rate >= 0.0.
    pending.push_back(Pending{flow.date, *rate});
  }

  // Upstream inserts in reverse, which cannot change the result -- every rate
  // was already computed and the points land in a sorted container either
  // way. Forward here, because reverse would imply an ordering dependency
  // that does not exist.
  for (const Pending &p : pending) {
    if (!curve.add_rate(p.date, p.rate)) {
      return false;
    }
  }
  return true;
}

/// Bootstrap one instrument into the curve. Replaces
/// `JpmcdsZCAddCashFlowList`.
///
/// `price` is the instrument's value today; `flows` are its payments. The
/// curve gains a rate at `date` -- defaulting to the final payment date --
/// chosen so that discounting the uncovered payments reproduces `price`.
///
/// ## UPSTREAM-21 -- the error path dereferences a null pointer
///
/// Upstream's first statement, and its error handler, both read the last
/// element without checking that one exists:
///
/// ```c
/// if (date==0L)
///     date = cfl->fArray[ cfl->fNumItems-1].fDate;
/// ...
/// done:
///   if (status == FAILURE)
///       JpmcdsErrMsg("%s: Failed for CFL maturing %s.\n", routine,
///                    JpmcdsFormatDate(cfl->fArray[cfl->fNumItems-1].fDate));
/// ```
///
/// `JpmcdsNewEmptyCFL(0)` -- a public constructor -- yields `fNumItems = 0`
/// with `fArray = NULL`, so both reads are `NULL[-1]`. Measured: **SIGSEGV**,
/// exit status 139.
///
/// The error handler is the part that makes this unrecoverable. Pass an
/// explicit `date` and the first read is skipped; the function then correctly
/// detects "no cash flows in list beyond ZeroCurve", writes the message, and
/// goes to `done:` -- where it crashes. Measured: SIGSEGV again. So the
/// diagnostic path for an empty list is the one path that cannot survive an
/// empty list, and there is no way for a caller to receive the error the
/// function was written to report.
///
/// In the port an empty span is refused before anything reads from it, and
/// nothing formats a payment that is not there.
///
/// ## UPSTREAM-22 -- success with nothing added
///
/// When the final payment falls exactly on the curve's last date, every branch
/// declines and the function returns SUCCESS having changed nothing:
///
///  * the guard rejects a final payment date *earlier* than the curve's last
///    date, so equality passes it;
///  * the search for the first uncovered payment then walks off the end,
///    leaving `firstUncovered == fNumItems`;
///  * `firstUncovered == fNumItems-1` is false, so the single-payment branch
///    is skipped;
///  * `firstUncovered < fNumItems` is false, so the solver branch is skipped,
///    and so is the call that would add the remaining dates.
///
/// Measured, on a 3-point curve ending 2027-01-01 and a list ending on that
/// same date, asked to add a point at 2030-01-01:
///
/// ```
/// rc=SUCCESS   numItems 3 -> 3   point at 2030 present? NO
/// ```
///
/// The caller is told the instrument was bootstrapped. The curve does not
/// contain the date it asked for, and no rate was solved. The next instrument
/// bootstraps against a curve that is one point shorter than the caller
/// believes, which is how one silent no-op becomes a wrong curve rather than a
/// missing point.
///
/// The port returns false: if there is nothing to solve for, the instrument
/// was not added, and saying so is the only honest answer.
[[nodiscard]] inline bool add_cash_flow_list(ZeroCurveBuilder &curve,
                                             std::span<const CashFlow> flows, double price,
                                             std::optional<Date> date, InterpType interp) {
  if (flows.empty()) {
    return false; // UPSTREAM-21: upstream reads flows.back() and crashes
  }
  if (!is_ordered_by_date(flows)) {
    return false;
  }

  // Upstream's `date == 0L` sentinel means "use the last payment date". TDate
  // 0 is 1601-01-01, a real date, so the sentinel also makes that date
  // unaskable. std::optional says the same thing without spending a value.
  const Date target = date.value_or(flows.back().date);

  std::size_t first_uncovered = 0;
  if (!curve.empty()) {
    const Date last_curve_date = curve.points().back().date;
    if (target <= last_curve_date) {
      return false; // the date to add is already covered
    }
    if (flows.back().date < last_curve_date) {
      return false; // nothing in the list reaches beyond the curve
    }
    // The first payment strictly after the curve's end.
    const auto beyond = std::ranges::upper_bound(flows, last_curve_date, {}, &CashFlow::date);
    first_uncovered = static_cast<std::size_t>(std::ranges::distance(flows.begin(), beyond));
    if (first_uncovered >= flows.size()) {
      // UPSTREAM-22: upstream falls through every branch and reports success
      // having added nothing.
      return false;
    }
  } else {
    // Upstream overrides the caller's choice when the curve is empty: with no
    // points there is nothing for flat forwards to be flat between.
    interp = InterpType::Linear;
  }

  double covered_pv = 0.0;
  if (first_uncovered > 0) {
    const Expected<double> pv =
        present_value_cash_flows(curve, flows.first(first_uncovered), interp);
    if (!pv) {
      return false;
    }
    covered_pv = *pv;
  }

  const std::span<const CashFlow> uncovered = flows.subspan(first_uncovered);
  const double target_pv = price - covered_pv;

  if (uncovered.size() == 1 && uncovered.front().date == target) {
    // One payment, so the discount factor follows directly and no solve is
    // needed. Upstream rejects a zero on either side of the division; a zero
    // future value would divide by zero, and a zero present value implies a
    // discount factor of zero, which no curve can hold.
    const double future_value = uncovered.front().amount;
    if (future_value == 0.0 || target_pv == 0.0) {
      return false;
    }
    return curve.add_discount_factor(target, target_pv / future_value);
  }

  // More than one uncovered payment: solve for the rate at `target` that
  // makes them worth `target_pv`.
  double guess = kFallbackRateGuess;
  if (!curve.empty()) {
    const Expected<double> extrapolated = curve.interpolate(target, InterpType::Linear);
    // Upstream falls back to 0.06 when the extrapolation fails, then clamps.
    guess = std::clamp(extrapolated.value_or(kFallbackRateGuess), 0.01, 1.00);
  }
  if (!curve.add_rate(target, guess)) {
    return false;
  }
  const std::optional<std::size_t> solved_at = curve.find_exact(target);
  if (!solved_at) {
    return false;
  }

  // The objective: with `rate` standing at `target`, how far is the value of
  // the uncovered payments from the price we have to match? Upstream carries
  // this through a void* and a ten-field struct, six of whose fields belong
  // to a different caller.
  bool evaluation_failed = false;
  const auto objective = [&](double rate) -> Expected<double> {
    if (rate <= kMinZeroRate || kMaxZeroRate <= rate) {
      return fail(ErrorKind::bad_argument,
                  "trial rate " + detail::num(rate) + " is outside the permitted zero-rate range");
    }
    if (!curve.set_rate(*solved_at, rate)) {
      return fail(ErrorKind::bad_argument, "the trial rate could not be stored on the curve");
    }
    const Expected<double> pv = present_value_cash_flows(curve, uncovered, interp);
    if (!pv) {
      evaluation_failed = true;
      return fail(ErrorKind::bad_argument, "the objective could not be valued at this rate");
    }
    return target_pv - *pv;
  };

  RootFindOptions options;
  options.bound_lo = kMinZeroRate;
  options.bound_hi = kMaxZeroRate;
  options.max_iterations = kMaxBootstrapIterations;
  options.guess = guess;
  options.initial_x_step = kInitialRateStep;
  options.initial_f_derivative = 0.0;
  options.x_tolerance = kRateXTolerance;
  options.f_tolerance = kRateFTolerance;

  const Expected<double> root = root_find_brent(objective, options);
  if (!root || evaluation_failed) {
    return false;
  }
  // Upstream writes `zc->rate[zcIndex] = rate` and leaves the cached discount
  // describing whatever the solver last evaluated. `set_rate` recomputes it,
  // so the curve cannot carry a rate and a discount that disagree.
  if (!curve.set_rate(*solved_at, *root)) {
    return false;
  }

  // Finally, make sure every remaining payment date is in the curve. The day
  // before the first uncovered payment, so that payment is included too.
  return add_cash_flow_points(curve, uncovered, from_tdate(to_tdate(uncovered.front().date) - 1),
                              interp);
}

} // namespace cdsmodel
