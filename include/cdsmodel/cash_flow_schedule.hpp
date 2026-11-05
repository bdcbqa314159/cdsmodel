#pragma once

// Port of upstream cashflow.c -- the schedule constructors, `JpmcdsMakeCFL`
// and `JpmcdsNewCFLFromDL3`. The container half is in cash_flow_list.hpp; this
// is what was waiting on datelist.c and the rest of stub.c, and with it
// cashflow.c is complete.
//
// Separate header because the dependency weight is different: a `CashFlow` is
// a date and an amount and needs nothing, while building a schedule of them
// pulls in date generation, business-day adjustment, calendars and stubs.
//
// ## Upstream's two bitfields become named fields
//
// `JpmcdsMakeCFL` takes a `long flags` carrying five independent booleans and
// a `long stubPlacement` carrying two more, decoded by mask at the top of the
// function:
//
//     flags & JPMCDS_SUBTRACT_INITIAL            (1)
//     flags & JPMCDS_ADD_FINAL                   (2)
//     flags & JPMCDS_PRESTART_ZERO_PAYMENT       (4)
//     flags & JPMCDS_POST_MATURITY_ZERO_PAYMENT  (8)
//     flags & JPMCDS_KEEP_START_DATE            (16)
//     stubPlacement & JPMCDS_STUB_POSN_MASK      (1)  -> stub at end
//     stubPlacement & JPMCDS_STUB_LENGTH_LONG    (2)  -> long stub
//
// Seven booleans in two integers, and nothing stops a caller passing the stub
// bits in the flags word. Structs with named fields here, so the two
// documented illegal combinations can be stated as conditions on names rather
// than on bit patterns.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_schedule.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// What a coupon stream pays and how its periods are measured.
struct CashFlowConventions {
  double rate = 0.0;
  DayCount day_count = DayCount::Act365Fixed;

  /// Upstream's `longStub` TBoolean, which decides whether an odd period is
  /// left short or merged into its neighbour.
  StubLength stub_length = StubLength::Short;

  /// Upstream takes a holiday FILE name per side and resolves it through a
  /// process-wide cache. Calendars are explicit here, and the two sides stay
  /// separate because upstream's signature keeps them separate -- even though
  /// `JpmcdsMakeCFL`, its only caller, passes the same file to both.
  BadDayConv accrual_bad_day = BadDayConv::None;
  BadDayConv pay_bad_day = BadDayConv::None;
};

/// Upstream's `flags` word, one field per bit.
struct CashFlowFlags {
  /// Subtract a unit payment at the start -- the price side of a bond.
  bool subtract_initial = false;
  /// Add a unit repayment of principal at maturity.
  bool add_final = false;
  /// With a front stub, keep the payment date that would have fallen BEFORE
  /// the start date, carrying a zero amount.
  bool prestart_zero_payment = false;
  /// Advance the maturity by one interval and zero the final payment.
  bool post_maturity_zero_payment = false;
  /// Keep the start date even when its amount is zero.
  bool keep_start_date = false;
};

