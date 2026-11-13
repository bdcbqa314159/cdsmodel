#!/usr/bin/env python3
"""Refuse a wheel PyPI would refuse.

Artifact #5, P1. The first version of this check asserted only that
`cp312-abi3` appeared in the filename, and passed
`cdsmodel-0.1.0-cp312-abi3-linux_x86_64.whl` -- a wheel PyPI rejects on
upload, because a bare `linux_` platform tag says nothing about which glibc
the binary needs. The ABI tag was verified and the platform tag was never
looked at.

That mattered more than it sounds: a release uploads every platform at once,
so macOS and Windows would have been accepted and Linux refused, with the
version number already burned and unusable again.
"""

from __future__ import annotations

import sys
from pathlib import Path

# What PyPI accepts as a platform tag. `linux_*` is deliberately absent.
PUBLISHABLE = ("manylinux", "musllinux", "macosx_", "win_amd64", "win32", "any")


def check(path: Path) -> list[str]:
    name = path.name
    problems: list[str] = []
    parts = name.removesuffix(".whl").split("-")
    if len(parts) < 5:
        return [f"{name}: not a wheel filename"]
    _, _, python_tag, abi_tag, platform_tag = parts[:5]

    # D1: one wheel per platform covering 3.12 and later. A wheel tagged
    # cp312-cp312 installs and imports perfectly well on 3.12 and then does
    # not exist for anyone on 3.13.
    if abi_tag != "abi3":
        problems.append(f"{name}: abi tag is {abi_tag!r}, expected 'abi3'")
    if python_tag != "cp312":
        problems.append(f"{name}: python tag is {python_tag!r}, expected 'cp312'")
    if not platform_tag.startswith(PUBLISHABLE):
        problems.append(
            f"{name}: platform tag {platform_tag!r} is not publishable -- "
            f"PyPI accepts only {', '.join(PUBLISHABLE)}. "
            f"A Linux wheel must be built with cibuildwheel so it is tagged manylinux."
        )
    return problems


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]]
    if not paths:
        print("FAIL: no wheels given -- this check would pass vacuously", file=sys.stderr)
        return 1
    problems = [p for path in paths for p in check(path)]
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    if problems:
        return 1
    for path in paths:
        print(f"ok: {path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
