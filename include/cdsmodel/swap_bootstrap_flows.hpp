#pragma once

// Port of upstream zcswutil.c -- the cash flows of a swap benchmark, as the
// curve bootstrap sees them. Two functions, and the last thing in the way of
// `zcswap.c`.
//
//   JpmcdsZCGetSwapCFL      -> swap_cash_flows
//   JpmcdsZCGetSwapCouponDL -> swap_coupon_dates
//
// Both are declared in `zcprvt.h` -- a private header -- but both are exported
// symbols, so a caller outside the library can reach them.
//
// This is NOT the same job as stream_cash_flows.hpp. That builds a swap leg's
// coupons from a `CouponSchedule`, with stubs, resets, pay offsets and a
// floating index. This builds the much simpler thing the bootstrap needs: a
// unit-notional fixed bond whose coupons accrue from the value date, used as
// the instrument whose par price pins one curve point. Upstream keeps them
// separate and so does the port.
//
// ## The zero-rate special case
//
// `JpmcdsZCGetSwapCFL` opens with its own comment:
//
//     /* Bizarre rate==0.0 case */
//     if (rate==0.0)
//     {
//         cfl = JpmcdsNewEmptyCFL(1);    /* simply cash flow of 1.0 at date */
//         ...
//         return cfl;
//     }
//
// which is right -- a zero-coupon swap benchmark is a single redemption of 1.0
// at maturity -- and is reproduced. Note the `return cfl` rather than a jump
// to `done:`: at that point `status` is still FAILURE, so routing through the
// shared exit would free the list and return NULL. The early return is load
// bearing, not a shortcut.
//
// ## Deliberate difference: UPSTREAM-32
//
// The degenerate maturity is not given the same care as the degenerate rate.
// See the note on `swap_cash_flows`.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_schedule.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/swap_dates.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// How a benchmark's dates are moved off non-business days.
///
/// Upstream passes `TBadDayList *badDayList` and `long badDayConv` as two
/// unrelated parameters, and `JpmcdsZCAdjustDate` then branches on which is
/// PRESENT rather than consulting both (UPSTREAM-26). Grouping them says they
/// are one decision; the `optional` says which.
struct SwapBadDayRule {
  /// When present, dates are substituted through this table and the convention
  /// below is not consulted. Empty and absent are different inputs -- see
  /// UPSTREAM-26.
  /// D7: genuinely absent -- a rule with no substitution table is normal,
  /// not a failed attempt to build one.
  std::optional<std::span<const BadDayPair>> substitutions{};

  BadDayConv conv = BadDayConv::None;
};

/// Replaces `JpmcdsZCGetSwapCouponDL`.
///
/// The coupon dates of a swap benchmark: the payment dates from `value_date`
/// to `maturity`, business-day adjusted, then put through the substitution
/// table. The value date itself is NOT among them -- `pay_dates` drops it.
///
/// Upstream skips the business-day adjustment entirely when the convention is
/// `JPMCDS_BAD_DAY_NONE`. That is an optimisation, not a behaviour: `adjust`
/// with `None` returns its input. Reproduced as written so the two
/// implementations have the same shape, not because the branch is observable.
///
/// Note the ORDER, which is load-bearing: adjust first, substitute second. The
/// substitution table maps bad days to good ones, so running it before the
/// adjustment would hand the adjustment dates the table had already settled.
[[nodiscard]] inline Expected<std::vector<Date>>
swap_coupon_dates(Date value_date, Date maturity, DateInterval interval, bool stub_at_end,
                  const SwapBadDayRule &bad_day_rule, std::span<const Calendar> calendars) {
  const Expected<std::vector<Date>> dates = pay_dates(value_date, maturity, interval, stub_at_end);
  if (!dates) {
    return unexpected(dates.error());
  }
  std::vector<Date> adjusted = *dates;
  if (bad_day_rule.conv != BadDayConv::None) {
    const Expected<std::vector<Date>> moved =
        adjust_date_list(adjusted, bad_day_rule.conv, calendars);
    if (!moved) {
      return unexpected(moved.error());
    }
    adjusted = *moved;
  }
  if (bad_day_rule.substitutions) {
    adjusted = fix_bad_dates(*bad_day_rule.substitutions, adjusted);
  }
  return adjusted;
}

