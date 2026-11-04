#pragma once

// Port of upstream cxzerocurve.c + tcurve.c -- the zero curve, and the first floating
// point in this project.
//
// Everything ported so far has been integer and calendar logic, where
// "agrees with the original" means exact equality and that is simply true or
// false. From here the answers are doubles produced by a chain of multiplies,
// a divide, a log and an exp, so the question becomes whether the port can
// reproduce them BIT-FOR-BIT or only to a tolerance. Those are very different
// verification standards and the second is much weaker.
//
// The arithmetic below is therefore written in upstream's exact expression
// order, including groupings that look redundant. `zt` is
//
//     z1t1 + (z2t2 - z1t1) * (double)(t - t1) / (double)(t2 - t1)
//
// and not any algebraically equivalent rearrangement, because floating point
// addition and multiplication are not associative and a rearrangement would
// change the last bits. Where a comment says "order matters", it does.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cdsmodel {

/// Upstream's `JPMCDS_CONTINUOUS_BASIS`: the sentinel basis meaning
/// continuous compounding rather than a number of periods per year.
inline constexpr double kContinuousBasis = 5000.0;

/// One point on a curve. Upstream's `TRatePt`.
struct RatePoint {
  Date date;
  double rate;

  friend constexpr bool operator==(const RatePoint &, const RatePoint &) = default;
};

/// A zero-coupon rate curve, or a clean spread curve -- upstream uses the same
/// `TCurve` for both.
///
/// Preconditions that upstream checks on every call, or not at all, are
/// checked once at construction instead. That is what lets `zero_rate` and
/// `zero_price` return a plain `double`: upstream returns `NaN` on failure,
/// Whether a stored rate can produce a positive discount factor at this
/// basis. Replaces the part of `JpmcdsRateValidYearFrac` that applies to the
/// bases a `ZeroCurve` admits.
///
/// Upstream's rule, by basis sentinel:
///
///   * continuous (5000) -- any rate is valid, since the discount factor is
///     `exp(-rate * t)` and that is positive for every real rate;
///   * a compounding frequency n in 1..365 -- `rate > -n`, because the factor
///     goes through `log(1 + rate/n)` and the argument must be positive.
///
/// The other sentinels upstream carries in the same field -- 0 for simple
/// interest, -2 for "the rate IS a discount factor", 512 for a discount rate
/// -- have their own rules and their own validity tests, and a `ZeroCurve`
/// cannot hold them: `create` rejects any basis outside continuous or
/// 1 to 365. That restriction is `cxzerocurve.c`'s, not `tcurve.c`'s, and it
/// is the right one here because it is exactly the domain
/// `convert_compound_rate` supports.
[[nodiscard]] constexpr bool rate_is_valid(double rate, double basis) noexcept {
  if (basis == kContinuousBasis) {
    return true;
  }
  // Upstream's `default:` branch: `if (rate <= -basis) reject`.
  return rate > -basis;
}

/// a sentinel inside the result space of exactly the kind UPSTREAM-2 is about.
class ZeroCurve {
public:
  /// Build a curve, or fail.
  ///
  /// Requires: at least one point; strictly increasing dates (upstream's
  /// binary search assumes ascending order and does not verify it); and a
  /// ## UPSTREAM-15 -- a validating constructor that admits unpriceable curves
  ///
  /// `tcurve.c`'s `JpmcdsMakeTCurve` runs `checkTCurve`, whose stated purpose
  /// is "to guarantee we can generate positive discount factors". It checks
  /// the day count with `JpmcdsDayCountValid` -- which correctly accepts
  /// 30/360, a perfectly standard convention -- and it accepts an empty curve
  /// (`fNumItems >= 0`).
  ///
  /// Neither can be priced. `JpmcdsZeroPrice` routes through
  /// `JpmcdsConvertCompoundRate`, which supports ACT/365F and ACT/360 and
  /// explicitly rejects everything else. And `JpmcdsZeroPrice` returns a bare
  /// `double`, so it has nowhere to report that:
  ///
  /// ```
  /// ACT/365F   construct=accepted  JpmcdsZeroPrice=0.7833167239
  /// 30/360     construct=accepted  JpmcdsZeroPrice=nan
  /// 30E/360    construct=accepted  JpmcdsZeroPrice=nan
  /// empty      construct=accepted  JpmcdsZeroPrice=nan
  /// ```
  ///
  /// So a caller picks a standard day count, a constructor named "Make" with
  /// a validation pass accepts it, and every discount factor afterwards is
  /// NaN. The same sentinel-in-the-value-space problem as UPSTREAM-2, in a
  /// different disguise: there is no error channel, so the error becomes a
  /// number.
  ///
  /// The port closes it by rejecting at construction instead. That is why
  /// `create` is stricter than `checkTCurve` on two counts -- a non-empty
  /// curve, and a day count the conversion actually supports -- and it is why
  /// `zero_price` can return a plain `double` with no NaN sentinel: every
  /// curve that exists can be priced.
  ///
  /// basis and day count that the rate conversion actually supports --
  /// continuous or 1 to 365 periods per year, with ACT/365F or ACT/360.
  /// Upstream reports those last two per call, deep inside a hot loop.
  [[nodiscard]] static std::optional<ZeroCurve>
  create(Date base_date, std::span<const RatePoint> points, double basis, DayCount day_count) {
    if (points.empty()) {
      return std::nullopt;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
      if (points[i].date <= points[i - 1].date) {
        return std::nullopt;
      }
    }
    if (day_count != DayCount::Act365Fixed && day_count != DayCount::Act360) {
      return std::nullopt; // the only conventions the conversion supports
    }
    if (basis != kContinuousBasis && !(basis >= 1.0 && basis <= 365.0)) {
      return std::nullopt;
    }
    // PORT-11. Upstream's checkTCurve calls JpmcdsRateValid on every point,
    // "to guarantee we can generate positive discount factors". The port had
    // no such check, so a rate at or below -basis was accepted and
    // `zero_price` then returned inf or NaN -- silently, because the only
    // guard was the one that never ran.
    //
    // Found by comparing CONSTRUCTION against upstream, which no earlier test
    // did: every curve differential test built a valid curve on both sides,
    // so the constructor's rejection behaviour was never in the comparison.
    // The third time that shape has hidden a defect, after PORT-9.
    for (const RatePoint &point : points) {
      if (!rate_is_valid(point.rate, basis)) {
        return std::nullopt;
      }
    }
    return ZeroCurve{base_date, std::vector<RatePoint>{points.begin(), points.end()}, basis,
                     day_count};
  }

