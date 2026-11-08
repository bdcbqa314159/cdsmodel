#pragma once

// A minimal stand-in for <windows.h>, enough for the Excel SDK's xlcall.h to
// parse on a machine that is not Windows.
//
// This exists for ONE purpose: typechecking xl/sdk and xl/interface against
// the real xlcall.h without a Windows machine. Those ~300 lines are the only
// code in the project that CI cannot compile, and "written blind" was a worse
// answer than "written blind but typechecked".
//
// It is NOT a Windows compatibility layer and nothing links against it. The
// types below are the ones xlcall.h actually names -- checked by grepping it,
// not guessed -- with the sizes Win64 uses, because the layout of XLOPER12 is
// what we are trying to verify.

#include <cstddef>
#include <cstdint>

using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = std::uint32_t;
using DWORD_PTR = std::uint64_t;
using BOOL = int;
using INT32 = std::int32_t;
using WCHAR = wchar_t;
using LPSTR = char *;
using LPWSTR = wchar_t *;
using HANDLE = void *;

using HWND = void *;
using HINSTANCE = void *;
using HKEY = void *;
struct POINT {
  long x;
  long y;
};

// The calling conventions and attributes xlcall.h decorates its declarations
// with. No-ops here; on Windows they matter and the real header supplies them.
// `far` and `cdecl` without the underscore are 16-bit leftovers that the
// SDK's framework.h still carries.
#define far
#define cdecl
#define pascal
#define _cdecl
#define __cdecl
#define __stdcall
#define CALLBACK
#define WINAPI
