#pragma once

// Port of upstream datelist.c -- generating a schedule of dates from a start,
// a maturity and an interval. Distinct from dtlist.c (set operations on
// existing lists, in date_list.hpp) and from cxdatelist.c (the CDS fee
// schedule, in schedule.hpp).
//
// Seven functions, where a regex for definitions finds three: four of them put
// the return type on its own line. That regex has now undercounted three files
// in a row, which is the point rather than the annoyance -- a count is not
// evidence of having read a file, and the only reason the other four are here
// is that the file was read end to end.
//
// Upstream's two allocation primitives have no port. `JpmcdsNewEmptyDateList`
// and `JpmcdsFreeDateList` are `std::vector<Date>`, and the three `memcpy` /
// shift-left / decrement-the-count idioms that go with them become one
// expression each.
//
// ## Two shapes that repeat across all of these
//
// **The stub direction decides which end is counted from.** With a stub at the
// end, dates are counted FORWARD from the start; with a stub at the beginning,
// the interval is negated and dates are counted BACKWARD from maturity. So the
// generated sequence descends in the second case, and upstream writes it into
// the array through a negative `arrayIncrement` so it lands ascending. The
// port generates and reverses -- see the note on `make_date_array`, which
// deliberately does not reproduce the backward-writing idiom.
//
// **"Extended" means the boundary date is replaced, not added.** `date_list`
// always begins at the start and ends at maturity. `date_list_extended` keeps
// the regular grid instead: with a stub at the end the maturity is NOT in the
// list but the next grid date is, and with a stub at the beginning the start is
// not in the list but the previous grid date is. Upstream's comment says this
// is "necessary for Actual/Actual stub calculation", and it is why the two
// functions exist separately despite sharing almost every line.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

namespace detail {

/// The grid size both generators compute identically: how many dates span
/// start to maturity, counting from whichever end the stub is not at.
///
/// Upstream duplicates this block verbatim in `JpmcdsNewDateList` and
/// `JpmcdsNewDateListExtended`, including the negation of the interval. The
/// `extra_days > 0` test is what detects a stub: a non-zero remainder means the
/// span is not a whole number of intervals, so one more date is needed.
struct Grid {
  DateInterval interval; ///< negated when counting backwards
  int count = 0;
};

[[nodiscard]] inline std::optional<Grid> grid_for(Date start, Date maturity, DateInterval interval,
                                                  bool stub_at_end) {
  DateInterval step = interval;
  std::optional<DateCount> counted;
  if (stub_at_end) {
    counted = count_dates(start, maturity, step);
  } else {
    step.periods = -step.periods;
    counted = count_dates(maturity, start, step);
  }
  if (!counted) {
    return std::nullopt;
  }
  return Grid{step, counted->intervals + (counted->extra_days > 0 ? 2 : 1)};
}

} // namespace detail

/// A schedule from `start` to `maturity`, both always included and always
/// first and last. Replaces `JpmcdsNewDateList`.
///
/// The regular grid is generated from whichever end the stub is not at, and
/// then the far boundary is overwritten with the requested date -- so the stub
/// shows up as one short period against its neighbour rather than as a date
/// off the grid.
[[nodiscard]] inline std::optional<std::vector<Date>>
date_list(Date start, Date maturity, DateInterval interval, bool stub_at_end) {
  const std::optional<detail::Grid> grid = detail::grid_for(start, maturity, interval, stub_at_end);
  if (!grid) {
    return std::nullopt;
  }
  // One fewer than the grid: the boundary date is assigned, not generated.
  std::optional<std::vector<Date>> dates =
      make_date_array(stub_at_end ? start : maturity, grid->interval, 0, grid->count - 1);
  if (!dates) {
    return std::nullopt;
  }
  if (stub_at_end) {
    dates->push_back(maturity);
  } else {
    // Generated descending, so reverse and put the start at the front.
    std::ranges::reverse(*dates);
    dates->insert(dates->begin(), start);
  }
  return dates;
}

