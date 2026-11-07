#pragma once

// Port of upstream zr2fwd.c + zr2coup.c -- the market rates a zero curve
// implies. Two files and one idea: given a curve, what forward rate does it
// imply between two dates, and what coupon would price an instrument at par.
//
//     JpmcdsForwardFromZCurve      -> forward_from_curve
//     JpmcdsZerosToCouponsPoint    -> par_coupon_rate
//     JpmcdsZerosToCouponsPointAdj -> par_coupon_rate (one function, see below)
//
// Upstream's `Point` and `PointAdj` differ only in that the first calls the
// second with no bad-day adjustment and a "NO_WEEKENDS" holiday file. The port
// takes a convention and a calendar span, so the unadjusted case is
// `BadDayConv::None` with `Calendar::no_weekends()` and needs no second
// function. That is not a simplification of behaviour -- it is the same two
// arguments, named.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// The forward rate the curve implies from `start` to `end`. Replaces
/// `JpmcdsForwardFromZCurve`.
///
/// Three steps: discount both dates, divide, convert the ratio to a rate.
///
/// Upstream's comment explains why the dates are not ordered-checked: *"the
/// routine does not check to make sure that the start date comes before the
/// maturity date, since zero curve creation requires the calculation of
/// forwards which go backwards"*. So a backward forward is a legitimate
/// request here and the port does not check either.
///
/// ## A second site for UPSTREAM-19
///
/// Upstream divides with no guard at all:
///
/// ```c
/// forwardDiscount = endDiscount/startDiscount;
///
/// if (JpmcdsDiscountToRate(forwardDiscount, startDate, endDate,
///                       dayCountConv, basis, rate) == FAILURE)
/// ```
///
/// which is strictly less protection than `JpmcdsZCFwdDiscountFactor` in
/// zcall.c, where there is at least an `if (discount1 <= 0.0)` -- a broken
/// one (UPSTREAM-18), but a test. Here everything rests on
/// `JpmcdsDiscountToRate`, whose own guard is the one UPSTREAM-19 showed
/// cannot see NaN. Measured, continuous basis over 30 years:
///
/// ```
/// rate   disc(start)     disc(end)       fwd disc   rc        forward rate
/// 0.03   0.406302        0.394294        0.970446   SUCCESS   0.0304545
/// 24     1.20094e-313    4.94066e-324    4.11e-11   SUCCESS   2.43073e+10
/// 25     0               0               nan        SUCCESS   nan
/// -24    inf             inf             nan        SUCCESS   nan
/// ```
///
/// Both ends of the range produce a NaN rate reported as success: `0/0` where
/// the factors underflow and `inf/inf` where they overflow.
///
/// Recorded as a second site of UPSTREAM-19 rather than a new entry. The
/// mechanism is the same blind guard, the consequence is the same NaN-as-
/// success, and the fix was the same one line in `discount_to_rate`. Giving it
/// its own number would make the register count entry points rather than
/// defects.
///
/// **Cannot occur in the port.** `discount_to_rate` refuses a non-finite
/// discount factor, which was UPSTREAM-19's fix, so the NaN is rejected where
/// it is formed into a rate. No guard is added here on purpose: the invariant
/// lives at the conversion, and a second check would suggest the conversion
/// could not be trusted.
[[nodiscard]] inline std::optional<double> forward_from_curve(const ZeroCurve &curve,
                                                              InterpType interp, Date start,
                                                              Date end, DayCount day_count,
                                                              long basis) {
  const std::optional<double> at_start = discount_date(curve, start, interp);
  const std::optional<double> at_end = discount_date(curve, end, interp);
  if (!at_start || !at_end) {
    return std::nullopt;
  }
  // UPSTREAM-19, second site: upstream divides unguarded and lets a NaN
  // through. discount_to_rate rejects it.
  return discount_to_rate(*at_end / *at_start, start, end, day_count, basis);
}

/// The coupon that prices this instrument at par off the curve. Replaces
/// `JpmcdsZerosToCouponsPointAdj`, and with `BadDayConv::None` plus
/// `Calendar::no_weekends()` also `JpmcdsZerosToCouponsPoint`.
///
/// The arithmetic is three lines of upstream's file-local `JpmcdsCalcCoupon`:
///
/// ```c
/// JpmcdsCashFlowPV(cfl, zc, interpType, &coupons_PV);
/// JpmcdsDiscountDate(cfl->fArray[num_pts-1].fDate, zc, interpType, &last_PV);
/// *couponRate = (presentValue - last_PV) / coupons_PV;
/// ```
///
/// where the cash flow list carries a coupon of **1.0** -- so `coupons_PV` is
/// the present value of a unit coupon stream, the annuity factor, and the
/// quotient is the coupon that makes the instrument worth `presentValue`. The
/// `presentValue` passed in is the discount factor to the adjusted start date,
/// so for a spot-starting swap it is 1 and the result is the par rate.
///
/// Two guards are upstream's and are reproduced:
///
///   * the adjusted start date must not precede the curve's first date. Note
///     which date that is -- `JpmcdsCurveFirstDate` returns
///     `min(baseDate, points[0])`, so a curve whose first point precedes its
///     base date admits a start date before the base date too.
///   * a zero annuity factor is refused rather than divided by. Upstream's
///     message is *"coupons have zero present value"*, and it is the one
///     division in this file that is checked.
///
/// One thing upstream does that the port cannot: it passes the UNadjusted
/// `startDate` to `JpmcdsMakeCFL`, with the comment *"Don't pass startDateAdj,
/// because the routine will adjust that date itself"* -- while using the
/// ADJUSTED date for the present value. Both are reproduced, because the two
/// dates genuinely play different roles: the schedule is built from the
/// unadjusted date and discounted from the adjusted one.
[[nodiscard]] inline std::optional<double>
par_coupon_rate(const ZeroCurve &curve, InterpType interp, Date start, DateInterval interval,
                Date maturity, DayCount coupon_day_count, StubPosition stub_position,
                BadDayConv accrual_bad_day, BadDayConv pay_bad_day,
                std::span<const Calendar> calendars) {
  const std::optional<Date> adjusted_start = adjust(start, accrual_bad_day, calendars);
  if (!adjusted_start) {
    return std::nullopt;
  }
  if (*adjusted_start < curve.first_date()) {
    return std::nullopt; // upstream also reports whether a bad day moved it
  }

  // The schedule is built from the UNADJUSTED start; MakeCFL adjusts it.
  const CashFlowConventions conventions{.rate = 1.0,
                                        .day_count = coupon_day_count,
                                        .stub_length = StubLength::Short,
                                        .accrual_bad_day = accrual_bad_day,
                                        .pay_bad_day = pay_bad_day};
  const std::optional<std::vector<CashFlow>> flows = cash_flows_from_schedule(
      start, maturity, interval, conventions, stub_position, {}, calendars);
  if (!flows || flows->empty()) {
    return std::nullopt;
  }

  const std::optional<double> start_pv = discount_date(curve, *adjusted_start, interp);
  if (!start_pv) {
    return std::nullopt;
  }
  const std::optional<double> annuity = cash_flow_pv(curve, *flows, interp);
  if (!annuity) {
    return std::nullopt;
  }
  if (*annuity == 0.0) {
    return std::nullopt; // "coupons have zero present value"
  }
  const std::optional<double> final_pv = discount_date(curve, flows->back().date, interp);
  if (!final_pv) {
    return std::nullopt;
  }
  return (*start_pv - *final_pv) / *annuity;
}

} // namespace cdsmodel
