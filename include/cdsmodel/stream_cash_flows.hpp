#pragma once

// Port of upstream streamcf.c -- the cash flows of one leg of a swap, fixed or
// floating. Ten exported functions and five statics; the last module before
// `zcswap.c` and the curve bootstrap.
//
//   JpmcdsNewStreamFixedCFL       -> fixed_stream_cash_flows
//   JpmcdsGetFixedCouponRates     -> fixed_coupon_rates
//   JpmcdsNewStreamFloatCFLGen    -> float_stream_cash_flows
//   JpmcdsFloatingCashFlowGen     -> floating_cash_flow
//   JpmcdsNewStreamFixed          -> make_fixed_stream  (it validates)
//   JpmcdsNewStreamFloat          -> make_float_stream  (it validates)
//   JpmcdsNewEmptyTStreamFixed    -> (gone: std::vector)
//   JpmcdsNewEmptyTStreamFloat    -> (gone: std::vector)
//   JpmcdsFreeTStreamFixed        -> (gone: std::vector)
//   JpmcdsFreeTStreamFloat        -> (gone: std::vector)
//   CheckFixedStreamDetails       -> folded into make_fixed_stream
//   CheckFloatingCFArgs           -> folded into floating_cash_flow
//   GetFrontStub / GetBackStub    -> detail::stub_payment
//   GetFloatStubRate              -> folded into detail::stub_payment
//
// ## Most of this file's interface is vestigial
//
// streamcf.c is the remains of a larger library. The parameters for averaging
// and compounding swaps survived; the code that used them did not. Measured
// rather than assumed -- the differential tests sweep each of these and
// require the output not to move:
//
//   * `TSwapType` has ONE enumerator, `JPMCDS_SINGLE_REFIX`, and three
//     functions guard it with `if (swapType > JPMCDS_SINGLE_REFIX)`. That is
//     one-sided, and it holds only because an enum with one enumerator gets an
//     unsigned underlying type, so `(TSwapType)(-1)` is 4294967295 and the
//     guard catches it. A field with one possible value is not a choice.
//   * `TStreamFloat.firstCoupon` and `.finalCoupon` are `TStubRates`, and
//     NEITHER FIELD IS EVER READ. `stubInterval` is passed to
//     `GetFloatStubRate`, which ignores it; `stubRate` is not even passed --
//     `zcswap.c:615,618` set it to 0.0 and no line reads it back. So a caller
//     supplying floating-leg stub rates is silently ignored, while the FIXED
//     leg genuinely honours `firstCouponRate` and `finalCouponRate`.
//   * `TStreamFloat.averSoFar` and `.compSoFar` are read, passed down two
//     levels, and land in parameters `GetFloatStubRate` never uses.
//   * `GetFloatStubRate` takes eleven parameters and uses five. `spotOffset`,
//     `averSoFar`, `compSoFar`, `stubInterval`, `rateBadDayConv` and
//     `holidayFile` are all dead.
//   * `CheckFixedStreamDetails` takes `valueDate` and never reads it, so
//     `valueDate` is dead through the ENTIRE fixed path:
//     `JpmcdsNewStreamFixedCFL(valueDate, stream)` ignores it. Its `pricing`
//     flag has one call site, passing FALSE.
//   * `CheckFloatingCFArgs` takes `spread` and never reads it.
//
// Dropped here, each with a test proving the drop changes nothing. This is a
// different judgement from `FloatRate::weight`, which is kept: `weight` is one
// field a caller fills in and a consumer might apply, whereas `TStubRates` is
// a whole struct passed into a function that ignores every bit of it.
//
// ## The two legs disagree about the degenerate case
//
// For a `TWO_STUB` schedule of ONE period -- which
// `JpmcdsCouponDateListCheck` accepts, and `JpmcdsNewEmptyCouponDateList`
// makes constructible -- the front and back stub both address index 0:
//
//   * the fixed path writes `couponRates[fNumItems-1] = finalCouponRate` then
//     `couponRates[0] = firstCouponRate`, so FIRST wins;
//   * the float path runs `GetFrontStub` then `GetBackStub` into
//     `amounts[0]`, so BACK wins.
//
// Opposite precedence, from the same stub location, in the same file.
// Reproduced on both sides with a test asserting the disagreement, because
// picking a winner would be a convention decision rather than a porting one --
// the same reasoning as UPSTREAM-24 and UPSTREAM-27.
//
// ## Deliberate difference: UPSTREAM-31
//
// `GetFloatStubRate` writes its output AFTER its `done:` label, so a failure
// path copies an indeterminate `double` to the caller. See the note on
// `detail::stub_payment`.
//
// ## The float leg never validates its schedule
//
// `CheckFixedStreamDetails` calls `JpmcdsCouponDateListCheck`; there is no
// `CheckFloatStreamDetails` and `JpmcdsNewStreamFloatCFLGen` calls nothing. So
// the float leg is the unvalidated one -- which is how UPSTREAM-29's
// zero-period schedule reaches `zcswap.c`'s `fArray[fNumItems-1]` reads. The
// port cannot have that gap: `CouponSchedule` is non-empty by construction.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/curve_rates.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/float_rate.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Upstream's largest accepted fixed rate, `JPMCDS_MAX_RATE`.
inline constexpr double kMaxStreamRate = 100.0;

