#pragma once

// Port of upstream contingentleg.c + cx.c -- the protection leg of a CDS,
// and the first module in this project that computes a PRICE. cx.c supplies
// the constructor; contingentleg.c the pricing.
//
// The integral is
//
//     PV = integral of LGD(t) . Z(t) . dS/dt  dt
//
// where S is the survival probability, Z the discount factor and LGD the loss
// given default. Assuming flat forwards on both curves between timeline
// points, which the timeline is constructed to guarantee, each segment has an
// exact closed form and the integral is a finite sum.
//
// This is where the bit-exactness question from zero_curve.hpp genuinely
// reopens: the result ACCUMULATES over the timeline, and accumulation is
// where a compiler is most free to reassociate, vectorise or contract
// differently between the C and C++ front ends. So the arithmetic below
// preserves upstream's expression order and its loop order, and the
// differential suite checks the answer bit-for-bit rather than to a
// tolerance.

#include <cdsmodel/date.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <vector>

namespace cdsmodel {

/// When a protection payment is made. Upstream's `TProtPayConv`.
enum class ProtectionPayment {
  AtDefault,  ///< `PROT_PAY_DEF` -- paid when default happens
  AtMaturity, ///< `PROT_PAY_MAT` -- paid at the leg's maturity
};

/// The protection leg. Upstream's `TContingentLeg`.
struct ContingentLeg {
  Date start;
  Date end;
  double notional = 0.0;
  ProtectionPayment pay = ProtectionPayment::AtDefault;

