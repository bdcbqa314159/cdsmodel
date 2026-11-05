#pragma once

// Port of upstream cfinanci.cpp -- converting between an interest rate and a
// discount factor, and deciding which rates are valid at all.
//
// A leaf: its only calls outside itself are to the error log and the date
// formatter, neither of which the port has. And it is the prerequisite for
// the whole interest-rate curve stack -- `zcall.c`, `zcswap.c`, `zr2fwd.c`
// and `interpc.c` all go through it.
//
// It is also where "basis" is actually DEFINED, which is worth saying plainly
// because the field is a `double` carrying five different meanings:
//
//     0     simple interest
//     1-365 compounding periods per year
//    -2     the "rate" IS a discount factor
//   512     a discount rate rather than an interest rate
//  5000     continuous compounding
//
// Five branches, one field, no type. The port keeps the `double` because it
// is a published contract -- `TCurve::fBasis` and every function here take it
// -- but names the sentinels so a call site reads as something other than a
// magic number.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <cfloat>
#include <cmath>
#include <optional>

namespace cdsmodel {

/// `JPMCDS_SIMPLE_BASIS`. Simple interest: `d = 1 / (1 + r * t)`.
inline constexpr double kSimpleBasis = 0.0;

/// `JPMCDS_DISCOUNT_FACTOR`. The stored "rate" is already a discount factor
/// and passes through untouched.
inline constexpr double kDiscountFactorBasis = -2.0;

/// `JPMCDS_DISCOUNT_RATE`. A discount rate, not an interest rate:
/// `d = 1 - r * t`.
inline constexpr double kDiscountRateBasis = 512.0;

/// Upstream's `JPMCDS_CONTINUOUS_BASIS`: the sentinel basis meaning
/// continuous compounding rather than a number of periods per year.
inline constexpr double kContinuousBasis = 5000.0;

namespace detail {

/// Upstream's `IS_ALMOST_ZERO`, which is a hard `DBL_EPSILON` window rather
/// than anything relative. Reproduced exactly because it decides branches.
[[nodiscard]] constexpr bool is_almost_zero(double x) noexcept {
  return x < DBL_EPSILON && x > -DBL_EPSILON;
}

} // namespace detail

/// Whether a rate can produce a positive discount factor at this basis.
/// Replaces `JpmcdsRateValidYearFrac`.
///
/// Moved here from `zero_curve.hpp`, where it was first needed: this is its
/// upstream home, and `ZeroCurve::create` now includes this header to use it.
///
/// The year fraction only matters for the two bases whose formula is linear
/// in `r * t`; the rest decide on the rate alone, which is why
/// `JpmcdsRateValid` passes `yearFraction = 1.0` for them without computing
/// anything.
[[nodiscard]] constexpr bool rate_valid_year_frac(double rate, double year_fraction,
                                                  double basis) noexcept {
  switch (static_cast<long>(basis)) {
  case static_cast<long>(kSimpleBasis):
    return rate * year_fraction > -1.0;
  case static_cast<long>(kDiscountRateBasis):
    return rate * year_fraction < 1.0;
  case static_cast<long>(kContinuousBasis):
    return true;
  case static_cast<long>(kDiscountFactorBasis):
    return rate > 0.0;
  default:
    return rate > -basis;
  }
}

/// The rate-validity test with the year fraction computed from dates.
/// Replaces `JpmcdsRateValid`.
///
/// Upstream computes the year fraction ONLY for the simple and discount-rate
/// bases and otherwise hardcodes 1.0 -- an optimisation that is also a
/// semantic statement, since those are the only two formulas where `t`
/// appears. Reproduced, including the consequence: a bad day count convention
/// is detected for those two bases and ignored for the others.
[[nodiscard]] inline std::optional<bool> rate_valid(double rate, Date start, Date end,
                                                    DayCount day_count, double basis) {
  // Named `t` rather than `year_fraction`, which would shadow the function of
  // that name -- as the first draft of this file did, and the compiler caught.
  double t = 1.0;
  const long as_long = static_cast<long>(basis);
  if (as_long == static_cast<long>(kSimpleBasis) ||
      as_long == static_cast<long>(kDiscountRateBasis)) {
    const std::optional<double> yf = year_fraction(start, end, day_count);
    if (!yf) {
      return std::nullopt; // upstream propagates the day count failure
    }
    t = *yf;
  }
  return rate_valid_year_frac(rate, t, basis);
}

/// A discount factor from a rate. Replaces `JpmcdsRateToDiscountYearFrac`.
///
/// Returns nullopt where upstream returns FAILURE, which it does for a
/// denominator at or below zero -- or within `DBL_EPSILON` of it, since a
/// discount factor of 1e16 is not a useful answer either.
///
/// The compounding branch uses `pow`, not `exp(-basis * t * log(tmp))`.
/// Upstream's comment is explicit: "We prefer to be stodgy and not do the
/// log/exp thing here for performance." Those are not the same double, so the
/// choice is preserved rather than modernised.
[[nodiscard]] inline std::optional<double>
rate_to_discount_year_frac(double rate, double year_fraction, double basis) noexcept {
  switch (static_cast<long>(basis)) {
  case static_cast<long>(kSimpleBasis): {
    const double denominator = 1.0 + rate * year_fraction;
    if (denominator <= 0.0 || detail::is_almost_zero(denominator)) {
      return std::nullopt;
    }
    return 1.0 / denominator;
  }
  case static_cast<long>(kDiscountRateBasis): {
    if (detail::is_almost_zero(year_fraction)) {
      return 1.0;
    }
    const double discount = 1.0 - rate * year_fraction;
    if (discount <= 0.0) {
      return std::nullopt;
    }
    return discount;
  }
  case static_cast<long>(kContinuousBasis):
    return std::exp(-rate * year_fraction);
  case static_cast<long>(kDiscountFactorBasis):
    return rate; // already a discount factor
  default: {
    const double tmp = 1.0 + rate / basis;
    // pow(x, y) is undefined for x < 0 and non-integral y, so upstream checks
    // first rather than relying on the library.
    if (tmp <= 0.0 || detail::is_almost_zero(tmp)) {
      return std::nullopt;
    }
    return std::pow(tmp, -basis * year_fraction);
  }
  }
}

/// A rate from a discount factor. Replaces `JpmcdsDiscountToRateYearFrac`.
///
/// Note the asymmetry with the forward direction, which is upstream's and is
/// preserved. Here a zero year fraction is a FAILURE for simple, discount-rate
/// and continuous bases -- the rate is genuinely undefined over no time at all.
/// In `rate_to_discount_year_frac` the discount-rate branch treats the same
/// zero as "a discount factor of 1", because over no time nothing is
/// discounted. One direction has an answer at `t = 0` and the other does not.
[[nodiscard]] inline std::optional<double>
discount_to_rate_year_frac(double discount, double year_fraction, double basis) noexcept {
  if (discount <= 0.0) {
    return std::nullopt;
  }
  switch (static_cast<long>(basis)) {
  case static_cast<long>(kSimpleBasis):
    if (detail::is_almost_zero(year_fraction)) {
      return std::nullopt;
    }
    return (1.0 / discount - 1.0) / year_fraction;
  case static_cast<long>(kDiscountRateBasis):
    if (detail::is_almost_zero(year_fraction)) {
      return std::nullopt;
    }
    return (1.0 - discount) / year_fraction;
  case static_cast<long>(kContinuousBasis):
    if (detail::is_almost_zero(year_fraction)) {
      return std::nullopt;
    }
    return -std::log(discount) / year_fraction;
  case static_cast<long>(kDiscountFactorBasis):
    return discount; // the rate IS the discount factor
  default:
    if (detail::is_almost_zero(year_fraction)) {
      return std::nullopt;
    }
    // Again `pow` rather than log/exp, for the same stated reason.
    return basis * (std::pow(discount, -1.0 / (basis * year_fraction)) - 1.0);
  }
}

/// ## Why these two take `long` where the year-fraction forms take `double`
///
/// Because upstream does, and the difference is observable rather than
/// cosmetic. `JpmcdsRateToDiscountYearFrac` takes `double basis`;
/// `JpmcdsRateToDiscount` takes `long basis`. So a `TCurve`, whose `fBasis`
/// field is a `double`, TRUNCATES its basis whenever it is passed through the
/// date-based conversion -- and the formulas use the basis directly, so a
/// basis of 1.5 arriving as 1 is a different number out.
///
/// That is the same truncation `ZCurve` performs by storing `basis` as a
/// `long` (see the zcall.c notes), in a second and independent place. Widening
/// these to `double` would make the port disagree with upstream for every
/// fractional basis, so the signature is kept and the inconsistency is
/// documented instead. No real convention uses a fractional compounding
/// basis; `checkTCurve` admits them only because its test is `>= -2.0`.
///
/// `rate_to_discount_year_frac` with the year fraction taken from dates.
/// Replaces `JpmcdsRateToDiscount`.
///
/// The discount-factor basis short-circuits BEFORE the year fraction is
/// computed, so it needs no valid day count -- and it validates the rate,
/// which the year-fraction form does not. Both are upstream's ordering.
///
/// Returns nullopt for a basis below simple (0), which upstream rejects
/// explicitly, except for the discount-factor sentinel handled above. So -2
/// is admissible and -1 is not, which reads as an accident of the sentinel
/// encoding rather than a decision.
[[nodiscard]] inline std::optional<double> rate_to_discount(double rate, Date start, Date end,
                                                            DayCount day_count, long basis) {
  if (basis == static_cast<long>(kDiscountFactorBasis)) {
    if (rate <= 0.0) {
      return std::nullopt;
    }
    return rate;
  }
  if (basis < static_cast<long>(kSimpleBasis)) {
    return std::nullopt;
  }
  const std::optional<double> yf = year_fraction(start, end, day_count);
  if (!yf) {
    return std::nullopt;
  }
  return rate_to_discount_year_frac(rate, *yf, static_cast<double>(basis));
}

/// `discount_to_rate_year_frac` with the year fraction taken from dates.
/// Replaces `JpmcdsDiscountToRate`.
///
/// This is NOT the mirror of `rate_to_discount`, and the first draft of this
/// header assumed it was. Reading it rather than inferring it turned up three
/// behaviours the forward direction has no counterpart for:
///
///  * a zero-length period (`start == end`) is rejected outright for every
///    basis except the discount-factor sentinel, before the year fraction is
///    computed. The forward direction has no such check and happily asks for
///    the year fraction of nothing.
///
///  * for the discount-factor basis over a zero-length period, a discount
///    factor that is not within `DBL_EPSILON` of 1.0 is an ERROR -- over no
///    time, nothing can be discounted, so any other value is contradictory.
///
///  * and in that case the rate returned is **1.0**, not the discount factor.
///    So the discount-factor basis does not simply pass through in both
///    directions: it passes through over a real period and returns 1.0 over a
///    zero-length one.
[[nodiscard]] inline std::optional<double> discount_to_rate(double discount, Date start, Date end,
                                                            DayCount day_count, long basis) {
  // Upstream's guard is `if (discount <= 0.0) return FAILURE`, which NaN
  // passes -- every comparison with NaN is false. That is UPSTREAM-19: a NaN
  // forward discount factor walks through the one check meant to stop it and
  // comes back out as a NaN rate reported as success. Written as `isfinite`
  // here, so the guard means what it was for. A NaN discount factor is not a
  // rate, from any caller; this is the one place that has to know it.
  if (!(discount > 0.0) || !std::isfinite(discount)) {
    return std::nullopt;
  }
  if (basis == static_cast<long>(kDiscountFactorBasis)) {
    if (start == end) {
      // ARE_ALMOST_EQUAL(discount, 1.0) in upstream, which is
      // IS_ALMOST_ZERO(discount - 1.0).
      if (!detail::is_almost_zero(discount - 1.0)) {
        return std::nullopt;
      }
      return 1.0;
    }
    return discount;
  }
  if (start == end) {
    return std::nullopt; // no counterpart in the forward direction
  }
  if (basis < static_cast<long>(kSimpleBasis)) {
    return std::nullopt;
  }
  const std::optional<double> yf = year_fraction(start, end, day_count);
  if (!yf) {
    return std::nullopt;
  }
  return discount_to_rate_year_frac(discount, *yf, static_cast<double>(basis));
}

} // namespace cdsmodel
