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
    only change made is to its #include lines -- its executable code is left
    exactly as Microsoft ships it. The pristine sample compiles xlcall.cpp by
    #including it inline, which would double-define every callback when xlcall.cpp
    is also built on its own, so that one include is dropped; the framework and
    memory-manager includes are repointed at the names we stage them under.

    The sample's 8-bit (Excel4) helpers are NOT touched: a 64-bit link would
    otherwise leave `Excel4`/`Excel4v` unresolved, but that is fixed on OUR side
    of the seam by xl/sdk/src/excel4_stub.cpp, which defines the two symbols the
    helpers reference (a 64-bit add-in never calls them). So nothing in this
    sample's body is edited to make the add-in build.

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


def patch_framework(text: str) -> str:
    return fix_includes(text)


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
