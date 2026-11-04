#pragma once

// Port of upstream cdsbootstrap.c -- turning quoted par spreads into a clean
// spread curve, one benchmark maturity at a time. The last module between the
// ported pieces and the upfront charge.
//
// Upstream's own comment is worth keeping: "Very little attempt has been made
// at extreme optimisation - this is quite a basic bootstrap routine which
// simply calls the underlying CDS pricer for each benchmark instrument while
// it changes the CDS zero rate at the maturity date of the benchmark
// instrument." That is exactly what it does, and the port does the same.
//
// The mechanism deserves stating plainly because it is the one piece of
// shared mutable state in the whole model. The objective function handed to
// the root finder WRITES the trial spread into the curve, then prices the
// benchmark CDS against that same curve. Point `i` is being solved; points
// `0..i-1` already hold solved spreads; points `i+1..n-1` still hold their
// initial values, which are the quoted coupon rates. Those later points are
// never consulted, because a CDS maturing at `end[i]` is priced only from
// dates at or before `end[i]`.
//
// The port keeps that structure rather than tidying it into something
// functional. Rebuilding an immutable curve per evaluation is the one change:
// `ZeroCurve` validates on construction and has no setter, so the vector of
// rate points is the mutable state and a curve is built from it each time the
// objective runs. Same arithmetic, same order, same results -- just with the
// validation that upstream does once applied every time instead.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// One quoted benchmark CDS.
///
/// Upstream passes these as three parallel arrays -- `endDates`,
/// `couponRates`, and an optional `includes` -- with the third nullable to
/// mean "all of them". Three arrays that must agree in length is the same
/// shape that produced a defect in the fee leg, so they are one struct here
/// and `include` defaults to true.
struct Benchmark {
  Date end;
  double coupon_rate = 0.0;
  bool include = true;
};

namespace detail {

/// The solver settings upstream hardcodes in `CdsBootstrap`. Note they differ
/// from the ones `cdsone.c` uses for its own solve: the bounds here are
/// `[0, 1e10]` rather than `[0, 100]`, and the tolerances are 1e-10 rather
/// than 1e-8.
inline constexpr double kBootstrapBoundLo = 0.0;
inline constexpr double kBootstrapBoundHi = 1.0e10;
inline constexpr int kBootstrapIterations = 100;
inline constexpr double kBootstrapInitialStep = 0.0005;
inline constexpr double kBootstrapTolerance = 1.0e-10;

} // namespace detail