/// Which principal exchanges a leg pays. Upstream's `long flags`, carrying
/// `JPMCDS_ADD_FINAL` (1) and `JPMCDS_SUBTRACT_INITIAL` (2) -- the same two
/// bits `cashflow.c` uses, where they are two of five.
struct PrincipalFlows {
  /// Pay out the principal at the first period's accrual start.
  bool subtract_initial = false;

  /// Add the principal to the final coupon.
  bool add_final = false;

  friend constexpr bool operator==(const PrincipalFlows &, const PrincipalFlows &) = default;
};

/// One leg paying a known rate.
struct FixedStream {
  CouponSchedule schedule;
  double principal = 0.0;
  DayCount pay_day_count = DayCount::Act360;
  PrincipalFlows principal_flows{};

  /// Used only when the schedule has a front stub, and likewise for the back.
  double first_coupon_rate = 0.0;
  double final_coupon_rate = 0.0;

  /// Every regular period's rate.
  double fixed_rate = 0.0;
};

/// One leg paying a rate the curve implies.
///
/// No stub-rate fields, unlike `FixedStream`: upstream's `firstCoupon` and
/// `finalCoupon` are never read on this side, and the stub rate is always the
/// forward over the stub's own accrual period. See the header note.
struct FloatStream {
  CouponSchedule schedule;
  double principal = 0.0;
  DayCount pay_day_count = DayCount::Act360;
  PrincipalFlows principal_flows{};
  FloatRate rate{};
};

/// Reproduces `CheckFixedStreamDetails`. Separate from the aggregate because
/// upstream validates, and because it ACCUMULATES: it reports every problem it
/// finds rather than returning at the first, unlike
/// `CheckArgsSwapMakeSchedule` two modules down. Worth preserving as a
/// predicate even though the port's return type cannot carry the list.
///
/// `valueDate` is not a parameter: upstream takes one and never reads it.
[[nodiscard]] inline bool is_well_formed(const FixedStream &stream) {
  if (!is_well_formed(stream.schedule)) {
    return false;
  }
  if (stream.principal == 0.0) {
    return false;
  }
  // Upstream guards this with `pricing == FALSE &&`, and its one call site
  // passes FALSE, so the guard is always open.
  return stream.fixed_rate >= -1.0 && stream.fixed_rate <= kMaxStreamRate;
}

/// Replaces `JpmcdsNewStreamFixed`, which validates and returns NULL.
[[nodiscard]] inline Expected<FixedStream> make_fixed_stream(FixedStream stream) {
  if (!is_well_formed(stream)) {
    return fail(ErrorKind::bad_argument, "the fixed stream is not well formed");
  }
  return stream;
}

