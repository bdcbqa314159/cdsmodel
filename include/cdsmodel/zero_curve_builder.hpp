#pragma once

// Port of upstream zcall.c -- the building half. `ZCurve` is not a third curve
// type; it is the mutable thing you accumulate points into before producing a
// finished curve.
//
//     TCurve   {base date, array of (date, rate), basis (double), day count}
//     ZCurve   {value date, rate[], date[], discount[], numItems, numAlloc,
//               basis (long), day count}
//
// So three differences, and each one shapes the port:
//
//   * `numItems` + `numAlloc` is a hand-rolled growable vector, reallocating
//     in blocks of 32. A `std::vector` replaces the lot.
//   * `rate[]`, `date[]`, `discount[]` are THREE PARALLEL ARRAYS held in step
//     by hand -- the same shape that produced a defect in the premium leg.
//     One vector of one struct here, so they cannot drift.
//   * `basis` is a `long` where `TCurve::fBasis` is a `double`, so converting
//     between them truncates. See the note on `from_zero_curve`.
//
// The cached `discount[]` is the interesting part: this type stores the
// discount factor alongside every rate, so the two must agree at all times.
// That invariant is why the building functions cannot be split from each
// other, and it is where both defects in this file live.
//
// Evaluation came in a second slice, and is below: `ZCInterpolate`,
// `ZCDiscountFactor`, `ZCPresentValue`, `ZCMoneyMarketRate`, `ZCForwardRate`,
// `ZCFwdDiscountFactor`, plus `ZCAddMoneyMarket`, which belongs to building
// but is expressed in terms of the evaluation side's basis conventions.
// `ZCAddCashFlowList`, `ZCAddCFLPoints` and `JpmcdsJpmcdsZCPresentValueCFL`
// need cashflow.c and are the next slice.
//
// One thing the evaluation half reveals about the building half: the cached
// `discount[]` array is never read by any function here. Every evaluation
// path recomputes the discount factor from the rate. Upstream reads the cache
// in exactly one place -- `JpmcdsJpmcdsZCPresentValueCFL`, which is in the
// slice after this -- so within this file it is write-mostly state. That is
// also how UPSTREAM-17's stored infinity escapes: the cash-flow present value
// reads `zc->discount[j]` directly, without passing back through the
// computation that would have refused it.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace cdsmodel {

/// Upstream's `JpmcdsBasisValid`, a macro local to zcall.c:
///
/// ```c
/// #define JpmcdsBasisValid(x)  ((x)>=1 && (x)<=5001 ? SUCCESS : FAILURE)
/// ```
///
/// Reproduced, oddities and all, because it decides what a builder will
/// accept. Two of them:
///
///   * the upper bound is **5001**, one past the continuous sentinel of 5000.
///     So a basis of 5001 is accepted and then treated by the conversion layer
///     as a compounding frequency of 5001 periods per year -- arithmetically
///     fine, semantically nothing.
///   * the lower bound of 1 **rejects** simple interest (0) and the
///     discount-factor sentinel (-2), both of which `rate_to_discount` handles.
///     So a builder cannot hold a simple-interest curve. That is consistent
///     with how the type is used -- money-market instruments are converted to
///     the curve's own terms on the way in -- but it is a restriction, not a
///     consequence of anything.
[[nodiscard]] constexpr bool builder_basis_valid(long basis) noexcept {
  return basis >= 1 && basis <= 5001;
}

/// Upstream's interpolation types, as the two values a ZCurve supports.
///
/// ```c
/// #define JPMCDS_LINEAR_INTERP     0L
/// #define JPMCDS_FLAT_FORWARDS   124L
/// ```
///
/// `JpmcdsZCInterpolate` switches on a `long` and rejects anything else, so
/// these are the whole domain and an enum loses nothing. Distinct from
/// `interpolation.hpp`'s `Metric`, which selects how a gap between dates is
/// MEASURED; this selects what is interpolated -- the rate itself, or the
/// forward implied by it.
///
/// `FlatForwards` here is NOT the same code as `interpc.c`'s
/// `InterpRateFlatFwds`. The two are independent implementations of the same
/// idea on two different curve types.
enum class InterpType { Linear, FlatForwards };

/// A curve under construction: a rate and its discount factor at each date.
///
/// Replaces `ZCurve`. Produces a `ZeroCurve` via `to_zero_curve`.
class ZeroCurveBuilder {
public:
  /// One accumulated point. Upstream keeps these as three parallel arrays.
  struct Point {
    Date date;
    double rate = 0.0;
    double discount = 0.0;
  };

