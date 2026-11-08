#pragma once

// Port of upstream zcswap.c -- adding swap benchmarks to a zero curve. This is
// the bootstrap: the module every earlier slice has been clearing the way for.
//
//   JpmcdsZCAddSwaps          -> add_swaps
//   JpmcdsZCAddSwap           -> add_swap
//   JpmcdsZCValueFixFltSwap   -> value_fixed_float_swap
//   AddSwapFromPrevious       -> detail::add_swap_from_previous
//   JpmcdsObjFuncPVtheSwap    -> a lambda inside value_fixed_float_swap
//   CalcPV                    -> detail::swap_pv
//
// ## Five earlier findings converge here
//
// zcswap.c is where most of this register's reachability arguments end up, and
// porting it is what retires them:
//
//   * UPSTREAM-21/22 via `JpmcdsZCAddCashFlowList` at the par-float path;
//   * UPSTREAM-23 via `JpmcdsZCAddCFLPoints`, called on both paths;
//   * UPSTREAM-29 at lines 724, 743 and 744, which read
//     `floatDL->fArray[floatDL->fNumItems-1]` with no emptiness check;
//   * UPSTREAM-32 at line 475, `JpmcdsZCGetSwapCFL(zc->valueDate, matDate, ...)`.
//
// The last two share one trigger -- a benchmark maturing on the curve's own
// value date -- and `zcswap.c` compares `matDate` against `zc->valueDate`
// NOWHERE. Both are unreachable in the port because the functions they live in
// refuse to produce the degenerate value in the first place.
//
// ## Deliberate difference: UPSTREAM-33
//
// The root finder is pointed at the wrong curve point whenever the benchmark
// is not the longest instrument so far. See the note on
// `value_fixed_float_swap`.
//
// ## The solver settings, and a quirk they switch off
//
// Upstream's constants are reproduced exactly:
//
//     INITIAL_X_STEP   1e-8        X_TOLERANCE   DBL_MAX
//     INITIAL_F_DERIV  0           F_TOLERANCE   1e-11
//     LOWER_BOUND      -0.99       MAX_ITERATIONS 50
//     UPPER_BOUND      100.0
//
// `X_TOLERANCE = DBL_MAX` is an idiom, not a mistake. Brent's acceptance test
// is a conjunction -- `|f| <= fTol && |dx| <= xTol` -- so DBL_MAX says "judge
// convergence on f alone". It also switches OFF the quirk recorded in
// root_find.hpp: the initial guess is accepted when `|y0| <= fTol` AND the
// guess lies within `x_tolerance` of either bound, which is proximity standing
// in for a convergence test it cannot yet perform. With DBL_MAX that
// disjunction is always true and acceptance reduces to `|y0| <= fTol`, which
// is the sensible rule. The one caller that sets this parameter sets it to the
// value that neutralises the quirk.
//
// ## Three dead locals
//
// `JpmcdsZCAddSwaps` declares `TSwapDates *newDates` ("Swap dates w/
// synthetics"), `double *newRates`, and `TBoolean useFastZC`. None is ever
// read; `newDates` is never even freed, and `newRates` is freed at `done:`
// while provably still NULL. A synthetic-swap feature was removed and its
// variables stayed. Not reproduced.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_bootstrap.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/float_rate.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/stream_cash_flows.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/swap_bootstrap_flows.hpp>
#include <cdsmodel/swap_dates.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Upstream's root-finder settings for the swap solve, named rather than
/// `#define`d. See the header note on `x_tolerance`.
[[nodiscard]] inline RootFindOptions swap_solve_options(double guess) noexcept {
  RootFindOptions opt;
  opt.guess = guess;
  opt.bound_lo = -0.99;
  opt.bound_hi = 100.0;
  opt.max_iterations = 50;
  opt.initial_x_step = 1.0e-8;
  opt.initial_f_derivative = 0.0;
  opt.x_tolerance = std::numeric_limits<double>::max();
  opt.f_tolerance = 1.0e-11;
  return opt;
}

/// What a swap benchmark strip is quoted with.
///
/// Upstream passes fourteen positional parameters to `JpmcdsZCAddSwaps`, of
/// which seven are the conventions below. Grouped by what they describe.
struct SwapConventions {
  long fixed_frequency = 2;
  long float_frequency = 2;
  DayCount fixed_day_count = DayCount::Act360;
  DayCount float_day_count = DayCount::Act360;
  InterpType interp = InterpType::FlatForwards;

