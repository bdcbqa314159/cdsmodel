#pragma once

/// H7 and H8 -- how a calendar reaches a formula.
///
/// Three ways, and the one upstream had is deliberately not the primary (D5):
///
///     a range of dates   visible in the sheet, travels with the workbook
///     a file on disk     interop; desks share NYC.dat centrally
///     a handle           what a cell actually holds, either way
///
/// The file reading lives HERE rather than in `xl/interface`, even though D4
/// says the library must not do IO. `std::ifstream` is portable, so keeping it
/// in `xl/core` means it is tested on every platform; putting it one layer up
/// would make it Windows-only and unverifiable in CI, which is D8's whole
/// point. The LIBRARY stays IO-free; the add-in's own layers need not.

#include <cdsmodel/expected.hpp>
#include <cdsmodel/xl/handles.hpp>
#include <cdsmodel/xl/value.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cdsmodel::xl {

/// Reads and parses a holiday file.
///
/// Separate from `parse_holiday_file` so that the error a user sees
/// distinguishes "the file is not there" from "the file is malformed" -- two
/// problems with different fixes, which upstream reports as the same FAILURE.
[[nodiscard]] Expected<Calendar> read_calendar_file(std::string_view path);

/// Builds a calendar from a range of dates, as `CDS.Calendar` does.
///
/// `weekend` names which weekdays are always non-business: "STANDARD" for
/// Saturday and Sunday, "NONE" for none, or a comma-separated list of day
/// names. Dates may be in any order in the sheet -- a user selecting a column
/// should not have to sort it, and the file format's increasing-order rule is
/// upstream's constraint on a FILE, not on a calendar.
[[nodiscard]] Expected<Calendar> calendar_from_range(const XlValue &dates, const XlValue &weekend);

/// The dates in a calendar, as a single-column grid, for `CDS.CalendarDates`.
///
/// This is the migration path off files -- load `NYC.dat` once, spill its
/// dates into a sheet, delete the file dependency -- and the only way a loaded
/// calendar becomes inspectable. Upstream's cached handle never was, so a user
/// could not tell which calendar they had actually got.
[[nodiscard]] XlValue calendar_dates_grid(const Calendar &calendar);

/// Reads a range as a flat list of numbers, in row-major order.
///
/// Blank cells are skipped, because a user selecting a whole column picks up
/// the empty rows below their data and refusing that would make every
/// curve-building function unusable the obvious way. `what` names the argument
/// in any error, since these functions take three or four ranges and "not a
/// number" without a field name is a guessing game.
[[nodiscard]] Expected<std::vector<double>> read_numbers(const XlValue &range,
                                                         std::string_view what);

/// The same, as dates. Excel hands dates over as serials.
[[nodiscard]] Expected<std::vector<Date>> read_dates(const XlValue &range, std::string_view what);

/// Reads a range as text, one entry per non-blank cell.
[[nodiscard]] Expected<std::vector<std::string>> read_strings(const XlValue &range,
                                                              std::string_view what);

/// A column of rate points, for `DatesAndRates`: dates in column 1, rates in
/// column 2, one row per point.
[[nodiscard]] XlValue rate_points_grid(std::span<const RatePoint> points);

/// A date as an Excel serial.
[[nodiscard]] double to_excel_serial(Date date);

} // namespace cdsmodel::xl