  /// Replaces `JpmcdsZCMake`.
  ///
  /// `expected_points` is upstream's `n`, the initial allocation. Here it is
  /// only a `reserve` hint -- but upstream REJECTS `n <= 0`, and that
  /// rejection is observable, so it is kept rather than quietly made
  /// meaningless.
  ///
  /// Returns nullopt for a non-positive hint, a basis outside
  /// `builder_basis_valid`, or a day count upstream's `JpmcdsDayCountValid`
  /// refuses.
  [[nodiscard]] static Expected<ZeroCurveBuilder> create(Date value_date, int expected_points,
                                                         long basis, DayCount day_count) {
    if (expected_points <= 0 || !builder_basis_valid(basis)) {
      return fail(ErrorKind::bad_argument,
                  "a builder needs a positive point count and a valid basis");
    }
    // JpmcdsDayCountValid accepts every convention the port's DayCount can
    // hold except the two non-calculable codes, which `DayCount` cannot
    // represent at all -- so there is nothing further to check here.
    ZeroCurveBuilder builder{value_date, basis, day_count};
    builder.points_.reserve(static_cast<std::size_t>(expected_points));
    return builder;
  }

  [[nodiscard]] Date value_date() const noexcept { return value_date_; }
  [[nodiscard]] long basis() const noexcept { return basis_; }
  [[nodiscard]] DayCount day_count() const noexcept { return day_count_; }
  [[nodiscard]] std::span<const Point> points() const noexcept { return points_; }
  [[nodiscard]] bool empty() const noexcept { return points_.empty(); }

  /// The index of an exact date match, or nullopt. Replaces
  /// `JpmcdsZCFindDateExact`.
  ///
  /// D7: `optional`, not `Expected`. "No point sits on this date" is a lookup
  /// MISS, the same answer `map::find` gives, and callers branch on it as a
  /// question rather than handling it as a failure.
  [[nodiscard]] std::optional<std::size_t> find_exact(Date date) const noexcept {
    const Expected<std::size_t> closest = find_closest(date);
    if (!closest || points_[*closest].date != date) {
      return std::nullopt;
    }
    return *closest;
  }

  /// The index of the nearest date, or nullopt for an empty builder.
  /// Replaces `JpmcdsZCFindDateClosest`.
  ///
  /// Upstream brackets with the UNCHECKED binary search and then picks
  /// whichever end is nearer, with ties going to the lower index. It measures
  /// the distances as `(int)ABS(date - zc->date[i])`, narrowing a `TDate`
  /// difference to `int`; the port keeps the difference wide, which cannot
  /// change the comparison for any representable pair of dates but does not
  /// rely on that either.
  [[nodiscard]] Expected<std::size_t> find_closest(Date date) const noexcept {
    if (points_.empty()) {
      return fail(ErrorKind::bad_argument, "the builder has no points");
    }
    std::vector<long> dates;
    dates.reserve(points_.size());
    for (const Point &p : points_) {
      dates.push_back(to_tdate(p.date));
    }
    const Expected<Bracket> at =
        bracket(std::span<const long>{dates}, static_cast<double>(to_tdate(date)));
    if (!at) {
      return unexpected(at.error());
    }
    const auto lo = static_cast<std::size_t>(at->lo);
    const auto hi = static_cast<std::size_t>(at->hi);
    const long diff_lo = std::labs(to_tdate(date) - to_tdate(points_[lo].date));
    const long diff_hi = std::labs(to_tdate(date) - to_tdate(points_[hi].date));
    return diff_lo <= diff_hi ? lo : hi;
  }