/// Replaces `JpmcdsZCGetSwapCFL`.
///
/// A unit-notional fixed bond: `rate * yearFraction` at each coupon date, with
/// the first period accruing from `value_date`, and 1.0 added to the last
/// payment. This is the instrument whose par price pins a curve point.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-32. Upstream gives the degenerate RATE a
/// special path and the degenerate MATURITY none at all. With
/// `maturity == value_date` and a non-zero rate:
///
///   * `JpmcdsNewDateList` returns ONE date;
///   * `JpmcdsNewPayDates` drops the first element and decrements the count,
///     so it returns ZERO;
///   * `JpmcdsNewEmptyCFL(0)` gives `fNumItems == 0` with `fArray == NULL`;
///   * the coupon loop does not run;
///   * and then
///
///         cfl->fArray[cfl->fNumItems-1].fAmount += 1.0;   /* add principal */
///
///     is `NULL[-1].fAmount += 1.0` -- a read-modify-WRITE through a
///     null-derived pointer, not merely a read like UPSTREAM-21.
///
/// Both sanitizers catch it, with different diagnostics: UBSan reports
/// "applying non-zero offset 18446744073709551600 to null pointer" at
/// zcswutil.c:96 -- that is -16, one `TCashFlow` -- and ASan reports a SEGV on
/// `0xfffffffffffffff8`, which is -8, the offset of `.fAmount` inside
/// `fArray[-1]`. Reachable from `zcswap.c:475`, which passes `zc->valueDate`
/// and `matDate` with no comparison between them anywhere in the file: the
/// same trigger as UPSTREAM-29, a benchmark maturing on the curve's value
/// date. See `defect_evidence u32 --unsafe`.
///
/// The port refuses an empty coupon-date list. The guard is on the OUTCOME
/// rather than on `maturity == value_date`, for the reason UPSTREAM-29 taught:
/// substitution and bad-day adjustment are also capable of emptying a list,
/// and the input test is the wrong test.
[[nodiscard]] inline Expected<std::vector<CashFlow>>
swap_cash_flows(Date value_date, Date maturity, bool stub_at_end, double rate,
                DateInterval interval, DayCount day_count, const SwapBadDayRule &bad_day_rule,
                std::span<const Calendar> calendars) {
  // Upstream's "bizarre rate==0.0 case": one redemption of 1.0, no coupons.
  if (rate == 0.0) {
    const Expected<Date> at =
        zc_adjust_date(maturity, bad_day_rule.substitutions, bad_day_rule.conv, calendars);
    if (!at) {
      return unexpected(at.error());
    }
    return std::vector<CashFlow>{CashFlow{*at, 1.0}};
  }

  const Expected<std::vector<Date>> dates =
      swap_coupon_dates(value_date, maturity, interval, stub_at_end, bad_day_rule, calendars);
  if (!dates) {
    return unexpected(dates.error());
  }
  // UPSTREAM-32: upstream carries on and writes to fArray[-1].
  if (dates->empty()) {
    return fail(ErrorKind::bad_argument, "the swap has no coupon dates");
  }

  std::vector<CashFlow> flows;
  flows.reserve(dates->size());
  Date previous = value_date;
  for (const Date coupon : *dates) {
    const Expected<double> year_frac = year_fraction(previous, coupon, day_count);
    if (!year_frac) {
      return unexpected(year_frac.error());
    }
    flows.push_back(CashFlow{coupon, rate * *year_frac});
    previous = coupon;
  }
  flows.back().amount += 1.0; // the principal
  return flows;
}

} // namespace cdsmodel
