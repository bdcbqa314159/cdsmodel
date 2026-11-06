#pragma once

// Port of upstream tcurve.c -- the two functions that discount off a curve in
// the curve's own terms, as opposed to `ZeroCurve::zero_price`, which is
// `JpmcdsZeroPrice` and takes a different route to the same number.
//
// A separate header from zero_curve.hpp for a dependency reason rather than a
// taste one: `JpmcdsDiscountDate` interpolates, so it needs interp_curve.hpp,
// which needs zero_curve.hpp. Putting it on the curve itself would close a
// cycle. `JpmcdsCurveFirstDate` and `JpmcdsDatesFromCurve` have no such
// dependency and are methods on the curve.
//
// ## Two routes to a discount factor, and they agree
//
// `JpmcdsZeroPrice` converts the stored rate to continuous compounding and
// then applies `exp(-rate * days/365)`. `JpmcdsDiscountDate` interpolates
// under a chosen interpolation type and then converts using the curve's own
// basis and day count. Different code, different intermediate quantities --
// and measured across four curve shapes they land in the same place:
//
//     continuous, ACT/365F   exact at curve points, exact between them
//     continuous, ACT/360    exact
//     annual,     ACT/365F   exact at points, 2.4e-16 relative between them
//     semiannual, ACT/365F   exact at points, 1.2e-16 relative between them
//
// That is a cross-check the port gets for free and a test asserts, because it
// is the UPSTREAM-17 shape -- two implementations of one quantity -- without
// the defect. The reason they agree is that `JpmcdsZeroRate` does the
// basis-and-day-count conversion before returning, which is what the port's
// `rate_cc` reproduces. I checked that rather than assuming the hardcoded
// `/365.0` meant the two disagreed off ACT/365F.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <optional>
#include <span>

namespace cdsmodel {

/// The discount factor to `date`, interpolating under `interp` and converting
/// in the curve's own terms. Replaces `JpmcdsDiscountDate`.
///
/// Upstream's three steps, kept separate:
///
/// ```c
/// JpmcdsInterpRate (date, zc, interpType, &interpRate);
/// JpmcdsDayCountFraction(zc->fBaseDate, date, zc->fDayCountConv, &yearsToDate);
/// JpmcdsRateToDiscountYearFrac(interpRate, yearsToDate, zc->fBasis, discount);
/// ```
///
/// Note the last line: the year fraction is computed first and passed in,
/// rather than letting `JpmcdsRateToDiscount` derive it from the two dates.
/// For every day count the port can hold those are the same number, but the
/// two-step form is what upstream evaluates and the one-step form has its own
/// validation (`rate_valid`) that this path does not run. Reproduced as two
/// steps so the set of accepted inputs matches.
///
/// Returns nullopt where upstream fails: an interpolation that fails, a day
/// count whose year fraction cannot be computed, or a rate the conversion
/// refuses for the curve's basis.
[[nodiscard]] inline std::optional<double> discount_date(const ZeroCurve &curve, Date date,
                                                         InterpType interp) {
  const std::optional<double> rate = interp_rate(curve, date, interp);
  if (!rate) {
    return std::nullopt;
  }
  const std::optional<double> years = year_fraction(curve.base_date(), date, curve.day_count());
  if (!years) {
    return std::nullopt;
  }
  return rate_to_discount_year_frac(*rate, *years, curve.basis());
}

/// The present value of a run of cash flows off a curve. Replaces
/// `JpmcdsCashFlowPV`.
///
/// One `discount_date` per flow, summed in order. The accumulation order is
/// upstream's and is load-bearing to the last bits:
///
/// ```c
/// pv += (discountFactor * cfl->fArray[idx].fAmount);
/// ```
///
/// Two things upstream does that are worth noticing. It writes `*presValue`
/// only on success -- the `else` branch of its own status check -- so a failed
/// call leaves the caller's variable untouched rather than zeroed, which
/// `std::optional` says without a convention. And it does not check `cfl` for
/// NULL, where its neighbour `JpmcdsMergeCFL` does; an empty span here is
/// simply worth nothing, which is the answer rather than an error.
///
/// Distinct from `present_value_cash_flows` in cash_flow_bootstrap.hpp: that
/// one prices off a `ZeroCurveBuilder` and reads its cached discount factors,
/// this one prices off a finished `ZeroCurve` and interpolates. Upstream has
/// the same pair for the same reason.
[[nodiscard]] inline std::optional<double>
cash_flow_pv(const ZeroCurve &curve, std::span<const CashFlow> flows, InterpType interp) {
  double pv = 0.0;
  for (const CashFlow &flow : flows) {
    const std::optional<double> discount = discount_date(curve, flow.date, interp);
    if (!discount) {
      return std::nullopt;
    }
    pv += *discount * flow.amount;
  }
  return pv;
}

} // namespace cdsmodel