  /// The discount factor a rate implies at this date, in the builder's own
  /// terms. Replaces `JpmcdsZCComputeDiscount`.
  ///
  /// ## UPSTREAM-17 -- an optimisation that changes the answer at its boundary
  ///
  /// Upstream has a fast path for the common case, guarded like this:
  ///
  /// ```c
  /// if (zc->basis == 1 && rate >= -1.0 && date >= zc->valueDate &&
  ///     (dcc == JPMCDS_ACT_365F || dcc == JPMCDS_ACT_360))
  ///     *discount = pow(1+rate, (zc->valueDate - date) / (dcc==ACT_360 ? 360.0 : 365.0));
  /// ```
  ///
  /// The guard is `rate >= -1.0`. At exactly -1.0 that computes
  /// `pow(0, negative)`, which is an infinity -- while the general path it is
  /// standing in for rejects the same input, because
  /// `rate_to_discount_year_frac` refuses a base at or below zero. Measured:
  ///
  /// ```
  /// rate         ZCComputeDiscount          JpmcdsRateToDiscount
  /// -0.9999999   1.0923357970930543e+56     1.0923357953681824e+35
  /// -1.0         inf                        FAILURE
  /// -1.5         FAILURE                    FAILURE
  /// ```
  ///
  /// And it is not confined to this function. The infinity is stored:
  ///
  /// ```
  /// JpmcdsZCAddRate(curve, date, -1.0) -> SUCCESS
  ///   stored: date=157419 rate=-1 discount=inf
  ///   JpmcdsZCDiscountFactor at that date -> SUCCESS  inf
  /// ```
  ///
  /// Two code paths for one quantity, disagreeing at the boundary, and the
  /// fast one wins because it is tested first. The guard wants `rate > -1.0`.
  ///
  /// The port takes the general path's answer at the boundary -- nullopt --
  /// which makes the two paths agree everywhere. The fast path is still
  /// reproduced for every input where it and the general path concur, because
  /// they are not bit-identical elsewhere either: `(valueDate - date)/365.0`
  /// and `-1.0 * ((date - valueDate)/365.0)` happen to be the same double for
  /// all representable dates, but only because negation is exact.
  [[nodiscard]] Expected<double> compute_discount(Date date, double rate) const {
    if (basis_ == 1 && rate > -1.0 && date >= value_date_ &&
        (day_count_ == DayCount::Act365Fixed || day_count_ == DayCount::Act360)) {
      const double denominator = day_count_ == DayCount::Act360 ? 360.0 : 365.0;
      return std::pow(1.0 + rate,
                      static_cast<double>(to_tdate(value_date_) - to_tdate(date)) / denominator);
    }
    return rate_to_discount(rate, value_date_, date, day_count_, basis_);
  }

  /// Insert a point whose rate and discount factor are already consistent.
  /// Replaces `JpmcdsZCAddRateAndDiscount` -- the function that keeps the
  /// invariant, and therefore the one everything else goes through.
  ///
  /// Three behaviours transcribed rather than tidied:
  ///
  ///  * a date already present with the SAME rate -- within upstream's
  ///    hardcoded `1e-7`, which is a tolerance on the rate and not on the
  ///    discount factor -- is a silent no-op returning success. With a
  ///    different rate it is an error. So this is idempotent for a repeat and
  ///    strict for a conflict.
  ///  * points are kept sorted by date, inserted by shifting the tail down.
  ///    The common case of appending at the end is special-cased, which is
  ///    why a date beyond the last is not compared against anything.
  ///  * the duplicate check only runs when `date <= last date`, so an
  ///    appended date is never searched for.
  [[nodiscard]] bool add_rate_and_discount(Date date, double rate, double discount) {
    if (!points_.empty() && date <= points_.back().date) {
      const std::optional<std::size_t> existing = find_exact(date);
      if (existing) {
        // Upstream's literal 1e-7, on the rate.
        return std::fabs(rate - points_[*existing].rate) < 0.0000001;
      }
    }
    const auto at = std::ranges::upper_bound(points_, date, {}, &Point::date);
    points_.insert(at, Point{date, rate, discount});
    return true;
  }

  /// Insert a rate, computing its discount factor. Replaces
  /// `JpmcdsZCAddRate`. The rate must already be in the builder's own basis
  /// and day count.
  [[nodiscard]] bool add_rate(Date date, double rate) {
    const Expected<double> discount = compute_discount(date, rate);
    if (!discount) {
      return false;
    }
    return add_rate_and_discount(date, rate, *discount);
  }

  /// Insert a discount factor, recovering its rate. Replaces
  /// `JpmcdsZCAddDiscountFactor`.
  [[nodiscard]] bool add_discount_factor(Date date, double discount) {
    const Expected<double> rate = discount_to_rate(discount, value_date_, date, day_count_, basis_);
    if (!rate) {
      return false;
    }
    return add_rate_and_discount(date, *rate, discount);
  }