  /// Upstream's `TBadDayAndStubPos`, a bad-day convention and a stub position
  /// packed into one long and split by `JpmcdsBadDayAndStubPosSplit`.
  BadDayConv bad_day = BadDayConv::None;
  StubPosDefault stub_position = StubPosDefault::Auto;
};

namespace detail {

/// Replaces the static `AddSwapFromPrevious`.
///
/// The optimisation: if the new swap is the previous one plus a single coupon
/// and shares its year fractions, the new discount factor follows in closed
/// form instead of a root solve. Upstream derives it in a comment block, and
/// the derivation is worth keeping:
///
///     price   = discount[n] + couponRate * sum(discount[i]*yearFrac[i])
///     sumDY   = (priceOld - discount[n]) / rateOld
///     discount[n] = (priceNew - rateNew*sumDY) / (1 + rateNew*yearFrac[n])
///
/// Upstream divides by `rateOld` with no check and says so: *"Here we assume
/// that calling routine has already checked that rateOld != 0."* It also reads
/// `zc->discount[zc->numItems-1]` without checking the curve is non-empty.
/// Both preconditions hold at its single call site. They are checked here
/// anyway -- this is a two-comparison cost to make a function safe on its own
/// terms rather than on its caller's.
[[nodiscard]] inline bool add_swap_from_previous(ZeroCurveBuilder &curve, Date date_new,
                                                 double rate_new, double price_new, Date date_old,
                                                 double rate_old, double price_old,
                                                 DayCount day_count) {
  if (curve.empty() || rate_old == 0.0) {
    return false; // upstream's two documented, unchecked preconditions
  }
  const Expected<double> year_frac = year_fraction(date_old, date_new, day_count);
  if (!year_frac) {
    return false;
  }
  const double divisor = 1.0 + rate_new * *year_frac;
  if (is_almost_zero(divisor)) {
    return false; // upstream: "Rate implies zero discount factor"
  }
  const std::span<const ZeroCurveBuilder::Point> points = curve.points();
  const double sum_dy = (price_old - points.back().discount) / rate_old;
  const double discount = (price_new - rate_new * sum_dy) / divisor;
  if (discount <= 0.0) {
    // Upstream's message is the honest one: "The swap rates may be
    // inconsistent with one another." This is the classical case where no
    // consistent zero curve exists, not a defect.
    return false;
  }
  return curve.add_discount_factor(date_new, discount);
}

/// The present value of the swap. Replaces the static `CalcPV`.
///
/// `price - 1.0` is upstream's: the fixed leg carries notional +1 and the
/// floating leg -1, so `fixed + float` is the swap's value and the root is
/// where it equals `1 - price`.
[[nodiscard]] inline Expected<double> swap_pv(const ZeroCurve &estimation_curve, InterpType interp,
                                              std::span<const CashFlow> fixed_flows,
                                              const FloatStream &float_stream, double price,
                                              BadDayConv rate_bad_day, const Calendar &calendar,
                                              const std::optional<ZeroCurve> &discount_curve) {
  const Expected<std::vector<CashFlow>> float_flows =
      float_stream_cash_flows(estimation_curve, interp, float_stream, rate_bad_day, calendar);
  if (!float_flows) {
    return unexpected(float_flows.error());
  }
  // Upstream reassigns its own parameter: `if (discZC == NULL) discZC = zeroCurve;`
  const ZeroCurve &discounting = discount_curve ? *discount_curve : estimation_curve;
  const Expected<double> float_pv = cash_flow_pv(discounting, *float_flows, interp);
  const Expected<double> fixed_pv = cash_flow_pv(discounting, fixed_flows, interp);
  if (!float_pv || !fixed_pv) {
    return unexpected(!float_pv ? float_pv.error() : (fixed_pv.error()));
  }
  return *fixed_pv + *float_pv + price - 1.0;
}

} // namespace detail

