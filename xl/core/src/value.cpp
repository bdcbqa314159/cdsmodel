#include <cdsmodel/xl/value.hpp>

#include <charconv>
#include <string_view>

namespace cdsmodel::xl {
namespace {

/// Trim what a spreadsheet user leaves around a typed value.
///
/// Upstream's XLL called `JpmcdsStringRemoveWhiteSpace` on every string before
/// parsing, for a reason worth keeping: people type and paste padded text into
/// cells. Measured on this port's own parsers, " f/s " and " 3M " are both
/// REFUSED without it -- and the interval one refuses with a misleading
/// message, because the trailing space becomes the period-type character and
/// the complaint lands on the interval type instead of the space.
[[nodiscard]] std::string_view trimmed(std::string_view s) noexcept {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

} // namespace

bool operator==(const XlGrid &a, const XlGrid &b) {
  return a.rows == b.rows && a.columns == b.columns && a.cells == b.cells;
}

const XlValue &XlGrid::at(std::size_t row, std::size_t column) const {
  static const XlValue kOutside{XlError{"cell is outside the range"}};
  if (!in_range(row, column)) {
    return kOutside;
  }
  return cells[row * columns + column];
}

std::string XlValue::describe() const {
  return std::visit(
      [](const auto &v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, XlMissing>) {
          return "empty";
        } else if constexpr (std::is_same_v<T, double>) {
          return "a number";
        } else if constexpr (std::is_same_v<T, bool>) {
          return "TRUE/FALSE";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return "the text \"" + v + "\"";
        } else if constexpr (std::is_same_v<T, XlGrid>) {
          return "a " + std::to_string(v.rows) + "x" + std::to_string(v.columns) + " range";
        } else {
          return "an error (" + v.text + ")";
        }
      },
      storage_);
}

Expected<double> XlValue::as_double() const {
  if (const auto *d = std::get_if<double>(&storage_)) {
    return *d;
  }
  if (const auto *b = std::get_if<bool>(&storage_)) {
    return *b ? 1.0 : 0.0;
  }
  if (const auto *s = std::get_if<std::string>(&storage_)) {
    // Text that IS a number is accepted -- upstream's XLL did, and a cell
    // formatted as text is an ordinary spreadsheet accident. Text that merely
    // STARTS with a number is not: `atof` would have returned the prefix and
    // priced a trade from "3 months" as 3.
    const std::string_view t = trimmed(*s);
    double out = 0.0;
    const auto *end = t.data() + t.size();
    const auto [stop, err] = std::from_chars(t.data(), end, out);
    if (err == std::errc{} && stop == end) {
      return out;
    }
    return fail(ErrorKind::bad_argument, "expected a number, got " + describe());
  }
  if (const auto *e = std::get_if<XlError>(&storage_)) {
    return fail(ErrorKind::bad_argument, "the cell holds an error: " + e->text);
  }
  return fail(ErrorKind::bad_argument, "expected a number, got " + describe());
}

Expected<std::string> XlValue::as_string() const {
  if (const auto *s = std::get_if<std::string>(&storage_)) {
    // Trimmed on the way in, so every parser downstream gets clean text and
    // none of them has to tolerate padding individually.
    return std::string(trimmed(*s));
  }
  if (const auto *e = std::get_if<XlError>(&storage_)) {
    return fail(ErrorKind::bad_argument, "the cell holds an error: " + e->text);
  }
  // A number is NOT stringified. A cell holding 3 where ACT/360 was wanted is
  // a mistake, and "3" is not a day count.
  return fail(ErrorKind::bad_argument, "expected text, got " + describe());
}

Expected<bool> XlValue::as_bool() const {
  if (const auto *b = std::get_if<bool>(&storage_)) {
    return *b;
  }
  if (const auto *d = std::get_if<double>(&storage_)) {
    return *d != 0.0; // Excel's own rule
  }
  if (const auto *s = std::get_if<std::string>(&storage_)) {
    const std::string_view t = trimmed(*s);
    if (t == "TRUE" || t == "true" || t == "True") {
      return true;
    }
    if (t == "FALSE" || t == "false" || t == "False") {
      return false;
    }
  }
  if (const auto *e = std::get_if<XlError>(&storage_)) {
    return fail(ErrorKind::bad_argument, "the cell holds an error: " + e->text);
  }
  return fail(ErrorKind::bad_argument, "expected TRUE or FALSE, got " + describe());
}

Expected<Date> date_from_excel_serial(double serial, std::string_view what) {
  if (serial != static_cast<double>(static_cast<long>(serial))) {
    return fail(ErrorKind::bad_argument, std::string(what) +
                                             ": a date must be a whole number of days, got " +
                                             detail::num(serial));
  }
  return Date{} + std::chrono::days{static_cast<long>(serial) - kExcelEpochOffset};
}

double excel_serial_from_date(Date date) {
  return static_cast<double>(date.time_since_epoch().count() + kExcelEpochOffset);
}

std::string ascii_from_utf16(std::u16string_view units) {
  std::string out;
  out.reserve(units.size());
  for (const char16_t unit : units) {
    // Narrow deliberately and visibly: anything outside ASCII becomes '?'
    // rather than being truncated silently. The full length is kept whatever
    // it is, which is what XLOPER12 buys over XLOPER's 255-char ceiling.
    out.push_back(unit < 128 ? static_cast<char>(unit) : '?');
  }
  return out;
}

Expected<XlGrid> XlValue::as_grid() const {
  if (const auto *g = std::get_if<XlGrid>(&storage_)) {
    return *g;
  }
  if (const auto *e = std::get_if<XlError>(&storage_)) {
    return fail(ErrorKind::bad_argument, "the cell holds an error: " + e->text);
  }
  // A single cell is a 1x1 range, which is what Excel passes when a user
  // selects one cell where a range was wanted.
  if (is_missing()) {
    return fail(ErrorKind::bad_argument, "expected a range, got nothing");
  }
  return XlGrid{1, 1, {*this}};
}

} // namespace cdsmodel::xl
