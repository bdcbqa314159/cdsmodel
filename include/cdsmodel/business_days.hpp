#pragma once

// Port of upstream busday.c -- counting business days between two dates.
//
// This module is TRANSCRIBED rather than reimplemented, which is the opposite
// of the choice made for ACT/365 in day_count.hpp. The reason is in upstream's
// own comment:
//
//     The values of the table were chosen so that the current ALIB behaviour
//     was left unchanged. Note that because fwdDiffTable is not identical
//     to (-1 x bwdDiffTable), swapping fromDate and toDate in the call to
//     this function does not simply reverse the sign of the result.
//
// The answer is DEFINED BY the two lookup tables, and they are deliberately
// not negatives of each other. There is no underlying formula to recover, so
// a cleaner implementation would be a different function. For ACT/365 the
// intent was recoverable and the port proved equivalence; here the intent is
// "whatever ALIB did", and the tables are the specification.
//
// Consequence worth knowing before using this: the result is NOT
// antisymmetric. See `business_days_between` and the test named for it.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace cdsmodel {

namespace detail {

/// Index into the difference tables: 0 = Monday through 6 = Sunday.
///
/// Upstream writes `date % 7`, which lands on Monday-is-0 only because TDate
/// 0 is a Monday. Deriving it from the weekday keeps the port independent of
/// that coincidence, and `calendar.hpp` has the same note for the weekend mask.
[[nodiscard]] constexpr std::size_t weekday_index(Date d) noexcept {
  return static_cast<std::size_t>(std::chrono::weekday{d}.iso_encoding() - 1U);
}

/// Business days from a Monday-indexed weekday to another, travelling
/// forwards, under standard (Saturday + Sunday) weekends.
inline constexpr std::array<std::array<long, 7>, 7> kForwardDiff{{
    /*            Mo Tu We Th Fr Sa Su */
    /* Monday */ {{0, 1, 2, 3, 4, 4, 4}},
    /* Tuesday */ {{4, 0, 1, 2, 3, 3, 3}},
    /* Wednesday */ {{3, 4, 0, 1, 2, 2, 2}},
    /* Thursday */ {{2, 3, 4, 0, 1, 1, 1}},
    /* Friday */ {{1, 2, 3, 4, 0, 0, 0}},
    /* Saturday */ {{1, 2, 3, 4, 5, 0, 0}},
    /* Sunday */ {{1, 2, 3, 4, 5, 5, 0}},
}};

/// The same travelling backwards. NOT the negation of kForwardDiff -- compare
/// row Monday, column Saturday: 4 forwards against -5 backwards. That
/// discrepancy is upstream's, preserved on purpose.
inline constexpr std::array<std::array<long, 7>, 7> kBackwardDiff{{
    /*            Mo  Tu  We  Th  Fr  Sa  Su */
    /* Monday */ {{0, -1, -2, -3, -4, -5, -5}},
    /* Tuesday */ {{-4, 0, -1, -2, -3, -4, -4}},
    /* Wednesday */ {{-3, -4, 0, -1, -2, -3, -3}},
    /* Thursday */ {{-2, -3, -4, 0, -1, -2, -2}},
    /* Friday */ {{-1, -2, -3, -4, 0, -1, -1}},
    /* Saturday */ {{0, -1, -2, -3, -4, 0, 0}},
    /* Sunday */ {{0, -1, -2, -3, -4, -5, 0}},
}};

/// Business weekdays per week under a weekend set, i.e. 7 minus the popcount.
[[nodiscard]] constexpr long business_weekdays_per_week(Weekend w) noexcept {
  long n = 7;
  for (unsigned bit = 0; bit < 7; ++bit) {
    if ((static_cast<std::uint8_t>(w) & (1U << bit)) != 0) {
      --n;
    }
  }
  return n;
}

} // namespace detail

/// Business days between two dates. Replaces `JpmcdsHolidayListBusinessDaysDiff`.
///
/// Upstream's documented definition:
///
///   * `from == to` gives 0.
///   * `from < to` gives the number of business days in the **closed**
///     interval `[from + 1, to]`.
///   * `from > to` gives the negated count over the closed interval
///     `[to, from - 1]`.
///
/// **This is not antisymmetric.** `business_days_between(a, b)` is not in
/// general `-business_days_between(b, a)`, because the two lookup tables it
/// is built from are not negatives of each other. That is upstream behaviour
/// and is preserved; `BusinessDaysDiff.IsNotAntisymmetric` pins it with a
/// concrete pair, so a future "tidy-up" that makes it symmetric fails loudly.
///
/// Returns nullopt where the dates fall outside the representable range, or
/// where the calendar admits no business day at all.
[[nodiscard]] inline std::optional<long> business_days_between(Date from, Date to,
                                                               const Calendar &calendar) noexcept {
  if (from == to) {
    return 0L;
  }
  if (!is_representable_tdate(to_tdate(from)) || !is_representable_tdate(to_tdate(to))) {
    return std::nullopt;
  }

  const TDate from_t = to_tdate(from);
  const TDate to_t = to_tdate(to);
  const long signum = to_t < from_t ? -1 : 1;
  const Weekend weekends = calendar.weekends();

  long result = 0;
  if (weekends == Weekend::Standard) {
    // Whole weeks contribute five business days each; the residue comes from
    // the table. Integer division truncates toward zero, which is what
    // upstream relies on for the backwards case.
    const long weeks = (to_t - from_t) / 7;
    const TDate landed = from_t + 7 * weeks;
    const long residue =
        landed > to_t
            ? detail::kBackwardDiff[detail::weekday_index(to)][detail::weekday_index(from)]
            : detail::kForwardDiff[detail::weekday_index(from)][detail::weekday_index(to)];
    result = weeks * 5 + residue;
  } else if (weekends == Weekend::None) {
    result = to_t - from_t;
  } else {
    // Any other mask: whole weeks by arithmetic, the residue by stepping.
    const long per_week = detail::business_weekdays_per_week(weekends);
    const long weeks =
        (to_t - from_t) / 7 * signum >= 0 ? (to_t - from_t) / 7 : -((to_t - from_t) / 7);
    const long abs_weeks = weeks < 0 ? -weeks : weeks;
    TDate cursor = from_t + 7 * abs_weeks * signum;
    long extra = 0;
    while (cursor != to_t) {
      cursor += signum;
      if (!calendar.is_weekend(from_tdate(cursor))) {
        ++extra;
      }
    }
    result = (abs_weeks * per_week + extra) * signum;
  }

  // Holidays in the closed interval, in the direction of travel. Stored
  // holidays never fall on a weekend, so every one found is a business day
  // that the arithmetic above already counted.
  long holidays = 0;
  const TDate lo = signum > 0 ? from_t + 1 : to_t;
  const TDate hi = signum > 0 ? to_t : from_t - 1;
  for (Date h : calendar.holidays()) {
    const TDate t = to_tdate(h);
    if (t >= lo && t <= hi) {
      ++holidays;
    }
  }
  return result - holidays * signum;
}

} // namespace cdsmodel
