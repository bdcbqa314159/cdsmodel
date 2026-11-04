#pragma once

// Port of upstream dateadj.c (JpmcdsDtFwdAdj) plus JpmcdsDateFwdThenAdjust
// from ldate.c -- advancing a date and then landing it on a business day.
//
// One design change, forced by a normalisation made six modules ago and worth
// stating plainly.
//
// Upstream's `TDateAdjIntvl` carries an `isBusDays` field with three values,
// and `JpmcdsDtFwdAdj` then branches AGAIN on whether the interval's unit
// letter is literally 'D'. So the BUSINESS case means two different things:
//
//   * 'D' -- step this many business days, skipping weekends and holidays;
//   * anything else -- advance by the period, carry end-of-month if the start
//     was a business end of month, then bad-day adjust.
//
// A `DateInterval` cannot express that distinction, because
// `make_date_interval` normalises 'W' to 7 days and 'U' to 28 at
// construction. Under upstream's test a weekly interval is NOT 'D' and takes
// the period path; under a naive port it would look like a day count and take
// the other one.
//
// Rather than smuggle the unit letter back in, the choice becomes explicit:
// `OffsetBasis` has four values where upstream had three, splitting
// BUSINESS into BusinessDays and BusinessPeriod. The caller says which it
// means instead of encoding it in a character the type no longer carries.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>

#include <optional>

namespace cdsmodel {

/// Advance by an interval, then land on a business day. Replaces
/// `JpmcdsDateFwdThenAdjust`.
[[nodiscard]] inline std::optional<Date>
date_fwd_then_adjust(Date date, DateInterval interval, BadDayConv conv, const Calendar &calendar) {
  const std::optional<Date> advanced = advance(date, interval);
  if (!advanced) {
    return std::nullopt;
  }
  return adjust(*advanced, conv, calendar);
}

/// How an offset is to be applied. Upstream's `isBusDays`, with its implicit
/// 'D'-or-not branch promoted into the enumeration.
enum class OffsetBasis {
  /// `JPMCDS_DATE_ADJ_TYPE_CALENDAR`: advance by the calendar period, then
  /// bad-day adjust.
  CalendarPeriod,

  /// `JPMCDS_DATE_ADJ_TYPE_BUSINESS` with a 'D' interval: step this many
  /// business days. The result is a business day already, so no bad-day
  /// adjustment is applied -- and upstream applies none either.
  BusinessDays,

  /// `JPMCDS_DATE_ADJ_TYPE_BUSINESS` with any other interval: advance by the
  /// period, and if the start date was the business end of its month, force
  /// the result to its own month end before adjusting. Upstream's comment
  /// explains the intent: "if the start date is the end of the business
  /// month, then the end date must also be the end of the month".
  BusinessPeriod,

  /// `JPMCDS_DATE_ADJ_TYPE_WEEKDAY`: step this many WEEKDAYS, ignoring
  /// holidays entirely, then bad-day adjust against the real calendar.
  /// Upstream hardcodes the holiday file "NONE" for the stepping phase, which
  /// resolves to no holidays with a Saturday/Sunday weekend.
  Weekdays,
};

/// Upstream's `TDateAdjIntvl`, minus the holiday-file name.
struct AdjustedOffset {
  DateInterval interval;
  OffsetBasis basis = OffsetBasis::CalendarPeriod;

  /// Unused for `BusinessDays`, which cannot land on a bad day.
  BadDayConv bad_day = BadDayConv::None;
};

/// Advance a date by an adjusted offset. Replaces `JpmcdsDtFwdAdj`.
///
/// The library's only caller is `streamcf.c`, for a floating rate's spot
/// offset -- conventionally two business days, so `BusinessDays` with a day
/// interval. The other three paths exist because the struct is supplied from
/// outside and upstream honours all of them.
///
/// Returns nullopt where upstream fails, including its one explicit refusal:
/// a `Weekdays` offset whose interval is not a count of days. Upstream tests
/// that by unit letter; here the interval must have `PeriodUnit::Days`, which
/// is the same condition for a 'D' interval and additionally admits 'W' and
/// 'U' -- see the header note.
[[nodiscard]] inline std::optional<Date> advance_adjusted(Date start, const AdjustedOffset &offset,
                                                          const Calendar &calendar) {
  switch (offset.basis) {
  case OffsetBasis::BusinessDays:
    return add_business_days(start, offset.interval.periods, calendar);

  case OffsetBasis::BusinessPeriod: {
    const std::optional<Date> business_eom = business_end_of_month(start, calendar);
    if (!business_eom) {
      return std::nullopt;
    }
    std::optional<Date> end = advance(start, offset.interval);
    if (!end) {
      return std::nullopt;
    }
    // Order matters: the end-of-month carry happens BEFORE the bad day
    // adjustment, so a month end rolled off a weekend keeps the roll.
    if (start == *business_eom) {
      end = end_of_month(*end);
    }
    return adjust(*end, offset.bad_day, calendar);
  }

  case OffsetBasis::CalendarPeriod:
    return date_fwd_then_adjust(start, offset.interval, offset.bad_day, calendar);

  case OffsetBasis::Weekdays: {
    if (offset.interval.unit != PeriodUnit::Days) {
      return std::nullopt; // upstream: "requires date interval to be in days"
    }
    // Upstream steps against the built-in "NONE" calendar: no holidays, with
    // a Saturday/Sunday weekend. Synthesised here rather than looked up.
    const std::optional<Calendar> weekends_only = Calendar::create({}, Weekend::Standard);
    if (!weekends_only) {
      return std::nullopt;
    }
    const std::optional<Date> stepped =
        add_business_days(start, offset.interval.periods, *weekends_only);
    if (!stepped) {
      return std::nullopt;
    }
    // Then adjusted against the REAL calendar, which may well move it again.
    return adjust(*stepped, offset.bad_day, calendar);
  }
  }
  return std::nullopt;
}

} // namespace cdsmodel