  /// Insert a rate quoted in some OTHER basis and day count, converting it
  /// into the builder's terms. Replaces `JpmcdsZCAddGenRate`.
  ///
  /// ## UPSTREAM-16 -- a failure that leaves the curve modified
  ///
  /// Upstream short-circuits when the terms already match, and then forgets
  /// to stop:
  ///
  /// ```c
  /// if (basis==zc->basis && dayCountConv==zc->dayCountConv)
  /// {
  ///      if (JpmcdsZCAddRate(zc,date,rate) == FAILURE)
  ///          goto done;
  /// }
  /// /* no return -- falls through */
  /// if (JpmcdsRateToDiscount(rate, zc->valueDate, date, dayCountConv, basis, &df) == FAILURE)
  ///     goto done;
  /// if (JpmcdsZCAddDiscountFactor(zc,date,df) == FAILURE)
  ///     goto done;
  /// ```
  ///
  /// So the matching case adds the point and then adds it AGAIN by a second
  /// route. Usually harmless: the duplicate check in
  /// `add_rate_and_discount` sees the same date with a rate that survived a
  /// round trip through the discount factor, finds it within 1e-7, and
  /// returns success without doing anything.
  ///
  /// Not harmless when the second route fails. A date equal to the value date
  /// makes `discount_to_rate` reject a zero-length period, so the function
  /// returns FAILURE -- having already committed the point:
  ///
  /// ```
  /// date == valueDate : rc=FAILURE  numItems=1   *** curve MUTATED ***
  ///    orphaned point: date=154497 rate=0.05 discount=1
  /// ```
  ///
  /// A caller that discards the builder on failure is fine. One that retries,
  /// or carries on, now holds a point it never successfully added.
  ///
  /// The port returns after the matching case, which is what the missing
  /// `return` would have done. That also removes the duplicated work, and it
  /// means a failure never leaves a point behind -- every path here commits
  /// only after all its conversions have succeeded.
  [[nodiscard]] bool add_general_rate(Date date, double rate, long basis, DayCount day_count) {
    if (basis == basis_ && day_count == day_count_) {
      return add_rate(date, rate); // UPSTREAM-16: upstream falls through here
    }
    const Expected<double> discount = rate_to_discount(rate, value_date_, date, day_count, basis);
    if (!discount) {
      return false;
    }
    return add_discount_factor(date, *discount);
  }

  /// Copy in every point of `other` that falls BEFORE this builder's first
  /// date. Replaces `JpmcdsZCAddPrefixCurve`.
  ///
  /// An empty target takes everything. The two value dates must agree --
  /// upstream's comment says so and nothing checks it, so the port checks it.
  /// The bases and day counts may differ, which is why each point goes
  /// through `add_general_rate`.
  [[nodiscard]] bool add_prefix(const ZeroCurveBuilder &other) {
    if (other.value_date_ != value_date_) {
      return false; // upstream documents this requirement and does not enforce it
    }
    if (other.points_.empty()) {
      return true;
    }
    // D7: genuinely absent, not failed -- an empty builder has no cutoff.
    const std::optional<Date> cutoff =
        points_.empty() ? std::nullopt : std::optional{points_.front().date};
    for (const Point &p : other.points_) {
      if (cutoff && p.date >= *cutoff) {
        break; // the source is sorted, so nothing later qualifies either
      }
      if (!add_general_rate(p.date, p.rate, other.basis_, other.day_count_)) {
        return false;
      }
    }
    return true;
  }

  /// Copy in every point of `other` that falls AFTER this builder's last
  /// date. Replaces `JpmcdsZCAddSuffixCurve`.
  ///
  /// Upstream walks the source BACKWARDS from its end, stopping at the first
  /// date not after the target's last. Order of insertion does not change the
  /// result -- `add_rate_and_discount` sorts -- but it does change which
  /// point is reported first if one of them fails, so the direction is kept.
  [[nodiscard]] bool add_suffix(const ZeroCurveBuilder &other) {
    if (other.value_date_ != value_date_) {
      return false;
    }
    if (other.points_.empty()) {
      return true;
    }
    // D7: genuinely absent, not failed.
    const std::optional<Date> cutoff =
        points_.empty() ? std::nullopt : std::optional{points_.back().date};
    for (auto it = other.points_.rbegin(); it != other.points_.rend(); ++it) {
      if (cutoff && it->date <= *cutoff) {
        break;
      }
      if (!add_general_rate(it->date, it->rate, other.basis_, other.day_count_)) {
        return false;
      }
    }
    return true;
  }

  /// The finished curve. Replaces `JpmcdsZCToTCurve`.
  ///
  /// Returns nullopt where `ZeroCurve::create` refuses what has been
  /// accumulated -- an empty builder, or a basis or day count the rate
  /// conversion cannot support. The builder's basis is widened to `double`
  /// here, which is lossless in this direction.
  [[nodiscard]] Expected<ZeroCurve> to_zero_curve() const {
    std::vector<RatePoint> rate_points;
    rate_points.reserve(points_.size());
    for (const Point &p : points_) {
      rate_points.push_back(RatePoint{p.date, p.rate});
    }
    return ZeroCurve::create(value_date_, rate_points, static_cast<double>(basis_), day_count_);
  }

