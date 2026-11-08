#pragma once

// Port of upstream schedule.c -- the coupon-date schedule for one leg of a
// swap. This is the module `streamcf.c` and `zcswap.c` are both waiting on,
// and the last leaf on the path to the curve bootstrap.
//
// Five exported functions and one static:
//
//   JpmcdsNewCouponDatesSwap      -> coupon_schedule
//   JpmcdsGenerateUnadjDates      -> generate_unadjusted_dates
//   JpmcdsCouponDateListCheck     -> is_well_formed
//   JpmcdsNewEmptyCouponDateList  -> (gone: std::vector)
//   JpmcdsFreeCouponDateList      -> (gone: std::vector)
//   CheckArgsSwapMakeSchedule     -> folded into coupon_schedule's refusals
//
// ## Sixteen parameters become three arguments
//
// `JpmcdsNewCouponDatesSwap` takes sixteen parameters, eleven of them scalars
// that differ only in meaning: four TDates, three bad-day conventions, three
// TBooleans and two day offsets, all positional. Swapping `payBadDayConv` and
// `accBadDayConv`, or `firstRollDate` and `lastRollDate`, compiles silently
// and produces a different schedule. The split here is by lifetime rather
// than by type: what the contract says (`CouponScheduleTerms`), what the
// market convention says (`CouponConventions`), and the calendar.
//
// ## Three sentinels and an illegal combination leave the value space
//
// Upstream uses `> 0` to mean "this TDate was supplied" for `firstRollDate`,
// `lastRollDate` and `fullFirstCoupDate`. TDate 0 is 1601-01-01, a real date,
// so the sentinel and the value space overlap -- these are `std::optional`
// here. `CheckArgsSwapMakeSchedule` then rejects both roll dates being
// supplied at once; a variant makes that unrepresentable instead, so the
// check has nothing left to check.
//
// ## Upstream's stub location is an accumulating sum
//
// `TStubType` is NO_STUB 0, BACK_STUB 1, FRONT_STUB 2, TWO_STUB 3, and
// `JpmcdsGenerateUnadjDates` builds its answer with `*stubInfo += ...`,
// relying on FRONT + BACK happening to equal TWO. Two independent bools here,
// combined once at the end, so the encoding is a consequence rather than a
// precondition.
//
// ## The dead field
//
// `TCouponDates` carries an `exDividendDate` that no line in the library ever
// reads or writes -- it is zero in every schedule upstream has ever produced.
// Dropped rather than ported as a field that is always 1601-01-01.
//
// ## Dead code, faithfully not reproduced
//
// The long-stub branch is guarded by `couponInterval->prd_typ != 'I' && != 'E'
// && != 'J'`. Those three period types are accepted by
// `JpmcdsMakeDateInterval` and then refused by `JpmcdsDtFwdAny`, which runs
// first in the stepping loop -- so the guard can never be false. In the port
// they are not representable at all (`make_date_interval` returns nullopt for
// them, see date_interval.hpp), so the guard has no expression.
//
// Deliberate differences: UPSTREAM-29 (a zero-period schedule) and
// UPSTREAM-30 (the sizing of the unadjusted-date buffer). Both in
// docs/port-notes.md.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/stub.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <variant>
#include <vector>

namespace cdsmodel {

/// Where the odd period sits. Upstream's `TStubType`, same numeric values so
/// the interop in the differential tests stays a cast.
enum class StubLocation { None = 0, Back = 1, Front = 2, Both = 3 };

/// The dates one coupon period carries.
struct CouponDates {
  Date reset{};
  Date pay{};
  Date accrual_start{};
  Date accrual_end{};

