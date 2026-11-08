#pragma once

/// C4 and C5 of specs/03_xll.md -- the registration table and dispatch.
///
/// Upstream's XLL had one genuinely good idea: a declarative `TFuncDef` table
/// of name, parameters and help, walked once at load time. `cpp_fdm_world`
/// instead hand-writes an `Excel12f(xlfRegister, 0, 11, ...)` call per
/// function, which is fine for six and copy-paste for twenty-four. This keeps
/// the table.
///
/// A dispatcher takes the arguments as `XlValue` and returns one, so dispatch
/// is ordinary C++ and testable with no Excel anywhere (D8). The Windows layer
/// walks this same table to register, and converts `XLOPER12` at the edge.

#include <cdsmodel/expected.hpp>
#include <cdsmodel/xl/handles.hpp>
#include <cdsmodel/xl/value.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace cdsmodel::xl {

struct ParamSpec {
  std::string_view name;
  std::string_view help;
};

/// Everything the add-in needs to register one function, and the context a
/// dispatcher needs to run it.
struct CallContext {
  CurveRegistry &curves;
  CalendarRegistry &calendars;
};

/// Returns `Expected<XlValue>`, not `XlValue`.
///
/// An earlier revision returned a bare `XlValue` and treated any string result
/// as a failure -- the convention `cpp_fdm_world` uses, and it works there
/// because every function returns a number. It breaks here: upstream's
/// `IRZeroCurveBuild` and `CleanSpreadCurveBuild` return a curve HANDLE, which
/// is text, so a successful build would have been reported as an error. Found
/// by auditing before those functions were written, which is the only cheap
/// time to find it.
using Dispatcher = Expected<XlValue> (*)(std::span<const XlValue>, CallContext &);

struct FunctionSpec {
  /// The name as it appears in a cell.
  std::string_view name;
  /// Excel's type-code string: one per return plus one per argument.
  /// `Q` is an `XLOPER12` in and out, which is what every function here uses
  /// so that an error can come back as text in the cell rather than `#VALUE!`.
  std::string_view type_codes;
  std::span<const ParamSpec> params;
  std::string_view help;
  std::string_view category;
  Dispatcher dispatch;

  /// C4: the arity the table claims, derived from the type codes. One leading
  /// code is the return value, so arity is `type_codes.size() - 1`.
  [[nodiscard]] constexpr std::size_t declared_arity() const noexcept {
    return type_codes.empty() ? 0 : type_codes.size() - 1;
  }
};

/// Every function the add-in exports. Walked at load time to register, and by
/// `verify.py` to check the table against itself.
[[nodiscard]] std::span<const FunctionSpec> all_functions();

/// Looks a function up by the name a cell would use.
[[nodiscard]] const FunctionSpec *find_function(std::string_view name);

/// What the cell shows for a failed call. Artifact #01 reaching the user: the
/// message the library produced, not `#VALUE!` and not a log entry somebody
/// has to go and read.
///
/// Called at the EDGE -- by the Windows layer, once -- rather than inside each
/// dispatcher, so that the error stays typed for as long as possible and a
/// text result can never be confused with a text error.
[[nodiscard]] XlValue to_cell(const Error &error);

/// Runs a function by name and renders the outcome for a cell. This is what
/// the five entry points call, and the only place an error becomes text.
[[nodiscard]] XlValue invoke(const FunctionSpec &spec, std::span<const XlValue> args,
                             CallContext &context);

} // namespace cdsmodel::xl
