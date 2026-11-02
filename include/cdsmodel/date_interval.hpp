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
  switch (t) {
  case 'A': // Annual
  case 'Y': // Year
    return DateInterval{periods * 12, PeriodUnit::Months};
  case 'S': // Semi-annual
    return DateInterval{periods * 6, PeriodUnit::Months};
  case 'Q': // Quarterly
    return DateInterval{periods * 3, PeriodUnit::Months};
  case 'M': // Month
    return DateInterval{periods, PeriodUnit::Months};
  case 'W': // Week
    return DateInterval{periods * 7, PeriodUnit::Days};
  case 'U': // Lunar period, 28 days
    return DateInterval{periods * 28, PeriodUnit::Days};
  case 'D': // Day
    return DateInterval{periods, PeriodUnit::Days};
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
/// ONE DELIBERATE DIFFERENCE: upstream copies the digits into a 32-byte stack
/// buffer with no bounds check (`while (isdigit(*inp)) *nump++ = *inp++;`),
/// which is a stack buffer overflow for any input with more than 32 digits.
/// See UPSTREAM-4 in docs/port-notes.md. This bounds the digit count and
/// rejects anything longer, rather than reproducing the overflow.
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
    periods = periods * 10 + (s[i] - '0');
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
    // Upstream does a bare addition with no range check, so neither do we.
    return start + std::chrono::days{iv.periods};
  }
  const std::chrono::year_month_day ymd = to_ymd(start);
  const std::optional<std::chrono::year_month_day> moved =
      normalize_ymd(static_cast<int>(ymd.year()),
                    static_cast<int>(static_cast<unsigned>(ymd.month())) + iv.periods,
                    static_cast<int>(static_cast<unsigned>(ymd.day())));
  if (!moved) {
    return std::nullopt;
  }
  return make_date(static_cast<int>(moved->year()),
                   static_cast<int>(static_cast<unsigned>(moved->month())),
                   static_cast<int>(static_cast<unsigned>(moved->day())));
}

} // namespace cdsmodel