  /// A builder seeded from a finished curve. Replaces `JpmcdsZCFromTCurve`.
  ///
  /// Upstream allocates `fNumItems + 32` -- "Allow room for expansion" -- and
  /// then adds each rate through `JpmcdsZCAddRate`, so the discount factors
  /// are recomputed rather than carried over. Reproduced: the discount cache
  /// is this type's own, and seeding it any other way would make the two
  /// disagree.
  ///
  /// The basis TRUNCATES. `ZeroCurve::basis()` is a `double` and a builder's
  /// is a `long`, so upstream's `(long) tc->fBasis` loses anything
  /// fractional -- measured, 1.5 becomes 1 and 4.7 becomes 4, and the round
  /// trip back out does not restore it. No convention uses a fractional
  /// compounding basis, and `ZeroCurve::create` admits them only because its
  /// range test does not exclude them.
  [[nodiscard]] static Expected<ZeroCurveBuilder> from_zero_curve(const ZeroCurve &curve) {
    const std::span<const RatePoint> points = curve.points();
    Expected<ZeroCurveBuilder> builder =
        create(curve.base_date(), static_cast<int>(points.size()) + 32,
               static_cast<long>(curve.basis()), curve.day_count());
    if (!builder) {
      return unexpected(builder.error());
    }
    for (const RatePoint &p : points) {
      if (!builder->add_rate(p.date, p.rate)) {
        return fail(ErrorKind::bad_argument, "a point could not be added to the builder");
      }
    }
    return builder;
  }

  // =====================================================================
  // EVALUATION -- reading a curve rather than building one.
  // =====================================================================

  /// Add a run of money-market instruments. Replaces `JpmcdsZCAddMoneyMarket`.
  ///
  /// Money-market quotes are simple-interest rates, which a builder cannot
  /// hold -- `builder_basis_valid` rejects basis 0 -- so each one is converted
  /// into the curve's own terms on the way in. That is the whole function:
  /// `add_general_rate` with the simple basis.
  ///
  /// Upstream takes `dates[]` and `rates[]` as parallel arrays and indexes
  /// them together; one span of `RatePoint` here, for the same reason the
  /// points themselves are one vector.
  ///
  /// Upstream returns on the first failure, leaving the curve holding every
  /// instrument before it. That is kept: a partial curve is the caller's to
  /// discard, and unwinding would be a new behaviour rather than a port. It is
  /// NOT the partial mutation of UPSTREAM-16 -- there, a single call reported
  /// failure after committing its own point; here each committed point
  /// genuinely succeeded.
  [[nodiscard]] bool add_money_market(std::span<const RatePoint> quotes, DayCount day_count) {
    for (const RatePoint &quote : quotes) {
      if (!add_general_rate(quote.date, quote.rate, static_cast<long>(kSimpleBasis), day_count)) {
        return false;
      }
    }
    return true;
  }

  /// Replace the rate at an existing point, recomputing its cached discount.
  ///
  /// Upstream's bootstrap does this by hand, at the end of its root find:
  ///
  /// ```c
  /// zc->rate[ objData.zcIndex ] = rate;
  /// ```
  ///
  /// with no corresponding write to `zc->discount[zcIndex]`. The pair survives
  /// only because the objective function updates both on every evaluation, so
  /// the cache ends up describing whichever rate the solver last *tried* --
  /// which is the returned root only if the solver's last act was to evaluate
  /// there. Measured over the bootstrap's own instruments, it is: every stored
  /// point still satisfies the invariant afterwards. It holds by coincidence of
  /// the solver's internals, not by construction.
  ///
  /// So the port does not expose the unpaired write at all. Setting a rate
  /// recomputes the discount or fails; there is no sequence of public calls
  /// that leaves the two disagreeing.
  [[nodiscard]] bool set_rate(std::size_t index, double rate) {
    if (index >= points_.size()) {
      return false;
    }
    const Expected<double> discount = compute_discount(points_[index].date, rate);
    if (!discount) {
      return false;
    }
    points_[index].rate = rate;
    points_[index].discount = *discount;
    return true;
  }