/// Build a coupon stream from a schedule of dates. Replaces
/// `JpmcdsNewCFLFromDL3`.
///
/// `dates` is the accrual grid; `start` and `maturity` are the instrument's
/// own dates, which may sit inside the grid's first and last periods. That is
/// what makes a stub: upstream detects one by comparing them.
///
///     frontStub = startDate    > dates[0]
///     backStub  = maturityDate < dates[numDates-1]
///
/// Those are mutually exclusive for any grid `date_list_extended` produces,
/// because it anchors on whichever end the stub is NOT at -- measured:
///
/// ```
/// stubAtEnd=0: first=154261 last=155722 -> frontStub=1 backStub=0
/// stubAtEnd=1: first=154547 last=156008 -> frontStub=0 backStub=1
/// ```
///
/// But this function is public and takes the grid as an argument, so a caller
/// can hand it a list overhanging both ends and get both. Everything below
/// that depends on which stubs exist therefore has to work for all four
/// combinations, not the two the in-library caller can produce.
///
/// ## UPSTREAM-25 -- a heap buffer overflow on a one-date grid
///
/// With `numDates == 1` the stub branches index past the array in both
/// directions. The front branch:
///
/// ```c
/// JpmcdsStubPayment(accrualDates->fArray[0],
///                   accrualDates->fArray[1],      /* index 1 of a 1-element list */
///                   accrualStart,
///                   accrualDates->fArray[1],
///                   rate, couponDayCountConv,
///                   &cfl->fArray[1].fAmount)      /* WRITE past a 1-element array */
/// ```
///
/// and the back branch reads `accrualDates->fArray[numDates-2]`, which is
/// `fArray[-1]` — before the allocation.
///
/// Both are confirmed by AddressSanitizer, which is worth noting because the
/// previous finding in this same file was invisible to it:
///
/// ```
/// front stub: ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...0f8
///             READ of size 8    #0 JpmcdsNewCFLFromDL3
///             allocated by      #2 JpmcdsNewEmptyDateList
///
/// back stub:  ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...0e8
///             READ of size 8    #0 JpmcdsNewCFLFromDL3
/// ```
///
/// In the unsanitised build the front case happens to return NULL -- the
/// garbage it read as `nextCouponDate` failed `JpmcdsStubPayment`'s
/// `prevCouponDate >= nextCouponDate` check. That is luck, not safety: a
/// garbage value on the other side of that comparison lets the function
/// proceed to the out-of-bounds WRITE, which ASan never gets to report
/// because it aborts on the read first.
///
/// Reachable only through the public header -- `JpmcdsMakeCFL` builds its own
/// grid and `date_list_extended` never returns one date with a stub -- which
/// is why this is medium and not high.
///
/// The port refuses a grid too short for the stub it was told about. There is
/// no answer to compute: a stub needs the period surrounding it, and a
/// one-date grid has none.
///
/// ## The memcpy that copies a buffer onto itself
///
/// A second, smaller thing in the same function, recorded here rather than in
/// the defect register because nothing observable comes of it. The long-stub
/// coalescing block runs two branches in sequence, and the first reassigns the
/// pointer the second reads from:
///
/// ```c
/// if ( frontStub )
/// {
///     datesCopy->fArray[0] = dates->fArray[0];
///     memcpy(&datesCopy->fArray[1], &dates->fArray[2], (numDates - 2) * sizeof(TDate));
///     dates = datesCopy;          /* simply overwrite pointer, hiding original */
///     --numDates;
/// }
/// if (backStub)
/// {
///     memcpy(&datesCopy->fArray[0], &dates->fArray[0], (numDates - 2) * sizeof(TDate));
/// ```
///
/// With both stubs, `dates == datesCopy` by the time the second `memcpy` runs,
/// so source and destination are the same pointer. C11 7.24.1p2 makes copying
/// between overlapping objects undefined, and identical objects overlap.
///
/// Measured: the result is correct anyway (both periods coalesce as intended,
/// five dates to three), and **neither AddressSanitizer nor
/// UndefinedBehaviorSanitizer reports it**. So the project's harness cannot
/// see this class at all, which is the same gap already recorded for
/// uninitialised reads. Not filed as a defect -- no implementation mishandles
/// `memcpy(p, p, n)` -- but recorded, because a compiler entitled to assume
/// `restrict` on those parameters would be within its rights to make it stop
/// working.
///
/// The port has no copy here: coalescing drops one element from a vector.
[[nodiscard]] inline std::optional<std::vector<CashFlow>>
cash_flows_from_dates(std::span<const Date> dates, Date start, Date maturity,
                      const CashFlowConventions &conventions,
                      std::span<const Calendar> accrual_calendars,
                      std::span<const Calendar> pay_calendars, bool use_pre_start_date) {
  if (dates.empty()) {
    return std::vector<CashFlow>{}; // upstream returns an empty list, not an error
  }

  bool front_stub = start > dates.front();
  bool back_stub = maturity < dates.back();

  // Long stubs coalesce the odd period with its neighbour by dropping the
  // adjacent grid date. Upstream does this with two in-place memcpys; one
  // erase each here.
  std::vector<Date> coalesced;
  if (conventions.stub_length == StubLength::Long && (front_stub || back_stub) &&
      dates.size() > 2) {
    coalesced.assign(dates.begin(), dates.end());
    if (front_stub) {
      coalesced.erase(coalesced.begin() + 1);
    }
    if (back_stub) {
      coalesced.erase(coalesced.end() - 2);
    }
    dates = coalesced;
  }

  // UPSTREAM-25: upstream indexes dates[1] / dates[-1] here.
  if ((front_stub || back_stub) && dates.size() < 2) {
    return std::nullopt;
  }

  // The accrual and payment grids, adjusted or not. Upstream aliases the
  // input list when the convention is NONE, specifically to avoid a copy, and
  // then has to remember not to free it.
  std::vector<Date> accrual_storage;
  std::span<const Date> accrual_dates = dates;
  Date accrual_start = start;
  Date accrual_maturity = maturity;
  if (conventions.accrual_bad_day != BadDayConv::None) {
    const std::optional<std::vector<Date>> adjusted =
        adjust_date_list(dates, conventions.accrual_bad_day, accrual_calendars);
    const std::optional<Date> adjusted_start =
        adjust(start, conventions.accrual_bad_day, accrual_calendars);
    const std::optional<Date> adjusted_maturity =
        adjust(maturity, conventions.accrual_bad_day, accrual_calendars);
    if (!adjusted || !adjusted_start || !adjusted_maturity) {
      return std::nullopt;
    }
    accrual_storage = *adjusted;
    accrual_dates = accrual_storage;
    accrual_start = *adjusted_start;
    accrual_maturity = *adjusted_maturity;
  }

  std::vector<Date> pay_storage;
  std::span<const Date> pay_dates = dates;
  Date pay_start = start;
  Date pay_maturity = maturity;
  if (conventions.pay_bad_day != BadDayConv::None) {
    const std::optional<std::vector<Date>> adjusted =
        adjust_date_list(dates, conventions.pay_bad_day, pay_calendars);
    const std::optional<Date> adjusted_start =
        adjust(start, conventions.pay_bad_day, pay_calendars);
    const std::optional<Date> adjusted_maturity =
        adjust(maturity, conventions.pay_bad_day, pay_calendars);
    if (!adjusted || !adjusted_start || !adjusted_maturity) {
      return std::nullopt;
    }
    pay_storage = *adjusted;
    pay_dates = pay_storage;
    pay_start = *adjusted_start;
    pay_maturity = *adjusted_maturity;
  }

  const std::size_t n = dates.size();
  std::vector<CashFlow> flows(n);

  // The first flow carries the start date and nothing else; the last carries
  // the maturity. Note the order: for a single-date grid the second assignment
  // overwrites the first, so the one flow ends up dated at the maturity.
  flows[0].date = use_pre_start_date ? pay_dates.front() : pay_start;
  flows[0].amount = 0.0;
  flows[n - 1].date = pay_maturity;
  for (std::size_t i = 1; i + 1 < n; ++i) {
    flows[i].date = pay_dates[i];
  }

  // Which flows are regular periods, and which are stubs.
  std::size_t first_regular = 1;
  std::size_t last_regular = n - 1;

  if (front_stub) {
    first_regular = 2;
    const std::optional<double> payment =
        stub_payment(accrual_dates[0], accrual_dates[1], accrual_start, accrual_dates[1],
                     conventions.rate, conventions.day_count);
    if (!payment) {
      return std::nullopt;
    }
    flows[1].amount = *payment;
  }
  if (back_stub) {
    last_regular = n - 2;
    const std::optional<double> payment =
        stub_payment(accrual_dates[n - 2], accrual_dates[n - 1], accrual_dates[n - 2],
                     accrual_maturity, conventions.rate, conventions.day_count);
    if (!payment) {
      return std::nullopt;
    }
    flows[n - 1].amount = *payment;
  }

  // An inclusive upper bound, matching upstream's
  // `for (idx = startIdx; idx <= endIdx; idx++)`. It can be LESS than
  // `first_regular` -- a two-date grid with a back stub gives 1 and 0 -- in
  // which case the loop simply does not run.
  //
  // `last_regular` cannot underflow: it is `n - 1` with no stub, and `n - 2`
  // only on the back-stub path, which the size check above has already
  // established needs `n >= 2`. Written out because `std::size_t` would wrap
  // silently if that reasoning were ever wrong.
  for (std::size_t i = first_regular; i <= last_regular && i < n; ++i) {
    const std::optional<double> year_frac =
        year_fraction(accrual_dates[i - 1], accrual_dates[i], conventions.day_count);
    if (!year_frac) {
      return std::nullopt;
    }
    flows[i].amount = conventions.rate * *year_frac;
  }
  return flows;
}

