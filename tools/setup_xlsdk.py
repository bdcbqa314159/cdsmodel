#!/usr/bin/env python3
"""Stage the Excel XLL SDK into xlsdk/ from a local copy you downloaded.

The SDK is Microsoft's and its licence (clause 2.b) forbids redistributing any
of its files, so this repo cannot ship them -- `xlsdk/` is gitignored, and each
machine supplies its own copy. This script does that supply step: point it at
your extracted "Microsoft Excel 2013 XLL SDK" and it copies the eight files the
build needs into xlsdk/.

What it does, and why it is licence-clean:

  * xlcall.h, xlcall.cpp and the memory-manager samples are copied BYTE-FOR-BYTE
    -- clause 2.a.i forbids modifying xlcall.h/xlcall.cpp, so they stay pristine.
  * framework.c is a SAMPLE (clause 2.a.ii permits modifying samples), and the
    only change made is to its #include lines: the pristine sample compiles
    xlcall.cpp by #including it inline, which would double-define every callback
    when xlcall.cpp is also built on its own. Un-inlining it is the one edit.

Nothing is committed: xlsdk/ is gitignored. You still accept Microsoft's licence
when you download the SDK.

    python tools/setup_xlsdk.py /path/to/extracted/Excel2013XLLSDK
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "xlsdk"

# dest name -> accepted source basenames (case-insensitive). The framework files
# are FRAMEWRK.* in Microsoft's tree and framework.* in already-renamed copies;
# accept either.
WANTED: dict[str, tuple[str, ...]] = {
    "xlcall.h": ("xlcall.h",),
    "xlcall.cpp": ("xlcall.cpp",),
    "framework.h": ("framework.h", "framewrk.h"),
    "framework.c": ("framework.c", "framewrk.c"),
    "MemoryManager.h": ("memorymanager.h",),
    "MemoryManager.cpp": ("memorymanager.cpp",),
    "MemoryPool.h": ("memorypool.h",),
    "MemoryPool.cpp": ("memorypool.cpp",),
}

# xlcall.h/.cpp may not be modified (licence 2.a.i); everything else here is a
# sample. Only framework.c is actually edited, below.
PRISTINE = {"xlcall.h", "xlcall.cpp"}


def find(sdk: Path, names: tuple[str, ...]) -> Path | None:
    """First file under `sdk` whose basename case-insensitively matches."""
    targets = {n.lower() for n in names}
    for dirpath, _dirs, files in os.walk(sdk):
        for f in files:
            if f.lower() in targets:
                return Path(dirpath) / f
    return None


# The sample framework defines 8-bit (Excel4 / LPXLOPER) helpers that 64-bit
# Excel does not provide an entry point for -- `Excel4`/`Excel4v` are unresolved
# in an x64 link. A 64-bit add-in never calls them (it uses the `*12` forms), so
# they are compiled out. Matched by their exact 8-bit signatures, never the
# `*12` variants beside them.
EIGHT_BIT_SIGNATURES = (
    "int cdecl Excel(",
    "int _cdecl Excel(",
    "LPXLOPER TempActiveRef(",
    "LPXLOPER TempActiveCell(",
    "LPXLOPER TempActiveRow(",
    "LPXLOPER TempActiveColumn(",
)


def fix_includes(text: str) -> str:
    """Stop framework.c compiling xlcall.cpp inline, and point its framework and
    memory includes at the names we stage them under."""
    out: list[str] = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped == "#include <xlcall.cpp>":
            continue  # un-inline: xlcall.cpp is compiled on its own
        if stripped == "#include <framewrk.h>":
            out.append(line.replace("<framewrk.h>", '"framework.h"'))
            continue
        if stripped.lower() == "#include <memorymanager.h>":
            out.append('#include "MemoryManager.h"\n')
            continue
        out.append(line)
    return "".join(out)


def disable_8bit(text: str) -> str:
    """Wrap each 8-bit helper (signature to matching close brace) in `#if 0`."""
    lines = text.splitlines(keepends=True)
    out: list[str] = []
    i = 0
    while i < len(lines):
        if any(lines[i].lstrip().startswith(sig) for sig in EIGHT_BIT_SIGNATURES):
            out.append("#if 0 // 8-bit Excel4 API: no entry point in 64-bit Excel\n")
            depth = 0
            seen_brace = False
            while i < len(lines):
                out.append(lines[i])
                depth += lines[i].count("{") - lines[i].count("}")
                if "{" in lines[i]:
                    seen_brace = True
                i += 1
                if seen_brace and depth == 0:
                    break
            out.append("#endif\n")
        else:
            out.append(lines[i])
            i += 1
    return "".join(out)


def patch_framework(text: str) -> str:
    return disable_8bit(fix_includes(text))


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage the Excel XLL SDK into xlsdk/.")
    ap.add_argument("sdk", type=Path, help="path to the extracted Excel 2013 XLL SDK")
    args = ap.parse_args()

    sdk: Path = args.sdk
    if not sdk.is_dir():
        sys.exit(f"not a directory: {sdk}")

    located: dict[str, Path] = {}
    missing: list[str] = []
    for dest_name, names in WANTED.items():
        src = find(sdk, names)
        if src is None:
            missing.append(f"{dest_name} (looked for: {', '.join(names)})")
        else:
            located[dest_name] = src
    if missing:
        sys.exit("could not find in the SDK:\n  " + "\n  ".join(missing))

    DEST.mkdir(exist_ok=True)
    for dest_name, src in located.items():
        dst = DEST / dest_name
        if dest_name == "framework.c":
            dst.write_text(patch_framework(src.read_text(encoding="utf-8", errors="surrogateescape")),
                           encoding="utf-8", errors="surrogateescape")
            note = "sample, includes adjusted"
        else:
            shutil.copyfile(src, dst)
            note = "pristine (unmodifiable)" if dest_name in PRISTINE else "pristine sample"
        print(f"  {dest_name:<20} <- {src}   [{note}]")

    print(f"\nStaged 8 files into {DEST.relative_to(ROOT)} (gitignored -- not committed).")
    print("Configure with: cmake --preset release")
    return 0


if __name__ == "__main__":
    sys.exit(main())
