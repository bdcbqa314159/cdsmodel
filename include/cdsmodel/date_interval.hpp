#pragma once

// Port of upstream convert.c (interval parts) plus JpmcdsDtFwdAny from
// ldate.c -- the date interval type and arithmetic on it.
//
// Upstream's type is:
//
//     typedef struct { int prd; char prd_typ; int flag; } TDateInterval;
//
// with prd_typ a character from a documented set of fourteen and `flag`
// selecting what the interval is measured from. In practice:
//
//   * JpmcdsMakeDateInterval always sets flag = 0, and nothing in the library
//     sets it otherwise, so the field is vestigial. It is not modelled here.
//   * JpmcdsMakeDateInterval normalises A/Y to months x12, S to x6, Q to x3,
//     W to days x7 and U (lunar) to days x28. So of the fourteen period
//     types, only 'D' and 'M' ever reach the arithmetic.
//   * JpmcdsDtFwdAny supports only D, W, M, Q, S, A, Y -- seven. The other
//     seven (F, E, B, G, H, I, J, K, L, T, V) can be CONSTRUCTED but cannot
//     be used to advance a date: upstream stores them and then rejects them.
//
// This models the canonical, usable form: a count of days or of months.
// The construct-but-cannot-use period types are deliberately not modelled --
// a value you can build and not use is not a feature. They will arrive with
// schedule.c, which is where anything that consumes them would live.

#include <cdsmodel/date.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cdsmodel {

enum class PeriodUnit { Days, Months };

/// An interval expressed as a count of days or of months.
///
/// Upstream's `flag` field is dropped: it is always 0 and nothing reads it.
struct DateInterval {
  int periods = 0;
  PeriodUnit unit = PeriodUnit::Days;

  friend constexpr bool operator==(const DateInterval &, const DateInterval &) = default;
};

namespace detail {

/// `periods * factor`, or nullopt if the product would not fit an `int`.
///
/// Upstream wraps silently: `JpmcdsMakeDateInterval(999999999, 'Y')` scales by
/// 12 and stores a wrapped month count. Checking before multiplying is the
/// only option -- signed overflow is undefined, so it cannot be detected
/// afterwards.
[[nodiscard]] constexpr std::optional<int> scale(int periods, int factor) noexcept {
  if (periods > std::numeric_limits<int>::max() / factor ||
      periods < std::numeric_limits<int>::min() / factor) {
    return std::nullopt;
  }
  return periods * factor;
}

} // namespace detail

/// Build an interval from a count and an upstream period-type character.
///
/// Reproduces `JpmcdsMakeDateInterval`'s normalisation. Case-insensitive, as
/// upstream is here (note that `advance` below is NOT -- see UPSTREAM-3 in
/// docs/port-notes.md).
///
/// Returns nullopt for the period types upstream accepts but cannot advance
/// by, as well as for genuinely invalid characters.
[[nodiscard]] constexpr std::optional<DateInterval> make_date_interval(int periods,
                                                                       char period_type) noexcept {
  const char t = (period_type >= 'a' && period_type <= 'z')
                     ? static_cast<char>(period_type - 'a' + 'A')
                     : period_type;
  // Scaling can overflow for extreme counts, and upstream wraps when it does.
  // Rejecting is the third refusal to reproduce undefined behaviour.
  const auto months = [periods](int factor) -> std::optional<DateInterval> {
    const std::optional<int> n = detail::scale(periods, factor);
    return n ? std::optional{DateInterval{*n, PeriodUnit::Months}} : std::nullopt;
  };
  const auto days = [periods](int factor) -> std::optional<DateInterval> {
    const std::optional<int> n = detail::scale(periods, factor);
    return n ? std::optional{DateInterval{*n, PeriodUnit::Days}} : std::nullopt;
  };

  switch (t) {
  case 'A': // Annual
  case 'Y': // Year
    return months(12);
  case 'S': // Semi-annual
    return months(6);
  case 'Q': // Quarterly
    return months(3);
  case 'M': // Month
    return months(1);
  case 'W': // Week
    return days(7);
  case 'U': // Lunar period, 28 days
    return days(28);
  case 'D': // Day
    return days(1);
  default:
    // Includes F, E, B, G, H, I, J, K, L, T, V -- which upstream's
    // JpmcdsMakeDateInterval accepts and JpmcdsDtFwdAny then refuses.
    return std::nullopt;
  }
}

/// Parse an interval such as "1Y", "3M", "-6M", or bare "M" (meaning "1M").
///
/// Reproduces `JpmcdsStringToDateInterval`: an optional sign, optional
/// digits defaulting to 1, then a period-type character.
///
/// TWO DELIBERATE DIFFERENCES, both refusals to reproduce undefined behaviour:
///
///   1. Upstream copies the digits into a 32-byte stack buffer with no bounds
///      check (`while (isdigit(*inp)) *nump++ = *inp++;`), a stack buffer
///      overflow for any input with more than 31 digits. See UPSTREAM-4.
///      This bounds the digit count.
///   2. Upstream converts with `atoi`, whose overflow is undefined and which
///      silently yields garbage -- `"9999999999M"` parses as 1410065407
///      months. This rejects any digit run that would not fit an `int`.
///      Found by auditing the port itself under UBSan, not by differential
///      testing: both sides were producing garbage, so they "agreed".
[[nodiscard]] constexpr std::optional<DateInterval>
parse_date_interval(std::string_view s) noexcept {
  constexpr std::size_t kMaxDigits = 31; // upstream's buffer, minus the NUL

  std::size_t i = 0;
  bool negative = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    negative = s[i] == '-';
    ++i;
  }

  int periods = 0;
  std::size_t digits = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    if (++digits > kMaxDigits) {
      return std::nullopt; // upstream overflows its buffer here
    }
    const int digit = s[i] - '0';
    // Check before multiplying: signed overflow is undefined, so it cannot be
    // detected after the fact. Upstream's atoi has the same hole and silently
    // produces a wrapped value.
    if (periods > (std::numeric_limits<int>::max() - digit) / 10) {
      return std::nullopt;
    }
    periods = periods * 10 + digit;
    ++i;
  }
  if (digits == 0) {
    periods = 1; // upstream's default when no digits are present
  }
  if (negative) {
    periods = -periods;
  }

  if (i >= s.size()) {
    return std::nullopt; // no period type character
  }
  return make_date_interval(periods, s[i]);
}