  friend bool operator==(const CouponDates &, const CouponDates &) = default;
};

/// A schedule of coupon periods. Non-empty by construction: `coupon_schedule`
/// refuses rather than return the zero-period list upstream can produce, and
/// every other invariant `JpmcdsCouponDateListCheck` tests is established by
/// the construction itself.
struct CouponSchedule {
  std::vector<CouponDates> periods;
  StubLocation stub = StubLocation::None;
};

/// Anchor the cycle to the first cash flow. Upstream's `firstRollDate`.
struct FirstRollDate {
  Date date;
};

/// Anchor the cycle to the accrual start of the final period. Upstream's
/// `lastRollDate`.
struct LastRollDate {
  Date date;
};

/// At most one roll date may be supplied -- upstream checks this at runtime
/// and fails; here the other case does not exist.
using RollAnchor = std::variant<std::monostate, FirstRollDate, LastRollDate>;

/// What the contract says.
struct CouponScheduleTerms {
  Date start{};
  Date maturity{};
  DateInterval coupon_interval{};

  /// Upstream's `stubAtEnd` TBoolean: TRUE means a back stub.
  StubPosition stub_position = StubPosition::Front;

  /// Upstream's `longStub` TBoolean.
  StubLength stub_length = StubLength::Short;

  RollAnchor roll_anchor{};

  /// Upstream's `fullFirstCoupDate`: the accrual start the first period takes
  /// instead of the schedule's own, which must be on or before `start`.
  /// D7: genuinely absent -- most schedules have no full-first-coupon date,
  /// which is a fact about the terms, not a failure to compute one.
  std::optional<Date> full_first_coupon_date{};
};

/// What the market convention says.
struct CouponConventions {
  BadDayConv pay = BadDayConv::None;
  BadDayConv accrual = BadDayConv::None;
  BadDayConv reset = BadDayConv::None;

  /// Upstream's `adjustLastAccDate`: whether the final accrual date is
  /// business-day adjusted, or left on the unadjusted cycle.
  bool adjust_last_accrual_date = true;

  /// Upstream's `inArrears`: the reset comes off the accrual end, not start.
  bool in_arrears = false;

  /// Business days from accrual end to payment. Upstream does not check the
  /// sign of this one (it does check `reset_offset_days`), so neither does the
  /// port -- a negative value pays before the period ends, which is strange
  /// but is upstream's answer.
  int pay_offset_days = 0;

