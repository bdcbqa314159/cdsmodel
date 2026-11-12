#!/usr/bin/env python3
"""Project invariants, each enforceable and each gating CI.

Every gate here exists because the alternative is a promise in a markdown
file that nothing checks. Run a single gate:  python verify.py <gate>
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import os
import re
from pathlib import Path
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
TEST_FLOOR_WITHOUT_ORACLE = 551


_PRESET = ["release"]


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
    # Not ports of anything: nothing upstream to name.
    exempt = {
        "version.hpp",
        "expected.hpp",  # the error channel -- specs/01_error_channel.md
        "cdsmodel.hpp",  # the umbrella -- specs/02_consumability.md
    }
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


def gate_claims() -> None:
    """No upstream function may be claimed by two headers.

    The provenance gate above catches a function nobody claims. This catches
    the opposite, and it cost a slice to learn the difference: `cxbsearch.c`'s
    JpmcdsBinarySearchLong was ported twice -- once as an uncalled `search`
    member on ZeroCurve, months earlier, and once as `match_range` -- and
    port_progress.py reported 100% either way, because it counts a function as
    done when ANY header claims it.

    A duplicate claim means two implementations of one function, which is the
    one thing a port exists to prevent. It is also invisible to every other
    gate: both compile, both pass their own tests, and the provenance audit is
    satisfied twice over.
    """
    headers = sorted((ROOT / "include" / "cdsmodel").glob("*.hpp"))

    claims: dict[str, list[str]] = {}
    for h in headers:
        text = h.read_text(encoding="utf-8")
        for clause in re.findall(
            r"(?:Replaces|Equivalent to)\b(.{0,400}?)(?:\.\s|\.$)", text, re.S
        ):
            for name in re.findall(r"Jpmcds\w+", clause):
                claims.setdefault(name, [])
                if h.name not in claims[name]:
                    claims[name].append(h.name)

    if not claims:
        fail("no provenance claims found at all, which cannot be right")

    duplicates = {n: v for n, v in claims.items() if len(v) > 1}
    if duplicates:
        fail(
            "the same upstream function is claimed by more than one header, "
            "which means it was ported twice:\n  "
            + "\n  ".join(f"{n}: {', '.join(v)}" for n, v in sorted(duplicates.items()))
        )
    ok(f"{len(claims)} claimed upstream functions, none claimed twice")


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

    # Every golden header declares its types in the one namespace
    # cdsmodel::golden, and they are all linked into the same test binary. Two
    # headers declaring the same type name is therefore an ODR violation: it
    # compiles, it links, and the program reads one struct's bytes through the
    # other's layout. That cost half an hour to diagnose once, from a
    # std::length_error inside an unrelated function, so it is a gate now --
    # the failure names itself instead of arriving as garbage fields.
    declarations: dict[str, list[str]] = {}
    for g in goldens.values():
        text = g.read_text(encoding="utf-8")
        for name in re.findall(r"^struct (\w+)", text, re.M):
            declarations.setdefault(name, []).append(g.name)
        # The declared name is the last identifier before the initialiser, so
        # match that rather than the type -- an earlier version of this regex
        # captured `std::array<Case, 360>`'s SIZE as the name and reported
        # every pair of headers that happened to record 360 cases.
        for name in re.findall(r"^inline constexpr .*?(\w+)\s*(?:=|\{\{)", text, re.M):
            declarations.setdefault(name, []).append(g.name)
    clashes = {n: sorted(set(f)) for n, f in declarations.items() if len(set(f)) > 1}
    if clashes:
        fail(
            "the same name is declared in more than one golden header, which is an "
            "ODR violation across the test binary:\n  "
            + "\n  ".join(f"{n}: {', '.join(files)}" for n, files in sorted(clashes.items()))
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


def gate_error_channel() -> None:
    """A5 + A7 from specs/01_error_channel.md.

    A5 -- no namespace-scope function in include/cdsmodel/ RETURNS
    std::optional. Parameters, fields, and function-local lambdas are out of
    scope by D7: `optional` still means "legitimately absent", and the error
    channel governs only failure. The two are distinguished semantically, so
    this gate checks the one thing that is mechanical -- a return type.

    A7 -- every Error carries a message, and the messages are not boilerplate.
    An empty message would let a 341-site migration produce errors that
    type-check and say nothing; a handful of messages repeated everywhere
    would do the same while passing a non-empty check. So the DISTINCT count
    is asserted, not just the non-empty one.
    """
    headers = sorted((ROOT / "include" / "cdsmodel").glob("*.hpp"))


    # D7 exemptions, named individually and never as a pattern. Each returns
    # "legitimately absent", the answer `map::find` gives -- a lookup miss, not
    # a failure. Listed here so adding a third is a deliberate act with a
    # reason, in the spirit of the no-silent-debt bar item.
    d7_exempt = {
        ("match_range.hpp", "exact"): "nothing matched the search key",
        ("zero_curve_builder.hpp", "find_exact"): "no curve point sits on this date",
    }

    # A5. A return type is a declaration whose optional<> precedes a function
    # name and an open paren, or sits alone on the line above one.
    offenders: list[str] = []
    for h in headers:
        lines = h.read_text(encoding="utf-8").split("\n")
        for i, line in enumerate(lines):
            stripped = line.strip()
            if "std::optional<" not in stripped or stripped.startswith("//"):
                continue
            # a lambda is not a namespace-scope function (D7)
            if "[](" in line or "[&](" in line or "= [" in line:
                continue
            decl = re.match(
                r"^(\[\[nodiscard\]\]\s*)?(inline\s+|static\s+|constexpr\s+)*"
                r"std::optional<.+>\s*(\w+\s*\(|$)",
                stripped,
            )
            if decl:
                nxt = lines[i + 1].strip() if i + 1 < len(lines) else ""
                # on its own line, the next line must open a function
                if decl.group(3) or re.match(r"^\w+\s*\(", nxt):
                    name_src = decl.group(3) or nxt
                    fn = re.match(r"^(\w+)", name_src)
                    if fn and (h.name, fn.group(1)) in d7_exempt:
                        continue
                    offenders.append(f"{h.name}:{i + 1}: {stripped[:72]}")
    if offenders:
        fail(
            "A5: these namespace-scope functions still return std::optional:\n  "
            + "\n  ".join(offenders)
        )

    # A7. Count Error constructions and their messages.
    empty: list[str] = []
    messages: list[str] = []
    for h in headers:
        text = h.read_text(encoding="utf-8")
        for m in re.finditer(r"fail\(ErrorKind::\w+,\s*(.*?)\);", text, re.S):
            body = " ".join(m.group(1).split())
            if body in ('""', "''") or not body:
                empty.append(f"{h.name}: {m.group(0)[:60]}")
            else:
                messages.append(body)
    if empty:
        fail("A7: these errors carry no message:\n  " + "\n  ".join(empty))

    distinct = len(set(messages))
    # Derived from the slice: 198 origin sites were authored by hand. Some
    # messages legitimately repeat (several functions refuse an empty curve
    # the same way), so the floor is below the total -- but boilerplate must
    # not dominate, which a ratio near 1.0 would hide.
    floor = 120
    if distinct < floor:
        fail(
            f"A7: only {distinct} distinct error messages across {len(messages)} "
            f"sites (floor {floor}); boilerplate is taking over"
        )

    print(
        f"ok: no namespace-scope function returns std::optional "
        f"({len(d7_exempt)} named D7 exemptions); "
        f"{len(messages)} errors, {distinct} distinct messages"
    )


def gate_umbrella() -> None:
    """B6 from specs/02_consumability.md.

    The umbrella header must include every public header. An umbrella that
    drifts is worse than none: a consumer who includes it reasonably believes
    they have the library, and a silently omitted header turns into a confusing
    compile error in THEIR code, not ours.

    D1 chose a hand-written umbrella over a generated one so the file is
    greppable and visible in the tree. This gate is what makes that choice
    safe.
    """
    header_dir = ROOT / "include" / "cdsmodel"
    umbrella = header_dir / "cdsmodel.hpp"
    if not umbrella.exists():
        fail("include/cdsmodel/cdsmodel.hpp does not exist")

    public = {h.name for h in header_dir.glob("*.hpp")} - {"cdsmodel.hpp"}
    included = set(re.findall(r"#include <cdsmodel/([\w.]+)>", umbrella.read_text(encoding="utf-8")))

    missing = sorted(public - included)
    if missing:
        fail(
            "B6: the umbrella header omits these public headers:\n  "
            + "\n  ".join(missing)
        )
    stale = sorted(included - public)
    if stale:
        fail("B6: the umbrella includes headers that no longer exist:\n  " + "\n  ".join(stale))

    # The includes must be sorted, so a hand-edited umbrella stays reviewable
    # and two people adding a header do not conflict in the same place.
    order = re.findall(r"#include <cdsmodel/([\w.]+)>", umbrella.read_text(encoding="utf-8"))
    if order != sorted(order):
        fail("B6: the umbrella's includes are not in sorted order")

    print(f"ok: the umbrella header includes all {len(public)} public headers, sorted")


def gate_install_tree(preset: str) -> None:
    """B7 from specs/02_consumability.md -- rule 2 of CLAUDE.md, mechanically.

    Now that install rules exist, "the oracle is never redistributed" stops
    being a property of nobody having written a bad install(DIRECTORY) line.
    Staging the install into a temporary prefix and inspecting it is the only
    way to check what a consumer would actually receive.
    """
    import shutil
    import subprocess
    import tempfile

    build = ROOT / "build" / preset
    if not (build / "CMakeCache.txt").exists():
        # CI configures release only; a local run usually has debug. Take
        # whichever exists rather than demanding one, so this gate does not
        # depend on which preset the caller happened to build.
        alternatives = [d for d in (ROOT / "build").glob("*") if (d / "CMakeCache.txt").exists()]
        if not alternatives:
            fail(f"B7: no configured build found (looked for build/{preset})")
        build = alternatives[0]

    with tempfile.TemporaryDirectory() as prefix:
        run = subprocess.run(
            ["cmake", "--install", str(build), "--prefix", prefix],
            capture_output=True,
            text=True,
        )
        if run.returncode != 0:
            fail("B7: cmake --install failed:\n" + (run.stderr or run.stdout)[-1500:])

        staged = [p for p in Path(prefix).rglob("*") if p.is_file()]
        if not staged:
            fail("B7: the install produced no files at all")

        # A gate that only checks for FORBIDDEN content passes on a tree with
        # nothing in it. This one reported "52 files, none upstream-derived"
        # over an install that contained GoogleTest and not one cdsmodel
        # header, because the build directory predated the install rules. So
        # the expected content is asserted first.
        want_headers = len(sorted((ROOT / "include" / "cdsmodel").glob("*.hpp")))
        got_headers = sorted((Path(prefix) / "include" / "cdsmodel").glob("*.hpp"))
        if len(got_headers) != want_headers:
            fail(
                f"B1/B7: the install tree has {len(got_headers)} cdsmodel headers, "
                f"expected {want_headers}. If this is 0, the build directory was "
                f"configured before the install rules existed -- reconfigure it."
            )
        if not list((Path(prefix) / "lib").glob("*cdsmodel*")):
            fail("B1/B7: the install tree has no cdsmodel library")

        # GoogleTest brings its own install rules; INSTALL_GTEST OFF suppresses
        # them, but only for a build configured after that line was added.
        gtest = [str(p.relative_to(prefix)) for p in staged if "gtest" in p.name.lower()
                 or "gmock" in p.name.lower()]
        if gtest:
            fail(
                "B7: the install tree carries GoogleTest, so a consumer would "
                f"receive our test framework ({len(gtest)} files, e.g. {gtest[:3]}). "
                "Reconfigure the build so INSTALL_GTEST=OFF takes effect."
            )

        # Nothing derived from the licence-gated oracle may appear.
        banned = ("isda_cds_model", "cds_reference", "jpmcds")
        offenders = [
            str(p.relative_to(prefix))
            for p in staged
            if any(b in p.name.lower() for b in banned)
        ]
        if offenders:
            fail(
                "B7: the install tree carries upstream-derived files, which "
                "routes around the licence gate (CLAUDE.md rule 2):\n  "
                + "\n  ".join(offenders)
            )

        # And no upstream SOURCE text, however it got there.
        for p in staged:
            if p.suffix.lower() in {".h", ".hpp", ".c", ".cpp", ".inc"}:
                text = p.read_text(encoding="utf-8", errors="ignore")
                if "ISDA CDS Standard Model Public License" in text:
                    fail(
                        f"B7: {p.relative_to(prefix)} carries the upstream licence "
                        "header, so it is upstream source"
                    )

        print(f"ok: install tree is {len(staged)} files, none upstream-derived")


def gate_sdk_absent() -> None:
    """Artifact #3: the Excel XLL SDK is never committed.

    Same reasoning as rule 2 for the ISDA oracle, different owner. `xlcall.h`
    and `xlcall32.lib` are Microsoft's, obtained from them under their terms;
    committing them here redistributes them. The repo is public, so this is
    not hypothetical.
    """
    tracked = subprocess.run(
        ["git", "ls-files"], cwd=ROOT, capture_output=True, text=True
    ).stdout.split("\n")

    banned_names = {"xlcall.h", "xlcall32.lib", "xlcall.cpp", "xlcall64.lib"}
    offenders = [
        t for t in tracked
        if t and (t.startswith("xlsdk/") or Path(t).name.lower() in banned_names)
    ]
    if offenders:
        fail(
            "the Excel XLL SDK must never be committed (it is Microsoft's):\n  "
            + "\n  ".join(offenders)
        )
    print("ok: no Excel XLL SDK files are tracked")


# Coverage floors, per area, measured with `verify.py coverage`.
#
# Two areas and not one: they have genuinely different ceilings. The ported
# library carries 341 error sites whose messages name an offending value, and
# reaching every one means constructing bad input for every refusal upstream
# ever made -- worth doing, not worth blocking a commit on. xl/core is new code
# with a small surface and no excuse.
#
# Raise these when coverage rises. NEVER lower one to make a red run green --
# the whole reason this gate exists is that coverage decayed silently through
# artifact #01 (341 new branches, no test for most) and nothing noticed until
# someone asked.
COVERAGE_FLOORS = {
    "include/cdsmodel/": {"lines": 85.0, "branches": 80.0},
    "xl/core/": {"lines": 90.0, "branches": 86.0},
}


def gate_python_layer() -> None:
    """Artifact #5, D23: the Python layer never depends on the Excel layer.

    `python/` and `xl/core/` are peers -- both bind the same `cdsmodel`
    functions, neither sits above the other. The temptation is real and
    specific: `cdsmodel::xl` already defines the four argument blocks Python
    mirrors, so including `<cdsmodel/xl/arguments.hpp>` would delete an
    obvious duplication. It would also drag `XlValue` and the handle
    registries into a wheel that has no use for either, and break the
    layering `CLAUDE.md` states as law.

    Comments naming `xl/` are fine -- `blocks.hpp` explains the duplication,
    and a gate that forbade the word would forbid its own rationale. Only
    `#include` lines and link targets count.
    """
    offenders = []
    for path in sorted((ROOT / "python").rglob("*")):
        if path.suffix not in {".cpp", ".hpp", ".h"}:
            continue
        for n, line in enumerate(path.read_text().splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("#include") and (
                "cdsmodel/xl/" in stripped or '"xl/' in stripped
            ):
                offenders.append(f"{path.relative_to(ROOT)}:{n}: {stripped}")

    cmake = (ROOT / "python" / "CMakeLists.txt").read_text()
    for m in re.finditer(r"target_link_libraries\([^)]*\)", cmake):
        if "cdsmodel_xl" in m.group(0):
            offenders.append(f"python/CMakeLists.txt: links {m.group(0)}")

    if offenders:
        fail(
            "the Python layer must not depend on the Excel layer (D23):\n  "
            + "\n  ".join(offenders)
        )

    # A gate that can only say "nothing bad found" would pass an empty tree.
    sources = [
        p for p in (ROOT / "python").rglob("*") if p.suffix in {".cpp", ".hpp"}
    ]
    if not sources:
        fail("no Python binding sources found -- the gate would pass vacuously")
    print(
        f"ok: {len(sources)} Python binding sources, none depending on xl/"
    )


def gate_sdist() -> None:
    """Artifact #5, P11: the source distribution leaks neither licensed tree.

    PyPI is a redistribution channel, and a far more consequential one than
    git: `pip download cdsmodel` would hand out whatever is in here. Rule 2
    of CLAUDE.md therefore applies with more force, not less -- the ISDA
    sources are gated behind accepting a licence on cdsmodel.com, and the
    Excel SDK is Microsoft's.

    Both are gitignored and scikit-build-core honours that, so the default is
    already correct. This gate exists because "already correct" is a property
    of one line of `.gitignore`, and because the exclusion is *ignore*-driven
    rather than *tracked*-driven: an untracked file nobody thought to ignore
    ships too.

    The positive half is not decoration. A gate that only looks for forbidden
    names passes an empty archive, which is how B3 and B7 false-passed
    earlier in this project.
    """
    import tarfile
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        result = subprocess.run(
            [sys.executable, "-m", "build", "--sdist", "--outdir", tmp, "."],
            cwd=ROOT, capture_output=True, text=True,
        )
        if result.returncode != 0:
            fail("could not build the sdist:\n" + result.stdout[-2000:] + result.stderr[-2000:])

        archives = list(pathlib.Path(tmp).glob("*.tar.gz"))
        if len(archives) != 1:
            fail(f"expected one sdist, found {[a.name for a in archives]}")

        with tarfile.open(archives[0]) as tar:
            names = [n.split("/", 1)[1] for n in tar.getnames() if "/" in n]

    forbidden = {
        "isda_cds_model": "the ISDA oracle",
        "xlsdk/": "the Excel XLL SDK",
        "xlcall": "the Excel XLL SDK",
        "framework.h": "the Excel XLL SDK",
    }
    leaks = [
        f"{n}  ({why})"
        for n in names
        for pat, why in forbidden.items()
        if pat in n.lower()
    ]
    if leaks:
        fail("the sdist must not redistribute licensed sources:\n  " + "\n  ".join(leaks))

    required = [
        "CMakeLists.txt", "pyproject.toml", "LICENSE", "NOTICE.md",
        "src/holiday_file.cpp", "python/src/pricing.cpp",
        "python/cdsmodel/__init__.py", "python/cdsmodel/py.typed",
        "python/cdsmodel/_core.pyi", "include/cdsmodel/cds.hpp",
    ]
    missing = [r for r in required if r not in names]
    if missing:
        fail(
            "the sdist is incomplete -- without these the absence check above "
            "would pass on an empty archive:\n  " + "\n  ".join(missing)
        )

    print(
        f"ok: sdist holds {len(names)} files, no licensed sources, "
        f"all {len(required)} required entries present"
    )


def gate_coverage() -> None:
    """Measures line and branch coverage per area against COVERAGE_FLOORS.

    Test files are EXCLUDED from the measurement. They are 100% line-covered
    by definition and ~51% branch-covered by gtest's never-taken
    assertion-failure paths, so including them measures the framework and
    understates the code -- which is exactly the mistake that produced a
    bogus "54.1%" for xl/core before this gate existed.
    """
    import shutil
    import tempfile

    profdata = shutil.which("llvm-profdata")
    cov = shutil.which("llvm-cov")
    if not profdata or not cov:
        # Apple ships them under xcrun rather than on PATH; xcrun itself is
        # absent off macOS, so calling it must not raise.
        try:
            which = subprocess.run(["xcrun", "--find", "llvm-cov"], capture_output=True, text=True)
            on_xcrun = which.returncode == 0
        except FileNotFoundError:
            on_xcrun = False
        if not on_xcrun:
            # This gate measures Clang source-based coverage; a box with no LLVM
            # toolchain (e.g. Windows + MSVC) cannot produce it. Skip cleanly
            # rather than crash -- the same posture upstream-unmodified takes
            # when the oracle is absent. Where llvm-cov exists (macOS, Linux CI)
            # this still runs and still enforces the floors, so nothing is
            # weakened; the measurement is simply taken where it can be.
            print("coverage: no llvm-cov/llvm-profdata on PATH -- skipping "
                  "(run on a machine with an LLVM toolchain: macOS or Linux)")
            return
        cov = which.stdout.strip()
        profdata = subprocess.run(
            ["xcrun", "--find", "llvm-profdata"], capture_output=True, text=True
        ).stdout.strip()

    with tempfile.TemporaryDirectory() as tmp:
        build = Path(tmp) / "b"
        # The instrumentation flags are Clang's. macOS/Linux default to a
        # compiler that understands them; Windows defaults to MSVC, which
        # silently ignores -f* and produces an uninstrumented binary (no
        # .profraw). Since this gate *requires* llvm-cov anyway, pin the
        # coverage build to Clang+Ninja on Windows.
        toolchain = (
            ["-G", "Ninja", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"]
            if os.name == "nt" else []
        )
        configure = subprocess.run(
            [
                "cmake", "-S", str(ROOT), "-B", str(build),
                *toolchain,
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping",
                "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate",
            ],
            capture_output=True, text=True,
        )
        if configure.returncode != 0:
            fail("coverage configure failed:\n" + (configure.stderr or configure.stdout)[-1200:])
        built = subprocess.run(
            ["cmake", "--build", str(build), "-j", "8", "--target", "cdsmodel_tests"],
            capture_output=True, text=True,
        )
        if built.returncode != 0:
            fail("coverage build failed:\n" + (built.stderr or built.stdout)[-1200:])

        # The bare name is POSIX; Windows adds .exe and a multi-config generator
        # (Visual Studio) drops it under build/Debug/. Find whatever got built.
        hits = list(build.glob("cdsmodel_tests")) + list(build.glob("cdsmodel_tests.exe"))
        hits += list(build.glob("*/cdsmodel_tests.exe"))
        if not hits:
            fail("coverage built cdsmodel_tests but the binary was not found under " + str(build))
        test_bin = hits[0]

        raw = build / "cov.profraw"
        run = subprocess.run(
            [str(test_bin)],
            capture_output=True, text=True,
            env={**os.environ, "LLVM_PROFILE_FILE": str(raw)},
        )
        if run.returncode != 0:
            fail("the instrumented test binary failed; fix the tests before measuring coverage")

        merged = build / "cov.profdata"
        subprocess.run([profdata, "merge", "-sparse", str(raw), "-o", str(merged)], check=True)
        report = subprocess.run(
            [cov, "report", str(test_bin), f"-instr-profile={merged}"],
            capture_output=True, text=True,
        ).stdout

    problems: list[str] = []
    for area, floors in COVERAGE_FLOORS.items():
        lines_total = lines_missed = branches_total = branches_missed = 0
        for raw_row in report.split("\n"):
            # llvm-cov prints native paths; on Windows that is backslashes,
            # but the area keys and the /tests/ exclusion use forward slashes.
            row = raw_row.replace("\\", "/")
            if area not in row or "/tests/" in row:
                continue
            parts = row.split()
            if len(parts) < 13:
                continue
            try:
                lines_total += int(parts[7])
                lines_missed += int(parts[8])
                branches_total += int(parts[10])
                branches_missed += int(parts[11])
            except ValueError:
                continue
        if lines_total == 0:
            fail(f"coverage found no instrumented lines for {area}; the filter is wrong")

        line_pct = 100.0 * (lines_total - lines_missed) / lines_total
        branch_pct = (
            100.0 * (branches_total - branches_missed) / branches_total if branches_total else 100.0
        )
        print(f"  {area:<20} lines {line_pct:5.1f}% (floor {floors['lines']:.0f})"
              f"   branches {branch_pct:5.1f}% (floor {floors['branches']:.0f})")
        if line_pct < floors["lines"]:
            problems.append(f"{area} line coverage {line_pct:.1f}% is below its floor "
                            f"{floors['lines']:.0f}%")
        if branch_pct < floors["branches"]:
            problems.append(f"{area} branch coverage {branch_pct:.1f}% is below its floor "
                            f"{floors['branches']:.0f}%")

    if problems:
        fail("coverage regressed:\n  " + "\n  ".join(problems))
    print("ok: every area is at or above its coverage floor")


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
    "claims": gate_claims,
    "error-channel": gate_error_channel,
    "umbrella": gate_umbrella,
    "sdk-absent": gate_sdk_absent,
    "python-layer": gate_python_layer,
    "sdist": gate_sdist,
    "coverage": gate_coverage,
    "install-tree": lambda: gate_install_tree(_PRESET[0]),
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
    _PRESET[0] = args.preset

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
