#!/usr/bin/env python3
"""B1-B5 of specs/02_consumability.md -- act as the consumer.

The oracle ladder in ~/.claude/verification.md is written for numerical
results and has no rung for "is this installable", so the strongest check
available is to BE the consumer: install into a throwaway prefix, then
configure and build a project that lives outside this tree and knows nothing
about cdsmodel beyond `find_package`.

Reading the CMake and nodding is not an oracle. This is.

Run from the repo root:  python3 tools/check_consumer.py
"""

from __future__ import annotations

import shutil
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FAILURES: list[str] = []


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)


def find_executable(build: Path, stem: str) -> Path | None:
    """The built binary, across single- and multi-config generators.

    Windows needs care on two counts: the suffix is `.exe`, and MSVC's
    multi-config generator creates a `<stem>.dir/` DIRECTORY next to it. An
    earlier revision globbed `consumer*` and found the directory first, then
    tried to execute it -- `PermissionError: [WinError 5] Access is denied`,
    which reads like a permissions problem and is not one.
    """
    names = [f"{stem}.exe", stem]
    for name in names:
        direct = build / name
        if direct.is_file():
            return direct
    for name in names:
        for found in sorted(build.rglob(name)):
            if found.is_file():
                return found
    return None


def check(label: str, ok: bool, detail: str = "") -> bool:
    print(f"  {'ok  ' if ok else 'FAIL'}  {label}")
    if not ok:
        FAILURES.append(label + (f"\n        {detail.strip()[-900:]}" if detail else ""))
    return ok


def configured_build(preferred: str | None = None) -> Path:
    """Whichever build directory is actually configured.

    CI configures `release` only; a local run usually has `debug`. An earlier
    revision hardcoded `build/debug` and failed on all three CI runners -- the
    same mistake `verify.py install-tree` had made one file over, and which
    was fixed there and not carried across.
    """
    names = ([preferred] if preferred else []) + ["release", "debug", "asan"]
    for name in names:
        build = ROOT / "build" / name
        if (build / "CMakeCache.txt").exists():
            # A build configured before the install rules existed installs
            # nothing of ours, which produces a baffling "cdsmodel not found"
            # from the consumer instead of a clear failure here. Check the
            # cache for the export set rather than discovering it downstream.
            if (build / "cmake_install.cmake").exists():
                text = (build / "cmake_install.cmake").read_text(
                    encoding="utf-8", errors="ignore"
                )
                if "cdsmodelTargets" not in text:
                    print(f"  skip  build/{name} predates the install rules; reconfigure it")
                    continue
            return build
    sys.exit(
        "no build with install rules found; run `cmake --preset release` "
        "(or debug) after the install rules landed"
    )


def install_to(prefix: Path, build: Path) -> bool:
    r = run(["cmake", "--install", str(build), "--prefix", str(prefix)])
    if not check(
        f"B1  cmake --install succeeds (from {build.name})",
        r.returncode == 0,
        r.stderr or r.stdout,
    ):
        return False
    # Fail here, not three checks later in the consumer's configure step: an
    # install that stages none of our library is the failure, and saying so at
    # the point of discovery is the difference between a clear message and a
    # confusing one.
    staged = list((prefix / "include" / "cdsmodel").glob("*.hpp"))
    return check(
        f"B1  the install staged our headers ({len(staged)})",
        len(staged) > 0,
        "the build directory predates the install rules",
    )


def b1_tree_shape(prefix: Path) -> None:
    """The counts are computable, so they are asserted and not eyeballed."""
    headers = sorted((prefix / "include" / "cdsmodel").glob("*.hpp"))
    expected = len(sorted((ROOT / "include" / "cdsmodel").glob("*.hpp")))
    check(
        f"B1  all {expected} headers installed (found {len(headers)})",
        len(headers) == expected,
    )

    lib = list((prefix / "lib").glob("*cdsmodel*"))
    check("B1  the static library is installed", len(lib) == 1, str(lib))

    cm = prefix / "lib" / "cmake" / "cdsmodel"
    for f in ("cdsmodelConfig.cmake", "cdsmodelConfigVersion.cmake", "cdsmodelTargets.cmake"):
        check(f"B1  {f} installed", (cm / f).exists())

    # B7's concern, checked here too because this is the tree a consumer gets.
    strays = [p.name for p in prefix.rglob("*") if p.is_file() and "gtest" in p.name.lower()]
    check("B7  no GoogleTest in the install tree", not strays, str(strays[:5]))


