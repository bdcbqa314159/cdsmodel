#include <cdsmodel/xl/calendars.hpp>

#include <cdsmodel/holiday_file.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace cdsmodel::xl {
namespace {

/// Excel serial 1 is 1900-01-01; the Unix epoch is serial 25569. Pinned by a
/// non-circular test against serials read off Excel itself.
constexpr long kExcelSerialToUnixDays = -25569;

[[nodiscard]] Expected<Date> date_from_serial(double serial, std::size_t row) {
  if (serial != static_cast<double>(static_cast<long>(serial))) {
    return fail(ErrorKind::bad_argument, "row " + detail::num(row) +
                                             ": a date must be a whole number of days, got " +
                                             detail::num(serial));
  }
  return Date{} + std::chrono::days{static_cast<long>(serial) + kExcelSerialToUnixDays};
}

[[nodiscard]] std::string upper(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

[[nodiscard]] Expected<Weekend> parse_weekend(std::string_view text) {
  struct Named {
    std::string_view name;
    Weekend bit;
  };
  constexpr Named kDays[]{{"MONDAY", Weekend::Monday},       {"TUESDAY", Weekend::Tuesday},
                          {"WEDNESDAY", Weekend::Wednesday}, {"THURSDAY", Weekend::Thursday},
                          {"FRIDAY", Weekend::Friday},       {"SATURDAY", Weekend::Saturday},
                          {"SUNDAY", Weekend::Sunday}};

  const std::string normalised = upper(text);
  // `text` is kept alongside for messages: quoting the upper-cased form back
  // at a user who typed "Someday" and seeing "SOMEDAY" is a small lie about
  // what they wrote.
  if (normalised.empty() || normalised == "STANDARD") {
    return Weekend::Standard;
  }
  if (normalised == "NONE") {
    return Weekend::None;
  }

  auto bits = static_cast<unsigned>(Weekend::None);
  std::size_t position = 0;
  while (position < normalised.size()) {
    const std::size_t comma = normalised.find(',', position);
    std::string_view piece =
        std::string_view(normalised)
            .substr(position, comma == std::string::npos ? std::string::npos : comma - position);
    // trim
    while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.front()))) {
      piece.remove_prefix(1);
    }
    while (!piece.empty() && std::isspace(static_cast<unsigned char>(piece.back()))) {
      piece.remove_suffix(1);
    }
    position = comma == std::string::npos ? normalised.size() : comma + 1;
    if (piece.empty()) {
      continue;
    }
    const auto found = std::find_if(std::begin(kDays), std::end(kDays),
                                    [&](const Named &d) { return d.name == piece; });
    if (found == std::end(kDays)) {
      return fail(ErrorKind::bad_argument, "\"" + std::string(text) +
                                               "\" is not a weekend rule. Use STANDARD, NONE, or a "
                                               "comma-separated list of day names.");
    }
    bits |= static_cast<unsigned>(found->bit);
  }
  return static_cast<Weekend>(bits);
}

} // namespace

Expected<Calendar> read_calendar_file(std::string_view path) {
  const std::filesystem::path file{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) {
    // Distinguished from a malformed file on purpose: a missing file is a
    // path problem and a malformed one is a content problem, and upstream
    // reports both as the same FAILURE.
    return fail(ErrorKind::bad_argument, "no holiday file at \"" + std::string(path) + "\"");
  }
  // A directory opens successfully with `ifstream` on some platforms and
  // reads as zero bytes -- and an empty holiday file is legitimate (upstream
  // accepts one, producing weekends only). So a user who points at the wrong
  // path would silently get a working calendar with no holidays in it, which
  // is the same silent-wrong-calendar failure UPSTREAM-41 produces. Checking
  // for a regular file is what separates "empty on purpose" from "not a file".
  if (!std::filesystem::is_regular_file(file, ec)) {
    return fail(ErrorKind::bad_argument, "\"" + std::string(path) + "\" is not a file");
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return fail(ErrorKind::bad_argument,
                "\"" + std::string(path) + "\" exists but could not be opened");
  }
  std::ostringstream contents;
  contents << in.rdbuf();

  Expected<Calendar> parsed = parse_holiday_file(contents.str());
  if (!parsed) {
    // Prefixed with the path, because a sheet may load several files and the
    // message otherwise says which LINE but not which file.
    return fail(parsed.error().kind, "\"" + std::string(path) + "\" " + parsed.error().message);
  }
  return parsed;
}

Expected<Calendar> calendar_from_range(const XlValue &dates, const XlValue &weekend) {
  const Expected<XlGrid> grid = dates.as_grid();
  if (!grid) {
    return fail(ErrorKind::bad_argument,
                "the holiday dates must be a range, got " + dates.describe());
  }

  Expected<Weekend> rule = Weekend::Standard;
  if (!weekend.is_missing()) {
    const Expected<std::string> text = weekend.as_string();
    if (!text) {
      return fail(ErrorKind::bad_argument,
                  "the weekend rule must be text, got " + weekend.describe());
    }
    rule = parse_weekend(*text);
    if (!rule) {
      return unexpected(rule.error());
    }
  }

  std::vector<Date> holidays;
  holidays.reserve(grid->cells.size());
  for (std::size_t i = 0; i < grid->cells.size(); ++i) {
    const XlValue &cell = grid->cells[i];
    // A blank cell is skipped, not refused: a user selecting a whole column
    // picks up the empty rows below their dates, and refusing that would make
    // the feature unusable for the obvious way to call it.
    if (cell.is_missing()) {
      continue;
    }
    const Expected<double> serial = cell.as_double();
    if (!serial) {
      return fail(ErrorKind::bad_argument,
                  "row " + detail::num(i + 1) +
                      " of the holiday range is not a date: " + cell.describe());
    }
    const Expected<Date> date = date_from_serial(*serial, i + 1);
    if (!date) {
      return unexpected(date.error());
    }
    holidays.push_back(*date);
  }

  if (holidays.empty()) {
    return fail(ErrorKind::bad_argument, "the holiday range contains no dates");
  }

  // Sorted here, because a sheet is not a file. Upstream's increasing-order
  // rule is a constraint on the FILE FORMAT, and a user selecting a column
  // should not have to sort it first. A duplicate is still an error -- two
  // identical holidays mean the sheet disagrees with itself.
  std::sort(holidays.begin(), holidays.end());
  const auto duplicate = std::adjacent_find(holidays.begin(), holidays.end());
  if (duplicate != holidays.end()) {
    return fail(ErrorKind::bad_argument, "the holiday range lists " +
                                             std::to_string(to_tdate(*duplicate)) +
                                             " more than once");
  }

  return Calendar::create(holidays, *rule);
}

