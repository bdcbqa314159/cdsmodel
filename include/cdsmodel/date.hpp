#pragma once

// Port of upstream dateconv.c + ldate.c + convert.c -- the calendar layer
// everything else sits on. dateconv.c supplies the representation; ldate.c
// the four year helpers at the bottom of this header; convert.c the string
// parser below them.
//
// Upstream represents a date as `typedef long int TDate`: days since
// 1601-01-01, proleptic Gregorian. That typedef is the single most
// consequential piece of primitive obsession in the codebase -- it appears in
// most of the 126 public entry points, and because it is an arithmetic type
// it lets you add a date to a date and get a plausible-looking number.
//
// Here a date is std::chrono::sys_days. TDate survives only as an interop
// type at the boundary (to_tdate / from_tdate), not as the working currency.
//
// What this replaces, measured:
//   - gDateCacheArray, a hardcoded table of the first day of every month from
//     1995 to 2119: 1,500 entries over 377 lines, plus the lookup/validate
//     path that used it and the 400/100/4/1-year loop fallback for dates
//     outside it. std::chrono does all of it.
//   - days[] / leapDays[] / cumDays[] / leapCumDays[] month tables.
//   - the JPMCDS_IS_LEAP macro.

#include <cdsmodel/expected.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string_view>

namespace cdsmodel {

/// A calendar day.
using Date = std::chrono::sys_days;

/// Upstream's representation: days since 1601-01-01. Interop only.
using TDate = long;

/// Upstream's epoch. TDate 0 is 1601-01-01, which was a Monday -- which is
/// why upstream's `(date + 1) % 7` day-of-week encoding comes out
/// Sunday-based, matching std::chrono::weekday::c_encoding() exactly.
inline constexpr Date kDateEpoch{std::chrono::year{1601} / std::chrono::January / 1};

/// Upstream's JpmcdsMDYToDate rejects any year below its epoch year.
/// (mdydate.h documents the range as [1600-]; the code enforces >= 1601.
/// The code wins.)
inline constexpr std::chrono::year kMinYear{1601};

/// Value upstream's JpmcdsDate returns for an invalid date: FAILURE, i.e. -1.
/// Safe as a sentinel only because valid TDates are never negative, which is
/// exactly the kind of coupling std::optional removes.
inline constexpr TDate kInvalidTDate = -1;

namespace detail {

/// `std::chrono::days` is only required to hold a signed integer of at least
/// 25 bits, and is `int` in practice -- not 64 bits, which is the surprise.
using DayRep = std::chrono::days::rep;

/// The 1601 epoch as a day number relative to `sys_days`'s own 1970 epoch:
/// -134774. Every conversion carries this offset, which is why the
/// representable TDate range is NOT symmetric.
inline constexpr long long kEpochDayNumber = kDateEpoch.time_since_epoch().count();

} // namespace detail

/// Smallest and largest TDate that convert without narrowing.
///
/// Two constraints, and they bite at opposite ends:
///
///   * `from_tdate(t)` forms `kEpochDayNumber + t` in `DayRep`, so `t` cannot
///     be smaller than `DayRep`'s minimum plus the offset.
///   * `to_tdate` must be able to return `t`, so `t` must also fit `DayRep`.
///
/// Together: `[DayRep::min() + 134774, DayRep::max()]`. The bounds are
/// derived from the epoch rather than written down, so they stay correct on an
/// implementation where `DayRep` is wider.
///
/// The actual numbers are IMPLEMENTATION-DEPENDENT in two independent ways,
/// so never write them down:
///
///   * `std::chrono::days::rep` is 32-bit on libc++ and 64-bit on libstdc++.
///   * `long`, and therefore `TDate`, is 32-bit on Windows and 64-bit
///     elsewhere -- upstream's `typedef long int TDate` inherits that, so the
///     date type is not a stable width across platforms. Harmless for day
///     numbers around 150,000, but it matters at any ABI boundary.
///
/// Either way the bounds sit far outside any real date -- on libc++, TDate
/// 2^31 is the year 5,881,581 -- but an undocumented domain with silently
/// wrong behaviour outside it is the same shape of defect this port exists to
/// remove. Before it was named, `from_tdate(5'000'000'000)` round-tripped to
/// 705032704 on macOS.
inline constexpr TDate kMinTDate = static_cast<TDate>(std::numeric_limits<detail::DayRep>::min()) -
                                   static_cast<TDate>(detail::kEpochDayNumber);
inline constexpr TDate kMaxTDate = static_cast<TDate>(std::numeric_limits<detail::DayRep>::max());

/// Can this TDate be converted without narrowing?
///
/// Check this at any boundary that accepts a TDate from outside the library:
/// a C API shim, a language binding, a file reader. Inside the library, dates
/// come from `make_date` or `to_tdate` and are representable by construction.
[[nodiscard]] constexpr bool is_representable_tdate(TDate t) noexcept {
  return t >= kMinTDate && t <= kMaxTDate;
}

[[nodiscard]] constexpr TDate to_tdate(Date d) noexcept {
  return static_cast<TDate>((d - kDateEpoch).count());
}

/// Convert an upstream TDate to a date.
///
/// Precondition: `is_representable_tdate(t)`. Outside that the arithmetic
/// overflows `std::chrono::days`'s representation.
[[nodiscard]] constexpr Date from_tdate(TDate t) noexcept {
  return kDateEpoch + std::chrono::days{static_cast<detail::DayRep>(t)};
}

/// Number of days in a given month. Replaces JpmcdsDaysInMonth, which indexed
/// a 13-element table with an unvalidated month and read out of bounds for
/// month > 12. std::chrono::month makes that unrepresentable.
[[nodiscard]] constexpr unsigned days_in_month(std::chrono::year_month ym) noexcept {
  return static_cast<unsigned>(
      std::chrono::year_month_day_last{ym.year(), std::chrono::month_day_last{ym.month()}}.day());
}

/// Replaces the JPMCDS_IS_LEAP macro.
[[nodiscard]] constexpr bool is_leap(std::chrono::year y) noexcept { return y.is_leap(); }

/// Construct a date from year/month/day, rejecting invalid input.
///
/// Replaces JpmcdsDate (which returned -1 on failure) and JpmcdsMDYToDate
/// (status code plus out-param). Validation matches upstream: year at or
/// after the epoch year, month in 1-12, day in 1-31 and no later than the
/// month's last day.
[[nodiscard]] constexpr Expected<Date> make_date(int year, int month, int day) noexcept {
  if (year < static_cast<int>(kMinYear)) {
    return fail(ErrorKind::bad_argument, "year " + detail::num(year) +
                                             " is before the epoch year " +
                                             detail::num(static_cast<int>(kMinYear)));
  }
  if (month < 1 || month > 12 || day < 1 || day > 31) {
    return fail(ErrorKind::bad_argument, "month " + detail::num(month) + " and day " +
                                             detail::num(day) + " are not a possible date");
  }
  const std::chrono::year_month_day ymd{std::chrono::year{year},
                                        std::chrono::month{static_cast<unsigned>(month)},
                                        std::chrono::day{static_cast<unsigned>(day)}};
  if (!ymd.ok()) { // e.g. 31 April, or 29 February in a common year
    return fail(ErrorKind::bad_argument, "day " + detail::num(day) + " does not exist in month " +
                                             detail::num(month) + " of year " + detail::num(year));
  }
  return Date{ymd};
}

/// Split a date into year/month/day. Replaces JpmcdsDateToMDY.
[[nodiscard]] constexpr std::chrono::year_month_day to_ymd(Date d) noexcept {
  return std::chrono::year_month_day{d};
}

/// Day of week. Replaces JpmcdsDayOfWeek; `.c_encoding()` reproduces
/// upstream's 0-6 Sunday-based numbering.
[[nodiscard]] constexpr std::chrono::weekday day_of_week(Date d) noexcept {
  return std::chrono::weekday{d};
}

/// Parse a date written as text. Replaces `JpmcdsStringToDate`.
///
/// Four formats are documented: `YYYYMMDD`, `MM/DD/YYYY`, `Mth-DD-YYYY` and
/// `DD-Mth-YYYY`. All four are accepted, and so are several things the
/// documentation does not mention -- measured rather than assumed:
///
///   * the delimiter set is `-/. ` and not just `-` and `/`, so
///     `03.15.2024` and `03 15 2024` parse;
///   * TRAILING TOKENS ARE SILENTLY IGNORED. Upstream calls `strtok` exactly
///     three times and never asks whether anything is left, so
///     `1-1-2024-GARBAGE` parses as 1 January 2024. Reproduced: a stricter
///     parser would reject input the library accepts, and the output for the
///     input it does accept is not in doubt;
///   * two-digit years pivot at `CROSSOVER_YEAR 60` -- `59` is 2059 and `60`
///     is 1960;
///   * years from 100 to 1600 are rejected outright, so the +1900 branch only
///     ever sees 60 to 99 and there is no three-digit-year surprise.
///
/// ## The contrast worth noticing
///
/// This function copies its input with
///
///     (void)strncpy(dateCopy, mmddyyyy, sizeof(dateCopy)-1);
///     dateCopy[sizeof(dateCopy)-1] = '\0';
///
/// into a 32-byte stack buffer, with a comment explaining why ("use strncpy to
/// prevent accidental use of long strings"). Measured: a 48-character input
/// truncates and still parses.
///
/// `JpmcdsStringToDateInterval`, in the SAME FILE with the SAME buffer size,
/// copies digits with `while (isdigit(*inp)) *nump++ = *inp++;` and overflows
/// -- that is UPSTREAM-4, the stack write driven by spreadsheet input. One
/// author bounded the copy and wrote down why; the other did not. The two
/// functions are 225 lines apart.
///
/// Not reproduced: `strtok`, which keeps static state and makes upstream's
/// version non-reentrant. Nothing about parsing a date needs that.
/// DELIBERATE DIFFERENCE, UPSTREAM-38. The month-name helper lowercases three
/// bytes unconditionally -- `cp[0] = tolower(cp[0]); cp[1] = ...; cp[2] = ...`
/// -- however short the token is, and those are read-modify-WRITES. With a
/// short non-numeric token at the end of the 32-byte buffer the third one
/// lands past it. Confirmed: `stack-buffer-overflow` at convert.c:395. See the
/// register. Here the token is a `string_view` and its length is checked
/// before any character is touched.
[[nodiscard]] inline Expected<Date> date_from_string(std::string_view text) {
  // Upstream copies into a 32-byte buffer and truncates, so a longer input is
  // parsed from its first 31 characters. Reproduced, because truncation is
  // observable: "03/15/2024" followed by padding still parses.
  text = text.substr(0, 31);

  constexpr std::string_view kDelimiters = "-/. ";

  // D7: `optional`, not `Expected`. A token that is not a month name is a
  // parse ALTERNATIVE -- the caller then reads it as a day -- not a failure to
  // report. Inventing an error message here would describe a non-event.
  const auto to_month_name = [](std::string_view token) -> std::optional<int> {
    // Upstream compares the lowercased first three characters against three-
    // letter names, so a token of any other length cannot match -- "January"
    // fails. Checking the length first is what makes this safe.
    if (token.size() != 3) {
      return std::nullopt;
    }
    constexpr std::string_view kNames[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                           "jul", "aug", "sep", "oct", "nov", "dec"};
    char lowered[3] = {};
    for (std::size_t i = 0; i < 3; ++i) {
      const char c = token[i];
      lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    const std::string_view key{lowered, 3};
    for (std::size_t i = 0; i < 12; ++i) {
      if (key == kNames[i]) {
        return static_cast<int>(i) + 1;
      }
    }
    return std::nullopt;
  };

  // A non-negative integer, or nullopt. Upstream reaches this through
  // JpmcdsSscanf, which overflows a signed long on a long digit run -- see
  // UPSTREAM-38's second observation. Bounded here.
  // D7: same -- "not a number" selects the month-name branch.
  const auto to_number = [](std::string_view token) -> std::optional<long> {
    if (token.empty()) {
      return std::nullopt;
    }
    long value = 0;
    for (const char c : token) {
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      if (value > (100000000L - (c - '0')) / 10) {
        return std::nullopt; // far beyond any year; upstream wraps
      }
      value = value * 10 + (c - '0');
    }
    return value;
  };

  if (text.find_first_of(kDelimiters) == std::string_view::npos) {
    // The undelimited YYYYMMDD form, which upstream requires to be exactly
    // eight characters.
    if (text.size() != 8) {
      return fail(ErrorKind::bad_argument,
                  "an undelimited date must be exactly 8 characters (YYYYMMDD), got " +
                      detail::num(text.size()));
    }
    const std::optional<long> year = to_number(text.substr(0, 4));
    const std::optional<long> month = to_number(text.substr(4, 2));
    const std::optional<long> day = to_number(text.substr(6, 2));
    // Upstream rejects a zero component because `atol` returns 0 both for
    // "00" and for unparseable text, so it cannot tell them apart. The port
    // can, but reproduces the refusal: a zero month or day is invalid anyway,
    // and a zero YEAR is below the epoch and would be refused next.
    if (!year || !month || !day || *year == 0 || *month == 0 || *day == 0) {
      return fail(ErrorKind::bad_argument,
                  "undelimited date \"" + std::string(text) + "\" has a non-numeric or zero field");
    }
    return make_date(static_cast<int>(*year), static_cast<int>(*month), static_cast<int>(*day));
  }

  // The delimited forms. Upstream takes exactly three tokens and never asks
  // whether more remain, so trailing text is ignored -- reproduced.
  std::array<std::string_view, 3> tokens{};
  std::size_t found = 0;
  std::size_t pos = 0;
  while (found < 3 && pos < text.size()) {
    const std::size_t start = text.find_first_not_of(kDelimiters, pos);
    if (start == std::string_view::npos) {
      break;
    }
    std::size_t end = text.find_first_of(kDelimiters, start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    tokens[found++] = text.substr(start, end - start);
    pos = end;
  }
  if (found < 3) {
    return fail(ErrorKind::bad_argument, "a delimited date needs 3 fields, found " +
                                             detail::num(found) + " in \"" + std::string(text) +
                                             "\"");
  }

  long month = 0;
  long day = 0;
  bool first_was_a_name = false;
  if (const std::optional<long> n = to_number(tokens[0])) {
    month = *n;
  } else if (const std::optional<int> name = to_month_name(tokens[0])) {
    month = *name;
    first_was_a_name = true;
  } else {
    return fail(ErrorKind::bad_argument,
                "field 1 of \"" + std::string(text) + "\" is neither a number nor a month name");
  }

  if (const std::optional<long> n = to_number(tokens[1])) {
    day = *n;
  } else {
    // The DD-Mth-YYYY form: what was read as the month was really the day.
    // Upstream's `isMonth` guard refuses two names.
    day = month;
    const std::optional<int> name = to_month_name(tokens[1]);
    if (first_was_a_name || !name) {
      return fail(ErrorKind::bad_argument,
                  first_was_a_name
                      ? std::string("a date cannot have two month names")
                      : "field 2 of \"" + std::string(text) + "\" is neither a day nor a month");
    }
    month = *name;
  }

  // DELIBERATE DIFFERENCE, UPSTREAM-39. The year is read with a bare `atol`,
  // whose "0" return is indistinguishable from a parsed zero, and which stops
  // at the first non-digit instead of reporting one. Upstream therefore
  // ACCEPTS "03/15/aaaa" as 2000-03-15 (0 is below CROSSOVER_YEAR, so it is
  // promoted) and "03/15/2024XX" as 2024-03-15. The month and day fields
  // three lines above it do detect an unparseable token -- that is what makes
  // this an internal inconsistency rather than a convention, and what lets
  // the port refuse instead of reproduce. See the register.
  const std::optional<long> parsed_year = to_number(tokens[2]);
  if (!parsed_year) {
    return fail(ErrorKind::bad_argument, "field 3 of \"" + std::string(text) + "\" is not a year");
  }
  long year = *parsed_year;
  // Years from 100 to 1600 are refused, which is what keeps the +1900 branch
  // below from ever seeing a three-digit value.
  if (year > 99 && year < static_cast<long>(static_cast<int>(kMinYear))) {
    return fail(ErrorKind::bad_argument, "year " + detail::num(year) + " is out of range: 100 to " +
                                             detail::num(static_cast<int>(kMinYear) - 1) +
                                             " cannot be read as a 2- or 4-digit year");
  }
  if (year < 60) {
    year += 2000; // CROSSOVER_YEAR
  } else if (year < static_cast<long>(static_cast<int>(kMinYear))) {
    year += 1900;
  }
  return make_date(static_cast<int>(year), static_cast<int>(month), static_cast<int>(day));
}

// ---------------------------------------------------------------------------
// ldate.c's four year helpers.
//
// These exist in upstream to serve `JpmcdsDayCountFraction`'s ACT/ACT branch,
// which walks whole years adding 365 or 366 and re-deriving the year from a
// running date. The port's `year_fraction` does not walk years -- it
// intersects the period with each calendar year directly, which is why these
// four were still unclaimed long after day_count.hpp was finished.
//
// They are ported anyway because all four are declared in `ldate.h` and are
// therefore public API: an external caller using the model's date layer can
// reach them even though nothing inside the library now needs them.

/// The date's year as a four-digit number. Replaces `JpmcdsYear`.
///
/// Upstream's version is the only function in ldate.c that ends `return 0;`
/// rather than `return SUCCESS;`. The two are the same -- `SUCCESS` is
/// `#define`d to 0 -- so this is a style inconsistency and not a defect, but
/// it is the sort of thing that makes a reader stop and check.
[[nodiscard]] constexpr int year_of(Date d) noexcept { return static_cast<int>(to_ymd(d).year()); }

/// Whether the date falls in a leap year. Replaces `JpmcdsIsLeap`.
///
/// Distinct from the `is_leap(std::chrono::year)` above, which replaces the
/// `JPMCDS_IS_LEAP` macro: upstream has both a macro taking a year and a
/// function taking a date, and the function routes through `JpmcdsDateToMDY`
/// to get the year the macro wants. Expressed here as the composition it is.
///
/// Upstream implements the Gregorian rule with three nested `if`s whose "no"
/// branches are empty statements (`; /* not divisible by 4 */`), which is
/// correct and reads like a trap. `std::chrono::year::is_leap()` is the same
/// rule.
[[nodiscard]] constexpr bool is_leap_year(Date d) noexcept { return is_leap(to_ymd(d).year()); }

/// 1 January of the date's own year. Replaces `JpmcdsYearStart`.
[[nodiscard]] constexpr Date year_start(Date d) noexcept {
  return Date{to_ymd(d).year() / std::chrono::January / 1};
}

/// 1 January of the year AFTER the date's. Not an upstream function, but the
/// value `JpmcdsDaysLeftThisYear` computes on its way and the only part of it
/// that is about dates rather than about day counts.
[[nodiscard]] constexpr Date next_year_start(Date d) noexcept {
  return Date{(to_ymd(d).year() + std::chrono::years{1}) / std::chrono::January / 1};
}

/// Last day of the date's own month. Replaces JpmcdsDateToEOM.
[[nodiscard]] constexpr Date end_of_month(Date d) noexcept {
  const std::chrono::year_month_day ymd{d};
  return Date{
      std::chrono::year_month_day_last{ymd.year(), std::chrono::month_day_last{ymd.month()}}};
}

/// Bring an out-of-range month into range by carrying into the year, then
/// clamp the day to the resulting month's length.
///
/// Replaces JpmcdsNormalizeMDY. Two upstream behaviours preserved exactly,
/// and they are not symmetric:
///   - a day beyond the month's length is CLAMPED (31 April -> 30 April)
///   - a day outside 1-31 entirely is REJECTED
/// Note upstream does not range-check the year here, so neither does this;
/// the epoch floor is enforced by make_date instead.
[[nodiscard]] constexpr Expected<std::chrono::year_month_day> normalize_ymd(int year, int month,
                                                                            int day) noexcept {
  if (day < 1 || day > 31) {
    return fail(ErrorKind::bad_argument, "day " + detail::num(day) + " is not in 1..31");
  }
  // Done in a wider type on purpose. `month` is any int, so `month - 1`
  // overflows at INT_MIN and `year + carry` overflows at either extreme.
  // Guarding each operation separately is easy to get subtly wrong; widening
  // once and range-checking at the end is not.
  const long long month_index = static_cast<long long>(month) - 1;
  long long year_carry = month_index / 12;
  long long month_rem = month_index % 12;
  if (month_rem < 0) { // floor division, matching upstream's two while-loops
    month_rem += 12;
    --year_carry;
  }
  const long long normalised_year = static_cast<long long>(year) + year_carry;

  // std::chrono::year represents [-32767, 32767]; anything outside is not a
  // date. Upstream has no equivalent check -- its month-carry loops would
  // simply run on -- but it cannot produce such a year from a valid TDate.
  if (normalised_year < -32767 || normalised_year > 32767) {
    return fail(ErrorKind::bad_argument,
                "year " + detail::num(normalised_year) + " is outside the representable range");
  }

  const std::chrono::year y{static_cast<int>(normalised_year)};
  const std::chrono::month m{static_cast<unsigned>(month_rem + 1)};
  const unsigned last = days_in_month(y / m);
  const unsigned clamped = static_cast<unsigned>(day) < last ? static_cast<unsigned>(day) : last;
  return std::chrono::year_month_day{y, m, std::chrono::day{clamped}};
}

} // namespace cdsmodel
