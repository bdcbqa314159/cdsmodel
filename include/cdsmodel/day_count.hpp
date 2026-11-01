#pragma once

// Port of upstream ldate.c -- the day count conventions, and the first piece
// of this codebase with financial semantics rather than calendar mechanics.
//
// Upstream passes conventions as `long method` carrying one of six #defined
// codes, so any long is accepted at the call site and an invalid one is only
// caught at the bottom of a switch. Here it is an enum class.
//
// Scope: the arithmetic (JpmcdsDaysDiff, JpmcdsDayCountFraction) and the
// calendar helpers it needs. The string parser JpmcdsStringToDayCountConv
// (yearfrac.c) is deliberately NOT in this header -- see port-notes.md.

#include <cdsmodel/date.hpp>

#include <chrono>
#include <optional>

namespace cdsmodel {

/// Day count conventions upstream supports.
///
/// NOTE on ActAct: upstream `#define`s JPMCDS_ACT_ACT to the same value as
/// JPMCDS_ACT_365 (both 1L), so the two are literally indistinguishable in
/// its API -- a caller asking for ACT/ACT and one asking for ACT/365 get
/// identical behaviour. That is preserved here rather than "fixed": splitting
/// them would change results. ActAct is an alias, not a separate enumerator,
/// so the conflation is visible in the type instead of hidden in a macro.
enum class DayCount {
  Act365 = 1,       ///< Actual/365, with a leap-weighted denominator (ISDA ACT/ACT)
  Act365Fixed = 2,  ///< Actual/365 Fixed -- denominator always 365
  Act360 = 3,       ///< Actual/360
  Thirty360 = 4,    ///< 30/360 (USA NASD)
  ThirtyE360 = 5,   ///< 30E/360
  EffectiveRate = 8 ///< year fraction is always +/-1, or 0 for equal dates
};

/// Upstream's JPMCDS_ACT_ACT is the same code as JPMCDS_ACT_365.
inline constexpr DayCount kActAct = DayCount::Act365;

[[nodiscard]] constexpr long to_upstream_code(DayCount dcc) noexcept {
  return static_cast<long>(dcc);
}

[[nodiscard]] constexpr std::optional<DayCount> from_upstream_code(long code) noexcept {
  switch (code) {
  case 1:
    return DayCount::Act365;
  case 2:
    return DayCount::Act365Fixed;
  case 3:
    return DayCount::Act360;
  case 4:
    return DayCount::Thirty360;
  case 5:
    return DayCount::ThirtyE360;
  case 8:
    return DayCount::EffectiveRate;
  default: // includes JPMCDS_BAD_DCC (-2), a sentinel for Brazilian bonds
    return std::nullopt;
  }
}

/// Numerator of the day count fraction: the convention's notion of "days
/// between". Replaces JpmcdsDaysDiff.
///
/// Returns nullopt for EffectiveRate, which upstream also rejects here (it
/// has no meaningful day count, only a year fraction).
///
/// Signed: swapping the dates negates the result, as upstream does by
/// swapping then multiplying by -1.
[[nodiscard]] constexpr std::optional<long> days_diff(Date from, Date to, DayCount dcc) noexcept {
  const bool reversed = from > to;
  if (reversed) {
    const Date tmp = from;
    from = to;
    to = tmp;
  }

  long days = 0;
  switch (dcc) {
  case DayCount::Act365:
  case DayCount::Act365Fixed:
  case DayCount::Act360:
    days = (to - from).count();
    break;

  case DayCount::Thirty360:
  case DayCount::ThirtyE360: {
    const std::chrono::year_month_day a = to_ymd(from);
    const std::chrono::year_month_day b = to_ymd(to);
    long d1 = static_cast<long>(static_cast<unsigned>(a.day()));
    long d2 = static_cast<long>(static_cast<unsigned>(b.day()));

    // 30/360 (NASD): a 31st start becomes the 30th; a 31st end becomes the
    // 30th only if the start was already the 30th. 30E/360 collapses both
    // unconditionally. That single conditional is the whole difference
    // between the two conventions.
    if (d1 == 31) {
      d1 = 30;
    }
    if (d2 == 31 && (dcc == DayCount::ThirtyE360 || d1 == 30)) {
      d2 = 30;
    }

    const long y1 = static_cast<long>(static_cast<int>(a.year()));
    const long y2 = static_cast<long>(static_cast<int>(b.year()));
    const long m1 = static_cast<long>(static_cast<unsigned>(a.month()));
    const long m2 = static_cast<long>(static_cast<unsigned>(b.month()));
    days = (y2 - y1) * 360 + (m2 - m1) * 30 + (d2 - d1);
    break;
  }

  case DayCount::EffectiveRate:
    return std::nullopt;
  }

  return reversed ? -days : days;
}

/// The day count fraction. Replaces JpmcdsDayCountFraction.
[[nodiscard]] constexpr std::optional<double> year_fraction(Date from, Date to,
                                                            DayCount dcc) noexcept {
  // Upstream short-circuits these two before any date ordering, purely for
  // speed. The subtraction is already signed, so the result is unchanged.
  if (dcc == DayCount::Act365Fixed) {
    return static_cast<double>((to - from).count()) / 365.0;
  }
  if (dcc == DayCount::Act360) {
    return static_cast<double>((to - from).count()) / 360.0;
  }
  if (from == to) {
    return 0.0;
  }

  const double sign = from > to ? -1.0 : 1.0;
  if (from > to) {
    const Date tmp = from;
    from = to;
    to = tmp;
  }

  if (dcc == DayCount::EffectiveRate) {
    return sign;
  }

  if (dcc == DayCount::Thirty360 || dcc == DayCount::ThirtyE360) {
    const std::optional<long> days = days_diff(from, to, dcc);
    if (!days) {
      return std::nullopt;
    }
    return sign * static_cast<double>(*days) / 360.0;
  }

  // Act365, i.e. ISDA ACT/ACT: days falling in leap years are divided by 366,
  // the rest by 365.
  //
  // Upstream walks the years by repeatedly adding 365 or 366 to a running
  // date and re-deriving the year from it. This instead intersects the period
  // with each calendar year directly -- same arithmetic, but the intent is
  // legible and there is no running date to drift. Equivalence is established
  // by the differential sweep, not by inspection.
  long leap_days = 0;
  long other_days = 0;
  const int first = static_cast<int>(to_ymd(from).year());
  const int last = static_cast<int>(to_ymd(to).year());
  for (int y = first; y <= last; ++y) {
    const Date year_start{std::chrono::year{y} / std::chrono::January / 1};
    const Date year_end{std::chrono::year{y + 1} / std::chrono::January / 1};
    const Date lo = from > year_start ? from : year_start;
    const Date hi = to < year_end ? to : year_end;
    if (hi <= lo) {
      continue;
    }
    const long n = (hi - lo).count();
    if (is_leap(std::chrono::year{y})) {
      leap_days += n;
    } else {
      other_days += n;
    }
  }
  return sign * (static_cast<double>(leap_days) / 366.0 + static_cast<double>(other_days) / 365.0);
}

} // namespace cdsmodel
