#pragma once

// Port of upstream buscache.c -- `JpmcdsHolidayListRead`, and only that: the
// process-wide cache around it is the reason `JpmcdsHolidayLoadFromDisk` is a
// declared non-port, and it stays one.

/// Artifact #4 -- reading a holiday file.
///
/// Replaces `JpmcdsHolidayListRead` (buscache.c), minus the process-wide cache
/// that made `JpmcdsHolidayLoadFromDisk` a declared non-port. The cache was
/// the objection; the reading was not.
///
/// **This takes CONTENTS, not a path (D4).** A path would drag `errno`,
/// encodings and working directories into a numerical library and force every
/// differential test through a temporary file. The caller reads the bytes, so
/// one parser serves a file, a range of cells pasted as text, or anything else
/// that produces characters.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/expected.hpp>

#include <string_view>

namespace cdsmodel {

/// Parses a holiday file.
///
/// The format, from upstream's own comment:
///
///     #                                a comment; blank lines are skipped too
///     19631026                         1963-10-26 is a holiday
///     # SATURDAY_NOT_ALWAYS_HOLIDAY    Saturday becomes a business day
///     # SUNDAY_NOT_ALWAYS_HOLIDAY      Sunday becomes a business day
///     # MONDAY_ALWAYS_HOLIDAY          ... and four more, Tuesday to Friday
///
/// Dates must strictly increase. Weekends start as Saturday and Sunday.
///
/// DELIBERATE DIFFERENCES, both argued in the spec:
///
///   * UPSTREAM-41 -- upstream's directive comparison is case-SENSITIVE,
///     because it builds an upper-cased copy of each comment line and then
///     compares the original. Measured: `# saturday_not_always_holiday`
///     leaves Saturday a holiday, with no diagnostic, and every adjusted date
///     silently wrong. Here a comment that case-insensitively matches a
///     directive but is not exactly it is REFUSED (D1) -- which reproduces
///     nothing silently and accepts nothing upstream rejects.
///
///   * A malformed date line is refused (D2). Upstream's test is
///     `atol(line) > 16010101`, so `abc` becomes a comment and `19960101x`
///     becomes 1996-01-01 -- the UPSTREAM-39 family, where `atol` cannot
///     report failure. A holiday file is small, hand-maintained, and
///     load-bearing for every date in a trade.
[[nodiscard]] Expected<Calendar> parse_holiday_file(std::string_view contents);

} // namespace cdsmodel
