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
#include <cdsmodel/match_range.hpp>
#include <cdsmodel/rate_conversion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cdsmodel {

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
// `rate_is_valid` used to live here, where it was first needed. Its upstream
// home is cfinanci.cpp, so it moved to rate_conversion.hpp as part of that
// slice -- and gained the three bases a ZeroCurve cannot hold but the rest of
// the interest-rate stack can.
/// a sentinel inside the result space of exactly the kind UPSTREAM-2 is about.
class ZeroCurve {
public:
  /// Build a curve, or fail.
  ///
  /// Requires: at least one point; strictly increasing dates (upstream's
  /// binary search assumes ascending order and does not verify it); and a
  /// ## UPSTREAM-15 -- a validating constructor that admits unpriceable curves
  ///
  /// Replaces `JpmcdsMakeTCurve`, and is deliberately stricter than it.
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
  ///
  /// DELIBERATE DIFFERENCE -- the TDate 0 terminator. `JpmcdsMakeTCurve`
  /// delegates to `JpmcdsMakeTCurveNoRateCheck`, which stops copying at the
  /// first point whose date is TDate 0, so the array length argument is an
  /// upper bound rather than a count. Measured:
  ///
  ///   - a second point with date 0  -> a one-point curve, silently
  ///   - a second point with date 1  -> FAILURE, so the header's "a 1 or 0
  ///     in the date field" is wrong; only 0 terminates
  ///   - the FIRST point with date 0 -> a ZERO-point curve, which is the
  ///     empty curve UPSTREAM-36 needs a caller to reach
  ///
  /// This port takes a `span` and copies all of it. TDate 0 is 1601-01-01, a
  /// real date a caller may legitimately hold, so honouring it as a
  /// terminator would silently truncate a valid curve -- the sentinel-in-the-
  /// value-space mistake this file already documents twice. A caller that
  /// wants a shorter curve passes a shorter span.
  ///
  /// `JpmcdsMakeTCurveNoRateCheck` itself is a deliberate NON-PORT. Its only
  /// difference from `JpmcdsMakeTCurve` is that it skips the rate check, and
  /// that check is what `create` exists to perform -- porting it would
  /// reintroduce the silent-NaN hazard described above behind a second door.
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
      if (!rate_valid_year_frac(point.rate, 1.0, basis)) {
        return std::nullopt;
      }
    }
    return ZeroCurve{base_date, std::vector<RatePoint>{points.begin(), points.end()}, basis,
                     day_count};
  }

  /// The earliest date the curve knows about. Replaces
  /// `JpmcdsCurveFirstDate`.
  ///
  /// Upstream takes the MINIMUM of the base date and the first point, which is
  /// not the same as either:
  ///
  /// ```c
  /// if (curve->fNumItems == 0)
  ///     *firstDate = curve->fBaseDate;
  /// else
  ///     *firstDate = MIN (curve->fBaseDate, curve->fArray[0].fDate);
  /// ```
  ///
  /// So a curve whose first point PRECEDES its base date reports the point.
  /// That is not a degenerate shape -- it is the straddled-base-date case the
  /// flat-forwards interpolator has a dedicated branch for, where the base
  /// date falls between two curve points. "First date" means the earliest date
  /// the curve has an opinion about, which may be earlier than the date it
  /// discounts to.
  ///
  /// The empty-curve branch is unreachable here: `create` refuses an empty
  /// curve (UPSTREAM-15), so there is always a first point to compare against.
  [[nodiscard]] Date first_date() const noexcept {
    return std::min(base_date_, points_.front().date);
  }

  /// The curve's dates without its rates. Replaces `JpmcdsDatesFromCurve`
  /// (tcurve.c) and `JpmcdsNewDateListFromTCurve` (datelist.c).
  ///
  /// Upstream has both: two public functions in two files extracting a
  /// curve's dates, one returning a `TDate *` and the other a `TDateList *`.
  /// The datelist version also dereferences `curve->fNumItems` with no NULL
  /// check. See the note in date_list.hpp -- this is the third such pair, and
  /// the pattern is that cxdatelist.c and parts of datelist.c re-implement
  /// what dtlist.c and tcurve.c already do.
  ///
  /// Upstream allocates a fresh array and returns NULL both for an empty curve
  /// -- its comment says "no dates in curve is not an error" -- and for a
  /// failed allocation, so the caller cannot tell those apart. Neither case
  /// exists here: a curve is never empty, and a vector either exists or threw.
  [[nodiscard]] std::vector<Date> dates() const {
    std::vector<Date> out;
    out.reserve(points_.size());
    for (const RatePoint &point : points_) {
      out.push_back(point.date);
    }
    return out;
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
    // Never nullopt: `create` guarantees a non-empty, strictly increasing
    // point list, which are exactly match_range's two refusal conditions.
    const MatchRange found = *match_range(date, std::span<const RatePoint>{points_},
                                          [](const RatePoint &point) { return point.date; });
    if (!found.empty()) {
      return rate_cc(found.first);
    }
    if (found.first == 0) {
      return rate_cc(0); // before the curve: flat at the first rate
    }
    if (found.first >= points_.size()) {
      if (points_.size() == 1) {
        return rate_cc(0);
      }
      // Beyond the curve: extrapolate along the last segment.
      return interp_rate(date, points_.size() - 2, points_.size() - 1);
    }
    return interp_rate(date, found.first - 1, found.first);
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

  /// `JpmcdsBinarySearchLong` is NOT ported here, though an earlier version of
  /// this header did port it -- as a `search` member returning upstream's
  /// three-value {exact, lo, hi} struct. It had no caller and no test, and it
  /// duplicated the general port in match_range.hpp, which was written later
  /// and is swept exhaustively.
  ///
  /// Deleted rather than kept, for two reasons. One implementation of a
  /// function is the point of a port. And the shape it returned -- three
  /// values tied together by a documented subtraction, with `-1` and `size`
  /// as sentinels -- is precisely the shape UPSTREAM-37 lives in; keeping it
  /// in a public interface would have preserved the hazard the port exists to
  /// remove.
  ///
  /// When `cxzerocurve.c`'s remaining functions are ported they will need to
  /// search a curve's dates, which are not contiguous in `points_`. That is a
  /// projection, and the overload for it belongs in match_range.hpp when there
  /// is a caller to shape it -- the same note bsearch.hpp carries.

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