  /// The interpolated zero rate at a date. Replaces `JpmcdsZCInterpolate`.
  ///
  /// Four behaviours before any interpolation happens, all upstream's:
  ///
  ///  * **flat extrapolation backwards.** A date at or before the first point
  ///    returns the first rate unchanged. Upstream's comment gives the reason:
  ///    *"so that swaps which have payments before the beginning of the stub
  ///    zero curve will still value to par"*. Forward extrapolation is NOT
  ///    flat -- the bracket for a late date is the last pair, and the linear
  ///    branch extends that slope indefinitely.
  ///  * **the value date is nudged forward a day.** `date == value_date` would
  ///    be a rate over no time, so upstream takes the next day instead. This
  ///    is only reachable when the curve has a point before its own value
  ///    date, since otherwise the flat-extrapolation branch has already
  ///    returned.
  ///  * **an exact hit on either bracket end** returns that rate without
  ///    interpolating, so the result at a stored date is exactly the stored
  ///    rate rather than an interpolation that merely ought to reproduce it.
  ///  * **the UNCHECKED binary search.** Upstream calls
  ///    `JpmcdsBSearchLongFast`, so the sorted precondition is the type's
  ///    invariant, not something re-verified per call. Here the builder keeps
  ///    the dates strictly increasing, so `bracket`'s walk past a run of equal
  ///    values is unreachable from this type.
  ///
  /// ## The day count has no effect on linear interpolation
  ///
  /// Upstream computes the day-count metric function and then never uses it:
  ///
  /// ```c
  /// TMetricDoubleFunc metricFP;   /* day counting metric function */
  /// ...
  /// metricFP = JpmcdsDayCountToMetricFunc(zc->dayCountConv);   /* never read */
  /// ```
  ///
  /// Both branches measure the gap between dates in raw days. `metricFP` and
  /// `yearsFromBaseDate` (also computed, also never read) are dead locals.
  ///
  /// The port interpolates in raw days too, because that is the behaviour, and
  /// the dead assignment is only evidence of what was once intended. It is
  /// worth recording what the intent would have changed: `JpmcdsLinInterpCurve`
  /// in linterpc.c performs the same linear interpolation on the same kind of
  /// curve and DOES pass the metric through, so for a 30/360 curve the two
  /// disagree. Not filed as a defect -- nothing says the two functions are
  /// meant to agree, and a raw-day interpolation of zero rates is a defensible
  /// convention on its own. Filed as an observation, because a dead metric
  /// lookup beside a live raw-day calculation is the kind of thing that looks
  /// like an oversight to the next reader too.
  ///
  /// Returns nullopt for an empty curve or an unknown interpolation type.
  [[nodiscard]] Expected<double> interpolate(Date date, InterpType interp) const {
    if (points_.empty()) {
      return fail(ErrorKind::bad_argument, "the builder has no points");
    }
    if (date <= points_.front().date) {
      return points_.front().rate;
    }
    if (date == value_date_) {
      // Upstream's `++date`: "cannot calculate rate for value date, so get
      // the value at the next date which is the next best thing".
      date = from_tdate(to_tdate(date) + 1);
    }

    const Expected<Bracket> at = bracket_dates(date);
    if (!at) {
      return unexpected(at.error());
    }
    const Point &lo = points_[at->lo];
    const Point &hi = points_[at->hi];
    if (lo.date == date) {
      return lo.rate;
    }
    if (hi.date == date) {
      return hi.rate;
    }

    const TDate hi_lo = to_tdate(hi.date) - to_tdate(lo.date);
    const TDate dt_lo = to_tdate(date) - to_tdate(lo.date);

    switch (interp) {
    case InterpType::Linear:
      if (hi_lo == 0) {
        return lo.rate;
      }
      // Divide then multiply, in upstream's order. Reassociating this is a
      // one-ULP change, which is exactly how PORT-10 happened.
      return lo.rate +
             ((hi.rate - lo.rate) / static_cast<double>(hi_lo)) * static_cast<double>(dt_lo);

    case InterpType::FlatForwards: {
      if (hi_lo == 0) {
        return lo.rate;
      }
      // Recomputed from the rates, NOT read from the cached discounts --
      // upstream does the same, which is why the cache is write-mostly.
      const Expected<double> disc_lo = compute_discount(lo.date, lo.rate);
      const Expected<double> disc_hi = compute_discount(hi.date, hi.rate);
      if (!disc_lo || !disc_hi || *disc_lo == 0.0) {
        return fail(ErrorKind::bad_argument,
                    "a bracket endpoint has no discount factor, or the lower one is zero");
      }
      const double disc_date =
          *disc_lo *
          std::pow(*disc_hi / *disc_lo, static_cast<double>(dt_lo) / static_cast<double>(hi_lo));
      return discount_to_rate(disc_date, value_date_, date, day_count_, basis_);
    }
    }
    return fail(ErrorKind::unsupported, "unrecognised enumerator reached a switch default");
  }

