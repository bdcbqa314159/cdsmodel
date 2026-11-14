#pragma once

// Port of upstream gtozc.c -- adding a class of instrument to a zero curve and
// returning the extended curve. Two exported functions and two static
// validators.
//
//   JpmcdsZCCash        -> curve_with_cash
//   JpmcdsZCSwaps       -> curve_with_swaps
//   CheckZCCashInputs   -> folded into curve_with_cash's refusals
//   CheckZCSwapsInputs  -> folded into curve_with_swaps's refusals
//
// Both take a curve, add instruments, and hand back a new curve -- so they are
// the layer a caller actually drives, with `zcall.c` and `zcswap.c` underneath.
//
// ## Deliberate difference: UPSTREAM-34
//
// Both validators detect a NULL curve, log it, and then dereference it. See
// the note below; the port's parameter is a reference, so the input does not
// exist.
//
// ## An empty input curve
//
// Upstream handles one explicitly -- `if (zeroCurve->fNumItems > 0)  /* only
// want it if we need it */` -- because building a curve from scratch starts
// with an empty `TCurve`, adds cash, then adds swaps.
//
// The port's `ZeroCurve` is non-empty by construction, decided back in the
// zero_curve slice, so that input does not exist here. Building from nothing
// goes through `ZeroCurveBuilder` and `add_money_market` directly, which is
// what these functions do internally anyway. The branch is reproduced for a
// curve that HAS points; the other side of it has no caller.
//
// ## The hardcoded curve terms
//
// `JpmcdsZCCash` builds its working curve with
//
//     #define ZC_DEFAULT_BASIS 1
//     #define ZC_DEFAULT_DAYCNT JPMCDS_ACT_365F
//
// ignoring the input curve's own basis and day count, and then merges the
// input curve into it with `JpmcdsZCAddPrefixCurve` / `JpmcdsZCAddSuffixCurve`.
// Reproduced, because the merge copies rates without converting terms and a
// caller's curve in other terms would come back meaning something different --
// which is upstream's behaviour and not something a port may quietly improve.
// `JpmcdsZCSwaps` has no such constant: it converts the input curve directly
// and keeps its terms.
//
// ## A stub point inside the cash range is discarded
//
// `JpmcdsZCCash` merges the input curve back with
// `JpmcdsZCAddPrefixCurve` -- the points BEFORE the cash curve's first date --
// and `JpmcdsZCAddSuffixCurve` -- the ones AFTER its last. Nothing copies the
// middle, so a caller's curve point that happens to fall inside the span of
// the cash quotes is silently dropped.
//
// Reproduced, and asserted both ways by a tier-1 property test: outside the
// range the point must survive, inside it must not. The first version of that
// test claimed every stub point survives, and the recorded data refuted it.
//
// ## Dead parameters and locals
//
// `JpmcdsZCSwaps` takes `char fwdLength` -- "For fwd smoothing, length of
// fwds" -- and NEVER READS IT. It appears three times in the file: the static
// declaration, the definition, and the call that passes it to the validator,
// which does not read it either. Forward smoothing was removed.
//
// It also declares `TInterpData *interpData = NULL` and
// `TBadDayList *badDayList = NULL`, assigns neither, passes both to
// `JpmcdsZCAddSwaps`, and calls `FREE(interpData)` at `done:` on a pointer
// that is provably still NULL -- the third such always-NULL free in this port
// (`zcswap.c` has `newRates`, `zcall.c`'s caller has this one). Interpolation
// is hardcoded to `JPMCDS_FLAT_FORWARDS` at the call site regardless.
//
// ## The skip loop, and what it does for UPSTREAM-33
//
// `JpmcdsZCSwaps` drops benchmarks that mature before the stub curve ends:
//
//     while (numSwaps > 0 && dates[offset] < lastStubDate)
//     {
//         offset++;
//         numSwaps--;
//     }
//
// Note the `<`: a benchmark maturing exactly ON `lastStubDate` survives. That
// looked like it would walk into UPSTREAM-33, whose trigger is a benchmark not
// beyond the curve's end -- but it does not, and the reason is worth keeping.
// The solved point is at `max(indexMaturity, payDate)`, and the index maturity
// extends a floating period PAST the swap's maturity, so the added point is
// still the latest in the curve and `numItems-1` still identifies it. The
// filter is `<` where avoiding UPSTREAM-33 would want `<=`, and the index
// maturity covers the gap.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Upstream's `ZC_DEFAULT_BASIS`: the terms `JpmcdsZCCash` builds in whatever
/// the input curve uses.
inline constexpr long kCashCurveBasis = 1;
inline constexpr DayCount kCashCurveDayCount = DayCount::Act365Fixed;

