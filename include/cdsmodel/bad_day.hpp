#pragma once

// Port of upstream busday.c -- the bad day conventions, which decide what to
// do when a date lands on a non-business day.
//
// Upstream passes the convention as a `long` holding a character code
// (`((long)'F')` and friends), so any long reaches the call site and an
// invalid one is caught at the bottom of a switch. Here it is an enum class.
//
// Scope: the conventions and the adjustment. `badday.c` itself is NOT ported
// here despite the name -- it holds TBadDayList, an explicit bad-to-good
// override table used only by the zero-curve swap machinery (zcswap.c,
// zcswdate.c, gtozc.c), so it belongs with layer 6.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

namespace cdsmodel {

/// What to do with a date that is not a business day.
///
/// The values are upstream's character codes, so interop is a cast.
enum class BadDayConv : char {
  Follow = 'F',   ///< the next business day
  Previous = 'P', ///< the previous business day
  None = 'N',     ///< leave the date alone, business day or not
  Modified = 'M', ///< follow, unless that leaves the month, then go back
};

[[nodiscard]] constexpr long to_upstream_code(BadDayConv conv) noexcept {
  return static_cast<long>(static_cast<char>(conv));
}

/// Replaces `JpmcdsBadDayConvValid`.
[[nodiscard]] constexpr std::optional<BadDayConv> bad_day_conv_from_upstream(long code) noexcept {
  switch (code) {
  case 'F':
    return BadDayConv::Follow;
  case 'P':
    return BadDayConv::Previous;
  case 'N':
    return BadDayConv::None;
  case 'M':
    return BadDayConv::Modified;
  default:
    return std::nullopt;
  }
}

/// Roll `start` in `direction` (+1 or -1) until it is a business day in every
/// calendar. Returns `start` itself when it already is.
///
/// Replaces upstream's `getNextBusDate` / `getNextBusDateMulti`. Upstream's
/// version is an unbounded `do { ... } while (aHoliday)`, which never
/// terminates for a calendar whose weekend set covers all seven days -- a
/// calendar it happily constructs (UPSTREAM-6).
///
/// The bound here is derived, not guessed -- and the first version of it was
/// wrong, which the differential suite caught. The reasoning:
///
///   * Stored holiday lists never contain weekend dates (`Calendar::create`
///     strips them), so every stored holiday blocks one *business* weekday.
///   * Reaching the next business weekday costs at most 7 days, since in the
///     worst case exactly one weekday is a business day. That is where the
///     first attempt went wrong: it charged one day per holiday, which fails
///     for a weekend set like 0x7E where only Monday trades.
///   * The initial run of weekend days before the first candidate costs at
///     most 6.
///
/// So `7 * (total holidays + 2)` is a safe ceiling. Exceeding it means the
/// precondition was violated, not that the search needs longer.
[[nodiscard]] inline std::optional<Date>
roll_to_business_day(Date start, int direction, std::span<const Calendar> calendars) noexcept {
  if (direction != 1 && direction != -1) {
    return std::nullopt;
  }

  std::size_t total_holidays = 0;
  for (const Calendar &cal : calendars) {
    if (!cal.allows_business_days()) {
      return std::nullopt; // no business day exists in this calendar, ever
    }
    total_holidays += cal.holidays().size();
  }
  const std::size_t budget = 7 * (total_holidays + 2);

  const auto is_business_everywhere = [calendars](Date d) {
    for (const Calendar &cal : calendars) {
      if (!cal.is_business_day(d)) {
        return false;
      }
    }
    return true;
  };

  Date d = start;
  for (std::size_t step = 0; step <= budget; ++step) {
    if (is_business_everywhere(d)) {
      return d;
    }
    const long long next = static_cast<long long>(to_tdate(d)) + direction;
    if (next < static_cast<long long>(kMinTDate) || next > static_cast<long long>(kMaxTDate)) {
      return std::nullopt; // rolled off the representable range
    }
    d = from_tdate(static_cast<TDate>(next));
  }
  return std::nullopt; // unreachable for a calendar that allows business days
}

/// Adjust a date onto a business day under the given convention.
///
/// Replaces `JpmcdsHolidayListBusinessDay` and
/// `JpmcdsMultiHolidayListBusinessDay`. With several calendars the result is
/// the first date in the chosen direction that is a business day in *all* of
/// them, which is what upstream's restart-on-change loop computes.
///
/// `Modified` follows first and, if that lands in a different month, goes
/// back instead -- comparing month only, as upstream does. A year boundary is
/// covered incidentally, since crossing one also changes the month.
[[nodiscard]] inline std::optional<Date> adjust(Date date, BadDayConv conv,
                                                std::span<const Calendar> calendars) noexcept {
  if (conv == BadDayConv::None) {
    return date; // upstream returns the input untouched, business day or not
  }
  if (calendars.empty()) {
    return std::nullopt; // upstream rejects a null holiday list array
  }

  if (conv == BadDayConv::Previous) {
    return roll_to_business_day(date, -1, calendars);
  }

  const std::optional<Date> forward = roll_to_business_day(date, +1, calendars);
  if (conv == BadDayConv::Follow || !forward) {
    return forward;
  }

  // Modified: only fall back to rolling backwards if following left the month.
  if (to_ymd(*forward).month() == to_ymd(date).month()) {
    return forward;
  }
  return roll_to_business_day(date, -1, calendars);
}

/// Single-calendar convenience, the common case.
[[nodiscard]] inline std::optional<Date> adjust(Date date, BadDayConv conv,
                                                const Calendar &calendar) noexcept {
  return adjust(date, conv, std::span<const Calendar>{&calendar, 1});
}

} // namespace cdsmodel