  /// The discount factor to a date. Replaces `JpmcdsZCDiscountFactor`.
  ///
  /// The value date discounts by nothing and short-circuits before any
  /// interpolation, so it needs no points at all -- an empty curve still has
  /// a discount factor of 1.0 at its own value date, which is upstream's
  /// behaviour and is defensible.
  [[nodiscard]] Expected<double> discount_factor(Date date, InterpType interp) const {
    if (date == value_date_) {
      return 1.0;
    }
    const Expected<double> rate = interpolate(date, interp);
    if (!rate) {
      return unexpected(rate.error());
    }
    return compute_discount(date, *rate);
  }

  /// A payment discounted to the value date. Replaces `JpmcdsZCPresentValue`.
  [[nodiscard]] Expected<double> present_value(double price, Date date, InterpType interp) const {
    const Expected<double> discount = discount_factor(date, interp);
    if (!discount) {
      return unexpected(discount.error());
    }
    return price * *discount;
  }

  /// The discount factor for a forward period. Replaces
  /// `JpmcdsZCFwdDiscountFactor`.
  ///
  /// ## UPSTREAM-18 -- an error that is logged, then reported as success
  ///
  /// Upstream detects a non-positive discount factor at the start of the
  /// forward, writes the error message, and returns SUCCESS anyway with a
  /// fabricated answer:
  ///
  /// ```c
  /// *discount = 1.0;
  /// if (discount1<=0.0)
  /// {
  ///     JpmcdsErrMsg("JpmcdsZCFwdDiscountFactor: bad discount at start of fwd\n");
  /// }
  /// else
  /// {
  ///     *discount = discount2 / discount1;
  /// }
  /// return SUCCESS;
  /// ```
  ///
  /// There is no missing error channel here -- the function returns `int` and
  /// the caller checks it. The status simply does not match what the function
  /// found. And 1.0 is the worst possible stand-in: it means "no discounting
  /// over this period", so `forward_rate` converts it to a forward rate of
  /// exactly **zero**.
  ///
  /// Reachable by underflow, and the boundary is sharp. Continuous basis, a
  /// 30-year point, sweeping the rate:
  ///
  /// ```
  /// rate=20   df1=1.70976e-261  fwd rate=4.85165e+08
  /// rate=23   df1=1.31182e-300  fwd rate=9.7448e+09
  /// rate=24   df1=1.20094e-313  fwd rate=2.43073e+10     <- subnormal
  /// rate=25   df1=0             fwd rate=0               <- fabricated
  /// rate=30   df1=0             fwd rate=0               <- fabricated
  /// ```
  ///
  /// Note the direction of travel. As the input gets worse the output stops
  /// looking wrong: 2.4e10 is visibly nonsense and a monitor would catch it,
  /// 0 is not. The failure mode improves in appearance exactly as the
  /// situation deteriorates.
  ///
  /// A 2500% continuous rate is not a market quote. It is what a diverged
  /// bootstrap produces, which is precisely when a caller needs the failure
  /// reported rather than smoothed over.
  ///
  /// The port returns nullopt where upstream fabricates, and also where the
  /// result is not finite -- see UPSTREAM-19 on `forward_rate`.
  [[nodiscard]] Expected<double> forward_discount_factor(Date start, Date end,
                                                         InterpType interp) const {
    const Expected<double> at_start = discount_factor(start, interp);
    const Expected<double> at_end = discount_factor(end, interp);
    if (!at_start || !at_end) {
      return unexpected(!at_start ? at_start.error() : (at_end.error()));
    }
    if (*at_start <= 0.0) {
      return fail(ErrorKind::bad_argument, "the discount factor at the start is not positive");
    }
    const double forward = *at_end / *at_start;
    if (!std::isfinite(forward)) {
      return fail(ErrorKind::bad_argument, "the forward discount factor is not finite");
    }
    return forward;
  }

