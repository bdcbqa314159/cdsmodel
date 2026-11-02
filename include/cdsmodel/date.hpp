#pragma once

// Port of upstream dateconv.c -- the calendar layer everything else sits on.
//
// Upstream represents a date as `typedef long int TDate`: days since
// 1601-01-01, proleptic Gregorian. That typedef is the single most
// consequential piece of primitive obsession in the codebase -- it appears in
// most of the 126 public entry points, and because it is an arithmetic type
// it lets you add a date to a date and get a plausible-looking number.
//
// Here a date is std::chrono::sys_days. TDate survives only as an interop
// type at the boundary (to_tdate / from_tdate), not as the working currency.
//
// What this replaces, measured:
//   - gDateCacheArray, a hardcoded table of the first day of every month from
//     1995 to 2119: 1,500 entries over 377 lines, plus the lookup/validate
//     path that used it and the 400/100/4/1-year loop fallback for dates
//     outside it. std::chrono does all of it.
//   - days[] / leapDays[] / cumDays[] / leapCumDays[] month tables.
//   - the JPMCDS_IS_LEAP macro.

#include <chrono>
#include <limits>
#include <optional>

namespace cdsmodel {

/// A calendar day.
using Date = std::chrono::sys_days;

/// Upstream's representation: days since 1601-01-01. Interop only.
using TDate = long;

/// Upstream's epoch. TDate 0 is 1601-01-01, which was a Monday -- which is
/// why upstream's `(date + 1) % 7` day-of-week encoding comes out
/// Sunday-based, matching std::chrono::weekday::c_encoding() exactly.
inline constexpr Date kDateEpoch{std::chrono::year{1601} / std::chrono::January / 1};

/// Upstream's JpmcdsMDYToDate rejects any year below its epoch year.
/// (mdydate.h documents the range as [1600-]; the code enforces >= 1601.
/// The code wins.)
inline constexpr std::chrono::year kMinYear{1601};

/// Value upstream's JpmcdsDate returns for an invalid date: FAILURE, i.e. -1.
/// Safe as a sentinel only because valid TDates are never negative, which is
/// exactly the kind of coupling std::optional removes.
inline constexpr TDate kInvalidTDate = -1;

namespace detail {

/// `std::chrono::days` is only required to hold a signed integer of at least
/// 25 bits, and is `int` in practice -- not 64 bits, which is the surprise.
using DayRep = std::chrono::days::rep;

/// The 1601 epoch as a day number relative to `sys_days`'s own 1970 epoch:
/// -134774. Every conversion carries this offset, which is why the
/// representable TDate range is NOT symmetric.
inline constexpr long long kEpochDayNumber = kDateEpoch.time_since_epoch().count();

} // namespace detail

/// Smallest and largest TDate that convert without narrowing.
///
/// Two constraints, and they bite at opposite ends:
///
///   * `from_tdate(t)` forms `kEpochDayNumber + t` in `DayRep`, so `t` cannot
///     be smaller than `DayRep`'s minimum plus the offset.
///   * `to_tdate` must be able to return `t`, so `t` must also fit `DayRep`.
///
/// Together: `[DayRep::min() + 134774, DayRep::max()]`. The bounds are
/// derived from the epoch rather than written down, so they stay correct on an
/// implementation where `DayRep` is wider.
///
/// The actual numbers are IMPLEMENTATION-DEPENDENT in two independent ways,
/// so never write them down:
///
///   * `std::chrono::days::rep` is 32-bit on libc++ and 64-bit on libstdc++.
///   * `long`, and therefore `TDate`, is 32-bit on Windows and 64-bit
///     elsewhere -- upstream's `typedef long int TDate` inherits that, so the
///     date type is not a stable width across platforms. Harmless for day
///     numbers around 150,000, but it matters at any ABI boundary.
///
/// Either way the bounds sit far outside any real date -- on libc++, TDate
/// 2^31 is the year 5,881,581 -- but an undocumented domain with silently
/// wrong behaviour outside it is the same shape of defect this port exists to
/// remove. Before it was named, `from_tdate(5'000'000'000)` round-tripped to
/// 705032704 on macOS.
inline constexpr TDate kMinTDate = static_cast<TDate>(std::numeric_limits<detail::DayRep>::min()) -
                                   static_cast<TDate>(detail::kEpochDayNumber);
inline constexpr TDate kMaxTDate = static_cast<TDate>(std::numeric_limits<detail::DayRep>::max());

/// Can this TDate be converted without narrowing?
///
/// Check this at any boundary that accepts a TDate from outside the library:
/// a C API shim, a language binding, a file reader. Inside the library, dates
/// come from `make_date` or `to_tdate` and are representable by construction.
[[nodiscard]] constexpr bool is_representable_tdate(TDate t) noexcept {
  return t >= kMinTDate && t <= kMaxTDate;
}

[[nodiscard]] constexpr TDate to_tdate(Date d) noexcept {
  return static_cast<TDate>((d - kDateEpoch).count());
}

/// Convert an upstream TDate to a date.
///
/// Precondition: `is_representable_tdate(t)`. Outside that the arithmetic
/// overflows `std::chrono::days`'s representation.
[[nodiscard]] constexpr Date from_tdate(TDate t) noexcept {
  return kDateEpoch + std::chrono::days{static_cast<detail::DayRep>(t)};
}

/// Number of days in a given month. Replaces JpmcdsDaysInMonth, which indexed
/// a 13-element table with an unvalidated month and read out of bounds for
/// month > 12. std::chrono::month makes that unrepresentable.
[[nodiscard]] constexpr unsigned days_in_month(std::chrono::year_month ym) noexcept {
  return static_cast<unsigned>(
      std::chrono::year_month_day_last{ym.year(), std::chrono::month_day_last{ym.month()}}.day());
}

/// Replaces the JPMCDS_IS_LEAP macro.
[[nodiscard]] constexpr bool is_leap(std::chrono::year y) noexcept { return y.is_leap(); }

/// Construct a date from year/month/day, rejecting invalid input.
///
/// Replaces JpmcdsDate (which returned -1 on failure) and JpmcdsMDYToDate
/// (status code plus out-param). Validation matches upstream: year at or
/// after the epoch year, month in 1-12, day in 1-31 and no later than the
/// month's last day.
[[nodiscard]] constexpr std::optional<Date> make_date(int year, int month, int day) noexcept {
  if (year < static_cast<int>(kMinYear)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return std::nullopt;
  }
  const std::chrono::year_month_day ymd{std::chrono::year{year},
                                        std::chrono::month{static_cast<unsigned>(month)},
                                        std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok()) { // e.g. 31 April, or 29 February in a common year
    return std::nullopt;
  }
  return Date{ymd};
}

/// Split a date into year/month/day. Replaces JpmcdsDateToMDY.
[[nodiscard]] constexpr std::chrono::year_month_day to_ymd(Date d) noexcept {
  return std::chrono::year_month_day{d};
}

/// Day of week. Replaces JpmcdsDayOfWeek; `.c_encoding()` reproduces
/// upstream's 0-6 Sunday-based numbering.
[[nodiscard]] constexpr std::chrono::weekday day_of_week(Date d) noexcept {
  return std::chrono::weekday{d};
}

/// Last day of the date's own month. Replaces JpmcdsDateToEOM.
[[nodiscard]] constexpr Date end_of_month(Date d) noexcept {
  const std::chrono::year_month_day ymd{d};
  return Date{
      std::chrono::year_month_day_last{ymd.year(), std::chrono::month_day_last{ymd.month()}}};
}

/// Bring an out-of-range month into range by carrying into the year, then
/// clamp the day to the resulting month's length.
///
/// Replaces JpmcdsNormalizeMDY. Two upstream behaviours preserved exactly,
/// and they are not symmetric:
///   - a day beyond the month's length is CLAMPED (31 April -> 30 April)
///   - a day outside 1-31 entirely is REJECTED
/// Note upstream does not range-check the year here, so neither does this;
/// the epoch floor is enforced by make_date instead.
[[nodiscard]] constexpr std::optional<std::chrono::year_month_day>
normalize_ymd(int year, int month, int day) noexcept {
  if (day < 1 || day > 31) {
    return std::nullopt;
  }
  // Done in a wider type on purpose. `month` is any int, so `month - 1`
  // overflows at INT_MIN and `year + carry` overflows at either extreme.
  // Guarding each operation separately is easy to get subtly wrong; widening
  // once and range-checking at the end is not.
  const long long month_index = static_cast<long long>(month) - 1;
  long long year_carry = month_index / 12;
  long long month_rem = month_index % 12;
  if (month_rem < 0) { // floor division, matching upstream's two while-loops
    month_rem += 12;
    --year_carry;
  }
  const long long normalised_year = static_cast<long long>(year) + year_carry;

  // std::chrono::year represents [-32767, 32767]; anything outside is not a
  // date. Upstream has no equivalent check -- its month-carry loops would
  // simply run on -- but it cannot produce such a year from a valid TDate.
  if (normalised_year < -32767 || normalised_year > 32767) {
    return std::nullopt;
  }

  const std::chrono::year y{static_cast<int>(normalised_year)};
  const std::chrono::month m{static_cast<unsigned>(month_rem + 1)};
  const unsigned last = days_in_month(y / m);
  const unsigned clamped = static_cast<unsigned>(day) < last ? static_cast<unsigned>(day) : last;
  return std::chrono::year_month_day{y, m, std::chrono::day{clamped}};
}

} // namespace cdsmodel
