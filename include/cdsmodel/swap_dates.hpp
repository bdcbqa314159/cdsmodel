#pragma once

// Port of upstream zcswdate.c -- the dates a swap needs before it can be
// bootstrapped: each maturity in both its adjusted and unadjusted form, the
// coupon date before it, and whether the schedule counts forward from the
// value date or backward from the maturity.
//
//     typedef struct _swapDates
//     {
//         TDate    *adjusted;    /* Adjusted dates */
//         TDate    *original;    /* UNadjusted dates */
//         TDate    *previous;    /* Previous coupon dates */
//         TBoolean *onCycle;     /* T=Count fwd from Val date */
//         int       numDates;    /* Length of all 4 arrays. */
//     } TSwapDates;
//
// FOUR parallel arrays, one more than `ZCurve` and the most in the project.
// The comment "Length of all 4 arrays" is the invariant, maintained by hand
// across three functions. One vector of one struct here.
//
// `JpmcdsSwapDatesNewEmpty` and `JpmcdsSwapDatesFree` have no port -- they
// allocate and release the four arrays, which is what a vector does. Worth
// recording that the constructor accepts a NEGATIVE count and reports success:
//
//     NewEmpty(-3) -> a TSwapDates  numDates=-3 arrays=NULL
//
// so a caller gets an object whose stated length is -3. Every loop in the file
// is `idx < numDates`, so none of them runs and nothing reads through the null
// arrays -- the nonsense is contained by accident rather than refused. Not
// filed: no consumer in the library trusts `numDates` as a size, and the port
// has no equivalent because a vector's size cannot be negative.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/expected.hpp>

#include <chrono>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cdsmodel {

/// One swap maturity and everything derived from it. Replaces one index into
/// all four of upstream's arrays.
struct SwapDate {
  Date adjusted;
  Date original;
  Date previous;         ///< the coupon date before the maturity
  bool on_cycle = false; ///< true: count forward from the value date

  friend constexpr bool operator==(const SwapDate &, const SwapDate &) = default;
};

/// Move a date onto a business day. Replaces `JpmcdsZCAdjustDate`.
///
/// ## UPSTREAM-26 -- a substitution list silently disables the convention
///
/// Upstream branches on the presence of a list, not on its contents:
///
/// ```c
/// if (badDayList != NULL)
/// {
///     *adjDate = JpmcdsBad2GoodBadDayList(badDayList, date);
///     return SUCCESS;
/// }
/// else
/// {
///     if (JpmcdsBusinessDay(date, badDayConv, holidayFile, adjDate) == FAILURE)
///         goto done;
/// }
/// ```
///
/// So when a list is supplied, `badDayConv` and `holidayFile` are never
/// consulted -- and `JpmcdsBad2GoodBadDayList` returns the input unchanged for
/// any date the list does not mention. The function is documented as
/// *"Adjusts date to fall on a business day, if necesssary"*, and on that path
/// it does not. Measured, with a list containing one Saturday:
///
/// ```
/// Saturday, no list, conv=F                154504   (moved to Monday)
/// Saturday, in the list, conv=F            154504   (from the list)
/// another Saturday, no list, conv=F        154511   (moved)
/// another Saturday, list given, conv=F     154509   <- STILL A SATURDAY
/// ```
///
/// The second Saturday is absent from the list, so it comes back untouched
/// even though a convention was supplied and would have moved it.
///
/// An **empty** list does this to every date, because the test is on the
/// pointer:
///
/// ```
/// no list (NULL),   conv=F -> 154504
/// EMPTY list,       conv=F -> 154502   <- unchanged, still a Saturday
/// ```
///
/// That measurement decided this function's signature. "No list" and "a list
/// with nothing in it" are genuinely different inputs with different results,
/// so the port takes `std::optional<std::span<const BadDayPair>>` rather than
/// a plain span -- a span alone would merge the two cases and silently change
/// what an empty list means.
///
/// The behaviour is reproduced, not corrected. The two modes are evidently
/// intended as alternatives -- a caller either supplies a complete
/// substitution table or asks for a convention -- and a list that omits a bad
/// day is incomplete, which is the caller's business. What is wrong is the
/// documented purpose, not the code, and the port's answer to that is the
/// signature: `bad_days` being an `optional` says "either this or the
/// convention" where upstream's two unrelated parameters say "both".
[[nodiscard]] inline Expected<Date>
zc_adjust_date(Date date, std::optional<std::span<const BadDayPair>> bad_days, BadDayConv conv,
               std::span<const Calendar> calendars) {
  if (bad_days) {
    // UPSTREAM-26: the convention and calendars are not consulted here.
    return bad_to_good(*bad_days, date);
  }
  return adjust(date, conv, calendars);
}

