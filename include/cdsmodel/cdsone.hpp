#pragma once

// Port of upstream cdsone.c -- the upfront charge, and the flat spread that
// produces a given one. This is the top of the stack: the number the ISDA CDS
// Standard Model exists to produce, and the only reason every module beneath
// it was ported.
//
// The composition is thin, which is the point. A single benchmark at the
// quoted spread is bootstrapped into a flat clean spread curve, and the actual
// contract is priced against that curve. Both halves were already verified
// independently, so what is new here is whether they are wired together the
// same way upstream wires them -- and the wiring has four same-typed dates in
// it that do different jobs.
//
// Which is why the dates are a struct. `cds.hpp` flags four adjacent TDates
// as the hazard that argument order alone cannot protect against; this is the
// published entry point, the one Python and spreadsheet layers will bind, and
// it is worth paying a struct to make a transposition a compile error instead
// of a plausible number.

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <array>
#include <optional>
#include <span>

namespace cdsmodel {

/// The four dates an upfront calculation needs, each doing a different job.
///
/// Upstream passes these positionally, adjacent, all of type `TDate`.
struct UpfrontDates {
  /// Curve base date. Observations never precede it.
  Date today;

  /// Settlement date. Discounting is to here, and it is also the cash settle
  /// date handed to the bootstrap.
  Date value;

  /// Start date of the synthetic benchmark CDS used to imply the flat curve.
  /// NOT the traded contract's start date -- that lives in `Cds::start`.
  Date benchmark_start;

  /// Step-in date: protection and accrual both begin from here onward.
  Date stepin;
};

namespace detail {

/// The solver settings `JpmcdsCdsoneSpread` hardcodes. Note they are the
/// bootstrap's settings loosened by two orders of magnitude on both
/// tolerances, and bounded at 100 rather than 1e10 -- a spread of 10,000% is
/// taken as the ceiling for a quotable market.
inline constexpr double kSpreadBoundLo = 0.0;
inline constexpr double kSpreadBoundHi = 100.0;
inline constexpr int kSpreadIterations = 100;
inline constexpr double kSpreadGuess = 0.01;
inline constexpr double kSpreadInitialStep = 0.0001;
inline constexpr double kSpreadTolerance = 1.0e-8;

} // namespace detail

/// The upfront charge for a CDS quoted on a flat spread curve. Replaces
/// `JpmcdsCdsoneUpfrontCharge`.
///
/// Two steps. First `one_spread` is bootstrapped as a single benchmark
/// maturing on `cds.end` into a flat clean spread curve. Then `cds` is priced
/// against that curve, at its own coupon.
///
/// So the sign convention is `cds_price`'s: positive when the quoted market
/// spread exceeds the contract coupon, i.e. when the protection buyer is
/// owed money.
///
/// Three argument mappings are easy to get wrong and are worth naming,
/// because all three are same-typed values crossing a boundary:
///
///  * `dates.benchmark_start` becomes the bootstrap's CDS start date, while
///    `cds.start` is the traded contract's. Upstream's own comment on the
///    second is "cds can start from past". They are independent.
///  * `dates.value` becomes the bootstrap's CASH SETTLE date, not its value
///    date -- the bootstrap has no separate settlement parameter.
///  * `pay_accrued_at_start` is passed straight through as `cds_price`'s
///    `clean` flag. The two names suggest opposite polarity and the code
///    treats them as identical; the code is the specification.
///
/// The conventions in `cds` -- interval, stub, day count, bad day, accrual --
/// are shared by the benchmark and the priced contract, as upstream shares
/// them. `cds.coupon_rate` is the CONTRACT coupon and is not used for the
/// benchmark, which uses `one_spread`.
///
/// Returns nullopt wherever either step fails: an unbootstrappable spread
/// (see `clean_spread_curve` on the feasible region), or a contract
/// `cds_price` refuses.
[[nodiscard]] inline Expected<double>
cdsone_upfront_charge(const Cds &cds, const UpfrontDates &dates,
                      std::span<const Calendar> calendars, const ZeroCurve &discount,
                      double one_spread, double recovery, bool pay_accrued_at_start) {
  const std::array<Benchmark, 1> benchmark{Benchmark{cds.end, one_spread, true}};
  const Expected<ZeroCurve> flat_curve =
      clean_spread_curve(dates.today, discount, dates.benchmark_start, dates.stepin,
                         /*cash_settle=*/dates.value, benchmark, recovery, cds, calendars);
  if (!flat_curve) {
    return unexpected(flat_curve.error());
  }
  return cds_price(cds, dates.today, /*settle=*/dates.value, dates.stepin, calendars, discount,
                   *flat_curve, recovery, /*clean=*/pay_accrued_at_start);
}

/// The flat spread whose upfront charge equals `upfront_charge`. Replaces
/// `JpmcdsCdsoneSpread`.
///
/// This is `cdsone_upfront_charge` inverted numerically: a root find over the
/// quoted spread, where every evaluation bootstraps a fresh flat curve and
/// reprices the contract. So a single call runs two nested solvers -- the
/// outer one over the spread, and the bootstrap's own over the hazard rate --
/// which is why it is the most expensive entry point in the model and why its
/// tolerances are looser than the bootstrap's.
///
/// Returns nullopt where upstream returns FAILURE: no spread in `[0, 100]`
/// reproduces the charge within tolerance, or the objective cannot be
/// evaluated at a spread the search needs.
///
/// Note the objective is NOT guaranteed monotone over the whole bracket. It is
/// monotone wherever the bootstrap is feasible, and `nullopt` outside -- which
/// the root finder treats as a failed evaluation and propagates. In practice
/// the guess of 1% and a first step of 1bp keep the search inside the
/// feasible region for any quotable market.
[[nodiscard]] inline Expected<double> cdsone_spread(const Cds &cds, const UpfrontDates &dates,
                                                    std::span<const Calendar> calendars,
                                                    const ZeroCurve &discount,
                                                    double upfront_charge, double recovery,
                                                    bool pay_accrued_at_start) {
  auto objective = [&](double one_spread) -> Expected<double> {
    const Expected<double> charge = cdsone_upfront_charge(
        cds, dates, calendars, discount, one_spread, recovery, pay_accrued_at_start);
    if (!charge) {
      return unexpected(charge.error());
    }
    return *charge - upfront_charge;
  };

  RootFindOptions opt;
  opt.bound_lo = detail::kSpreadBoundLo;
  opt.bound_hi = detail::kSpreadBoundHi;
  opt.max_iterations = detail::kSpreadIterations;
  opt.guess = detail::kSpreadGuess;
  opt.initial_x_step = detail::kSpreadInitialStep;
  opt.initial_f_derivative = 0.0;
  opt.x_tolerance = detail::kSpreadTolerance;
  opt.f_tolerance = detail::kSpreadTolerance;
  return root_find_brent(objective, opt);
}

} // namespace cdsmodel
