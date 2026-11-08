#pragma once

// Port of upstream defaulted.c -- the coupon accrued up to a credit event,
// owed when a defaulted CDS is cash settled.
//
// 95 lines, one function, and it carries the most consequential defect found
// in this port: UPSTREAM-11 returns a NEGATIVE accrual, reporting SUCCESS, on
// realistic inputs. See the note on `default_accrual` below. This module
// therefore CORRECTS upstream rather than reproducing it, which makes it the
// second such module after the date list.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/fee_leg.hpp>

#include <chrono>
#include <optional>
#include <span>

namespace cdsmodel {

/// What is owed in accrued coupon when a defaulted CDS settles.
struct DefaultAccrual {
  /// Days from the start of the coupon period containing the credit event,
  /// through the event date inclusive.
  ///
  /// Upstream returns this as a `double` through an out-parameter even though
  /// it is always integral. A count of days is an integer here.
  long days = 0;

  /// `notional * coupon_rate * yearFraction(period start, event + 1 day)`.
  double amount = 0.0;
};

/// Accrued coupon owed at a credit event. Replaces `JpmcdsDefaultAccrual`.
///
/// `event_determination_date` is upstream's `edd`: the date the credit event
/// is determined, which is the date accrual stops. Accrual runs from the start
/// of the coupon period containing it, through the event date inclusive --
/// hence `event + 1 day` as the year-fraction end.
///
/// Nothing is owed when `trade_date < event_determination_date`: the trade
/// predates the determination, so there is no defaulted accrual to settle.
/// That returns a zero result rather than nullopt, matching upstream.
///
/// `cds` supplies the schedule and conventions. Upstream forces
/// `payAccOnDefault` and `protectStart` to TRUE regardless of what a caller
/// asks, and so does this -- a defaulted-accrual calculation only makes sense
/// for a contract that pays accrual on default. `cds.coupon_rate` and
/// `notional` are used for the amount; the fee leg is built only for its
/// schedule, which is why upstream's passing them into the leg builder has no
/// effect on the result.
///
/// Returns nullopt where upstream fails: a credit event before the contract
/// starts, or an event outside every coupon period.
///
/// ## UPSTREAM-11, corrected here
///
/// Upstream locates the coupon period by the TRADE date and then accrues to
/// the EVENT date:
///
/// ```c
/// if (fl->accStartDates[i] <= tradeDate && tradeDate < fl->accEndDates[i])
/// {
///     *accrualDays = edd + 1.0 - fl->accStartDates[i];
/// ```
///
/// It guards `edd >= startDate` but never `edd >= accStartDates[i]`. So once
/// the trade date crosses a coupon boundary that the event date did not, the
/// subtraction goes negative -- and it is returned with SUCCESS.
///
/// Measured, quarterly schedule from 20 March 2024, event on 1 August 2024:
///
/// ```
///   trade date      accrual days   amount
///   event + 0..45         43        11944.44
///   event + 50..120      -49       -13611.11
///   event + 365         -322       -89444.44
/// ```
///
/// The flip happens exactly when the trade date passes 20 September. Over a
/// settlement-lag grid (event dates across April to December, lags 0 to 120
/// days) 62% of 2,250 cases disagree with asking as of the event date itself,
/// and 60% are negative.
///
/// The decisive argument is not the sign but the dependence: **the coupon
/// accrued at a credit event cannot depend on when someone asks.** Upstream's
/// answer does. Settlement after a CDS auction routinely lands weeks after
/// the determination date, which crosses a quarterly coupon boundary often.
///
/// The correction is one date: locate the period containing
/// `event_determination_date`. Then `period start <= event` holds by
/// construction, the result is always positive, and it no longer varies with
/// the trade date. Nothing else changes -- the arithmetic, the conventions and
/// the `trade_date < event` gate are all upstream's.
///
/// One consequence worth stating, because it is a second behavioural change
/// from the same root cause: upstream FAILS when the trade date lies past the
/// final accrual end, since no period contains it. Locating by the event date
/// instead means a late settlement still returns what is owed. That is the
/// intended behaviour -- a debt does not lapse because the question was asked
/// late -- but it is a divergence, not just a sign fix.
[[nodiscard]] inline Expected<DefaultAccrual> default_accrual(const Cds &cds, Date trade_date,
                                                              Date event_determination_date,
                                                              double notional,
                                                              std::span<const Calendar> calendars) {
  if (trade_date < event_determination_date) {
    return DefaultAccrual{}; // nothing determined yet; upstream returns zeros
  }
  if (event_determination_date < cds.start) {
    return fail(ErrorKind::bad_argument, "the event determination date is before the trade start");
  }

  Cds terms = cds;
  terms.pay_accrual_on_default = true; // forced by upstream regardless
  terms.protect_start = true;          // likewise
  const Expected<FeeLeg> leg = cds_fee_leg(terms, notional, calendars);
  if (!leg) {
    return unexpected(leg.error());
  }

  for (const FeePayment &period : leg->payments) {
    // UPSTREAM-11: upstream tests `trade_date` here. Testing the event date is
    // the correction -- see the note above.
    if (period.accrual_start <= event_determination_date &&
        event_determination_date < period.accrual_end) {
      const Date accrual_end = event_determination_date + std::chrono::days{1};
      const Expected<double> fraction =
          year_fraction(period.accrual_start, accrual_end, cds.day_count);
      if (!fraction) {
        return unexpected(fraction.error());
      }
      // Association matters. Upstream computes the year fraction into the
      // output and then does `*defaultAccrual *= couponRate * notional`, so
      // the product is `fraction * (coupon_rate * notional)`. Writing it
      // left to right as `(fraction * coupon_rate) * notional` differs by one
      // ULP -- 3333.333333333333 against 3333.3333333333335 for a 30/360
      // stub -- which the differential suite caught as PORT-10.
      return DefaultAccrual{to_tdate(accrual_end) - to_tdate(period.accrual_start),
                            *fraction * (cds.coupon_rate * notional)};
    }
  }
  return fail(ErrorKind::bad_argument, "the credit event falls outside every coupon period");
}

} // namespace cdsmodel
