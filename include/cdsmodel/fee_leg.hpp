#pragma once

// Port of upstream feeleg.c -- the premium leg of a CDS: the coupons the
// protection buyer pays, plus the part-period accrual owed if default
// interrupts a coupon period.
//
// Two things make this the most intricate module in the slice.
//
// First, the accrual-on-default term is a second integral over the timeline,
// with its own closed form and its own Taylor expansion for small
// denominators -- a different expansion from the contingent leg's, with a
// different nesting of five terms. Both are reproduced in upstream's exact
// order, for the reason set out in contingent_leg.hpp: summing the same terms
// differently is algebraically identical and numerically is not.
//
// Second, upstream carries the schedule as THREE PARALLEL ARRAYS with one
// shared length:
//
//     int    nbDates;
//     TDate *accStartDates;
//     TDate *accEndDates;
//     TDate *payDates;
//
// Nothing ties them together, so a caller can hand over three arrays of
// different lengths and the library will read past the end of two of them.
// Here they are one vector of a three-field struct, which makes a mismatched
// schedule unrepresentable rather than merely discouraged.
//
// UPSTREAM-8 lives here too, at feeleg.c:101 and :108 -- the same
// `!price > THRESHOLD` precedence bug as the contingent leg. Resolved the same
// way and for the same measured reason: the port rejects a non-finite result
// rather than a proxy threshold. See the defect report.

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// What happens to the current period's accrual when default interrupts it.
/// Upstream's `TAccrualPayConv`.
enum class AccrualPayment {
  None, ///< `ACCRUAL_PAY_NONE` -- the period's coupon is simply lost
  All,  ///< `ACCRUAL_PAY_ALL` -- accrual to the default date is still due
};

/// One coupon period. Upstream keeps these as three parallel arrays.
struct FeePayment {
  Date accrual_start;
  Date accrual_end;
  Date pay;
};

/// The premium leg.
struct FeeLeg {
  std::vector<FeePayment> payments;
  double notional = 0.0;
  double coupon_rate = 0.0;
  DayCount day_count = DayCount::Act360;
  AccrualPayment accrual_pay = AccrualPayment::All;

  /// Whether default is observed at the start of the accrual day rather than
  /// its end. Upstream's `obsStartOfDay`, applied as a one-day offset because
  /// survival is computed at end of day.
  bool observe_start_of_day = true;
};

/// Present value of the accrual owed if default lands inside `[start, end]`.
///
/// Replaces `JpmcdsAccrualOnDefaultPVWithTimeLine`. `critical` is an
/// already-built timeline to truncate; pass an empty span to have one built
/// for the period, which is what upstream does when its pointer is NULL.
[[nodiscard]] inline std::optional<double>
accrual_on_default_pv(Date today, Date stepin, Date start, Date end, double amount,
                      const ZeroCurve &discount, const ZeroCurve &spread,
                      std::span<const Date> critical) {
  if (end <= start) {
    return std::nullopt;
  }

  const std::optional<std::vector<Date>> tl = critical.empty()
                                                  ? risky_timeline(start, end, discount, spread)
                                                  : truncate_timeline(critical, start, end);
  if (!tl) {
    return std::nullopt;
  }

  Date sub_start = std::max(stepin, start);
  // The accrual rate: the period's full coupon spread over its length.
  double t = static_cast<double>(to_tdate(end) - to_tdate(start)) / 365.0;
  const double acc_rate = amount / t;
  double s0 = spread.forward_zero_price(today, sub_start);
  double df0 = discount.forward_zero_price(today, std::max(today, sub_start));

  double pv = 0.0;
  for (std::size_t i = 1; i < tl->size(); ++i) {
    const Date node = (*tl)[i];
    if (node <= stepin) {
      continue;
    }
    const double s1 = spread.forward_zero_price(today, node);
    const double df1 = discount.forward_zero_price(today, node);

    // The half-day offsets are upstream's: accrual is measured to the middle
    // of the day on which default occurs.
    const double t0 =
        (static_cast<double>(to_tdate(sub_start)) + 0.5 - static_cast<double>(to_tdate(start))) /
        365.0;
    const double t1 =
        (static_cast<double>(to_tdate(node)) + 0.5 - static_cast<double>(to_tdate(start))) / 365.0;
    t = t1 - t0;

    const double lambda = std::log(s0) - std::log(s1);
    const double fwd_rate = std::log(df0) - std::log(df1);
    const double lambda_fwd_rate = lambda + fwd_rate + 1.0e-50;

    double this_pv = 0.0;
    if (std::fabs(lambda_fwd_rate) > 1e-4) {
      this_pv = lambda * acc_rate * s0 * df0 *
                ((t0 + t / (lambda_fwd_rate)) / (lambda_fwd_rate) -
                 (t1 + t / (lambda_fwd_rate)) / (lambda_fwd_rate)*s1 / s0 * df1 / df0);
    } else {
      // A different expansion from the contingent leg's: each term carries a
      // (t0 + k*t1) factor and the coefficients nest differently. Upstream's
      // order, term by term.
      const double a0 = lambda * s0 * df0 * acc_rate * 0.5;
      const double p1 = a0 * (t0 + t1);

      const double a1 = a0 * lambda_fwd_rate / 3.;
      const double p2 = -a1 * (t0 + 2. * t1);

      const double a2 = a1 * lambda_fwd_rate * .25;
      const double p3 = a2 * (t0 + 3. * t1);

      const double a3 = a2 * lambda_fwd_rate * .2;
      const double p4 = -a3 * (t0 + 4. * t1);

      const double a4 = a3 * lambda_fwd_rate / 6.;
      const double p5 = a4 * (t0 + 5. * t1);

      this_pv += p1;
      this_pv += p2;
      this_pv += p3;
      this_pv += p4;
      this_pv += p5;
    }

    pv += this_pv;
    s0 = s1;
    df0 = df1;
    sub_start = node;
  }
  return pv;
}

