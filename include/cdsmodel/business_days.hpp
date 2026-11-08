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

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/expected.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
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

// ## Upstream's file-wrapper pattern, and why it collapses here
//
// busday.c publishes most of its work twice: once as `X(args, char *holidayFile)`
// and once as `XFromList(args, THolidayList *hl)`, where the first looks the
// file up in a process-wide cache and calls the second. The port takes a
// `Calendar` explicitly, so ONE function covers both -- the file lookup is the
// caller's business and `JpmcdsHolidayLoadFromDisk` is not ported.
//
// That means each port function here replaces a PAIR, and both names have to
// be claimed or the provenance audit reports a gap that is really a naming
// omission. This has now cost two rounds of chasing false positives, so the
// pairs are named explicitly below rather than left to be inferred:
//
//     JpmcdsHolidayListAddBusinessDays   + JpmcdsDateFromBusDaysOffset
//     JpmcdsHolidayListDateToBusinessEOM + JpmcdsDateToBusinessEOM
//     JpmcdsHolidayListBusinessDay       + JpmcdsBusinessDay      (bad_day.hpp)
//     JpmcdsHolidayListIsBusinessDay     + JpmcdsIsBusinessDay    (calendar.hpp)

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
[[nodiscard]] inline Expected<long> business_days_between(Date from, Date to,
                                                          const Calendar &calendar) noexcept {
  if (from == to) {
    return 0L;
  }
  if (!is_representable_tdate(to_tdate(from)) || !is_representable_tdate(to_tdate(to))) {
    return fail(ErrorKind::bad_argument, "one of the dates is outside the representable range");
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

/// Largest business-day offset `add_business_days` will attempt.
///
/// The walk is O(count), so an astronomical count would spin for a very long
/// time before discovering it cannot finish. The representable date range is
/// not a useful limit here: on libstdc++ `std::chrono::days::rep` is 64-bit,
/// so a date 2^62 days away is "representable" and a count that large is
/// technically satisfiable. It is not a financial quantity -- the longest CDS
/// schedules run to a few thousand business days -- so the port states a
/// limit instead. Upstream has no limit and simply spins.
inline constexpr long kMaxBusinessDayOffset = 10'000'000;

/// Move `count` business days from `from`. Replaces
/// `JpmcdsHolidayListAddBusinessDays` and `JpmcdsDateFromBusDaysOffset`, the
/// file-based wrapper that looks a holiday file up in the cache and calls it.
///
/// Unlike `business_days_between` above, this one is REIMPLEMENTED, because
/// the intent is recoverable: step in the given direction, counting business
/// days, until `count` of them have been passed. Upstream splits this across
/// three code paths -- a trivial case, a table-driven fast path for forwards
/// under standard weekends, and a general stepper -- which are optimisations
/// of that single idea, not separate behaviours.
///
/// Worth recording how that was confirmed rather than assumed. Upstream's
/// `offsetTable` gives -1 for Saturday and -2 for Sunday at `n % 5 == 0`,
/// which normalises a weekend start back to the preceding Friday. Checked
/// against the plain walk: five business days from a Saturday is
/// `Sat + 7 - 1 = Friday`, and counting Mon-Fri from that Saturday also lands
/// on Friday. The table encodes the walk; it does not contradict it. The
/// differential sweep then proves it across every weekday, count and
/// weekend set.
///
/// `count == 0` returns the input untouched, business day or not, as upstream
/// does -- so this is not the same as adjusting onto a business day.
///
/// Returns nullopt when the calendar admits no business day at all (the
/// walk could not terminate), or when the result would leave the
/// representable date range.
[[nodiscard]] inline Expected<Date> add_business_days(Date from, long count,
                                                      const Calendar &calendar) noexcept {
  if (count == 0) {
    return from;
  }
  if (!calendar.allows_business_days()) {
    return fail(ErrorKind::unsupported,
                "the calendar has no business weekday, so no offset can be applied");
  }

  // Negating the most negative value is undefined, and `long` is the same
  // width as `long long` on most platforms, so widening does not help.
  // Found by the test written to check absurd counts -- the guard code needed
  // a guard. See PORT-7 in docs/port-notes.md.
  if (count == std::numeric_limits<long>::min()) {
    return fail(ErrorKind::bad_argument, "the offset magnitude is not representable");
  }
  const long long signed_count = static_cast<long long>(count);
  const long long magnitude = signed_count < 0 ? -signed_count : signed_count;
  const int direction = signed_count < 0 ? -1 : 1;

  if (magnitude > kMaxBusinessDayOffset) {
    return fail(ErrorKind::bad_argument, "offset magnitude " + detail::num(magnitude) +
                                             " exceeds the supported maximum " +
                                             detail::num(kMaxBusinessDayOffset));
  }

  // Each business day consumes at least one calendar day, so a count larger
  // than the calendar days available in that direction cannot be satisfied.
  //
  // Computed UNSIGNED on purpose. `kMinTDate` sits near the minimum of
  // `std::chrono::days::rep`, which is 64-bit on libstdc++, so the signed
  // expression `to_tdate(from) - kMinTDate` overflows there -- which it did,
  // rejecting ordinary backward offsets, and only the Linux leg of CI
  // noticed. Both subtractions below are exact because
  // `kMinTDate <= to_tdate(from) <= kMaxTDate`. See PORT-8.
  const auto here = static_cast<unsigned long long>(to_tdate(from));
  const unsigned long long room = direction > 0 ? static_cast<unsigned long long>(kMaxTDate) - here
                                                : here - static_cast<unsigned long long>(kMinTDate);
  if (static_cast<unsigned long long>(magnitude) > room) {
    return fail(ErrorKind::bad_argument, "the offset would leave the representable date range");
  }

  // Same reasoning as roll_to_business_day: reaching the next business day
  // costs at most 7 calendar days, and each holiday can block one more. The
  // offset cap above keeps this product well inside range.
  const long long budget = 7 * (magnitude + static_cast<long long>(calendar.holidays().size()) + 2);

  TDate cursor = to_tdate(from);
  long long remaining = magnitude;
  for (long long step = 0; step < budget && remaining > 0; ++step) {
    const long long next = static_cast<long long>(cursor) + direction;
    if (next < static_cast<long long>(kMinTDate) || next > static_cast<long long>(kMaxTDate)) {
      return fail(ErrorKind::bad_argument,
                  "stepping a business day leaves the representable date range");
    }
    cursor = static_cast<TDate>(next);
    if (calendar.is_business_day(from_tdate(cursor))) {
      --remaining;
    }
  }
  if (remaining > 0) {
    return fail(ErrorKind::no_convergence,
                "ran out of candidate dates before the offset was satisfied");
  }
  return from_tdate(cursor);
}

/// Last business day of the date's own month. Replaces
/// `JpmcdsHolidayListDateToBusinessEOM` and `JpmcdsDateToBusinessEOM`, its
/// file-based wrapper.
///
/// Upstream is literally `JpmcdsDateToEOM` followed by an adjustment with
/// `JPMCDS_BAD_DAY_PREVIOUS`, so the port composes the two pieces it already
/// has. Rolling backwards is what keeps the result inside the month.
[[nodiscard]] inline Expected<Date> business_end_of_month(Date date,
                                                          const Calendar &calendar) noexcept {
  return adjust(end_of_month(date), BadDayConv::Previous, calendar);
}

} // namespace cdsmodel