/// Replaces `JpmcdsZCValueFixFltSwap`: solve for the zero rate that prices one
/// swap benchmark at `price`, valuing the floating leg explicitly against
/// `discount_curve`.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-33. Upstream adds a placeholder point and
/// then solves into `&zc->rate[zc->numItems-1]`:
///
/// ```c
/// if (JpmcdsZCAddRate(zc, maxDate, rate) == FAILURE) goto done;
/// ...
/// JpmcdsRootFindBrent(..., &zc->rate[zc->numItems-1])
/// ```
///
/// which identifies the point just added only when that point is the LATEST in
/// the curve. `JpmcdsZCAddRateAndDiscount` inserts in sorted position, not at
/// the end, so for a benchmark inside an existing curve the solver varies a
/// different, already-solved point and leaves the new one at the placeholder.
///
/// Measured: with the benchmark inside the curve, the solve FAILS in every one
/// of 1,400 swept combinations -- varying a distant zero rate barely moves a
/// shorter swap's present value, so Brent cannot bracket a root. Positive
/// controls with the benchmark beyond the curve's end succeed, so the failures
/// are the misdirection and not the fixture. I tried and FAILED to produce a
/// silently wrong curve; the observable consequence is a misleading
/// "Root finder failed" where the real problem is the benchmark's position.
///
/// Here the index of the added point is looked up and solved for by index, so
/// the solve targets what it added regardless of order.
[[nodiscard]] inline bool value_fixed_float_swap(ZeroCurveBuilder &curve,
                                                 const ZeroCurve &discount_curve, double price,
                                                 Date maturity, double rate,
                                                 const SwapConventions &conventions,
                                                 const Calendar &calendar) {
  const Expected<DateInterval> fixed_interval = frequency_to_interval(conventions.fixed_frequency);
  const Expected<DateInterval> float_interval = frequency_to_interval(conventions.float_frequency);
  if (!fixed_interval || !float_interval) {
    return false;
  }
  // Upstream: "make sure that the pay interval that is contained in the
  // floating rate is bigger than the maturity interval or otherwise
  // JpmcdsZerosToCouponsPoint will not calculate a basic *IBOR rate".
  const DateInterval pay_interval{24, PeriodUnit::Months};

  const std::vector<Calendar> calendars{calendar};

  const Expected<bool> fixed_end_stub =
      is_end_stub(curve.value_date(), maturity, *fixed_interval, conventions.stub_position);
  if (!fixed_end_stub) {
    return false;
  }
  const CouponConventions coupon_conventions{
      .pay = conventions.bad_day, .accrual = conventions.bad_day, .reset = conventions.bad_day};
  const Expected<CouponSchedule> fixed_schedule =
      coupon_schedule({.start = curve.value_date(),
                       .maturity = maturity,
                       .coupon_interval = *fixed_interval,
                       .stub_position = *fixed_end_stub ? StubPosition::Back : StubPosition::Front},
                      coupon_conventions, calendar);
  if (!fixed_schedule) {
    return false;
  }

  // Notional +1 on the fixed side, and the same rate for stubs and regular
  // periods -- upstream passes `rate, rate, rate`.
  const Expected<std::vector<CashFlow>> fixed_flows =
      fixed_stream_cash_flows({.schedule = *fixed_schedule,
                               .principal = 1.0,
                               .pay_day_count = conventions.fixed_day_count,
                               .first_coupon_rate = rate,
                               .final_coupon_rate = rate,
                               .fixed_rate = rate});
  if (!fixed_flows) {
    return false;
  }

  const Expected<bool> float_end_stub =
      is_end_stub(curve.value_date(), maturity, *float_interval, conventions.stub_position);
  if (!float_end_stub) {
    return false;
  }
  const Expected<CouponSchedule> float_schedule =
      coupon_schedule({.start = curve.value_date(),
                       .maturity = maturity,
                       .coupon_interval = *float_interval,
                       .stub_position = *float_end_stub ? StubPosition::Back : StubPosition::Front},
                      coupon_conventions, calendar);
  if (!float_schedule) {
    return false;
  }

  // Notional -1 on the floating side. No stub-rate fields and no `soFar`
  // values: upstream sets them (0.0 and 1.0) and never reads them -- see
  // stream_cash_flows.hpp, where the drop is proven against upstream.
  const FloatStream float_stream{.schedule = *float_schedule,
                                 .principal = -1.0,
                                 .pay_day_count = conventions.float_day_count,
                                 .rate = {.maturity_interval = *float_interval,
                                          .pay_interval = pay_interval,
                                          .day_count = conventions.float_day_count,
                                          .spot_offset = calendar_day_offset(0),
                                          .spread = 0.0,
                                          .weight = 1.0}};

  // The furthest-reaching date. UPSTREAM-29 lives at these three reads
  // upstream, where the schedule can be empty; here it cannot be.
  const CouponDates &last = float_schedule->periods.back();
  const Expected<Date> unadjusted_index_maturity = advance(last.reset, *float_interval);
  if (!unadjusted_index_maturity) {
    return false;
  }
  const Expected<Date> index_maturity =
      adjust(*unadjusted_index_maturity, conventions.bad_day, calendars);
  if (!index_maturity) {
    return false;
  }
  const Date max_date = std::max(*index_maturity, last.pay);
  const Date min_date = std::min(*index_maturity, last.pay);

  // Upstream's comment: pricing the floating side depends on both the pay date
  // and the index maturity, so the later one gets the solved point and the
  // earlier one is interpolated afterwards.
  if (!curve.add_rate(max_date, rate)) {
    return false;
  }
  // UPSTREAM-33: upstream assumes this is the last index. It need not be.
  const std::optional<std::size_t> solved_index = curve.find_exact(max_date);
  if (!solved_index) {
    return false;
  }

  const Expected<ZeroCurve> snapshot = curve.to_zero_curve();
  if (!snapshot) {
    return false;
  }

  // The objective: write the guess into the snapshot's solved point and price.
  // Upstream mutates `params->zeroCurve->fArray[last].fRate` directly; here the
  // snapshot is rebuilt from a builder whose rate is set, so the invariant
  // between rate and discount cannot drift.
  ZeroCurveBuilder trial = curve;
  const auto objective = [&](double guess) -> Expected<double> {
    if (!trial.set_rate(*solved_index, guess)) {
      return fail(ErrorKind::bad_argument, "the trial rate could not be stored on the curve");
    }
    const Expected<ZeroCurve> estimation = trial.to_zero_curve();
    if (!estimation) {
      return unexpected(estimation.error());
    }
    return detail::swap_pv(*estimation, conventions.interp, *fixed_flows, float_stream, price,
                           conventions.bad_day, calendar, discount_curve);
  };

  const Expected<double> solved = root_find_brent(objective, swap_solve_options(rate));
  if (!solved) {
    return false;
  }
  if (!curve.set_rate(*solved_index, *solved)) {
    return false;
  }

  // Zero rates at every cash-flow date, so a benchmark re-priced off this
  // curve comes back to par whatever interpolation the pricer uses.
  const Expected<ZeroCurve> solved_curve = curve.to_zero_curve();
  if (!solved_curve) {
    return false;
  }
  const Expected<std::vector<CashFlow>> float_flows = float_stream_cash_flows(
      *solved_curve, conventions.interp, float_stream, conventions.bad_day, calendar);
  if (!float_flows) {
    return false;
  }
  const Expected<std::vector<CashFlow>> swap_flows = merge_cash_flows(*fixed_flows, *float_flows);
  if (!swap_flows) {
    return false;
  }
  if (!add_cash_flow_points(curve, *swap_flows, curve.value_date(), conventions.interp)) {
    return false;
  }

  // "If we are adjusting for bad days, the index maturity date is not
  // necessarily the same as the pay date. Add another zero point to help
  // guarantee that the benchmarks price out to par."
  if (min_date != max_date) {
    const Expected<double> at_min = curve.interpolate(min_date, conventions.interp);
    if (!at_min) {
      return false;
    }
    if (!curve.add_rate(min_date, *at_min)) {
      return false;
    }
  }
  return true;
}