namespace detail {

/// Is the maturity on the value date's coupon cycle, and what is the coupon
/// date before it? Replaces the file-local `SetPrevDateAndOnCycle`.
///
/// ## UPSTREAM-27 -- a day-of-month heuristic that suppresses real cycles
///
/// Upstream decides on-cycle in two steps, and the first is a guess:
///
/// ```c
/// if ((valueMDY.day <= 28) && (origMDY.day <= 28))
/// {
///     /*
///      * We assume we can only be on cycle if date is not on or after
///      * the 29th of the month.
///      */
///     JpmcdsCountDates(valueDate, origDate, &interval, &numIntervals, &extraDays);
///     *onCycle = (extraDays == 0);
/// }
/// else
/// {
///     *onCycle = FALSE;
/// }
/// ```
///
/// The assumption is false. `JpmcdsCountDates` reports an exact whole number
/// of intervals for month-end dates perfectly well:
///
/// ```
/// day 29: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
/// day 30: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
/// day 31: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
/// ```
///
/// Two things follow. The reported `onCycle` flag is wrong for those dates --
/// swept over every month, the days 26 to 31 and four frequencies, the
/// heuristic suppresses **600 of 7,296** genuinely on-cycle combinations.
///
/// And because the flag chooses how `previous` is computed -- forward from the
/// value date when on cycle, backward from the maturity when not -- the
/// suppression can change the date:
///
/// ```
/// freq=1   2024-02-29 -> 2025-02-28 : on-cycle 154556, off-cycle 154555  DIFFER by 1
/// freq=2   2024-02-29 -> 2027-02-28 : on-cycle 155468, off-cycle 155467  DIFFER by 1
/// ...
/// 14 of the 600 give a different previous coupon date (2.3%)
/// ```
///
/// Every one has a **29 February value date**, which is the same clamping
/// asymmetry as UPSTREAM-24: forward from the 29th and backward from the 28th
/// do not meet, because month arithmetic is not invertible.
///
/// Worth being precise about which answer is which. In the annual case the
/// forward route is `valueDate + 0 intervals`, i.e. the value date itself --
/// which is by definition on the schedule. The backward route gives the day
/// before it, a date the schedule does not contain. So the heuristic is not
/// merely less precise here; on these fourteen it picks the route that returns
/// a date that is not a coupon date.
///
/// Reproduced, not corrected. Deciding that a swap's previous coupon date
/// should move by a day is a convention question, not a porting one -- the
/// same reasoning as UPSTREAM-24, and the same conclusion.
///
/// One further note on the original, which the port does not need to
/// reproduce: `numIntervals` is declared uninitialised and read only in the
/// on-cycle branch. That is safe, because `*onCycle` can only be true where
/// `JpmcdsCountDates` has just set it -- but the safety is an argument about
/// control flow rather than a property of the code, which is exactly the shape
/// UPSTREAM-14 is. Here the argument happens to hold.
[[nodiscard]] inline Expected<std::pair<bool, Date>>
previous_and_on_cycle(Date value_date, Date original, long freq) {
  const Expected<DateInterval> interval = frequency_to_interval(freq);
  if (!interval) {
    return unexpected(interval.error());
  }
  const unsigned value_day = static_cast<unsigned>(to_ymd(value_date).day());
  const unsigned original_day = static_cast<unsigned>(to_ymd(original).day());

  bool on_cycle = false;
  int intervals = 0;
  if (value_day <= 28 && original_day <= 28) {
    // UPSTREAM-27: the guard, reproduced.
    const Expected<DateCount> counted = count_dates(value_date, original, *interval);
    if (!counted) {
      return unexpected(counted.error());
    }
    intervals = counted->intervals;
    on_cycle = counted->extra_days == 0;
  }

  // On cycle: forward from the value date. Off cycle: back one from the
  // maturity. `intervals` is only read on the first path, which is the only
  // one that sets it -- the port keeps it initialised regardless.
  const Expected<Date> previous =
      on_cycle ? date_from_date_and_offset(value_date, *interval, intervals - 1)
               : date_from_date_and_offset(original, *interval, -1);
  if (!previous) {
    return unexpected(previous.error());
  }
  return std::pair{on_cycle, *previous};
}

} // namespace detail

