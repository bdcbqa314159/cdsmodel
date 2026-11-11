// Ours, not the SDK's. Lets the sample framework.c stay BYTE-FOR-BYTE as
// Microsoft ships it instead of editing its body to build.
//
// The sample framework defines a handful of 8-bit helpers (`TempActiveRef`,
// `TempActiveCell`, the variadic `Excel()` wrapper, ...) that call `Excel4` /
// `Excel4v`. Those entry points existed in 32-bit Excel; the 64-bit SDK's
// xlcall.cpp defines only the `*12` forms, so a 64-bit link leaves `Excel4` and
// `Excel4v` unresolved. A 64-bit add-in never calls the 8-bit helpers (every
// path through this project uses Excel12 / the `*12` opers), so defining the two
// symbols here as hard failures satisfies the linker without ever running.
//
// This is the seam working as intended: the workaround lives on OUR side of the
// line, so framework.c is not modified to build -- see docs/port-notes.md and
// CLAUDE.md "Getting the SDK".
// ponytail: stubs, not real 8-bit support; if a caller ever needs Excel4 at
// runtime this returns xlretFailed loudly rather than silently misbehaving.

#if defined(_WIN32) || defined(CDSMODEL_XL_TYPECHECK)

#include <windows.h>

#include "xlcall.h"

// Signatures mirror xlcall.h exactly (calling convention included) so these
// DEFINE the symbols the sample framework's 8-bit helpers reference, rather
// than introducing differently-mangled ones.
int _cdecl Excel4(int /*xlfn*/, LPXLOPER /*operRes*/, int /*count*/, ...) {
  return xlretFailed;
}

int pascal Excel4v(int /*xlfn*/, LPXLOPER /*operRes*/, int /*count*/,
                   LPXLOPER /*opers*/[]) {
  return xlretFailed;
}

#endif