/// Replaces `JpmcdsZCAddSwap`: add one swap benchmark.
///
/// With no discount curve the floating leg is assumed to price at par, so the
/// benchmark reduces to a fixed bond and the solve is the cash-flow-list
/// bootstrap. With one, the floating leg is valued explicitly.
[[nodiscard]] inline bool
add_swap(ZeroCurveBuilder &curve, const std::optional<ZeroCurve> &discount_curve, double price,
         Date maturity, bool on_cycle, double rate, const SwapConventions &conventions,
         std::optional<std::span<const BadDayPair>> bad_days, const Calendar &calendar) {
  if (discount_curve) {
    return value_fixed_float_swap(curve, *discount_curve, price, maturity, rate, conventions,
                                  calendar);
  }

  const Expected<DateInterval> interval = frequency_to_interval(conventions.fixed_frequency);
  if (!interval) {
    return false;
  }
  // An on-cycle maturity means counting forward from the value date, so the
  // stub -- if the schedule needs one -- is at the end.
  bool end_stub = true;
  if (!on_cycle) {
    const Expected<bool> detected =
        is_end_stub(curve.value_date(), maturity, *interval, conventions.stub_position);
    if (!detected) {
      return false;
    }
    end_stub = *detected;
  }

  const std::vector<Calendar> calendars{calendar};
  const SwapBadDayRule rule{.substitutions = bad_days, .conv = conventions.bad_day};

  // UPSTREAM-32 lives in this call upstream, with matDate == valueDate.
  const Expected<std::vector<CashFlow>> flows =
      swap_cash_flows(curve.value_date(), maturity, end_stub, rate, *interval,
                      conventions.fixed_day_count, rule, calendars);
  if (!flows) {
    return false;
  }
  const Expected<Date> adjusted_maturity =
      zc_adjust_date(maturity, bad_days, conventions.bad_day, calendars);
  if (!adjusted_maturity) {
    return false;
  }
  if (!add_cash_flow_list(curve, *flows, price, *adjusted_maturity, conventions.interp)) {
    return false;
  }
  // "Since JpmcdsZCInterpolate does not support linear forwards, we switch to
  // linear interp here if linear fwds was specified." The port has no linear
  // forwards in InterpType, so there is nothing to switch.
  return add_cash_flow_points(curve, *flows, curve.value_date(), conventions.interp);
}