  [[nodiscard]] Date base_date() const noexcept { return base_date_; }
  [[nodiscard]] std::span<const RatePoint> points() const noexcept { return points_; }
  [[nodiscard]] double basis() const noexcept { return basis_; }
  [[nodiscard]] DayCount day_count() const noexcept { return day_count_; }

  /// Continuously compounded ACT/365F zero rate for a date.
  ///
  /// Replaces `JpmcdsZeroRate`. Flat-forward interpolation between curve
  /// points, the first point's rate before the curve starts, and
  /// extrapolation along the last segment beyond its end.
  [[nodiscard]] double zero_rate(Date date) const noexcept {
    const Search s = search(date);
    if (s.exact >= 0) {
      return rate_cc(static_cast<std::size_t>(s.exact));
    }
    if (s.lo < 0) {
      return rate_cc(0); // before the curve: flat at the first rate
    }
    if (static_cast<std::size_t>(s.hi) >= points_.size()) {
      if (points_.size() == 1) {
        return rate_cc(0);
      }
      // Beyond the curve: extrapolate along the last segment.
      return interp_rate(date, points_.size() - 2, points_.size() - 1);
    }
    return interp_rate(date, static_cast<std::size_t>(s.lo), static_cast<std::size_t>(s.hi));
  }

  /// Discount factor from the base date to `date`. Replaces `JpmcdsZeroPrice`.
  [[nodiscard]] double zero_price(Date date) const noexcept {
    const double rate = zero_rate(date);
    // Order matters: upstream divides the integer day count by 365.0 and then
    // negates the rate before multiplying.
    const double time = static_cast<double>(to_tdate(date) - to_tdate(base_date_)) / 365.0;
    return std::exp(-rate * time);
  }

  /// Discount factor between two dates. Replaces `JpmcdsForwardZeroPrice`.
  ///
  /// Upstream computes it as a ratio of two zero prices rather than
  /// integrating the forward rate, so the port does too -- the two differ in
  /// the last bits.
  [[nodiscard]] double forward_zero_price(Date start, Date maturity) const noexcept {
    const double start_price = zero_price(start);
    const double maturity_price = zero_price(maturity);
    return maturity_price / start_price;
  }

private:
  ZeroCurve(Date base_date, std::vector<RatePoint> points, double basis, DayCount day_count)
      : base_date_{base_date}, points_{std::move(points)}, basis_{basis}, day_count_{day_count} {}

  struct Search {
    long exact; ///< index of an equal value, or -1
    long lo;    ///< index of the last value strictly less, or -1
    long hi;    ///< index of the first value strictly greater, or size
  };

  /// Replaces `JpmcdsBinarySearchLong`, which is a different function from the
  /// bracketing search in bsearch.hpp: it also reports an exact match, and
  /// its bounds are strict rather than surrounding.
  [[nodiscard]] Search search(Date date) const noexcept {
    const long n = static_cast<long>(points_.size());
    if (date < points_.front().date) {
      return {-1, -1, 0};
    }
    if (date > points_.back().date) {
      return {-1, n - 1, n};
    }
    if (n == 1) {
      return {0, -1, n}; // the only remaining possibility is an exact match
    }

    const auto at = [this](long i) { return points_[static_cast<std::size_t>(i)].date; };
    long lo = 0;
    long hi = n - 2;
    long mid = 0;
    for (long guard = n + 1; guard > 0; --guard) {
      mid = (hi + lo) / 2;
      if (date < at(mid)) {
        hi = mid - 1;
      } else if (date > at(mid + 1)) {
        lo = mid + 1;
      } else {
        break;
      }
    }

    lo = mid;
    hi = mid + 1;
    const long exact = at(lo) == date ? lo : (at(hi) == date ? hi : -1);
    while (lo >= 0 && at(lo) >= date) {
      --lo;
    }
    while (hi < n && at(hi) <= date) {
      ++hi;
    }
    return {exact, lo >= 0 ? lo : -1, hi < n ? hi : n};
  }

