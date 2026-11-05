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
// This slice stops at the boundary between BUILDING a curve and EVALUATING
// one. `ZCInterpolate`, `ZCDiscountFactor`, `ZCPresentValue`,
// `ZCMoneyMarketRate`, `ZCForwardRate`, `ZCFwdDiscountFactor` and
// `ZCAddMoneyMarket` only read a finished curve and are the next slice;
// `ZCAddCashFlowList`, `ZCAddCFLPoints` and `JpmcdsJpmcdsZCPresentValueCFL`
// need cashflow.c and are the one after.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
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
  [[nodiscard]] static std::optional<ZeroCurveBuilder> create(Date value_date, int expected_points,
                                                              long basis, DayCount day_count) {
    if (expected_points <= 0 || !builder_basis_valid(basis)) {
      return std::nullopt;
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
  [[nodiscard]] std::optional<std::size_t> find_exact(Date date) const noexcept {
    const std::optional<std::size_t> closest = find_closest(date);
    if (!closest || points_[*closest].date != date) {
      return std::nullopt;
    }
    return closest;
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
  [[nodiscard]] std::optional<std::size_t> find_closest(Date date) const noexcept {
    if (points_.empty()) {
      return std::nullopt;
    }
    std::vector<long> dates;
    dates.reserve(points_.size());
    for (const Point &p : points_) {
      dates.push_back(to_tdate(p.date));
    }
    const std::optional<Bracket> at =
        bracket(std::span<const long>{dates}, static_cast<double>(to_tdate(date)));
    if (!at) {
      return std::nullopt;
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
  /// -0.9999999   1.0923357953681824e+35     1.0923357953681824e+35
  /// -1.0         inf                        FAILURE
  /// -1.5         FAILURE                    FAILURE
  /// ```
  ///
  /// And it is not confined to this function. The infinity is stored:
  ///
  /// ```
  /// JpmcdsZCAddRate(curve, date, -1.0) -> SUCCESS
  ///   stored: date=156324 rate=-1 discount=inf
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
  [[nodiscard]] std::optional<double> compute_discount(Date date, double rate) const {
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
    const std::optional<double> discount = compute_discount(date, rate);
    if (!discount) {
      return false;
    }
    return add_rate_and_discount(date, rate, *discount);
  }

  /// Insert a discount factor, recovering its rate. Replaces
  /// `JpmcdsZCAddDiscountFactor`.
  [[nodiscard]] bool add_discount_factor(Date date, double discount) {
    const std::optional<double> rate =
        discount_to_rate(discount, value_date_, date, day_count_, basis_);
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
    const std::optional<double> discount =
        rate_to_discount(rate, value_date_, date, day_count, basis);
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
  [[nodiscard]] std::optional<ZeroCurve> to_zero_curve() const {
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
  [[nodiscard]] static std::optional<ZeroCurveBuilder> from_zero_curve(const ZeroCurve &curve) {
    const std::span<const RatePoint> points = curve.points();
    std::optional<ZeroCurveBuilder> builder =
        create(curve.base_date(), static_cast<int>(points.size()) + 32,
               static_cast<long>(curve.basis()), curve.day_count());
    if (!builder) {
      return std::nullopt;
    }
    for (const RatePoint &p : points) {
      if (!builder->add_rate(p.date, p.rate)) {
        return std::nullopt;
      }
    }
    return builder;
  }

private:
  ZeroCurveBuilder(Date value_date, long basis, DayCount day_count)
      : value_date_{value_date}, basis_{basis}, day_count_{day_count} {}

  Date value_date_;
  long basis_;
  DayCount day_count_;
  std::vector<Point> points_;
};

} // namespace cdsmodel