/// Replaces `JpmcdsZCCash`: add money-market quotes to a curve.
///
/// With no quotes the input curve is returned unchanged -- and upstream takes
/// that shortcut BEFORE validating anything, so a day count convention it
/// would otherwise reject is accepted when there is nothing to apply it to.
/// Reproduced: there is genuinely nothing to validate.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-34. `CheckZCCashInputs` accumulates
/// failures rather than returning at the first, which is good design -- it
/// reports every problem with the input instead of the earliest. But one of
/// the accumulated checks is a PRECONDITION for a later one:
///
/// ```c
/// if(zeroCurve == NULL)
/// {
///     JpmcdsErrMsg("%s: ZeroCurve is NULL\n", routine);
///     status = FAILURE;          /* and no return */
/// }
/// ...
/// for (i=0 ;i<numRates ;i++ )
/// {
///     ...
///     if (dateArr[i] <= zeroCurve->fBaseDate)    /* NULL->fBaseDate */
/// ```
///
/// so the validator detects the NULL curve, reports it, and then dereferences
/// it. UBSan: "member access within null pointer of type 'TCurve'" at
/// gtozc.c:345; ASan: SEGV on 0x10. `CheckZCSwapsInputs` has the same shape at
/// gtozc.c:464 and is worse -- its dereference is unconditional rather than
/// inside a count-guarded loop. One mechanism, two sites, one fix, so one
/// register entry. See `defect_evidence u34 --unsafe`.
///
/// The port's parameter is a reference, so the input does not exist. The
/// accumulating style is worth keeping where every check is independent --
/// `stream_cash_flows.hpp`'s `is_well_formed` does exactly that safely -- and
/// the lesson is narrower than "return early": an accumulating validator is
/// safe only when no check guards another.
[[nodiscard]] inline Expected<ZeroCurve>
curve_with_cash(const ZeroCurve &curve, std::span<const RatePoint> quotes, DayCount day_count) {
  if (quotes.empty()) {
    return curve; // upstream copies the curve and returns before validating
  }

  // CheckZCCashInputs, minus the NULL check the type makes unnecessary and the
  // negative-count check a span makes unrepresentable.
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (i > 0 && quotes[i - 1].date > quotes[i].date) {
      return fail(ErrorKind::bad_argument, "cash quote dates must not decrease: quote " +
                                               detail::num(i) + " predates quote " +
                                               detail::num(i - 1));
    }
    if (quotes[i].rate <= -1.0) {
      return fail(ErrorKind::bad_argument,
                  "cash rate " + detail::num(quotes[i].rate) + " is at or below -100%");
    }
    if (quotes[i].date <= curve.base_date()) {
      return fail(ErrorKind::bad_argument, "a cash quote matures on or before the curve base date");
    }
  }

  Expected<ZeroCurveBuilder> cash = ZeroCurveBuilder::create(
      curve.base_date(), static_cast<int>(quotes.size()) + 2, kCashCurveBasis, kCashCurveDayCount);
  if (!cash) {
    return unexpected(cash.error());
  }
  if (!cash->add_money_market(quotes, day_count)) {
    return fail(ErrorKind::bad_argument, "the money-market quotes could not be added to the curve");
  }

  // Upstream only converts the stub curve when it has points -- "only want it
  // if we need it" -- and merges it on both sides, so stub points before the
  // first cash date and after the last are both kept.
  if (!curve.points().empty()) {
    const Expected<ZeroCurveBuilder> stub = ZeroCurveBuilder::from_zero_curve(curve);
    if (!stub) {
      return unexpected(stub.error());
    }
    if (!cash->add_prefix(*stub) || !cash->add_suffix(*stub)) {
      return fail(ErrorKind::bad_argument,
                  "the stub curve could not be merged around the cash points");
    }
  }
  return cash->to_zero_curve();
}