  /// The stored rate at `idx`, converted to continuously compounded ACT/365F.
  /// Replaces `zcRateCC` composed with `JpmcdsConvertCompoundRate`.
  [[nodiscard]] double rate_cc(std::size_t idx) const noexcept {
    const double in_rate = points_[idx].rate;

    // Upstream short-circuits when the bases are equal, which it notes is a
    // hotspot. The branch structure is reproduced because the two paths do
    // not compute the same thing in floating point.
    if (basis_ == kContinuousBasis) {
      if (day_count_ == DayCount::Act365Fixed) {
        return in_rate;
      }
      return in_rate * 365.0 / 360.0; // ACT/360 in, ACT/365F out
    }

    const double day_factor = day_count_ == DayCount::Act365Fixed ? 1.0 : 365.0 / 360.0;
    // Order matters: (day_factor * basis) * log(1 + rate/basis).
    return day_factor * basis_ * std::log(1.0 + in_rate / basis_);
  }

  /// Flat-forward interpolation between two curve points. Replaces
  /// `zcInterpRate`.
  [[nodiscard]] double interp_rate(Date date, std::size_t lo, std::size_t hi) const noexcept {
    const long t1 = to_tdate(points_[lo].date) - to_tdate(base_date_);
    const long t2 = to_tdate(points_[hi].date) - to_tdate(base_date_);
    long t = to_tdate(date) - to_tdate(base_date_);

    const double z1 = rate_cc(lo);
    const double z2 = rate_cc(hi);

    // Continuously compounded rates mean flat forwards make z*t linear in t.
    const double z1t1 = z1 * static_cast<double>(t1);
    const double z2t2 = z2 * static_cast<double>(t2);
    if (t == 0) {
      // At the base date the zero rate is undefined; upstream uses the next
      // day as a stand-in, except when the far point is also the base date.
      if (t2 == 0) {
        return z2;
      }
      t = 1;
    }
    const double zt =
        z1t1 + (z2t2 - z1t1) * static_cast<double>(t - t1) / static_cast<double>(t2 - t1);
    return zt / static_cast<double>(t);
  }

  Date base_date_;
  std::vector<RatePoint> points_;
  double basis_;
  DayCount day_count_;
};

/// Convert a rate between compounding bases and day counts. Replaces
/// `JpmcdsConvertCompoundRate`.
///
/// Needed by the bootstrap, which solves on a continuously compounded curve
/// because that is cheaper and then converts the finished curve to annual
/// compounding "since that is traditional" -- upstream's words.
///
/// Only ACT/365F and ACT/360 are supported, which is upstream's restriction
/// and not an omission: it rejects every other pair explicitly.
///
/// The equal-bases short circuit is reproduced rather than folded into the
/// general path. Upstream notes it is a hotspot, and more importantly the two
/// paths do not compute the same thing in floating point -- the general path
/// would route an unchanged rate through `log` and `exp` and return a
/// different number.
[[nodiscard]] inline std::optional<double> convert_compound_rate(double in_rate, double in_basis,
                                                                 DayCount in_day_count,
                                                                 double out_basis,
                                                                 DayCount out_day_count) {
  const auto day_factor = [](DayCount from, DayCount to) -> std::optional<double> {
    if (from == to) {
      return 1.0;
    }
    if (from == DayCount::Act365Fixed && to == DayCount::Act360) {
      return 360.0 / 365.0;
    }
    if (from == DayCount::Act360 && to == DayCount::Act365Fixed) {
      return 365.0 / 360.0;
    }
    return std::nullopt; // upstream converts between these two and no others
  };

  // Upstream's IS_EQUAL is an exact `==` in the build configuration shipped,
  // so these comparisons are exact and not tolerant.
  if (in_basis == out_basis) {
    const std::optional<double> factor = day_factor(in_day_count, out_day_count);
    if (!factor) {
      return std::nullopt;
    }
    return in_day_count == out_day_count ? in_rate : in_rate * *factor;
  }

  const std::optional<double> factor = day_factor(in_day_count, out_day_count);
  if (!factor) {
    return std::nullopt;
  }

  double cc_rate = 0.0;
  if (in_basis == kContinuousBasis) {
    cc_rate = in_rate * *factor;
  } else if (in_basis >= 1.0 && in_basis <= 365.0) {
    cc_rate = *factor * in_basis * std::log(1.0 + in_rate / in_basis);
  } else {
    return std::nullopt;
  }

  if (out_basis == kContinuousBasis) {
    return cc_rate;
  }
  if (out_basis >= 1.0 && out_basis <= 365.0) {
    return out_basis * (std::exp(cc_rate / out_basis) - 1.0);
  }
  return std::nullopt;
}

} // namespace cdsmodel
