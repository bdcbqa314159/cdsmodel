#!/usr/bin/env python3
"""Project invariants, each enforceable and each gating CI.

Every gate here exists because the alternative is a promise in a markdown
file that nothing checks. Run a single gate:  python verify.py <gate>
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).parent.resolve()

# Every text read below passes encoding="utf-8" explicitly. Windows Python
# defaults to cp1252, which cannot decode the em-dashes in the reports, and a
# cross-OS tree's encoding is not a per-platform guess.

# Floor on tests actually RUN, not merely defined. Carried from pricebook's
# scar: a green run over a truncated collection is not evidence.
#
# Calibrated to the CI case -- tier 1 only, no oracle present -- because that
# is the binding one. A local run with the oracle executes strictly more.
# Bump it when a module lands; never lower it to make a red run green.
TEST_FLOOR_WITHOUT_ORACLE = 197


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def ok(msg: str) -> None:
    print(f"ok: {msg}")


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True
    )
    return out.stdout.splitlines()


def gate_upstream_absent() -> None:
    """The upstream ISDA sources must never be committed.

    They are distributed only from cdsmodel.com after accepting the licence
    and supplying an email address. Committing them here routes around that
    gate. This is the one invariant whose breach is a licence problem rather
    than a bug, so it is checked before anything is built.
    """
    offenders = [f for f in tracked_files() if re.match(r"isda_cds_model_c_v", f)]
    # Upstream's own filenames, in case a copy lands somewhere unexpected.
    fingerprints = ("cdsbootstrap.c", "cdsone.c", "buscache.c", "zcswap.c", "cfinanci.cpp")
    offenders += [f for f in tracked_files() if pathlib.Path(f).name in fingerprints]
    if offenders:
        fail(
            "upstream ISDA sources are tracked by git:\n  "
            + "\n  ".join(sorted(set(offenders)))
            + "\nThese must never be committed -- see NOTICE.md."
        )
    ok("no upstream ISDA sources are tracked")


def gate_provenance() -> None:
    """Every ported header must name the upstream file it came from.

    A port whose ancestry is undocumented cannot be reviewed against its
    original, and in six months nobody will remember which 2009 file a given
    function answers to.
    """
    exempt = {"version.hpp"}  # not a port of anything
    headers = sorted((ROOT / "include" / "cdsmodel").glob("*.hpp"))
    if not headers:
        fail("no headers found under include/cdsmodel")
    missing = []
    for h in headers:
        if h.name in exempt:
            continue
        text = h.read_text(encoding="utf-8")
        if not re.search(r"[Pp]ort of upstream \w+\.(c|cpp)", text):
            missing.append(h.name)
    if missing:
        fail(
            "headers do not name their upstream source "
            '(expected a line like "Port of upstream dateconv.c"):\n  '
            + "\n  ".join(missing)
        )
    ok(f"all {len(headers) - len(exempt & {h.name for h in headers})} ported headers name their source")


def gate_golden_generated() -> None:
    """Recorded reference vectors and their generators must come in pairs.

    A hand-written golden file is how a port's bug becomes the port's
    specification, and a generator whose output was never committed means CI
    is verifying nothing. Both directions are checked, so "none yet" is a
    consistent state and "one without the other" is not.
    """
    goldens = {
        g.name.removesuffix("_cases.hpp"): g
        for g in sorted((ROOT / "tests" / "golden").glob("*_cases.hpp"))
    }
    generators = {
        g.name.removeprefix("gen_").removesuffix("_golden.cpp"): g
        for g in sorted((ROOT / "tools").glob("gen_*_golden.cpp"))
    }

    orphan_vectors = sorted(set(goldens) - set(generators))
    if orphan_vectors:
        fail(
            "recorded vectors with no generator (so nobody can reproduce them): "
            + ", ".join(orphan_vectors)
        )
    orphan_generators = sorted(set(generators) - set(goldens))
    if orphan_generators:
        fail(
            "generators whose output was never committed (so CI verifies nothing): "
            + ", ".join(orphan_generators)
        )

    for stem, g in goldens.items():
        text = g.read_text(encoding="utf-8")
        if "GENERATED FILE -- DO NOT EDIT" not in text:
            fail(f"{g.name} lacks its generated-file banner")
        if text.count("\n    {") < 50:
            fail(f"{g.name} holds suspiciously few cases -- truncated regeneration?")

    ok(f"{len(goldens)} recorded vector sets, each paired with a generator")


def gate_reports() -> None:
    """Every catalogued defect must appear on all three surfaces.

    The register in port-notes.md, the full account in defect-report.md, and
    the published page in docs/index.html describe the same findings. Prose
    cannot be diffed usefully, but a missing id can be caught -- and adding a
    defect to one surface and forgetting the others is the realistic failure.
    """
    surfaces = {
        "docs/port-notes.md": None,
        "docs/defect-report.md": None,
        "docs/index.html": None,
    }
    # The journal narrates the findings rather than cataloguing them, so it is
    # not required to mention every id -- but it must not name one that does
    # not exist, which is how a retold story drifts from the record.
    journal = ROOT / "docs/journal.md"
    for name in surfaces:
        f = ROOT / name
        if not f.exists():
            fail(f"{name} is missing")
        surfaces[name] = set(re.findall(r"UPSTREAM-(\d+)", f.read_text(encoding="utf-8")))

    register = surfaces["docs/port-notes.md"]
    if not register:
        fail("docs/port-notes.md names no defects; the register is empty")
    for name, ids in surfaces.items():
        missing = sorted(register - ids, key=int)
        if missing:
            fail(
                f"{name} does not mention "
                + ", ".join(f"UPSTREAM-{i}" for i in missing)
                + " -- a defect was added to one surface and not the others."
            )
    if journal.exists():
        told = set(re.findall(r"UPSTREAM-(\d+)", journal.read_text(encoding="utf-8")))
        invented = sorted(told - register, key=int)
        if invented:
            fail(
                "docs/journal.md refers to "
                + ", ".join(f"UPSTREAM-{i}" for i in invented)
                + " which is not in the register"
            )

    ok(f"all {len(register)} catalogued defects appear on all three surfaces")


def gate_upstream_unmodified() -> None:
    """The oracle must be byte-identical to what ISDA ships.

    Only runnable where a local copy exists, so CI skips it. Editing upstream
    to make a port agree with it destroys the only independent check the
    project has, and would do so silently.
    """
    manifest = ROOT / "tools" / "upstream.sha256"
    upstream = ROOT / "isda_cds_model_c_v1.8.3"
    if not upstream.exists():
        ok("no local oracle present -- skipping (expected in CI)")
        return
    if not manifest.exists():
        fail(f"no manifest at {manifest.relative_to(ROOT)}; run: python verify.py write-manifest")
    expected = dict(
        line.split("  ", 1) for line in manifest.read_text(encoding="utf-8").splitlines() if line
    )
    problems = []
    for rel, want in expected.items():
        f = upstream / rel
        if not f.exists():
            problems.append(f"{rel}: missing")
            continue
        got = hashlib.sha256(f.read_bytes()).hexdigest()
        if got != want:
            problems.append(f"{rel}: MODIFIED")
    if problems:
        fail("the oracle has been altered:\n  " + "\n  ".join(problems))
    ok(f"all {len(expected)} upstream files are byte-identical to the recorded manifest")


def gate_write_manifest() -> None:
    """Record hashes of the local oracle. Run once, by hand, on a fresh copy."""
    upstream = ROOT / "isda_cds_model_c_v1.8.3"
    if not upstream.exists():
        fail(f"no local oracle at {upstream}")
    lines = []
    for f in sorted(upstream.rglob("*")):
        if f.is_file() and f.suffix in {".c", ".h", ".cpp", ".inc", ".txt", ".mk"}:
            rel = f.relative_to(upstream).as_posix()
            lines.append(f"{rel}  {hashlib.sha256(f.read_bytes()).hexdigest()}")
    out = ROOT / "tools" / "upstream.sha256"
    out.write_text("\n".join(lines) + "\n")
    ok(f"wrote {len(lines)} hashes to {out.relative_to(ROOT)}")


def gate_tests(preset: str) -> None:
    """Run ctest and enforce a floor on how many tests actually ran."""
    proc = subprocess.run(
        ["ctest", "--preset", preset, "--output-on-failure"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        # Print EVERYTHING on failure. An earlier version truncated to the last
        # 4000 characters, which reliably cut off the assertion detail and left
        # only ctest's summary -- the diagnostic was hidden by the gate that
        # exists to produce it.
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        fail("ctest reported failures")

    print(proc.stdout[-4000:])
    # CMake 4 omits the ", N tests failed" clause when nothing failed.
    m = re.search(r"(\d+)% tests passed(?:, (\d+) tests failed)? out of (\d+)", proc.stdout)
    if not m:
        fail("could not parse ctest summary -- did any tests run at all?")
    failed, total = int(m.group(2) or 0), int(m.group(3))
    if failed:
        fail(f"{failed} tests failed")
    if total < TEST_FLOOR_WITHOUT_ORACLE:
        fail(
            f"only {total} tests ran, floor is {TEST_FLOOR_WITHOUT_ORACLE}. "
            "A green run over a truncated collection is not evidence."
        )
    ok(f"{total} tests ran, all passed (floor {TEST_FLOOR_WITHOUT_ORACLE})")


GATES = {
    "upstream-absent": gate_upstream_absent,
    "provenance": gate_provenance,
    "golden": gate_golden_generated,
    "reports": gate_reports,
    "upstream-unmodified": gate_upstream_unmodified,
    "write-manifest": gate_write_manifest,
}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gate", choices=[*GATES, "tests", "all"])
    ap.add_argument("--preset", default="release")
    args = ap.parse_args()

    if args.gate == "tests":
        gate_tests(args.preset)
    elif args.gate == "all":
        for name, fn in GATES.items():
            if name != "write-manifest":
                fn()
        gate_tests(args.preset)
    else:
        GATES[args.gate]()


if __name__ == "__main__":
    main()
