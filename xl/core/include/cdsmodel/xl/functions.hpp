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
};

using Dispatcher = XlValue (*)(std::span<const XlValue>, CallContext &);

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

/// Turns a failed call into what the cell shows. Artifact #01 reaching the
/// user: the message the library produced, not `#VALUE!` and not a log entry
/// somebody has to go and read.
///
/// The result is TEXT, not an `XlError`. The two are different directions:
/// `XlError` is an Excel error value arriving FROM a cell (`#DIV/0!`), while a
/// function that failed returns a readable sentence. Since every function here
/// returns a number on success, text in the result IS the failure signal --
/// the same convention `cpp_fdm_world` uses with `TempStr12("input 1 is not a
/// matrix")`, and the reason C3 asks for a message rather than an error code.
[[nodiscard]] XlValue to_cell(const Error &error);

/// Whether a dispatcher's result is a failure, under the convention above.
/// Spelled out so a caller never has to remember which way round it is.
[[nodiscard]] bool is_failure(const XlValue &result);

/// The failure text, or empty if the result is a value.
[[nodiscard]] std::string failure_text(const XlValue &result);

} // namespace cdsmodel::xl