/// Build the swap dates from maturities that are ALREADY business-day
/// adjusted. Replaces `JpmcdsSwapDatesNewFromAdjusted`.
///
/// The unadjusted date is recovered by running the substitution table
/// backwards -- `good_to_bad` -- which only returns the original if the table
/// is injective. For a table that maps two bad days onto one good day the
/// recovered "original" is whichever entry comes first, and the on-cycle
/// decision is then made from that date rather than from the one the caller
/// meant. Not a defect in this function; it is the reverse lookup behaving as
/// `bad_day_map.hpp` records, and `is_well_formed` is the check that catches
/// the table.
[[nodiscard]] inline Expected<std::vector<SwapDate>>
swap_dates_from_adjusted(Date value_date, long freq, std::span<const Date> adjusted,
                         std::optional<std::span<const BadDayPair>> bad_days) {
  std::vector<SwapDate> dates;
  dates.reserve(adjusted.size());
  for (const Date date : adjusted) {
    SwapDate entry;
    entry.adjusted = date;
    entry.original = bad_days ? good_to_bad(*bad_days, date) : date;
    const Expected<std::pair<bool, Date>> derived =
        detail::previous_and_on_cycle(value_date, entry.original, freq);
    if (!derived) {
      return unexpected(derived.error());
    }
    entry.on_cycle = derived->first;
    entry.previous = derived->second;
    dates.push_back(entry);
  }
  return dates;
}

/// Build the swap dates from UNADJUSTED maturities, adjusting each.
/// Replaces `JpmcdsSwapDatesNewFromOriginal`.
///
/// Note which date the cycle decision uses: the **original**, in both
/// constructors. So two callers describing the same swap -- one holding
/// adjusted dates, one holding unadjusted -- agree on `on_cycle` only insofar
/// as the substitution table round trips. That is upstream's design and it is
/// why `from_adjusted` bothers to recover the original at all.
[[nodiscard]] inline Expected<std::vector<SwapDate>>
swap_dates_from_original(Date value_date, long freq, std::span<const Date> original,
                         std::optional<std::span<const BadDayPair>> bad_days, BadDayConv conv,
                         std::span<const Calendar> calendars) {
  std::vector<SwapDate> dates;
  dates.reserve(original.size());
  for (const Date date : original) {
    SwapDate entry;
    entry.original = date;
    const Expected<Date> moved = zc_adjust_date(date, bad_days, conv, calendars);
    if (!moved) {
      return unexpected(moved.error());
    }
    entry.adjusted = *moved;
    const Expected<std::pair<bool, Date>> derived =
        detail::previous_and_on_cycle(value_date, date, freq);
    if (!derived) {
      return unexpected(derived.error());
    }
    entry.on_cycle = derived->first;
    entry.previous = derived->second;
    dates.push_back(entry);
  }
  return dates;
}

} // namespace cdsmodel