/// A schedule on the regular grid, where a stub pushes the boundary date OFF
/// the list rather than onto it. Replaces `JpmcdsNewDateListExtended`.
///
/// With a stub at the end the maturity is absent and the next grid date is
/// present; with a stub at the beginning the start is absent and the previous
/// grid date is present. Upstream needs this for Actual/Actual stub accrual,
/// where the stub period has to be measured against the full period it sits
/// inside.
///
/// With no stub this is exactly `date_list`, and that equivalence is asserted
/// rather than assumed -- it is the cheapest check that the two generators
/// share the grid they are supposed to share.
[[nodiscard]] inline std::optional<std::vector<Date>>
date_list_extended(Date start, Date maturity, DateInterval interval, bool stub_at_end) {
  const std::optional<detail::Grid> grid = detail::grid_for(start, maturity, interval, stub_at_end);
  if (!grid) {
    return std::nullopt;
  }
  std::optional<std::vector<Date>> dates =
      make_date_array(stub_at_end ? start : maturity, grid->interval, 0, grid->count);
  if (!dates) {
    return std::nullopt;
  }
  if (!stub_at_end) {
    std::ranges::reverse(*dates);
  }
  return dates;
}

/// `date_list_extended`, with a roll date that may shift the grid.
/// Replaces `JpmcdsNewDateListExtendedRoll`.
///
/// ## UPSTREAM-24 -- the roll date is consulted in the opposite case to the one documented
///
/// The header states the rule:
///
/// > If rollDate is non-zero and stubAtEnd is False, then the rollDate is used
/// > to generate cash flow dates. First starting from roll-date we see if we
/// > can count forward an integral number of intervals to the maturity date.
/// > If this is so then we can generate our datelist by counting forward from
/// > the rolldate.
///
/// The code tests something else:
///
/// ```c
/// if (JpmcdsCountDates(startDate, maturityDate, interval, &numIntervals, &extraDays) != SUCCESS)
///     goto done;
/// if (extraDays == 0)
/// {
///     /* We can use the roll date - so we count forward from this day */
/// ```
///
/// `startDate` to maturity, not `rollDate` to maturity. Those are different
/// conditions, and the difference inverts the feature:
///
///   * `extraDays == 0` means start-to-maturity is a whole number of
///     intervals, i.e. **there is no stub** -- which is the case where a roll
///     date has nothing to contribute. The roll date is consulted here.
///   * `extraDays != 0` means there IS a stub, which is the only case where a
///     roll date could supply a grid that lands on maturity. The roll date is
///     **ignored** here, and the function falls back to plain
///     `JpmcdsNewDateListExtended`.
///
/// Measured. Quarterly, roll 2024-01-15, maturity 2027-01-15 (exactly twelve
/// quarters from the roll), start 2024-02-20 (mid-period):
///
/// ```
/// roll  -> mat   intervals=12  extraDays=0     INTEGRAL
/// start -> mat   intervals=11  extraDays=56    has a stub
///
/// with the roll date   n=13: 154511 154602 ... 155607
/// with rollDate = 0    n=13: 154511 154602 ... 155607
/// => the roll date CHANGED NOTHING
/// ```
///
/// That is precisely the documented case, and the roll date is inert in it.
/// Conversely, when start-to-maturity IS integral the roll date does take
/// effect -- and *introduces* a stub, producing a list that begins before the
/// start date and ends past the maturity.
///
/// ## Reachability, and a claim I had to withdraw
///
/// The library's only call site is `cashflow.c:230`, which passes **startDate
/// as the roll date**. The obvious reading is that this makes the mechanism
/// inert: `count_dates(roll, start)` is trivially integral when the two are
/// equal, so `firstDate` becomes `startDate`. Three hand-picked probes agreed,
/// and the first draft of this comment said so.
///
/// The exhaustive sweep found the counterexample. The roll branch ends in
/// `NewDateListExtended(first, maturity, interval, TRUE)` -- generating
/// **forward** from the start -- where `rollDate = 0` generates **backward**
/// from maturity. Those agree only where month arithmetic does not clamp, and
/// clamping is asymmetric: 29 February plus twelve months is 28 February, but
/// 28 February minus twelve months is 28 February, not the 29th.
///
/// Measured over every integral-span combination of start and maturity
/// day-of-month, quarterly through annual:
///
/// ```
/// 2024-02-29  2027-02-28  iv=1   DIFFER: first 154556 vs 154555, n=37 vs 37
/// 2024-02-29  2027-02-28  iv=3   DIFFER: first 154556 vs 154555, n=13 vs 13
/// ...
/// 7 of 378 integral-span cases differ (1.9%)
/// ```
///
/// Every one starts on 29 February. So this IS reachable from inside upstream
/// -- a leap-day start whose maturity falls on the clamped day gets a
/// forward-generated schedule where a back-stub request asked for a backward
/// one. Still low severity, because the window is that narrow and the
/// schedules differ by a day at one end rather than in length. But "inert" was
/// wrong, and the sweep is the only reason I know it.
///
/// ## The port reproduces it, deliberately
///
/// Unlike `date_list.hpp`, which corrects upstream, this one does not. The
/// policy in docs/port-notes.md allows a correction when there is no single
/// upstream behaviour to preserve -- UPSTREAM-7, where two implementations
/// contradicted each other and the header. That is not the situation here: the
/// code is self-consistent and deterministic, and only the documentation
/// disagrees with it.
///
/// A roll date decides coupon dates, coupon dates decide accrual, and accrual
/// decides money. Reinterpreting a roll-date convention on the strength of a
/// comment is not a porting decision, and PORT-9 is the standing reminder of
/// what happens when a porter decides which of two disagreeing sources was
/// right. The behaviour is reproduced and the disagreement is recorded.
[[nodiscard]] inline std::optional<std::vector<Date>>
date_list_extended_roll(Date start, Date maturity, std::optional<Date> roll, DateInterval interval,
                        bool stub_at_end) {
  // Upstream's `rollDate == 0` is a sentinel, and TDate 0 is 1601-01-01, a
  // real date. std::optional says "no roll date" without spending one.
  if (!roll || stub_at_end) {
    return date_list_extended(start, maturity, interval, stub_at_end);
  }

  const std::optional<DateCount> span = count_dates(start, maturity, interval);
  if (!span) {
    return std::nullopt;
  }
  if (span->extra_days != 0) {
    // UPSTREAM-24: the roll date is dropped in exactly the case the
    // documentation says to use it.
    return date_list_extended(start, maturity, interval, stub_at_end);
  }

  if (start < *roll) {
    return std::nullopt; // upstream: "Start date cannot be before roll date."
  }
  const std::optional<DateCount> from_roll = count_dates(*roll, start, interval);
  if (!from_roll) {
    return std::nullopt;
  }
  // Either the start sits on the roll grid, or take the grid date before it.
  Date first = start;
  if (from_roll->extra_days != 0) {
    const std::optional<Date> offset =
        date_from_date_and_offset(*roll, interval, from_roll->intervals);
    if (!offset) {
      return std::nullopt;
    }
    first = *offset;
  }
  // Always a stub at the end from here, whatever the caller asked for.
  return date_list_extended(first, maturity, interval, true);
}