/// Replaces `JpmcdsZCAddSwaps`: add a strip of swap benchmarks.
///
/// Upstream documents the two mutually exclusive ways to give bad days and
/// then CHECKS the exclusivity here -- which is notable, because
/// `JpmcdsZCAdjustDate` downstream silently assumes it (UPSTREAM-26). The
/// port's `std::optional` table plus a convention carries the same meaning in
/// the type, so the check is a consequence rather than a rule to remember:
/// when a table is supplied the maturity dates must already be adjusted, and
/// when a convention is supplied they must not be.
[[nodiscard]] inline bool add_swaps(ZeroCurveBuilder &curve,
                                    const std::optional<ZeroCurve> &discount_curve,
                                    std::span<const Date> maturities, std::span<const double> rates,
                                    const SwapConventions &conventions,
                                    std::optional<std::span<const BadDayPair>> bad_days,
                                    const Calendar &calendar) {
  if (maturities.size() != rates.size()) {
    return false; // upstream takes one count for two arrays
  }
  if (bad_days && conventions.bad_day != BadDayConv::None) {
    // Upstream's own check: "Bad days can be defined either by badDayList or
    // badDayConv, but not both."
    return false;
  }

  const std::vector<Calendar> calendars{calendar};
  const Expected<std::vector<SwapDate>> dates =
      bad_days ? swap_dates_from_adjusted(curve.value_date(), conventions.fixed_frequency,
                                          maturities, bad_days)
               : swap_dates_from_original(curve.value_date(), conventions.fixed_frequency,
                                          maturities, bad_days, conventions.bad_day, calendars);
  if (!dates) {
    return false;
  }

  bool one_already_added = false;
  for (std::size_t i = 0; i < dates->size(); ++i) {
    const SwapDate &swap = (*dates)[i];
    const std::span<const ZeroCurveBuilder::Point> points = curve.points();
    const bool beyond = !points.empty() && swap.adjusted > points.back().date;

    // The closed-form optimisation, available only when this swap is the
    // previous one plus a coupon. "Note linear forwards not OK because they
    // must include intermed. fwds" -- the port has no linear forwards.
    if (beyond && one_already_added && !discount_curve && i > 0 && rates[i - 1] != 0.0 &&
        (*dates)[i - 1].adjusted == points.back().date &&
        swap.previous == (*dates)[i - 1].original && swap.on_cycle) {
      if (!detail::add_swap_from_previous(curve, swap.adjusted, rates[i], 1.0,
                                          (*dates)[i - 1].adjusted, rates[i - 1], 1.0,
                                          conventions.fixed_day_count)) {
        return false;
      }
      continue;
    }

    if (!add_swap(curve, discount_curve, 1.0, swap.original, swap.on_cycle, rates[i], conventions,
                  bad_days, calendar)) {
      return false;
    }
    one_already_added = true;
  }
  return true;
}

} // namespace cdsmodel