double to_excel_serial(Date date) {
  return static_cast<double>(date.time_since_epoch().count() - kExcelSerialToUnixDays);
}

namespace {

/// One pass over a range, handing each non-blank cell to `take`.
///
/// Row-major, which is how Excel lays out `xltypeMulti`, so a 2-column range
/// interleaves. Every caller here passes a single column; a caller that needs
/// a grid should use `as_grid` and index it.
template <typename F>
[[nodiscard]] Expected<void> for_each_filled(const XlValue &range, std::string_view what, F take) {
  const Expected<XlGrid> grid = range.as_grid();
  if (!grid) {
    return fail(ErrorKind::bad_argument,
                std::string(what) + ": expected a range, got " + range.describe());
  }
  for (std::size_t i = 0; i < grid->cells.size(); ++i) {
    const XlValue &cell = grid->cells[i];
    if (cell.is_missing()) {
      continue; // a whole-column selection picks up the blanks below
    }
    const Expected<void> taken = take(cell, i + 1);
    if (!taken) {
      return taken;
    }
  }
  return Expected<void>{};
}

} // namespace

Expected<std::vector<double>> read_numbers(const XlValue &range, std::string_view what) {
  std::vector<double> out;
  const Expected<void> walked =
      for_each_filled(range, what, [&](const XlValue &cell, std::size_t row) -> Expected<void> {
        const Expected<double> value = cell.as_double();
        if (!value) {
          return fail(ErrorKind::bad_argument, std::string(what) + " row " + detail::num(row) +
                                                   ": expected a number, got " + cell.describe());
        }
        out.push_back(*value);
        return Expected<void>{};
      });
  if (!walked) {
    return unexpected(walked.error());
  }
  if (out.empty()) {
    return fail(ErrorKind::bad_argument, std::string(what) + " is empty");
  }
  return out;
}

Expected<std::vector<Date>> read_dates(const XlValue &range, std::string_view what) {
  std::vector<Date> out;
  const Expected<void> walked =
      for_each_filled(range, what, [&](const XlValue &cell, std::size_t row) -> Expected<void> {
        const Expected<double> serial = cell.as_double();
        if (!serial) {
          return fail(ErrorKind::bad_argument, std::string(what) + " row " + detail::num(row) +
                                                   ": expected a date, got " + cell.describe());
        }
        const Expected<Date> date = date_from_serial(*serial, row);
        if (!date) {
          return fail(date.error().kind, std::string(what) + " " + date.error().message);
        }
        out.push_back(*date);
        return Expected<void>{};
      });
  if (!walked) {
    return unexpected(walked.error());
  }
  if (out.empty()) {
    return fail(ErrorKind::bad_argument, std::string(what) + " is empty");
  }
  return out;
}

Expected<std::vector<std::string>> read_strings(const XlValue &range, std::string_view what) {
  std::vector<std::string> out;
  const Expected<void> walked =
      for_each_filled(range, what, [&](const XlValue &cell, std::size_t row) -> Expected<void> {
        const Expected<std::string> text = cell.as_string();
        if (!text) {
          return fail(ErrorKind::bad_argument, std::string(what) + " row " + detail::num(row) +
                                                   ": expected text, got " + cell.describe());
        }
        out.push_back(*text);
        return Expected<void>{};
      });
  if (!walked) {
    return unexpected(walked.error());
  }
  if (out.empty()) {
    return fail(ErrorKind::bad_argument, std::string(what) + " is empty");
  }
  return out;
}

XlValue rate_points_grid(std::span<const RatePoint> points) {
  XlGrid grid;
  grid.rows = points.empty() ? 1 : points.size();
  grid.columns = 2;
  grid.cells.reserve(grid.rows * 2);
  if (points.empty()) {
    grid.cells.emplace_back(std::string("the curve has no points"));
    grid.cells.emplace_back(XlValue{});
    return XlValue{grid};
  }
  for (const RatePoint &p : points) {
    grid.cells.emplace_back(to_excel_serial(p.date));
    grid.cells.emplace_back(p.rate);
  }
  return XlValue{grid};
}

XlValue calendar_dates_grid(const Calendar &calendar) {
  const std::span<const Date> dates = calendar.holidays();
  XlGrid grid;
  grid.rows = dates.empty() ? 1 : dates.size();
  grid.columns = 1;
  grid.cells.reserve(grid.rows);
  if (dates.empty()) {
    // A weekend-only calendar has no listed holidays, and an empty grid is not
    // something Excel can show. Say so instead.
    grid.cells.emplace_back(std::string("no listed holidays (weekends only)"));
    return XlValue{grid};
  }
  for (const Date d : dates) {
    grid.cells.emplace_back(to_excel_serial(d));
  }
  return XlValue{grid};
}

} // namespace cdsmodel::xl