/// Render an interval the way `JpmcdsFormatDateInterval` does.
///
/// Months collapse to the largest exact unit: a multiple of 12 becomes "A",
/// of 6 becomes "S", of 3 becomes "Q", otherwise "M". So 12 months formats
/// as "1A" and 18 months as "3S".
///
/// Days do NOT collapse to weeks, even though upstream contains code to do
/// it: its `case 'D':` has no `break` and falls through into `default:`,
/// which overwrites both values with the raw ones. 14 days formats as "14D",
/// never "2W". That is UPSTREAM-5; the behaviour is reproduced here, and the
/// unreachable intent is not.
///
/// Also note upstream returned a pointer into a toggling pair of static
/// buffers (`static char format[2][16]`), so its result was invalidated by
/// the second subsequent call and was never thread-safe. This returns a value.
[[nodiscard]] inline std::string format_date_interval(DateInterval iv) {
  int periods = iv.periods;
  char type = 'D';
  if (iv.unit == PeriodUnit::Months) {
    if (periods % 12 == 0) {
      type = 'A';
      periods /= 12;
    } else if (periods % 6 == 0) {
      type = 'S';
      periods /= 6;
    } else if (periods % 3 == 0) {
      type = 'Q';
      periods /= 3;
    } else {
      type = 'M';
    }
  }
  return std::to_string(periods) + type;
}

/// Advance a date by an interval. Replaces `JpmcdsDtFwdAny`.
///
/// Month arithmetic clamps rather than overflowing: 31 January plus one month
/// is 28 or 29 February, because upstream routes through JpmcdsNormalizeMDY.
///
/// Returns nullopt where upstream fails: a start date at or before its epoch
/// (it requires `startDate > 0`, so 1601-01-01 itself is rejected), or a
/// month result landing before the epoch year.
[[nodiscard]] constexpr std::optional<Date> advance(Date start, DateInterval iv) noexcept {
  if (to_tdate(start) <= 0) {
    return std::nullopt; // upstream: "startDate must be > 0"
  }
  if (iv.unit == PeriodUnit::Days) {
    // Upstream does a bare addition with no range check. The port cannot:
    // std::chrono::days has a 32-bit representation, so adding a large count
    // to the day number overflows it. Widen, check, then convert.
    const long long result = static_cast<long long>(to_tdate(start)) + iv.periods;
    if (result < static_cast<long long>(kMinTDate) || result > static_cast<long long>(kMaxTDate)) {
      return std::nullopt;
    }
    return from_tdate(static_cast<TDate>(result));
  }
  const std::chrono::year_month_day ymd = to_ymd(start);
  const int month = static_cast<int>(static_cast<unsigned>(ymd.month()));
  // DateInterval is an aggregate, so a caller can fill it with any int without
  // going through make_date_interval. normalize_ymd does its arithmetic in a
  // wider type and range-checks, so the only thing needed here is to avoid
  // overflowing while forming the argument.
  const long long target_month = static_cast<long long>(month) + iv.periods;
  if (target_month < std::numeric_limits<int>::min() ||
      target_month > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  const std::optional<std::chrono::year_month_day> moved =
      normalize_ymd(static_cast<int>(ymd.year()), static_cast<int>(target_month),
                    static_cast<int>(static_cast<unsigned>(ymd.day())));
  if (!moved) {
    return std::nullopt;
  }
  return make_date(static_cast<int>(moved->year()),
                   static_cast<int>(static_cast<unsigned>(moved->month())),
                   static_cast<int>(static_cast<unsigned>(moved->day())));
}

namespace detail {

// Moved here from schedule.hpp once a second module needed it: multiplying an
// interval is interval arithmetic, not scheduling.
/// `start` advanced by `count` whole intervals, with the multiplication done
/// wide. `detail::scale` cannot be used here: it is only correct for a
/// positive factor, and the front-stub branch steps backwards.
///
/// The range check is defensive, not load-bearing. Reaching |count| >= 2
/// requires the count == 1 step to land inside the span, which bounds
/// `periods` by the span length in days -- so while the caller's
/// `periods > 0` precondition holds, the product cannot overflow. It is kept
/// because DateInterval is an aggregate with no invariant of its own.
[[nodiscard]] constexpr std::optional<Date> advance_multiple(Date start, DateInterval iv,
                                                             int count) noexcept {
  const long long periods = static_cast<long long>(iv.periods) * count;
  if (periods < std::numeric_limits<int>::min() || periods > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return advance(start, DateInterval{static_cast<int>(periods), iv.unit});
}

} // namespace detail

} // namespace cdsmodel