def consumer_build(prefix: Path, label: str, extra_cache: list[str]) -> None:
    """Configure + build + RUN the consumer. Running is the point."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "consumer"
        shutil.copytree(ROOT / "tests" / "consumer", src)
        build = Path(tmp) / "build"

        cfg = run(
            ["cmake", "-S", str(src), "-B", str(build), f"-DCMAKE_PREFIX_PATH={prefix}"]
            + extra_cache
        )
        if not check(f"{label}  configures via find_package", cfg.returncode == 0,
                     cfg.stderr or cfg.stdout):
            return

        # --config matters for multi-config generators and is ignored by the
        # single-config ones, so it is always safe to pass.
        bld = run(["cmake", "--build", str(build), "--config", "Release"])
        if not check(f"{label}  builds (-Werror, umbrella only)", bld.returncode == 0,
                     bld.stderr or bld.stdout):
            return

        exe = find_executable(build, "consumer")
        if not check(f"{label}  produced a runnable binary", exe is not None,
                     f"nothing named consumer[.exe] under {build}"):
            return
        got = run([str(exe)])
        ok = got.returncode == 0 and "CONSUMER OK" in got.stdout
        check(f"{label}  runs: a CDS at its own par spread prices to ~0", ok,
              got.stdout + got.stderr)
        if ok:
            for line in got.stdout.strip().split("\n"):
                print(f"            {line}")


def b3_fetchcontent() -> None:
    """The second consumption path: build cdsmodel as a subproject.

    This is NOT a variant of B2 and the distinction matters -- an earlier
    revision of this script passed an unused cache variable to the
    install-based consumer and called it B3, which proved nothing. What is
    specific to FetchContent is that our CMakeLists runs inside someone
    else's build, so PROJECT_IS_TOP_LEVEL must suppress our tests and the
    consumer must not need the licence-gated oracle.
    """
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "consumer"
        shutil.copytree(ROOT / "tests" / "consumer", src)
        (src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(cdsmodel_fc_consumer LANGUAGES CXX)\n"
            "include(FetchContent)\n"
            # as_posix(): CMake accepts forward slashes everywhere, and a
            # Windows path interpolated raw makes `D:\a\cdsmodel` -- where
            # CMake reads `\a` as an invalid character escape and refuses to
            # parse the file.
            f'FetchContent_Declare(cdsmodel SOURCE_DIR "{ROOT.as_posix()}")\n'
            "FetchContent_MakeAvailable(cdsmodel)\n"
            "add_executable(consumer main.cpp)\n"
            "target_link_libraries(consumer PRIVATE cdsmodel::cdsmodel)\n"
        )
        build = Path(tmp) / "build"
        cfg = run(["cmake", "-S", str(src), "-B", str(build),
                   "-DCDSMODEL_UPSTREAM_DIR=/nonexistent"])
        if not check("B3  FetchContent consumer configures", cfg.returncode == 0,
                     cfg.stderr or cfg.stdout):
            return

        # The point of this criterion: our tests must NOT have been generated.
        # PROJECT_IS_TOP_LEVEL is false here, so CDSMODEL_BUILD_TESTS defaults
        # off and a consumer does not pay for our suite or need GoogleTest.
        targets = run(["cmake", "--build", str(build), "--target", "help"])
        ours = [t for t in targets.stdout.split("\n") if "cdsmodel_tests" in t]
        check("B3  our test targets are not generated for a consumer", not ours, str(ours))

        gtest_fetched = (build / "_deps" / "googletest-src").exists()
        check("B3  GoogleTest is not downloaded for a consumer", not gtest_fetched)

        bld = run(["cmake", "--build", str(build), "--target", "consumer",
                   "--config", "Release"])
        if not check("B3  FetchContent consumer builds", bld.returncode == 0,
                     bld.stderr or bld.stdout):
            return
        exe = find_executable(build, "consumer")
        if not check("B3  produced a runnable binary", exe is not None):
            return
        got = run([str(exe)])
        check("B3  runs without the oracle present",
              got.returncode == 0 and "CONSUMER OK" in got.stdout, got.stdout + got.stderr)


def b4_version(prefix: Path) -> None:
    """A version file that accepts everything is not a version file."""
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "v"
        src.mkdir()
        (src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(v LANGUAGES CXX)\n"
            "find_package(cdsmodel ${WANT} REQUIRED)\n"
        )
        # Read from CMakeLists rather than hardcoded. It said "0.1.0" through
        # the 0.1.1 bump and kept passing, so it was asserting that a
        # HISTORICAL version is findable -- and at 1.0.0 it would have failed
        # for the wrong reason, because SameMajorVersion refuses a 0.x request
        # from a 1.x package. Same bug as `version()` returning a literal.
        project_version = re.search(
            r"project\(cdsmodel VERSION ([0-9]+\.[0-9]+\.[0-9]+)",
            (ROOT / "CMakeLists.txt").read_text()).group(1)
        for want, should_pass in ((project_version, True), ("99.0", False)):
            r = run(["cmake", "-S", str(src), "-B", str(Path(tmp) / f"b{want}"),
                     f"-DCMAKE_PREFIX_PATH={prefix}", f"-DWANT={want}"])
            check(
                f"B4  find_package(cdsmodel {want}) "
                f"{'succeeds' if should_pass else 'is refused'}",
                (r.returncode == 0) == should_pass,
                r.stderr or r.stdout,
            )


def main() -> int:
    print("Artifact #2 -- acting as the consumer\n")
    preferred = sys.argv[1] if len(sys.argv) > 1 else None
    build = configured_build(preferred)
    with tempfile.TemporaryDirectory() as prefix_dir:
        prefix = Path(prefix_dir) / "prefix"
        if not install_to(prefix, build):
            print("\nFAILED: nothing else can be checked without an install")
            return 1
        b1_tree_shape(prefix)
        b4_version(prefix)
        consumer_build(prefix, "B2", [])
    # B3 is a genuinely different consumption path -- FetchContent, where the
    # consumer builds cdsmodel from source as a subproject rather than finding
    # an installed one. It gets its own prefix-free run below.
    b3_fetchcontent()

    print()
    if FAILURES:
        print(f"FAILED ({len(FAILURES)}):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("all consumer checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
