#pragma once

// Port of upstream cxdatelist.c -- JpmcdsDateListMakeRegular, which turns a
// start date, an end date and a period into the coupon schedule the fee leg
// is built from. This is the last piece between the ported legs and a priced
// CDS.
//
// Two structural changes, both of which delete code rather than add it.
//
// Upstream accumulates into a fixed `TDate tmpDates[100]` and flushes each
// full block into a growing TDateList via JpmcdsDateListAddDatesFreeOld. That
// routine is a sorted merge that drops duplicates, so the block scheme only
// produces the right answer because the schedule happens to be strictly
// monotone; were it ever not, dates would be silently coalesced and the count
// would come out short. Upstream guards that with ASSERT, which compiles away
// under NDEBUG -- so in a release build the invariant is unchecked. A
// std::vector removes the blocks, the merge, and the invariant together.
//
// Upstream also guards the block index with plain assert(), four times, which
// likewise vanishes under NDEBUG. Those asserts are not ported because the
// thing they were guarding no longer exists.
//
// The date arithmetic itself is transcribed exactly, including the one
// decision that matters numerically: each date is computed from the ANCHOR
// with a multiplied interval, never by stepping from the previous date. For
// month intervals those differ -- stepping one month at a time from 31 January
// clamps to the 28th and then stays there, while anchored arithmetic returns
// to the 31st whenever the target month is long enough. Anchoring is correct
// and is preserved.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/stub.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace cdsmodel {

/// The coupon schedule for a regular stream. Replaces
/// `JpmcdsDateListMakeRegular`.
///
/// Both `start` and `end` are always present, and the result is strictly
/// increasing with at least two dates. Every interior date is on cycle from
/// whichever end the stub is NOT at: a front stub anchors on `end`, a back
/// stub anchors on `start`.
///
/// A short stub leaves the odd period as its own entry. A long stub merges it
/// into the adjacent regular period, which is why that case overwrites a date
/// rather than inserting one. When the schedule divides evenly, or when it is
/// shorter than a single period, both lengths give the same answer.
///
/// Returns nullopt where upstream fails: a non-positive period count, `end`
/// not strictly after `start`, or date arithmetic leaving the representable
/// range. `interval.periods > 0` is also what makes the loop terminate -- the
/// date moves monotonically away from its anchor only while the period is
/// positive -- so it is a precondition on correctness, not just on taste.
[[nodiscard]] inline Expected<std::vector<Date>>
make_regular_date_list(Date start, Date end, DateInterval interval, StubMethod stub) {
  if (interval.periods <= 0 || end <= start) {
    return fail(ErrorKind::bad_argument,
                "a schedule needs a positive interval and an end after its start");
  }

  std::vector<Date> dates;
  if (stub.position == StubPosition::Front) {
    // Work backwards from `end`; the stub, if any, lands at the front.
    int count = 0;
    Date date = end;
    while (date > start) {
      dates.push_back(date);
      --count;
      const Expected<Date> next = detail::advance_multiple(end, interval, count);
      if (!next) {
        return unexpected(next.error());
      }
      date = *next;
    }
    // `date` is now the first on-cycle date at or before `start`, and
    // dates.back() is the last one after it.
    if (date == start || dates.size() == 1 || stub.length == StubLength::Short) {
      dates.push_back(start);
    } else {
      dates.back() = start; // long stub absorbs the first regular period
    }
    std::ranges::reverse(dates);
  } else {
    // Work forwards from `start`; the stub, if any, lands at the back.
    int count = 0;
    Date date = start;
    while (date < end) {
      dates.push_back(date);
      ++count;
      const Expected<Date> next = detail::advance_multiple(start, interval, count);
      if (!next) {
        return unexpected(next.error());
      }
      date = *next;
    }
    if (date == end || dates.size() == 1 || stub.length == StubLength::Short) {
      dates.push_back(end);
    } else {
      dates.back() = end; // long stub absorbs the last regular period
    }
  }
  return dates;
}

} // namespace cdsmodel