/// Replaces `JpmcdsNewStreamFloat`. It checks the day count convention and the
/// swap type, both of which are types here, so the only check left is the one
/// upstream does NOT do on this side -- see the header note on validation.
[[nodiscard]] inline Expected<FloatStream> make_float_stream(FloatStream stream) {
  if (!is_well_formed(stream.schedule)) {
    return fail(ErrorKind::bad_argument, "the float stream schedule is not well formed");
  }
  return stream;
}

/// Replaces `JpmcdsGetFixedCouponRates`.
///
/// One rate per period: the stub rates where the schedule says there is a
/// stub, `fixed_rate` everywhere else.
[[nodiscard]] inline Expected<std::vector<double>> fixed_coupon_rates(const FixedStream &stream) {
  if (!is_well_formed(stream)) {
    return fail(ErrorKind::bad_argument, "the fixed stream is not well formed");
  }
  const std::size_t n = stream.schedule.periods.size();
  std::vector<double> rates(n, stream.fixed_rate);

  // Upstream assigns the back stub first and the front stub second, so for a
  // one-period Both schedule the FRONT rate wins. The float path resolves the
  // same collision the other way -- see the header note. Written in upstream's
  // order so the collision resolves as upstream's does.
  if (stream.schedule.stub == StubLocation::Back || stream.schedule.stub == StubLocation::Both) {
    rates[n - 1] = stream.final_coupon_rate;
  }
  if (stream.schedule.stub == StubLocation::Front || stream.schedule.stub == StubLocation::Both) {
    rates[0] = stream.first_coupon_rate;
  }
  return rates;
}

namespace detail {

/// How many cash flows a leg produces, and where the coupons start.
struct CashFlowLayout {
  std::size_t count = 0;
  std::size_t first_coupon = 0;
};

[[nodiscard]] constexpr CashFlowLayout layout(std::size_t periods, PrincipalFlows flows) noexcept {
  // Upstream allocates fNumItems+1 when subtracting the initial principal, and
  // writes the principal at index 0 and coupon i at index i+1.
  return flows.subtract_initial ? CashFlowLayout{periods + 1, 1} : CashFlowLayout{periods, 0};
}

} // namespace detail

/// Replaces `JpmcdsNewStreamFixedCFL`.
///
/// `valueDate` is not a parameter. Upstream takes one, passes it to
/// `JpmcdsGetFixedCouponRates`, which passes it to `CheckFixedStreamDetails`,
/// which never reads it -- so it is dead through the whole path. A differential
/// test varies it over a century and requires the cash flows not to move.
[[nodiscard]] inline Expected<std::vector<CashFlow>>
fixed_stream_cash_flows(const FixedStream &stream) {
  const Expected<std::vector<double>> rates = fixed_coupon_rates(stream);
  if (!rates) {
    return unexpected(rates.error());
  }
  const std::size_t n = stream.schedule.periods.size();
  const detail::CashFlowLayout shape = detail::layout(n, stream.principal_flows);

  std::vector<CashFlow> flows;
  flows.reserve(shape.count);
  if (stream.principal_flows.subtract_initial) {
    flows.push_back(CashFlow{stream.schedule.periods.front().accrual_start, -stream.principal});
  }

  for (std::size_t i = 0; i < n; ++i) {
    const CouponDates &period = stream.schedule.periods[i];
    const Expected<double> year_frac =
        year_fraction(period.accrual_start, period.accrual_end, stream.pay_day_count);
    if (!year_frac) {
      return unexpected(year_frac.error());
    }
    double amount = stream.principal * (*rates)[i] * *year_frac;
    if (i == n - 1 && stream.principal_flows.add_final) {
      amount += stream.principal;
    }
    flows.push_back(CashFlow{period.pay, amount});
  }
  return flows;
}