/// Build a coupon stream from an instrument's terms. Replaces
/// `JpmcdsMakeCFL`.
///
/// Generates the schedule, builds the flows from it, then applies the flags.
/// The order matters and is upstream's: the principal adjustments happen
/// before the leading zero flow is dropped, so `subtract_initial` prevents the
/// drop by making that amount non-zero.
///
/// ## The two illegal flag combinations, as conditions on names
///
/// Upstream states them in bit arithmetic and the port states them in fields:
///
///   * `prestart_zero_payment` with either `subtract_initial` or
///     `keep_start_date`. Upstream's message explains both halves: *"cannot
///     subtract initial payment at prestart date"* and *"Also cannot keep both
///     the start and prestart dates."*
///   * `post_maturity_zero_payment` with a back stub, or with `add_final`.
///
/// ## The error message that names a function which does not exist
///
/// Every diagnostic from `JpmcdsMakeCFL` is written with
/// `static char routine[]="JpmcdsMakeCFLRoll"`. There is no
/// `JpmcdsMakeCFLRoll` anywhere in the library, so a user who greps for the
/// name in the message finds nothing. Not a behavioural defect and not filed
/// as one -- but a reminder that the project's own rule about error text
/// applies to error text too, which is why the port's failures carry no names
/// at all and the tests name the condition instead.
///
/// `roll_date` is upstream's third argument to `JpmcdsNewDateListExtendedRoll`
/// and `JpmcdsMakeCFL` always passes `startDate` for it. That is kept rather
/// than simplified away, because it is not a no-op -- see UPSTREAM-24, where
/// passing the start date flips the generation direction for a leap-day start.
[[nodiscard]] inline std::optional<std::vector<CashFlow>>
cash_flows_from_schedule(Date start, Date maturity, DateInterval interval,
                         const CashFlowConventions &conventions, StubPosition position,
                         const CashFlowFlags &flags, std::span<const Calendar> calendars) {
  if (maturity <= start) {
    return std::nullopt;
  }
  if ((flags.subtract_initial || flags.keep_start_date) && flags.prestart_zero_payment) {
    return std::nullopt;
  }

  Date effective_maturity = maturity;
  if (flags.post_maturity_zero_payment) {
    if (position == StubPosition::Back) {
      return std::nullopt; // no post-maturity zero payment with a stub at the end
    }
    if (flags.add_final) {
      return std::nullopt; // nor with a final principal payment
    }
    const std::optional<Date> advanced = advance(maturity, interval);
    if (!advanced) {
      return std::nullopt;
    }
    effective_maturity = *advanced;
  }

  const bool stub_at_end = position == StubPosition::Back;
  const std::optional<std::vector<Date>> schedule =
      date_list_extended_roll(start, effective_maturity, start, interval, stub_at_end);
  if (!schedule) {
    return std::nullopt;
  }

  std::optional<std::vector<CashFlow>> flows =
      cash_flows_from_dates(*schedule, start, effective_maturity, conventions, calendars, calendars,
                            flags.prestart_zero_payment);
  if (!flows) {
    return std::nullopt;
  }
  if (flows->empty()) {
    return flows; // upstream returns the empty list before touching any flag
  }

  if (flags.subtract_initial) {
    flows->front().amount -= 1.0;
  }
  if (flags.add_final) {
    flows->back().amount += 1.0;
  }
  if (flags.post_maturity_zero_payment) {
    flows->back().amount = 0.0;
  }

  // Drop the leading zero flow unless a flag asks for it. Upstream's comment
  // defends the exact comparison -- "Testing equality to zero is no problem
  // since no calculations were done" -- and it holds: that amount is assigned
  // 0.0 literally, and the only thing that can change it before this point is
  // subtract_initial, which makes it -1 and so prevents the drop.
  if (!flags.prestart_zero_payment && !flags.keep_start_date && flows->front().amount == 0.0) {
    flows->erase(flows->begin());
  }
  return flows;
}

} // namespace cdsmodel