namespace detail {

/// Present value of one coupon, as at `today`. Replaces
/// `FeePaymentPVWithTimeLine`.
[[nodiscard]] inline std::optional<double>
fee_payment_pv(const FeeLeg &leg, const FeePayment &payment, Date today, Date stepin,
               const ZeroCurve &discount, const ZeroCurve &spread, std::span<const Date> timeline) {
  // Survival is computed at end of day, so observing at start of day means
  // stepping the date back one.
  const std::chrono::days obs_offset{leg.observe_start_of_day ? -1 : 0};

  if (payment.accrual_end <= stepin) {
    return 0.0;
  }

  const std::optional<double> acc_time =
      year_fraction(payment.accrual_start, payment.accrual_end, leg.day_count);
  if (!acc_time) {
    return std::nullopt;
  }
  const double amount = leg.notional * leg.coupon_rate * *acc_time;
  const double survival = spread.forward_zero_price(today, payment.accrual_end + obs_offset);
  const double discount_factor = discount.forward_zero_price(today, payment.pay);
  double pv = amount * survival * discount_factor;

  if (leg.accrual_pay == AccrualPayment::All) {
    const std::optional<double> accrual =
        accrual_on_default_pv(today, stepin + obs_offset, payment.accrual_start + obs_offset,
                              payment.accrual_end + obs_offset, amount, discount, spread, timeline);
    if (!accrual) {
      return std::nullopt;
    }
    pv += *accrual;
  }
  return pv;
}

/// Accrued interest at `as_of`. Replaces `FeeLegAI`.
///
/// Zero outside the schedule and zero exactly on an accrual start date;
/// otherwise the day count fraction from the enclosing period's start,
/// times coupon and notional.
[[nodiscard]] inline std::optional<double> fee_leg_accrued(const FeeLeg &leg, Date as_of) {
  if (leg.payments.empty()) {
    return std::nullopt;
  }
  if (as_of <= leg.payments.front().accrual_start || as_of >= leg.payments.back().accrual_end) {
    return 0.0;
  }

  // Upstream binary-searches the accrual start dates for an exact match or
  // the enclosing period.
  std::size_t enclosing = 0;
  bool exact = false;
  for (std::size_t i = 0; i < leg.payments.size(); ++i) {
    if (leg.payments[i].accrual_start == as_of) {
      exact = true;
      break;
    }
    if (leg.payments[i].accrual_start < as_of) {
      enclosing = i;
    }
  }
  if (exact) {
    return 0.0;
  }

  std::optional<double> accrual =
      year_fraction(leg.payments[enclosing].accrual_start, as_of, leg.day_count);
  if (!accrual) {
    return std::nullopt;
  }
  // Order matters: the fraction is scaled by (coupon * notional) as a product.
  return *accrual * (leg.coupon_rate * leg.notional);
}

} // namespace detail

/// Present value of the premium leg, as at `value_date`.
///
/// Replaces `JpmcdsFeeLegPV`. With `pay_accrued_at_start` the accrued
/// interest is deducted, giving a clean price.
///
/// On UPSTREAM-8: upstream's two guards here have the same precedence defect
/// as the contingent leg's, and are resolved the same way -- the port rejects
/// a result that is not finite rather than testing a proxy threshold. Note
/// upstream applies those guards only when there is more than one payment;
/// the port's check is uniform, which costs nothing because it is the result
/// that is tested rather than an input.
[[nodiscard]] inline std::optional<double> fee_leg_pv(const FeeLeg &leg, Date today, Date stepin,
                                                      Date value_date, const ZeroCurve &discount,
                                                      const ZeroCurve &spread,
                                                      bool pay_accrued_at_start) {
  if (leg.payments.empty() || value_date < today || stepin < today) {
    return std::nullopt;
  }

  // One timeline for the whole leg, truncated per payment. Upstream builds it
  // only when there is more than one payment, and otherwise lets each accrual
  // build its own.
  std::vector<Date> timeline;
  if (leg.payments.size() > 1) {
    const std::optional<std::vector<Date>> built = risky_timeline(
        leg.payments.front().accrual_start, leg.payments.back().accrual_end, discount, spread);
    if (!built) {
      return std::nullopt;
    }
    timeline = *built;
  }

  const Date maturity = leg.observe_start_of_day
                            ? leg.payments.back().accrual_end - std::chrono::days{1}
                            : leg.payments.back().accrual_end;
  if (today > maturity || stepin > maturity) {
    return 0.0;
  }

  double pv = 0.0;
  for (const FeePayment &payment : leg.payments) {
    const std::optional<double> this_pv =
        detail::fee_payment_pv(leg, payment, today, stepin, discount, spread, timeline);
    if (!this_pv) {
      return std::nullopt;
    }
    pv += *this_pv;
  }

  const double value_date_df = discount.forward_zero_price(today, value_date);
  double result = pv / value_date_df;

  if (pay_accrued_at_start) {
    const std::optional<double> accrued = detail::fee_leg_accrued(leg, stepin);
    if (!accrued) {
      return std::nullopt;
    }
    result -= *accrued;
  }

  if (!std::isfinite(result)) {
    return std::nullopt; // the real failure UPSTREAM-8's threshold stood in for
  }
  return result;
}

} // namespace cdsmodel
