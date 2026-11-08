#pragma once

/// C2 of specs/03_xll.md -- the only layer that knows what an XLOPER12 is.
///
/// Everything above this (`xl/core`) works on `XlValue` and is tested on every
/// platform. Everything here needs Windows and the Excel SDK, so it is kept as
/// small as it can be: convert in, convert out, and nothing else. If a rule
/// about marshalling is worth testing, it belongs in `xl/core`, not here.
///
/// Guarded, like `cpp_fdm_world`'s `xlOper.h`, so a non-Windows build compiles
/// this file to nothing rather than needing a stub.

// `CDSMODEL_XL_TYPECHECK` compiles this on a non-Windows machine against the
// real `xlcall.h` plus the minimal shim in `tools/winshim`. It exists because
// this file is otherwise the only code in the project no compiler has ever
// seen -- see tools/typecheck_xl.sh. Defining `_WIN32` instead would make the
// standard library believe it is on Windows, which breaks libc++ immediately.
#if defined(_WIN32) || defined(CDSMODEL_XL_TYPECHECK)

#include <cdsmodel/xl/value.hpp>

// `<windows.h>` FIRST: xlcall.h's own header comment says "Include
// <windows.h> before you include this", and it means it -- every type in
// XLOPER12 comes from there, and the whole file is behind `#ifdef WIN32`,
// which MSVC defines for you. Found by typechecking this file off Windows
// (tools/typecheck_xl.sh); it was missing, and would have failed there too
// for any translation unit that did not happen to include windows.h first.
#include <windows.h>

#include "xlcall.h"

#include <string>
#include <vector>

namespace cdsmodel::xl {

/// Reads one Excel argument. Never throws and never fails: an input this layer
/// cannot make sense of becomes an `XlError`, so the decision about what to do
/// with it is made in `xl/core` where it can be tested.
[[nodiscard]] XlValue from_oper(const LPXLOPER12 oper);

/// Writes a result into Excel-owned temporary memory.
///
/// The returned pointer is allocated with the SDK's `GetTempMemory`, which the
/// framework frees on the next call into the add-in. That is why every
/// exported function calls `FreeAllTempMemory()` on entry -- the same
/// discipline `cpp_fdm_world` uses, and the reason `xlAutoFree` has nothing to
/// do here.
[[nodiscard]] LPXLOPER12 to_oper(const XlValue &value);

/// Builds a length-counted Excel string in temp memory.
///
/// Conversion is written out rather than delegated to `mbstowcs` (D10): that
/// function is locale-dependent, so the same workbook could marshal a path
/// differently on two machines. Input here is ASCII by construction -- it comes
/// from our own messages and from Excel's own type codes.
[[nodiscard]] LPXLOPER12 temp_string(std::string_view text);

} // namespace cdsmodel::xl

#endif // _WIN32 || CDSMODEL_XL_TYPECHECK