  /// The forward rate between two dates, in the requested terms. Replaces
  /// `JpmcdsZCForwardRate`.
  ///
  /// ## UPSTREAM-19 -- the guard against a bad discount factor cannot see NaN
  ///
  /// Upstream guards the conversion:
  ///
  /// ```c
  /// if (discount<=0.0)                  /* bad discount factor */
  /// {
  ///     JpmcdsErrMsg("JpmcdsZCForwardRate: bad discount for forward\n");
  ///     return FAILURE;
  /// }
  /// ```
  ///
  /// `NaN <= 0.0` is false. The one check whose entire job is to stop an
  /// unusable discount factor is written as the one comparison every NaN
  /// passes, so a NaN goes straight into `JpmcdsDiscountToRate` -- which
  /// guards the same way, `if (discount <= 0.0)` -- and comes out as a NaN
  /// rate reported as success.
  ///
  /// How the NaN arises matters, because it is not a defect. Continuous basis,
  /// a 30-year horizon, a large NEGATIVE rate overflows the discount factor:
  ///
  /// ```
  /// rate=-20   df1=5.84877e+260  df2=2.83762e+269  fwd=4.85165e+08  rate=-1
  /// rate=-23   df1=7.62298e+299  df2=inf           fwd=inf          rate=-1
  /// rate=-24   df1=inf           df2=inf           fwd=nan          rate=nan
  /// rate=-100  df1=inf           df2=inf           fwd=nan          rate=nan
  /// ```
  ///
  /// Both discount factors are legitimately infinite -- that is the same
  /// overflow UPSTREAM-15 concluded is a VALUE and not a finding, since the
  /// answer really is that large. The defect is downstream: the ratio of two
  /// infinities is indeterminate, and nothing stops it.
  ///
  /// So the lineage is UPSTREAM-15's accepted infinity, UPSTREAM-19's blind
  /// guard: a correct value flows into a check that cannot represent the
  /// result of combining two of them.
  ///
  /// The port rejects any non-finite forward discount factor in
  /// `forward_discount_factor`, which is upstream's guard written as
  /// `isfinite` instead of `> 0.0`. `discount_to_rate` was tightened the same
  /// way for the same reason -- a NaN discount factor is not a rate, from any
  /// caller.
  [[nodiscard]] Expected<double> forward_rate(Date start, Date end, long basis, DayCount day_count,
                                              InterpType interp) const {
    const Expected<double> discount = forward_discount_factor(start, end, interp);
    if (!discount || *discount <= 0.0) {
      return fail(ErrorKind::bad_argument,
                  "the forward discount factor is missing or not positive");
    }
    return discount_to_rate(*discount, start, end, day_count, basis);
  }

  /// The money-market rate to a date: a simple-interest forward from the value
  /// date. Replaces `JpmcdsZCMoneyMarketRate`, which is one call through to
  /// `forward_rate` with the simple basis.
  [[nodiscard]] Expected<double> money_market_rate(Date date, DayCount day_count,
                                                   InterpType interp) const {
    return forward_rate(value_date_, date, static_cast<long>(kSimpleBasis), day_count, interp);
  }

private:
  /// `bracket` over the point dates.
  ///
  /// `bracket` takes a span of one type; the dates here are a field of a
  /// struct, and upstream reaches them by striding `sizeof(TRatePt)` through
  /// memory, which has no span equivalent. Rather than copy the dates out on
  /// every call -- this sits inside bootstrap inner loops -- the search is
  /// expressed over the sorted points directly.
  ///
  /// That makes two implementations of one thing, which is the mistake
  /// UPSTREAM-17 is about, so it is not left to inspection: a test sweeps both
  /// against each other and requires identical brackets.
  [[nodiscard]] Expected<Bracket> bracket_dates(Date date) const noexcept {
    const std::size_t n = points_.size();
    if (n < 1) {
      return fail(ErrorKind::bad_argument, "the builder has no points to bracket");
    }
    if (n == 1) {
      return Bracket{0, 0};
    }
    if (date <= points_.front().date) {
      return Bracket{0, 1};
    }
    if (date >= points_.back().date) {
      return Bracket{n - 2, n - 1};
    }
    // Strictly increasing by construction, so the first point at or after
    // `date` has a predecessor and the pair brackets it. Upstream's walk past
    // a run of equal dates cannot apply to this type.
    const auto first_at_or_after = std::ranges::lower_bound(points_, date, {}, &Point::date);
    const std::size_t hi =
        static_cast<std::size_t>(std::ranges::distance(points_.begin(), first_at_or_after));
    return Bracket{hi - 1, hi};
  }

  ZeroCurveBuilder(Date value_date, long basis, DayCount day_count)
      : value_date_{value_date}, basis_{basis}, day_count_{day_count} {}

  Date value_date_;
  long basis_;
  DayCount day_count_;
  std::vector<Point> points_;
};

} // namespace cdsmodel
