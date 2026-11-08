#pragma once

/// C0 of specs/03_xll.md -- the seam between the add-in's logic and Excel.
///
/// `cpp_fdm_world` puts its whole Excel layer behind `#ifdef WIN32`, which is
/// right for a project whose author builds on Windows. Here it would put the
/// marshalling, the dispatch table and the labelled-range reader on the one
/// machine that cannot run CI -- exactly what `CLAUDE.md`'s two-tier contract
/// exists to prevent. So the boundary sits one layer lower: everything in this
/// header is ordinary C++ that compiles and runs on every platform, and only a
/// thin `XLOPER12` adapter in `xl/sdk` needs Windows.
///
/// `XlValue` is deliberately NOT a faithful `XLOPER12`. It carries only what
/// the 12 functions need, which is why it can be plain C++: a number, a
/// string, a boolean, a 2-D grid of values, a missing argument, or an error.

#include <cdsmodel/expected.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace cdsmodel::xl {

class XlValue;

/// A rectangular block of cells, as Excel hands over a range. Stored
/// row-major, which is the order `xltypeMulti` uses.
struct XlGrid {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::vector<XlValue> cells;

  [[nodiscard]] const XlValue &at(std::size_t row, std::size_t column) const;
  [[nodiscard]] bool in_range(std::size_t row, std::size_t column) const noexcept {
    return row < rows && column < columns;
  }

  /// Declared here, defined in the .cpp: `cells` holds `XlValue`, which is
  /// still incomplete at this point, so a defaulted comparison cannot be
  /// generated yet. The recursion is real -- a grid holds values, a value may
  /// be a grid -- and this is where it has to be broken.
  friend bool operator==(const XlGrid &a, const XlGrid &b);
};

/// Excel's "the user left this argument out". Distinct from an empty string
/// and from zero, both of which a user can type deliberately -- conflating
/// them is how an omitted argument silently becomes a default.
struct XlMissing {
  [[nodiscard]] friend bool operator==(XlMissing, XlMissing) noexcept { return true; }
};

/// A cell that already holds an error, which Excel propagates into ours.
struct XlError {
  std::string text;
  [[nodiscard]] friend bool operator==(const XlError &, const XlError &) = default;
};

/// One Excel value, as far as this add-in is concerned.
class XlValue {
public:
  using Storage = std::variant<XlMissing, double, bool, std::string, XlGrid, XlError>;

  XlValue() : storage_(XlMissing{}) {}
  XlValue(double v) : storage_(v) {}
  XlValue(bool v) : storage_(v) {}
  XlValue(std::string v) : storage_(std::move(v)) {}
  XlValue(const char *v) : storage_(std::string(v)) {}
  XlValue(XlGrid v) : storage_(std::move(v)) {}
  XlValue(XlError v) : storage_(std::move(v)) {}
  XlValue(XlMissing v) : storage_(v) {}

  [[nodiscard]] bool is_missing() const noexcept {
    return std::holds_alternative<XlMissing>(storage_);
  }
  [[nodiscard]] bool is_error() const noexcept { return std::holds_alternative<XlError>(storage_); }
  [[nodiscard]] bool is_grid() const noexcept { return std::holds_alternative<XlGrid>(storage_); }

  /// A number, however the cell spells it. Excel gives `TRUE` as a boolean and
  /// a typed number as a double; a user may also have typed the digits as
  /// text, which upstream's XLL accepted, so text that is entirely a number is
  /// accepted here too. Anything else is an error naming what was found --
  /// never a silent zero, which is what `atof` would have given.
  [[nodiscard]] Expected<double> as_double() const;

  /// A string. A number is NOT silently stringified: a cell holding 3 where a
  /// day count was wanted is a mistake worth reporting.
  [[nodiscard]] Expected<std::string> as_string() const;

  [[nodiscard]] Expected<bool> as_bool() const;
  [[nodiscard]] Expected<XlGrid> as_grid() const;

  [[nodiscard]] const Storage &storage() const noexcept { return storage_; }

  /// What this value is, for an error message. "a number", "text", "empty".
  [[nodiscard]] std::string describe() const;

  [[nodiscard]] friend bool operator==(const XlValue &, const XlValue &) = default;

private:
  Storage storage_;
};

} // namespace cdsmodel::xl
