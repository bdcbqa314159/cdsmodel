#!/bin/sh
# Typechecks the Windows-only XL layers on a machine that is not Windows.
#
# xl/sdk and xl/interface are the only code in this project that CI cannot
# compile: they sit behind a platform guard, so on macOS and Linux they vanish.
# That left ~300 lines no compiler had ever seen. This closes most of that gap
# by compiling them against the REAL Excel SDK header, with a ten-typedef shim
# standing in for <windows.h>.
#
# What it catches: wrong XLOPER12 field names, wrong union members, type
# mismatches, missing SDK declarations -- which is most of what a first
# Windows build would have found.
#
# What it cannot catch: anything about the ABI's actual layout, calling
# conventions, or whether Excel accepts what we build. Only Excel can.
#
# Needs the SDK in xlsdk/ (see CLAUDE.md). Exits non-zero on any diagnostic.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

if [ ! -f xlsdk/xlcall.h ]; then
  echo "typecheck_xl: no xlsdk/xlcall.h -- see CLAUDE.md for where the SDK goes"
  exit 2
fi

for src in xl/sdk/src/oper.cpp xl/interface/src/exports.cpp; do
  printf '  %-34s ' "$src"
  if c++ -std=c++20 -fsyntax-only -DCDSMODEL_XL_TYPECHECK -DWIN32 -fdeclspec \
      -I tools/winshim -I xlsdk -I include \
      -I xl/core/include -I xl/sdk/include "$src" 2>/tmp/xl_typecheck.log; then
    echo "ok"
  else
    echo "FAILED"
    head -40 /tmp/xl_typecheck.log
    exit 1
  fi
done
echo "both Windows-only layers typecheck against the real xlcall.h"