/// Build a curve from money-market quotes alone, with no input curve.
///
/// Upstream's caller for this is `zerocurve.c`, which starts with
/// `JpmcdsNewTCurve(valueDate, 0, ...)` -- a curve with ZERO points -- and
/// hands it to `JpmcdsZCCash`. The port's `ZeroCurve` is non-empty by
/// construction, so that input cannot be expressed; this overload is what
/// upstream's empty-curve branch means, written as its own entry point rather
/// than as a state the type forbids.
///
/// Same validation as the curve-taking overload, except that the "quote on or
/// before the value date" check compares against `value_date` directly.
[[nodiscard]] inline Expected<ZeroCurve>
curve_with_cash(Date value_date, std::span<const RatePoint> quotes, DayCount day_count) {
  if (quotes.empty()) {
    return fail(ErrorKind::bad_argument,
                "no input curve and no cash quotes is nothing to build from");
  }
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (i > 0 && quotes[i - 1].date > quotes[i].date) {
      return fail(ErrorKind::bad_argument, "cash quote dates must not decrease: quote " +
                                               detail::num(i) + " predates quote " +
                                               detail::num(i - 1));
    }
    if (quotes[i].rate <= -1.0) {
      return fail(ErrorKind::bad_argument,
                  "cash rate " + detail::num(quotes[i].rate) + " is at or below -100%");
    }
    if (quotes[i].date <= value_date) {
      return fail(ErrorKind::bad_argument, "a cash quote matures on or before the value date");
    }
  }
  Expected<ZeroCurveBuilder> cash = ZeroCurveBuilder::create(
      value_date, static_cast<int>(quotes.size()) + 2, kCashCurveBasis, kCashCurveDayCount);
  if (!cash) {
    return unexpected(cash.error());
  }
  if (!cash->add_money_market(quotes, day_count)) {
    return fail(ErrorKind::bad_argument, "the money-market quotes could not be added to the curve");
  }
  return cash->to_zero_curve();
}

/// Replaces `JpmcdsZCSwaps`: add swap benchmarks to a curve.
///
/// Interpolation is `FlatForwards`, which upstream hardcodes at the call site
/// rather than taking as a parameter. `fwdLength` is not a parameter here --
/// upstream takes one and never reads it.
///
/// Carries the same UPSTREAM-34 difference as `curve_with_cash`, at
/// gtozc.c:464.
[[nodiscard]] inline Expected<ZeroCurve>
curve_with_swaps(const ZeroCurve &curve, const std::optional<ZeroCurve> &discount_curve,
                 std::span<const Date> maturities, std::span<const double> rates,
                 const SwapConventions &conventions, const Calendar &calendar) {
  if (maturities.size() != rates.size()) {
    return fail(ErrorKind::bad_argument, "maturities and rates must be the same length, got " +
                                             detail::num(maturities.size()) + " and " +
                                             detail::num(rates.size()));
  }
  if (maturities.empty()) {
    return curve; // as above: nothing to add, and nothing validated
  }

  // CheckZCSwapsInputs. Its frequency and day-count checks are types here.
  for (const double rate : rates) {
    if (rate <= -1.0) {
      return fail(ErrorKind::bad_argument,
                  "swap rate " + detail::num(rate) + " is at or below -100%");
    }
  }
  for (std::size_t i = 0; i + 1 < maturities.size(); ++i) {
    // Note `<=`: unlike the cash validator, this one requires STRICTLY
    // increasing dates. The two validators in the same file disagree about
    // whether equal dates are acceptable, and both are reproduced.
    if (maturities[i + 1] <= maturities[i]) {
      return fail(ErrorKind::bad_argument, "swap maturities must strictly increase: maturity " +
                                               detail::num(i + 1) + " is not after maturity " +
                                               detail::num(i));
    }
  }
  for (const Date date : maturities) {
    if (date < curve.base_date()) {
      return fail(ErrorKind::bad_argument, "a swap matures before the curve base date");
    }
  }
  // Upstream requires the value date itself to be a business day.
  if (!calendar.is_business_day(curve.base_date())) {
    return fail(ErrorKind::bad_argument, "the curve base date is not a business day");
  }

  Expected<ZeroCurveBuilder> builder = ZeroCurveBuilder::from_zero_curve(curve);
  if (!builder) {
    return unexpected(builder.error());
  }

  // "only want to add swap points not already covered by stub curve, don't
  // want to replace anything in the existing curve"
  const std::span<const RatePoint> points = curve.points();
  const Date last_stub_date = points.empty() ? curve.base_date() : points.back().date;
  std::size_t offset = 0;
  while (offset < maturities.size() && maturities[offset] < last_stub_date) {
    ++offset;
  }

  if (offset < maturities.size()) {
    if (!add_swaps(*builder, discount_curve, maturities.subspan(offset), rates.subspan(offset),
                   conventions, std::nullopt, calendar)) {
      return fail(ErrorKind::bad_argument,
                  "the swap benchmarks could not be bootstrapped onto the curve");
    }
  }
  return builder->to_zero_curve();
}