/// Bootstrap a clean spread curve from quoted benchmarks. Replaces
/// `JpmcdsCleanSpreadCurve`.
///
/// `conventions` supplies the terms shared by every benchmark: the coupon
/// interval, stub, day count, bad day convention, accrual convention, and the
/// CDS start date. Its `end` and `coupon_rate` are IGNORED -- those come from
/// each `Benchmark` -- which follows the precedent already set by
/// `cds_par_spread`.
///
/// The returned curve is **annually compounded** ACT/365F, based at `today`,
/// with one point per included benchmark. Upstream solves on a continuously
/// compounded curve because it is faster and converts at the end "since that
/// is traditional"; the conversion is part of the published contract, so a
/// caller reading `points()` sees annual rates, not the continuous ones the
/// solver worked with.
///
/// Three asymmetries carried over verbatim, each observable in the result:
///
///  * the protection leg starts at `max(today, start)`, while the premium leg
///    starts at `start`. Note this is `today`, not the step-in date -- which
///    is what `cds_price` uses for the same clamp. The two entry points
///    genuinely differ.
///  * the objective prices as at the curve's base date, which is `today`, and
///    discounts to `cash_settle`.
///  * the premium leg is always priced CLEAN inside the bootstrap, regardless
///    of anything the caller asked for elsewhere.
///
/// Returns nullopt where upstream returns NULL: no benchmarks, none included,
/// end dates not strictly increasing, a benchmark whose spread cannot be
/// solved within 100 iterations on `[0, 1e10]`, a negative implied forward
/// hazard rate between consecutive benchmarks, or a basis conversion that
/// fails.
///
/// A degenerate recovery rate is rejected rather than specially handled, and
/// that was measured rather than assumed. Upstream's initial guess is
/// `coupon / (1 - recovery)` with no guard: `recovery == 1` with a non-zero
/// coupon gives an infinite guess, `recovery > 1` a negative one, and both
/// fall outside the solver's bounds. `recovery == 1` with a ZERO coupon gives
/// `0.0/0.0`, and a NaN guess passes the bounds check -- neither
/// `NaN < 0` nor `NaN > 1e10` is true -- so the search proceeds on NaN,
/// fails to bracket, retries at the bounds and ultimately fails. Measured
/// against upstream: all three cases return NULL. No wrong answer is
/// produced, so this is recorded as a non-finding rather than a defect.
[[nodiscard]] inline std::optional<ZeroCurve>
clean_spread_curve(Date today, const ZeroCurve &discount, Date start, Date stepin, Date cash_settle,
                   std::span<const Benchmark> benchmarks, double recovery, const Cds &conventions,
                   std::span<const Calendar> calendars) {
  std::vector<Benchmark> selected;
  for (const Benchmark &b : benchmarks) {
    if (b.include) {
      selected.push_back(b);
    }
  }
  if (selected.empty()) {
    return std::nullopt; // upstream: REQUIRE(nbDate > 0) and REQUIRE(nbInclude > 0)
  }

  // The working curve: continuously compounded, rates initialised to the
  // quoted coupons exactly as upstream initialises them.
  std::vector<RatePoint> points;
  points.reserve(selected.size());
  for (const Benchmark &b : selected) {
    points.push_back(RatePoint{b.end, b.coupon_rate});
  }
  // Built once up front purely to reject a bad set of dates before any
  // solving, the way JpmcdsMakeTCurve does.
  if (!ZeroCurve::create(today, points, kContinuousBasis, DayCount::Act365Fixed)) {
    return std::nullopt;
  }

  Cds terms = conventions;
  terms.start = start;
  terms.protect_start = true; // upstream hardcodes this

  for (std::size_t i = 0; i < selected.size(); ++i) {
    const double guess = selected[i].coupon_rate / (1.0 - recovery);

    const ContingentLeg protection =
        cds_contingent_leg(std::max(today, start), selected[i].end, 1.0, terms.protect_start);

    terms.end = selected[i].end;
    terms.coupon_rate = selected[i].coupon_rate;
    const std::optional<FeeLeg> premium = cds_fee_leg(terms, 1.0, calendars);
    if (!premium) {
      return std::nullopt;
    }

    // Writes the trial spread into the shared points and reprices. This is
    // upstream's in-place mutation, with a validated curve rebuilt each call
    // because ZeroCurve has no setter.
    auto objective = [&](double clean_spread) -> std::optional<double> {
      points[i].rate = clean_spread;
      const std::optional<ZeroCurve> curve =
          ZeroCurve::create(today, points, kContinuousBasis, DayCount::Act365Fixed);
      if (!curve) {
        return std::nullopt;
      }
      const std::optional<double> pv_protection =
          contingent_leg_pv(protection, today, cash_settle, stepin, discount, *curve, recovery);
      if (!pv_protection) {
        return std::nullopt;
      }
      const std::optional<double> pv_premium =
          fee_leg_pv(*premium, today, stepin, cash_settle, discount, *curve, /*clean=*/true);
      if (!pv_premium) {
        return std::nullopt;
      }
      return *pv_protection - *pv_premium;
    };

    RootFindOptions opt;
    opt.bound_lo = detail::kBootstrapBoundLo;
    opt.bound_hi = detail::kBootstrapBoundHi;
    opt.max_iterations = detail::kBootstrapIterations;
    opt.guess = guess;
    opt.initial_x_step = detail::kBootstrapInitialStep;
    opt.initial_f_derivative = 0.0;
    opt.x_tolerance = detail::kBootstrapTolerance;
    opt.f_tolerance = detail::kBootstrapTolerance;

    const std::optional<double> spread = root_find_brent(objective, opt);
    if (!spread) {
      return std::nullopt;
    }
    // Assigned again deliberately: the objective's LAST evaluation need not
    // have been at the returned root, so the curve would otherwise hold a
    // trial value. Upstream does the same re-assignment for the same reason.
    points[i].rate = *spread;

    if (i > 0) {
      // A forward survival probability above one means a negative hazard rate
      // between the two benchmarks -- an arbitrage the curve cannot express.
      //
      // Checked only from the second benchmark onward, which is upstream's
      // bound and is sufficient rather than an oversight: the first segment
      // runs from `today` and its rate is the solver output, which the lower
      // bound of 0 already keeps non-negative.
      const std::optional<ZeroCurve> curve =
          ZeroCurve::create(today, points, kContinuousBasis, DayCount::Act365Fixed);
      if (!curve) {
        return std::nullopt;
      }
      if (curve->forward_zero_price(selected[i - 1].end, selected[i].end) > 1) {
        return std::nullopt;
      }
    }
  }

  // Continuous to annual, in place, exactly as CreditCurveConvertRateType does.
  for (RatePoint &point : points) {
    const std::optional<double> annual = convert_compound_rate(
        point.rate, kContinuousBasis, DayCount::Act365Fixed, 1.0, DayCount::Act365Fixed);
    if (!annual) {
      return std::nullopt;
    }
    point.rate = *annual;
  }
  return ZeroCurve::create(today, points, 1.0, DayCount::Act365Fixed);
}

} // namespace cdsmodel
