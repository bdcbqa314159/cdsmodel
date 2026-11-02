#pragma once

// Port of upstream buscache.c -- the holiday calendar, and the end of the
// library's worst piece of global state.
//
// Upstream models a calendar as:
//
//     typedef struct { TDateList *dateList; long weekends; } THolidayList;
//
// and reaches them through a process-wide cache:
//
//     THoliday *cache = NULL;          /* buscache.c:62 -- note: no static */
//
// That declaration has no `static`, so it has external linkage and is
// exported from the compiled archive as the symbol `_cache`. It is a
// singly-linked list, mutated by JpmcdsHolidayListAddToCache, emptied by
// JpmcdsHolidayEmptyCache, and lazily initialised inside holidayFind under an
// unsynchronised `if (cache == NULL)` check. Consequences:
//
//   * Two threads pricing against different calendars race on the same list.
//   * The lazy initialisation is itself a data race: two threads arriving
//     with an empty cache both construct the built-ins and both assign.
//   * JpmcdsHolidayListFromCache returns a pointer INTO the cache, so the
//     caller does not own it and any later JpmcdsHolidayEmptyCache leaves
//     every outstanding pointer dangling.
//
// Here a calendar is a value. The caller holds it, copies it, and passes it
// where it is needed. There is no cache, so there is nothing to invalidate
// and nothing to synchronise. That is the whole fix: not a lock, an owner.

#include <cdsmodel/date.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cdsmodel {

/// Which weekdays are non-business days.
///
/// The bit layout matches upstream's `JPMCDS_WEEKEND_*` flags: Monday is bit
/// 0 through Sunday at bit 6. Upstream selects the bit with
/// `1 << (date % 7)`, which works because TDate 0 is 1601-01-01, a Monday.
/// The same bit is `std::chrono::weekday::iso_encoding() - 1`, so the port
/// needs no TDate arithmetic to agree.
enum class Weekend : std::uint8_t {
  None = 0,
  Monday = 1u << 0,
  Tuesday = 1u << 1,
  Wednesday = 1u << 2,
  Thursday = 1u << 3,
  Friday = 1u << 4,
  Saturday = 1u << 5,
  Sunday = 1u << 6,
  /// Upstream's `JPMCDS_WEEKEND_STANDARD`.
  Standard = (1u << 5) | (1u << 6),
};

[[nodiscard]] constexpr Weekend operator|(Weekend a, Weekend b) noexcept {
  return static_cast<Weekend>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr Weekend operator&(Weekend a, Weekend b) noexcept {
  return static_cast<Weekend>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

/// The raw flag value, for interop with upstream's `long weekends`.
[[nodiscard]] constexpr long to_upstream_weekends(Weekend w) noexcept {
  return static_cast<long>(static_cast<std::uint8_t>(w));
}

[[nodiscard]] constexpr std::optional<Weekend> weekend_from_upstream(long flags) noexcept {
  if (flags < 0 || flags > 0x7F) {
    return std::nullopt;
  }
  return static_cast<Weekend>(static_cast<std::uint8_t>(flags));
}

/// Does this set include the given weekday?
[[nodiscard]] constexpr bool includes(Weekend set, std::chrono::weekday day) noexcept {
  const unsigned bit = 1u << (day.iso_encoding() - 1u);
  return (static_cast<std::uint8_t>(set) & bit) != 0;
}

/// A holiday calendar: a set of holiday dates plus which weekdays are
/// non-business days. A value, not a cache entry.
class Calendar {
public:
  /// A calendar with standard weekends and no holidays.
  ///
  /// This is upstream's built-in `"NONE"`, whose name is misleading: it means
  /// "no holiday dates", NOT "nothing is a non-business day" -- Saturday and
  /// Sunday are still weekends.
  [[nodiscard]] static Calendar none() { return Calendar{{}, Weekend::Standard}; }

  /// A calendar where every day is a business day. Upstream's built-in
  /// `"NO_WEEKENDS"`.
  [[nodiscard]] static Calendar no_weekends() { return Calendar{{}, Weekend::None}; }

  /// Build a calendar, reproducing upstream's `verifyHolidayList` exactly.
  ///
  /// Two behaviours, both deliberate:
  ///
  ///   * **Rejects** a holiday list that is not strictly increasing. Upstream
  ///     rejects too, and must: `JpmcdsHolidayListIsHoliday` does a `bsearch`,
  ///     which would silently give wrong answers on unsorted input. The port
  ///     does not quietly sort, because accepting input upstream refuses
  ///     would be a behaviour change.
  ///   * **Drops** any holiday falling on a weekend. Upstream compacts those
  ///     out of the stored array, so `is_holiday` reports false for a holiday
  ///     that fell on a Saturday even though the caller supplied it. Visible
  ///     through `holidays()` as well as the predicate.
  ///
  /// Also rejects a date at or before the epoch: upstream's check starts from
  /// `lastDate = 0` and tests `thisDate <= lastDate`, so TDate 0 can never be
  /// a holiday.
  [[nodiscard]] static std::optional<Calendar> create(std::span<const Date> holidays,
                                                      Weekend weekends) {
    std::vector<Date> kept;
    kept.reserve(holidays.size());

    TDate last = 0;
    for (Date d : holidays) {
      const TDate t = to_tdate(d);
      if (t <= last) {
        return std::nullopt; // not strictly increasing (and excludes TDate 0)
      }
      last = t;
      if (!includes(weekends, std::chrono::weekday{d})) {
        kept.push_back(d); // weekend holidays are dropped, as upstream does
      }
    }
    return Calendar{std::move(kept), weekends};
  }

  /// Is this date a weekend under this calendar's weekend set?
  [[nodiscard]] bool is_weekend(Date d) const noexcept {
    return includes(weekends_, std::chrono::weekday{d});
  }

  /// Is this date in the holiday list?
  ///
  /// Does NOT consider weekends, matching `JpmcdsHolidayListIsHoliday`. Note
  /// that a holiday which fell on a weekend was dropped at construction, so
  /// this reports false for it.
  [[nodiscard]] bool is_holiday(Date d) const noexcept {
    return std::ranges::binary_search(holidays_, d);
  }

  /// Weekends first, then the holiday list -- the order upstream uses.
  [[nodiscard]] bool is_business_day(Date d) const noexcept {
    return !is_weekend(d) && !is_holiday(d);
  }

  /// The stored holiday dates: strictly increasing, with weekend dates
  /// already removed.
  [[nodiscard]] std::span<const Date> holidays() const noexcept { return holidays_; }

  [[nodiscard]] Weekend weekends() const noexcept { return weekends_; }

private:
  Calendar(std::vector<Date> holidays, Weekend weekends)
      : holidays_{std::move(holidays)}, weekends_{weekends} {}

  std::vector<Date> holidays_;
  Weekend weekends_;
};

} // namespace cdsmodel