  /// Business days before the accrual date that the rate is set. Upstream
  /// rejects a negative value, and the port refuses alongside it.
  int reset_offset_days = 0;
};

/// The unadjusted cycle, before any business-day adjustment.
struct UnadjustedDates {
  std::vector<Date> dates;
  StubLocation stub = StubLocation::None;
};

namespace detail {

/// FRONT + BACK == BOTH, stated once instead of relied on arithmetically.
[[nodiscard]] constexpr StubLocation stub_location(bool front, bool back) noexcept {
  if (front && back) {
    return StubLocation::Both;
  }
  if (front) {
    return StubLocation::Front;
  }
  if (back) {
    return StubLocation::Back;
  }
  return StubLocation::None;
}

} // namespace detail

/// Replaces `JpmcdsGenerateUnadjDates`.
///
/// Returns the unadjusted coupon dates in increasing order, and where the odd
/// period falls. Only `start`, `maturity`, `coupon_interval`, the stub fields
/// and `roll_anchor` are read; the conventions play no part this early.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-30. Upstream sizes its scratch array from
/// `JpmcdsCountDates(startDate, matDate, couponInterval) + 3` but steps the
/// generating loop from the roll date, so a roll date outside
/// `[startDate, matDate]` makes the stepped span longer than the array and
/// overflows it. `JpmcdsNewCouponDatesSwap` bounds the roll dates before
/// calling, so the library's own path is safe -- but the function is exported,
/// and a direct caller gets a heap-buffer-overflow (ASan-confirmed; see
/// `defect_evidence --section u30`). A vector removes the sizing question
/// entirely, and the bound is checked here as well.
[[nodiscard]] inline Expected<UnadjustedDates>
generate_unadjusted_dates(const CouponScheduleTerms &terms) {
  if (terms.start > terms.maturity) {
    return fail(ErrorKind::bad_argument, "start must be on or before maturity");
  }

  const auto *first_roll = std::get_if<FirstRollDate>(&terms.roll_anchor);
  const auto *last_roll = std::get_if<LastRollDate>(&terms.roll_anchor);

  // UPSTREAM-30's bound, which upstream applies in its caller rather than
  // here. Checked in both places so neither entry point can overflow.
  if (first_roll && (first_roll->date < terms.start || first_roll->date > terms.maturity)) {
    return fail(ErrorKind::bad_argument,
                "the first roll date is outside the start-to-maturity range");
  }
  if (last_roll && (last_roll->date < terms.start || last_roll->date > terms.maturity)) {
    return fail(ErrorKind::bad_argument,
                "the last roll date is outside the start-to-maturity range");
  }

  // Upstream's JpmcdsCountDates call. Its answer only sized the buffer, but
  // it also rejects a zero interval and one whose sign opposes the span, and
  // those refusals are load-bearing: without them the stepping loop below
  // never terminates.
  if (!count_dates(terms.start, terms.maturity, terms.coupon_interval)) {
    return fail(ErrorKind::bad_argument,
                "the coupon interval does not divide the start-to-maturity range");
  }

  bool front = false;
  bool back = false;
  Date begin{};
  Date end{};
  int direction = 1;
  std::vector<Date> dates;

  if (first_roll != nullptr) {
    begin = first_roll->date;
    end = terms.maturity;
    direction = 1;
    dates.push_back(terms.start);
    if (begin > terms.start) {
      front = true;
    }
  } else if (last_roll != nullptr) {
    begin = last_roll->date;
    end = terms.start;
    direction = -1;
    dates.push_back(terms.maturity);
    if (terms.maturity > begin) {
      back = true;
    }
  } else if (terms.stub_position == StubPosition::Back) {
    begin = terms.start;
    end = terms.maturity;
    direction = 1;
  } else {
    begin = terms.maturity;
    end = terms.start;
    direction = -1;
  }

  dates.push_back(begin);

  // Each step is taken from `begin` by a multiple of the interval, never from
  // the previous date -- so month-end clamping cannot accumulate.
  Date unadjusted{};
  int step = 1;
  do {
    // Widened deliberately: upstream computes `prd * direction * count` in an
    // int and wraps silently when it overflows. (`detail::scale` is no help --
    // its bounds check assumes a positive factor, and this one is signed.)
    const long long count =
        static_cast<long long>(terms.coupon_interval.periods) * direction * step;
    if (count < std::numeric_limits<int>::min() || count > std::numeric_limits<int>::max()) {
      return fail(ErrorKind::bad_argument,
                  "coupon count " + detail::num(count) + " is not representable as an int");
    }
    ++step;
    const Expected<Date> next =
        advance(begin, DateInterval{static_cast<int>(count), terms.coupon_interval.unit});
    if (!next) {
      return unexpected(next.error());
    }
    unadjusted = *next;
    dates.push_back((end - unadjusted).count() * direction > 0 ? unadjusted : end);
  } while ((end - unadjusted).count() * direction > 0);

  if (end != unadjusted) {
    if (direction == 1) {
      back = true;
    } else {
      front = true;
    }
  }

  // A long stub absorbs the odd period into its neighbour: drop the date
  // between them. Upstream writes `dateArr[n-2] = dateArr[n-1]; --nDates;`,
  // which is a removal of the second-to-last element, not of the last.
  if (first_roll == nullptr && last_roll == nullptr && terms.stub_length == StubLength::Long &&
      (front || back) && dates.size() > 2) {
    dates[dates.size() - 2] = dates.back();
    dates.pop_back();
  }

  if (direction == -1) {
    std::reverse(dates.begin(), dates.end());
  }

  return UnadjustedDates{std::move(dates), detail::stub_location(front, back)};
}

/// Replaces `JpmcdsCouponDateListCheck`.
///
/// Kept because it is exported and `streamcf.c` calls it on a schedule it did
/// not build. For a schedule from `coupon_schedule` this is always true: the
/// non-emptiness is a refusal, the stub location is an enum, and the orderings
/// follow from generating the dates in order and adjusting them monotonically.
/// It earns its place validating schedules assembled by hand.
///
/// Note what upstream does NOT check, here or anywhere: that the reset date is
/// on or before the pay date. `JpmcdsNewCouponDatesSwap` works to establish
/// exactly that, with a loop pulling the reset back a day at a time, and then
/// the validator never tests it.
[[nodiscard]] inline bool is_well_formed(const CouponSchedule &schedule) {
  if (schedule.periods.empty()) {
    return false;
  }
  if (schedule.periods.back().accrual_end < schedule.periods.front().accrual_start) {
    return false;
  }
  for (const CouponDates &period : schedule.periods) {
    if (period.accrual_end < period.accrual_start) {
      return false;
    }
  }
  for (std::size_t i = 1; i < schedule.periods.size(); ++i) {
    const CouponDates &prev = schedule.periods[i - 1];
    const CouponDates &cur = schedule.periods[i];
    if (prev.accrual_start > cur.accrual_start || prev.accrual_end > cur.accrual_end ||
        prev.pay > cur.pay || prev.reset > cur.reset) {
      return false;
    }
  }
  return true;
}

/// Replaces `JpmcdsNewCouponDatesSwap`.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-29. When every accrual period collapses to
/// zero length -- which `startDate == matDate` guarantees, and which bad-day
/// adjustment can also produce from `startDate < matDate` -- upstream returns
/// a non-NULL `TCouponDateList` with `fNumItems == 0`, a value its own
/// `JpmcdsCouponDateListCheck` rejects. `zcswap.c` reads
/// `floatDL->fArray[floatDL->fNumItems-1]` at three sites with no emptiness
/// check, so that schedule produces an out-of-bounds read at index -1
/// (ASan-confirmed; see `defect_evidence --section u29`). The port refuses
/// instead, which is why `CouponSchedule::periods` can be assumed non-empty.
[[nodiscard]] inline Expected<CouponSchedule> coupon_schedule(const CouponScheduleTerms &terms,
                                                              const CouponConventions &conventions,
                                                              const Calendar &calendar) {
  // CheckArgsSwapMakeSchedule. The bad-day conventions it validates are an
  // enum here, and the both-roll-dates case is unrepresentable, so what is
  // left is the three order checks. Note upstream's `startDate > matDate`
  // message reads "maturity date is on or before start date", which is wrong
  // for the condition it guards -- equality is accepted, and UPSTREAM-29 is
  // what equality then produces.
  if (terms.start > terms.maturity) {
    return fail(ErrorKind::bad_argument, "start must be on or before maturity");
  }
  if (terms.full_first_coupon_date && *terms.full_first_coupon_date > terms.start) {
    return fail(ErrorKind::bad_argument,
                "the full first coupon date must not be after the start date");
  }
  if (conventions.reset_offset_days < 0) {
    return fail(ErrorKind::bad_argument, "reset offset must not be negative, got " +
                                             detail::num(conventions.reset_offset_days));
  }

  const Expected<UnadjustedDates> unadjusted = generate_unadjusted_dates(terms);
  if (!unadjusted || unadjusted->dates.size() < 2) {
    return fail(ErrorKind::bad_argument, "the unadjusted schedule has fewer than 2 dates");
  }
  const std::vector<Date> &cycle = unadjusted->dates;
  const std::size_t last = cycle.size() - 1;

  // Adjust the accrual dates. Each adjusted date closes one period and opens
  // the next, so the loop runs over dates and writes to two periods.
  std::vector<CouponDates> adjusted(last);
  for (std::size_t i = 0; i < cycle.size(); ++i) {
    Date accrual{};
    if (i != last || conventions.adjust_last_accrual_date) {
      const Expected<Date> moved = adjust(cycle[i], conventions.accrual, calendar);
      if (!moved) {
        return unexpected(moved.error());
      }
      accrual = *moved;
    } else {
      accrual = cycle[i];
    }
    if (i > 0) {
      adjusted[i - 1].accrual_end = accrual;
    }
    if (i < last) {
      adjusted[i].accrual_start = accrual;
    }
  }

  // Drop the periods that adjustment collapsed to zero length, and move the
  // stub location if the one that went was the stub. Both arms read the
  // ORIGINAL stub location, not the one being written: when the front and the
  // back period both collapse, upstream maps Front->None and then Both->Front
  // off the same original value, landing on Front rather than None.
  CouponSchedule schedule;
  schedule.stub = unadjusted->stub;
  for (std::size_t i = 0; i < adjusted.size(); ++i) {
    if (adjusted[i].accrual_start != adjusted[i].accrual_end) {
      schedule.periods.push_back(adjusted[i]);
    } else if (i == 0) {
      // Upstream's `i == 0` arm wins over its `i == last` arm, so a
      // single-period schedule is treated as a collapsed FRONT, never a BACK.
      if (unadjusted->stub == StubLocation::Front) {
        schedule.stub = StubLocation::None;
      } else if (unadjusted->stub == StubLocation::Both) {
        schedule.stub = StubLocation::Back;
      }
    } else if (i == adjusted.size() - 1) {
      if (unadjusted->stub == StubLocation::Back) {
        schedule.stub = StubLocation::None;
      } else if (unadjusted->stub == StubLocation::Both) {
        schedule.stub = StubLocation::Front;
      }
    }
    // A collapsed period in the middle takes no stub with it.
  }

  // UPSTREAM-29: upstream carries on with a zero-period schedule.
  if (schedule.periods.empty()) {
    return fail(ErrorKind::bad_argument, "the coupon schedule has no periods");
  }

  for (CouponDates &period : schedule.periods) {
    const Expected<Date> pay = adjust(period.accrual_end, conventions.pay, calendar);
    if (!pay) {
      return unexpected(pay.error());
    }
    period.pay = *pay;
  }
  if (conventions.pay_offset_days != 0) {
    for (CouponDates &period : schedule.periods) {
      const Expected<Date> moved =
          add_business_days(period.pay, conventions.pay_offset_days, calendar);
      if (!moved) {
        return unexpected(moved.error());
      }
      period.pay = *moved;
    }
  }

  for (CouponDates &period : schedule.periods) {
    const Expected<Date> set =
        adjust(conventions.in_arrears ? period.accrual_end : period.accrual_start,
               conventions.reset, calendar);
    if (!set) {
      return unexpected(set.error());
    }
    period.reset = *set;

    if (conventions.reset_offset_days != 0) {
      const Expected<Date> moved =
          add_business_days(period.reset, -conventions.reset_offset_days, calendar);
      if (!moved) {
        return unexpected(moved.error());
      }
      period.reset = *moved;
    }

    // Upstream: "According to STIRT Traders, ISDA rules are incorrect:
    // Payment can occur on reset date". A reset later than its own payment is
    // pulled back a day at a time until it is not. Terminates for any calendar
    // with a business day, because the date strictly decreases each pass;
    // a calendar with none is UPSTREAM-6, refused by `adjust` before this.
    while (period.reset > period.pay) {
      const Expected<Date> back =
          adjust(period.reset - std::chrono::days{1}, BadDayConv::Previous, calendar);
      if (!back) {
        return unexpected(back.error());
      }
      period.reset = *back;
    }
  }

  // ORDER IS LOAD-BEARING. This override lands after the resets are computed,
  // so period 0 keeps a reset derived from the schedule's own accrual start
  // while its accrual start becomes the earlier full-first-coupon date. The
  // result has `reset > accrual_start` for that period, which no validator
  // objects to and which computing the resets after the override would lose.
  if (schedule.stub > StubLocation::Back && terms.full_first_coupon_date) {
    schedule.periods.front().accrual_start = *terms.full_first_coupon_date;
  }

  return schedule;
}

} // namespace cdsmodel