  /// Whether protection covers `start` itself.
  ///
  /// The one-day offset this implies is applied in TWO places, in two
  /// different upstream files, and they are not alternatives:
  ///
  ///   * `JpmcdsContingentLegMake` (cx.c) stores `startDate - 1`, because
  ///     `startDate` means "protection begins at the end of this day". That
  ///     belongs to leg CONSTRUCTION -- see `cds_contingent_leg`.
  ///   * `JpmcdsContingentLegPV` (contingentleg.c) subtracts the same offset
  ///     from the step-in and trade dates when clamping the integration
  ///     start. That is what the code below does.
  ///
  /// An earlier revision of this comment claimed the second replaced the
  /// first. It does not, and the differential suite could not catch the error
  /// because it fed both sides a pre-built leg -- so construction was never
  /// compared. `cds_differential_test` compares composed prices and found it.
  bool protect_start = true;
};

/// Build a protection leg. Replaces `JpmcdsContingentLegMake` (cx.c).
///
/// `protect_start` shifts the stored start date back by one day, because
/// `start` means "protection begins at the END of this day" -- the convention
/// the struct comment above records, and the reason that offset appears in two
/// upstream files without being a duplicate.
///
/// Upstream validates with `REQUIRE (p->endDate > p->startDate)` AFTER the
/// shift, which has a consequence worth stating and testing: `protect_start`
/// decides whether a ZERO-LENGTH leg is accepted. With it, the stored start is
/// `start - 1` and `end == start` passes; without it, the same input is
/// refused. Measured on both sides, and reproduced -- it is not an accident of
/// ordering but the right answer, since protecting on the start date makes a
/// single-day contract meaningful.
///
/// One difference, and it is the type's rather than a decision: upstream's
/// shift is plain `TDate` arithmetic with no lower bound, so
/// `JpmcdsContingentLegMake(0, ...)` with `protectStart` stores a start date of
/// **-1** -- a negative TDate, which no valid date is. Measured. `Date` is a
/// `std::chrono::sys_days`, so subtracting a day from the epoch is simply an
/// earlier date rather than an invalid one, and there is no sentinel to
/// collide with. The port therefore has nothing to refuse here and does not
/// pretend otherwise.
[[nodiscard]] inline Expected<ContingentLeg> make_contingent_leg(Date start, Date end,
                                                                 double notional,
                                                                 ProtectionPayment pay,
                                                                 bool protect_start) {
  const Date stored_start = protect_start ? start - std::chrono::days{1} : start;
  if (!(end > stored_start)) {
    return fail(ErrorKind::bad_argument, "the leg end must be strictly after its start");
  }
  return ContingentLeg{stored_start, end, notional, pay, protect_start};
}

namespace detail {

/// One segment of the integral, protection paid at default.
///
/// Upstream's `onePeriodIntegral`, including the block its own comment labels
/// the "Markit Proposed Fix": the divisions by the segment length cancel
/// algebraically, so they are removed, and a Taylor expansion replaces the
/// closed form when the denominator is small enough to amplify rounding.
///
/// Two upstream quirks preserved on purpose:
///   * `1.0e-50` is added to the denominator so it is never exactly zero.
///     A fudge, but removing it changes results.
///   * the segment length `t` is computed and never used. After the Markit
///     fix absorbed the divisions it became dead. Not reproduced here, since
///     an unused variable is not behaviour -- but worth knowing it is in the
///     original, where it would warn under `-Wall`.
[[nodiscard]] inline Expected<double> one_period_integral(Date today, Date start, Date end,
                                                          const ZeroCurve &discount,
                                                          const ZeroCurve &spread,
                                                          double recovery) {
  if (today > end) {
    return 0.0;
  }

  const Expected<std::vector<Date>> timeline = risky_timeline(start, end, discount, spread);
  if (!timeline) {
    return unexpected(timeline.error());
  }

  // Note the asymmetry, which is upstream's: the survival factor is taken to
  // `start` directly while the discount factor is taken to `max(today, start)`.
  double s1 = spread.forward_zero_price(today, start);
  double df1 = discount.forward_zero_price(today, std::max(today, start));
  const double loss = 1.0 - recovery;

  double pv = 0.0;
  for (std::size_t i = 1; i < timeline->size(); ++i) {
    const double s0 = s1;
    const double df0 = df1;
    s1 = spread.forward_zero_price(today, (*timeline)[i]);
    df1 = discount.forward_zero_price(today, (*timeline)[i]);

    // lambda and fwdRate are the integrals of the hazard and short rates over
    // the segment; the divisions by segment length cancel in the result.
    const double lambda = std::log(s0) - std::log(s1);
    const double fwd_rate = std::log(df0) - std::log(df1);
    const double lambda_fwd_rate = lambda + fwd_rate + 1.0e-50;

    double this_pv = 0.0;
    if (std::fabs(lambda_fwd_rate) > 1.e-4) {
      this_pv = loss * lambda / lambda_fwd_rate * (1.0 - std::exp(-lambda_fwd_rate)) * s0 * df0;
    } else {
      // Small denominator: expand instead of dividing by it. Terms are
      // summed in upstream's order, which matters for the last bits.
      const double t0 = loss * lambda * s0 * df0;
      const double t1 = -t0 * lambda_fwd_rate * .5;
      const double t2 = -t1 * lambda_fwd_rate / 3.;
      const double t3 = -t2 * lambda_fwd_rate * .25;
      const double t4 = -t3 * lambda_fwd_rate * .2;
      this_pv += t0;
      this_pv += t1;
      this_pv += t2;
      this_pv += t3;
      this_pv += t4;
    }
    pv += this_pv;
  }
  return pv;
}

/// One segment with the payment made at a fixed date. Upstream's
/// `onePeriodIntegralAtPayDate`, which its own comment calls trivial.
[[nodiscard]] inline Expected<double> one_period_integral_at_pay_date(Date today, Date start,
                                                                      Date end, Date pay_date,
                                                                      const ZeroCurve &discount,
                                                                      const ZeroCurve &spread,
                                                                      double recovery) {
  if (end <= start) {
    return fail(ErrorKind::bad_argument, "end must be strictly after start");
  }
  if (today > end) {
    return 0.0;
  }
  const double s0 = spread.forward_zero_price(today, start);
  const double s1 = spread.forward_zero_price(today, end);
  const double df = discount.forward_zero_price(today, pay_date);
  const double loss = 1.0 - recovery;
  return (s0 - s1) * df * loss;
}

} // namespace detail

/// Present value of the protection leg. Replaces `JpmcdsContingentLegPV`.
///
/// On UPSTREAM-8, now decided. Upstream guards the computation with
///
///     if (!JpmcdsZeroPrice(curve, endDate) > JPMCDS_LOG0_THRESHOLD)
///
/// which, because `!` binds tighter than `>`, rejects only an exactly zero
/// price rather than one too small to take a logarithm of. The threshold
/// `1e-100` was a PROXY for "the arithmetic will misbehave".
///
/// This port checks the thing the proxy stood for: it computes, and rejects a
/// result that is not finite. That is option 3 of the three recorded in the
/// defect report, chosen because it is testable rather than guessed, and
/// because implementing the stated intent would make the port refuse to price
/// cases upstream prices correctly.
///
/// Measured consequence: for survival probabilities down to about 1e-300 the
/// logarithms stay finite and the result is the correct limit,
/// `notional * (1 - recovery)`. Only an exactly zero survival probability
/// produces `log(0) = -inf` and hence a NaN, and that is the one case both
/// upstream and this port reject. So the port and upstream agree everywhere,
/// and the port's guard is honest about why.
[[nodiscard]] inline Expected<double> contingent_leg_pv(const ContingentLeg &leg, Date today,
                                                        Date value_date, Date stepin_date,
                                                        const ZeroCurve &discount,
                                                        const ZeroCurve &spread, double recovery) {
  const int offset = leg.protect_start ? 1 : 0;
  Date start = std::max(leg.start, stepin_date - std::chrono::days{offset});
  start = std::max(start, today - std::chrono::days{offset});

  // Assigned from the switch through a lambda rather than declared-then-set:
  // `Expected` has no empty state to declare, which is the point of it.
  const Expected<double> leg_pv = [&]() -> Expected<double> {
    switch (leg.pay) {
    case ProtectionPayment::AtMaturity:
      return detail::one_period_integral_at_pay_date(today, start, leg.end, leg.end, discount,
                                                     spread, recovery);
    case ProtectionPayment::AtDefault:
      return detail::one_period_integral(today, start, leg.end, discount, spread, recovery);
    }
    return fail(ErrorKind::unsupported, "unrecognised protection payment timing");
  }();
  if (!leg_pv) {
    return unexpected(leg_pv.error());
  }

  // Computed as at `today`; restate at the value date.
  const double pv = *leg_pv * leg.notional;
  const double value_date_df = discount.forward_zero_price(today, value_date);
  const double result = pv / value_date_df;

  if (!std::isfinite(result)) {
    return fail(ErrorKind::bad_argument, "the contingent leg integral is not finite");
  }
  return result;
}

} // namespace cdsmodel