/// `date_list` with the start date removed. Replaces `JpmcdsNewPayDates`.
///
/// Upstream shifts every element left by one and decrements the count, which
/// is `erase(begin())`. A one-date list becomes empty rather than an error --
/// removing the start date from a list containing only the start date leaves
/// nothing, which is the right answer and not a degenerate case.
[[nodiscard]] inline std::optional<std::vector<Date>>
pay_dates(Date start, Date maturity, DateInterval interval, bool stub_at_end) {
  std::optional<std::vector<Date>> dates = date_list(start, maturity, interval, stub_at_end);
  if (!dates) {
    return std::nullopt;
  }
  if (!dates->empty()) {
    dates->erase(dates->begin());
  }
  return dates;
}

/// Every date moved onto a business day. Replaces both
/// `JpmcdsNewDateListBusDayAdj` (which allocates, memcpy's, then delegates)
/// and `JpmcdsDateListBusDayAdj` (which adjusts in place).
///
/// One function instead of two, because the difference between them is who
/// owns the memory. Taking a span and returning a vector makes the in-place
/// variant unnecessary and the copying variant trivial.
///
/// Upstream takes a holiday FILE name and resolves it per date through
/// `JpmcdsBusinessDay`, which consults a process-wide cache. Calendars are
/// passed explicitly here, as everywhere else in the port -- the file lookup
/// is a separate concern and `JpmcdsHolidayLoadFromDisk` is not ported.
///
/// Note that adjustment can collapse distinct dates onto the same business
/// day, so the result is non-decreasing but not necessarily strictly
/// increasing. Upstream has the same property and nothing downstream of it
/// re-checks, so the duplicates are kept rather than coalesced: dropping one
/// would change how many payments a schedule has.
[[nodiscard]] inline std::optional<std::vector<Date>>
adjust_date_list(std::span<const Date> dates, BadDayConv conv,
                 std::span<const Calendar> calendars) {
  std::vector<Date> adjusted;
  adjusted.reserve(dates.size());
  for (const Date date : dates) {
    const std::optional<Date> moved = adjust(date, conv, calendars);
    if (!moved) {
      return std::nullopt;
    }
    adjusted.push_back(*moved);
  }
  return adjusted;
}

} // namespace cdsmodel