/// Replaces `JpmcdsFloatingCashFlowGen`.
///
/// The index rate is the par coupon over `[index_start, index_maturity]` at
/// `pay_interval`, and the payment is `notional * yearFrac * (rate + spread)`
/// over the COUPON's accrual period -- two different date ranges, which is the
/// point of taking both.
///
/// `spread` is added after the rate, not folded into it, because upstream adds
/// it here and `FloatRate::weight` is never applied (see float_rate.hpp).
///
/// Upstream additionally fills a local `TFloatRate floatRateDef` "used for
/// convexity adjustment" and never reads it. Not reproduced; there is nothing
/// to reproduce.
[[nodiscard]] inline Expected<double>
floating_cash_flow(const ZeroCurve &curve, InterpType interp, Date index_start, Date index_maturity,
                   DateInterval pay_interval, const CouponDates &coupon, DayCount rate_day_count,
                   DayCount pay_day_count, double notional, double spread) {
  // CheckFloatingCFArgs. Its day-count checks are types here, and it takes a
  // `spread` it never reads, so these two comparisons are all of it.
  if (index_start < curve.base_date()) {
    return fail(ErrorKind::bad_argument, "the index start date is before the curve begins");
  }
  if (index_start > index_maturity) {
    return fail(ErrorKind::bad_argument, "the index start date is after its maturity");
  }

  const Calendar no_adjustment = Calendar::no_weekends();
  const Expected<double> coupon_rate = par_coupon_rate(
      curve, interp, index_start, pay_interval, index_maturity, rate_day_count, StubPosition::Front,
      BadDayConv::None, BadDayConv::None, std::span<const Calendar>{&no_adjustment, 1});
  if (!coupon_rate) {
    return unexpected(coupon_rate.error());
  }
  const Expected<double> year_frac =
      year_fraction(coupon.accrual_start, coupon.accrual_end, pay_day_count);
  if (!year_frac) {
    return unexpected(year_frac.error());
  }
  return notional * *year_frac * (*coupon_rate + spread);
}

namespace detail {

/// The payment for a stub period on a floating leg. Replaces `GetFrontStub`,
/// `GetBackStub` and `GetFloatStubRate`, which differ only in which period
/// they address.
///
/// The stub rate is the forward over the stub's OWN accrual period -- not an
/// interpolation to the stub interval, despite `TStubRates.stubInterval`
/// existing for that and being passed in. Upstream's comment acknowledges it:
/// *"$$$ NB: This can be improved when the floating rate for the stubs is
/// changed to be described by a TFloatRate (LB)."*
///
/// DELIBERATE DIFFERENCE, UPSTREAM-31. `GetFloatStubRate` declares
/// `double rate;`, assigns it only inside the success branch, and then writes
///
/// ```c
///   done:
///     *stubRate = rate;
/// ```
///
/// AFTER the label -- so every failure path copies an indeterminate double to
/// the caller. Measured, not inferred: `JpmcdsForwardFromZCurve` does not
/// write its output on failure (an output sentinel survives the call), and a
/// real back-stub schedule reaches it. The value is discarded by both callers,
/// which check the status first, so nothing wrong escapes -- but the read is
/// undefined behaviour and it is invisible to this project's whole harness.
/// ASan and UBSan do not see uninitialised stack reads, there is no MSan job,
/// and single-translation-unit analysis cannot see it either, because whether
/// `rate` is indeterminate depends on a function in zr2fwd.c: both
/// `-Wconditional-uninitialized` and `clang --analyze` report nothing. See
/// `defect_evidence u31`.
///
/// Here the value is returned, so there is no output to write on a failure
/// path and the shape cannot recur.
[[nodiscard]] inline Expected<double> stub_payment(const ZeroCurve &curve, InterpType interp,
                                                   const FloatStream &stream,
                                                   const CouponDates &period) {
  const Expected<double> forward =
      forward_from_curve(curve, interp, period.accrual_start, period.accrual_end,
                         stream.rate.day_count, static_cast<long>(kSimpleBasis));
  if (!forward) {
    return unexpected(forward.error());
  }
  const double stub_rate = *forward + stream.rate.spread;
  const Expected<double> year_frac =
      year_fraction(period.accrual_start, period.accrual_end, stream.pay_day_count);
  if (!year_frac) {
    return unexpected(year_frac.error());
  }
  return stub_rate * stream.principal * *year_frac;
}

} // namespace detail