/// Build a curve from swap benchmarks alone, with no input curve.
///
/// The counterpart to the cash overload above, and reachable in upstream the
/// same way: `zerocurve.c` with an instrument list containing no money-market
/// entries hands `JpmcdsZCSwaps` the zero-point curve it started with. There
/// `lastStubDate` falls back to the curve's base date --
/// `if (zeroCurve->fNumItems < 1) lastStubDate = zeroCurve->fBaseDate;` --
/// which is what this does.
[[nodiscard]] inline Expected<ZeroCurve>
curve_with_swaps(Date value_date, const std::optional<ZeroCurve> &discount_curve,
                 std::span<const Date> maturities, std::span<const double> rates,
                 const SwapConventions &conventions, const Calendar &calendar) {
  if (maturities.size() != rates.size()) {
    return fail(ErrorKind::bad_argument, "maturities and rates must be the same length, got " +
                                             detail::num(maturities.size()) + " and " +
                                             detail::num(rates.size()));
  }
  if (maturities.empty()) {
    return fail(ErrorKind::bad_argument,
                "no input curve and no swap benchmarks is nothing to build from");
  }
  for (const double rate : rates) {
    if (rate <= -1.0) {
      return fail(ErrorKind::bad_argument,
                  "swap rate " + detail::num(rate) + " is at or below -100%");
    }
  }
  for (std::size_t i = 0; i + 1 < maturities.size(); ++i) {
    if (maturities[i + 1] <= maturities[i]) {
      return fail(ErrorKind::bad_argument, "swap maturities must strictly increase: maturity " +
                                               detail::num(i + 1) + " is not after maturity " +
                                               detail::num(i));
    }
  }
  for (const Date date : maturities) {
    if (date < value_date) {
      return fail(ErrorKind::bad_argument, "a swap matures before the value date");
    }
  }
  if (!calendar.is_business_day(value_date)) {
    return fail(ErrorKind::bad_argument, "the value date is not a business day");
  }

  Expected<ZeroCurveBuilder> builder = ZeroCurveBuilder::create(
      value_date, static_cast<int>(maturities.size()) + 2, kCashCurveBasis, kCashCurveDayCount);
  if (!builder) {
    return unexpected(builder.error());
  }

  // lastStubDate is the base date when the curve has no points.
  std::size_t offset = 0;
  while (offset < maturities.size() && maturities[offset] < value_date) {
    ++offset;
  }
  if (offset >= maturities.size()) {
    return fail(ErrorKind::bad_argument, "every swap benchmark matures before the value date");
  }
  if (!add_swaps(*builder, discount_curve, maturities.subspan(offset), rates.subspan(offset),
                 conventions, std::nullopt, calendar)) {
    return fail(ErrorKind::bad_argument,
                "the swap benchmarks could not be bootstrapped onto the curve");
  }
  return builder->to_zero_curve();
}

} // namespace cdsmodel
