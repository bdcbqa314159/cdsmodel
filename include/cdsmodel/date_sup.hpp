#pragma once

// Port of upstream date_sup.c -- converting intervals to years and
// frequencies, stepping a date by a multiple of an interval, and counting how
// many intervals fit between two dates.
//
// A leaf in the dependency graph with six modules above it, which is why it
// comes first in the remaining work.
//
// UPSTREAM-12 lives here: `JpmcdsDateIntervalToYears` computes the right
// answer for a lunar-month interval and then falls through a missing `break`
// into the error branch, returning FAILURE. It cannot occur in the port,
// because `make_date_interval` normalises 'U' to 28 days at construction and
// no `DateInterval` ever carries the unit letter the bug keys on. Recorded in
// full on `interval_to_years` below.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

namespace cdsmodel {

/// Upstream's JPMCDS_DAYS_PER_YEAR -- a flat 365, used for day-type intervals
/// regardless of leap years. Not a day count convention; a crude scale factor
/// for estimating how many intervals fit in a span.
inline constexpr double kDaysPerYear = 365.0;
inline constexpr double kMonthsPerYear = 12.0;

/// An interval expressed in years. Replaces `JpmcdsDateIntervalToYears`.
///
/// Month-type intervals use 30/360 (so a month is exactly 1/12 of a year) and
/// day-type intervals use ACT/365F. That split is upstream's and it is a scale
/// factor rather than a day count: nothing here looks at a calendar.
///
/// Upstream returns a status because its interval can carry a unit letter it
/// does not handle. A `DateInterval` carries only Days or Months, both of
/// which are handled, so this is total and returns a plain `double`.
///
/// ## UPSTREAM-12 -- a lunar month computes correctly and then fails
///
/// ```c
/// case 'U':
///     *years = (double)interval->prd *
///              (double)JPMCDS_DAYS_PER_LUNAR_MONTH/JPMCDS_DAYS_PER_YEAR;
/// default:
///     JpmcdsErrMsg("%s: unknown interval type %c.\n", routine, ...);
///     return FAILURE;
/// ```
///
/// `case 'U'` has no `break`, so it falls into `default:`. Measured: a 'U'
/// interval of one period writes 0.07671232877 -- exactly 28/365 -- into the
/// output and then returns FAILURE, while a genuinely unrecognised letter
/// leaves the output untouched. The computation ran; only the `break` is
/// missing. Second of its family after UPSTREAM-5.
///
/// It propagates: `JpmcdsDateIntervalToFreq` and `JpmcdsCountDates` both
/// consume this, so neither works for a lunar-month interval.
///
/// Severity is low, and the reachability is the reason -- stated precisely
/// because the first draft of this note overstated it. `JpmcdsMakeDateInterval`
/// normalises 'U' to 28 DAYS, so an interval built the way callers build one
/// arrives at the ACT/365F branch and works. The missing `break` is reachable
/// only through a hand-constructed `TDateInterval`.
///
/// That is not a theoretical concern of mine; upstream contemplates it. The
/// switch in `JpmcdsDtFwdAny` keeps code for 'A', 'S' and 'Q' with the comment
/// "left here for people who have set up their own TDateIntervals without
/// calling JpmcdsMakeDateInterval". It extends that courtesy to 'W' and not to
/// 'U', which is why a hand-built lunar interval is refused there and
/// mishandled here.
///
/// The port is immune by construction rather than by correction:
/// `make_date_interval('U')` yields `{28 * periods, Days}`, so the value
/// reaches the ACT/365F branch and comes out as `(28 * periods) / 365`. That
/// is the same quotient upstream computes before it fails, and the same for
/// every other letter: 'A' and 'Y' become 12 months (12/12), 'S' six (6/12),
/// 'Q' three (3/12), 'W' seven days (7/365). Normalisation divides numerator
/// and denominator by the same factor, so the correctly-rounded quotient is
/// identical -- which the differential suite checks rather than assumes.
[[nodiscard]] constexpr double interval_to_years(DateInterval interval) noexcept {
  return interval.unit == PeriodUnit::Months
             ? static_cast<double>(interval.periods) / kMonthsPerYear
             : static_cast<double>(interval.periods) / kDaysPerYear;
}

/// Payments per year. Replaces `JpmcdsDateIntervalToFreq`.
///
/// Returns nullopt for a non-positive interval, which upstream reports as
/// "interval is zero" -- its test is `years > 0.`, so a negative interval is
/// rejected by the same branch despite the message.
[[nodiscard]] constexpr std::optional<double>
interval_to_frequency(DateInterval interval) noexcept {
  const double years = interval_to_years(interval);
  if (!(years > 0.0)) {
    return std::nullopt;
  }
  return 1.0 / years;
}

/// An interval from a payment frequency. Replaces `JpmcdsFreq2TDateInterval`.
///
/// Upstream accepts 1 through 12 and divides 12 by the frequency in INTEGER
/// arithmetic, so a frequency that does not divide 12 silently truncates:
/// 5 gives a 2-month interval (2.4 months intended), 7 through 11 all give 1.
/// That truncation is preserved -- it is observable, and a caller passing 5
/// has asked for something the type cannot express.
[[nodiscard]] constexpr std::optional<DateInterval> frequency_to_interval(long freq) noexcept {
  if (freq <= 0 || freq > 12) {
    return std::nullopt;
  }
  return DateInterval{12 / static_cast<int>(freq), PeriodUnit::Months};
}

/// `start` advanced by `index` whole intervals. Replaces
/// `JpmcdsDateFromDateAndOffset`.
///
/// Always computed from `start` with a multiplied interval, never by stepping
/// -- upstream's own comment on `JpmcdsCountDates` explains why:
/// "date + (n*Interval) = ((date + Interval)+Interval) ... )+Interval is not
/// always true". Month arithmetic clamps, so stepping drifts.
///
/// Upstream multiplies `prd` by `index` in `int` with no check, so a large
/// index wraps. The product is formed wide here and the call is rejected
/// rather than wrapped.
[[nodiscard]] constexpr std::optional<Date>
date_from_date_and_offset(Date start, DateInterval interval, int index) noexcept {
  return detail::advance_multiple(start, interval, index);
}

/// `count` dates starting `start_index` intervals from `base`. Replaces
/// `JpmcdsMakeTDateArray`.
///
/// Upstream writes into a caller-supplied array with an `arrayIncrement`
/// parameter, "usually +1 or -1", so a negative increment fills the array
/// backwards from the pointer it was handed. That is not reproduced: a vector
/// is returned in forward order and a caller wanting the other order reverses
/// it. Writing backwards through a raw pointer is the shape of a buffer
/// overrun, and the only thing lost is an idiom.
[[nodiscard]] inline std::optional<std::vector<Date>>
make_date_array(Date base, DateInterval interval, int start_index, int count) {
  if (count < 0) {
    return std::nullopt;
  }
  std::vector<Date> dates;
  dates.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    // Upstream computes `interval->prd * (startIdx + idx)` in int. Widened.
    const long long index = static_cast<long long>(start_index) + i;
    if (index < std::numeric_limits<int>::min() || index > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    const std::optional<Date> date =
        date_from_date_and_offset(base, interval, static_cast<int>(index));
    if (!date) {
      return std::nullopt;
    }
    dates.push_back(*date);
  }
  return dates;
}

/// How many whole intervals fit between two dates, and the days left over.
struct DateCount {
  int intervals = 0;
  long extra_days = 0;
};

/// Replaces `JpmcdsCountDates`. `from` may be later or earlier than `to`.
///
/// The algorithm is upstream's and the structure is load-bearing. It starts
/// from a deliberately LOW estimate -- the quotient of the two spans in years,
/// floored, minus two -- and then walks forward one interval at a time until
/// the running date leaves the span. The two-interval margin is what makes the
/// estimate safe despite `interval_to_years` being a crude 30/360 or ACT/365F
/// scale factor rather than a calendar computation.
///
/// Every candidate date is computed from `from` with a multiplied interval
/// rather than by stepping, for the reason given on
/// `date_from_date_and_offset`.
///
/// Returns nullopt where upstream fails: a zero interval, an interval whose
/// sign opposes `to - from`, a date that cannot be formed, or the low estimate
/// landing outside the span -- which upstream detects after the fact by
/// checking that the answer did not come out below the estimate it started
/// from.
[[nodiscard]] inline std::optional<DateCount> count_dates(Date from, Date to,
                                                          DateInterval interval) {
  // Upstream's checkDateInterval. Note it is declared TBoolean and returns
  // FAILURE, which only works because TBoolean is a plain int.
  if (interval.periods == 0) {
    return std::nullopt;
  }
  const long long span = static_cast<long long>(to_tdate(to)) - to_tdate(from);
  if (span * interval.periods < 0) {
    return std::nullopt; // interval points the wrong way
  }

  const double interval_years = interval_to_years(interval);
  const double span_years = static_cast<double>(span) / kDaysPerYear;
  // The quotient is bounded by the representable date range divided by the
  // shortest interval, which cannot reach INT_MAX -- but upstream casts a
  // double to int here with no guard, so the bound is stated rather than
  // assumed.
  const double estimate = std::floor(std::fabs(span_years / interval_years)) - 2.0;
  const int low = estimate > 0.0 ? static_cast<int>(estimate) : 0;

  int index = low;
  std::optional<Date> current = date_from_date_and_offset(from, interval, index);
  if (!current) {
    return std::nullopt;
  }
  Date last = *current;

  const auto between = [](Date x, Date a, Date b) {
    return a < b ? (x >= a && x <= b) : (x >= b && x <= a);
  };
  while (between(*current, from, to)) {
    ++index;
    last = *current;
    current = date_from_date_and_offset(from, interval, index);
    if (!current) {
      return std::nullopt;
    }
  }

  const int intervals = index - 1;
  if (intervals < low) {
    return std::nullopt; // the low estimate was not inside the span
  }
  return DateCount{intervals, std::labs(to_tdate(to) - to_tdate(last))};
}

} // namespace cdsmodel