/// Replaces `JpmcdsNewStreamFloatCFLGen`.
///
/// Stub periods are paid at the forward over their own accrual period; regular
/// periods go through `floating_cash_flow`, whose index period starts at the
/// reset date advanced by the spot offset.
///
/// Note that the index start date is NOT business-day adjusted after the spot
/// offset is applied, while the index maturity date IS -- upstream's
/// asymmetry, reproduced. With upstream's own spot offsets that is visible: a
/// calendar offset from a Friday reset puts the index period's start on a
/// weekend (see float_rate.hpp).
[[nodiscard]] inline Expected<std::vector<CashFlow>>
float_stream_cash_flows(const ZeroCurve &curve, InterpType interp, const FloatStream &stream,
                        BadDayConv rate_bad_day, const Calendar &calendar) {
  if (!is_well_formed(stream.schedule)) {
    return fail(ErrorKind::bad_argument, "the float stream schedule is not well formed");
  }
  const std::size_t n = stream.schedule.periods.size();
  std::vector<double> amounts(n, 0.0);

  std::size_t first_coupon = 0;
  std::size_t last_coupon = n;
  const StubLocation stub = stream.schedule.stub;

  // Upstream runs the front stub BEFORE the back stub, so for a one-period
  // Both schedule the BACK rate wins -- the opposite of the fixed path. Kept
  // in upstream's order for that reason.
  if (stub == StubLocation::Front || stub == StubLocation::Both) {
    const Expected<double> front =
        detail::stub_payment(curve, interp, stream, stream.schedule.periods.front());
    if (!front) {
      return unexpected(front.error());
    }
    amounts.front() = *front;
    first_coupon = 1;
  }
  if (stub == StubLocation::Back || stub == StubLocation::Both) {
    const Expected<double> back =
        detail::stub_payment(curve, interp, stream, stream.schedule.periods.back());
    if (!back) {
      return unexpected(back.error());
    }
    amounts.back() = *back;
    last_coupon = n - 1;
  }

  for (std::size_t i = first_coupon; i < last_coupon; ++i) {
    const CouponDates &period = stream.schedule.periods[i];

    const Expected<Date> index_start =
        advance_adjusted(period.reset, stream.rate.spot_offset, calendar);
    if (!index_start) {
      return unexpected(index_start.error());
    }
    const Expected<Date> unadjusted_maturity = advance(*index_start, stream.rate.maturity_interval);
    if (!unadjusted_maturity) {
      return unexpected(unadjusted_maturity.error());
    }
    const Expected<Date> index_maturity = adjust(*unadjusted_maturity, rate_bad_day, calendar);
    if (!index_maturity) {
      return unexpected(index_maturity.error());
    }

    // When the pay interval equals the maturity interval the rate is a zero
    // coupon one, and upstream re-derives the pay interval in DAYS from the
    // adjusted dates so the adjustments are reflected in it. Its comment:
    // "Otherwise we may get a par fixed rate with a very small stub, when in
    // fact we just want a simple LIBOR (zero-coupon) type rate."
    DateInterval pay_interval = stream.rate.pay_interval;
    if (stream.rate.pay_interval == stream.rate.maturity_interval) {
      pay_interval = DateInterval{static_cast<int>((*index_maturity - *index_start).count()),
                                  PeriodUnit::Days};
    }

    const Expected<double> amount = floating_cash_flow(
        curve, interp, *index_start, *index_maturity, pay_interval, period, stream.rate.day_count,
        stream.pay_day_count, stream.principal, stream.rate.spread);
    if (!amount) {
      return unexpected(amount.error());
    }
    amounts[i] = *amount;
  }

  std::vector<CashFlow> flows;
  flows.reserve(detail::layout(n, stream.principal_flows).count);
  if (stream.principal_flows.subtract_initial) {
    flows.push_back(CashFlow{stream.schedule.periods.front().accrual_start, -stream.principal});
  }
  for (std::size_t i = 0; i < n; ++i) {
    flows.push_back(CashFlow{stream.schedule.periods[i].pay, amounts[i]});
  }
  if (stream.principal_flows.add_final) {
    flows.back().amount += stream.principal;
  }
  return flows;
}

} // namespace cdsmodel
