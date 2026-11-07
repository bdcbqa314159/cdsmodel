#!/usr/bin/env python3
"""How much of upstream is ported, measured rather than remembered.

    python3 tools/port_progress.py

Shares its provenance logic with tools/port_order.py, deliberately: the two
answer different questions ("what can I port next?" and "how far along am I?")
off the same facts, and letting them disagree is how a progress number becomes
a story. Both read the oracle archive's symbol table for the function list --
not a regex over the sources, which undercounted four files in a row because
the return type sat on its own line.

Three numbers, because no single one is honest on its own:

  * FILES is the coarsest and flatters most -- a header claiming a file does
    not mean every function in it is done.
  * FUNCTIONS is the one to quote. It counts a function as ported only when
    some header's "Replaces ..." or "Equivalent to ..." clause names it.
  * LINES weights by size, which is the best proxy for remaining effort.

Requires a local upstream copy and a built debug tree; without one it prints
nothing rather than guessing.
"""
import collections
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
UP = ROOT / "isda_cds_model_c_v1.8.3" / "lib" / "src"

# Files the port does not reproduce at all, with the reason. Stated so the
# denominator is honest rather than flattering.
OUT_OF_SCOPE = {
    "cerror": "error-message plumbing; the port returns std::optional",
    "lscanf": "a scanf reimplementation for the holiday-file reader",
    "lprintf": "a printf reimplementation for the same",
    "cfileio": "the file I/O layer underneath it",
    "strutil": "string helpers; std::string_view",
    "cmemory": "the allocator; new/delete and std::vector",
    "version": "replaced by version.hpp",
}

# Replaced structurally rather than one-for-one, so never countable as gaps.
#
# Allocator/deallocator PAIRS go in together. Three times now a slice has
# landed with only the Free half listed, and the audit then reported the New
# half as a gap -- a missing sentence looking exactly like a missing port.
# If you add one half here, add the other.
STRUCTURAL = {
    "JpmcdsFreeCFL", "JpmcdsNewEmptyCFL", "JpmcdsFreeTCurve", "JpmcdsNewTCurve",
    "JpmcdsCopyCurve", "JpmcdsFreeDateList", "JpmcdsNewEmptyDateList",
    "JpmcdsCopyDateList", "JpmcdsZCFree", "JpmcdsZCMake", "JpmcdsSwapDatesFree",
    "JpmcdsSwapDatesNewEmpty", "JpmcdsFreeHoliday", "JpmcdsHolidayListDelete",
    "JpmcdsFreeCouponDateList", "JpmcdsNewEmptyCouponDateList",
    "JpmcdsNewEmptyTStreamFixed", "JpmcdsNewEmptyTStreamFloat", "JpmcdsFreeTStreamFixed", "JpmcdsFreeTStreamFloat",
    "JpmcdsErrMsg", "JpmcdsErrMsgFailure", "JpmcdsFormatDate",
    "JpmcdsFormatDayCountConv", "JpmcdsFormatDateInterval",
    "JpmcdsBSearchCheckOrder",
}


def exported_by_file():
    out = subprocess.run(
        ["nm", "-g", "-A", str(ROOT / "build/debug/libcds_reference.a")],
        capture_output=True, text=True).stdout
    by_file = collections.defaultdict(set)
    for line in out.splitlines():
        m = re.match(r"^.*?:([^:]+)\.o:\s+\S+\s+T\s+_(Jpmcds\w+)$", line)
        if m:
            by_file[re.sub(r"\.(c|cpp)$", "", m.group(1))].add(m.group(2))
    return by_file


def provenance():
    """Which files a header claims, and which functions it says it replaces."""
    files, funcs = set(), set()
    for h in sorted((ROOT / "include" / "cdsmodel").glob("*.hpp")):
        text = h.read_text(encoding="utf-8")
        head = "".join(text.splitlines(keepends=True)[:8])
        m = re.search(r"Port of upstream ([a-z0-9_.+ ]+)", head)
        if m:
            for name in re.findall(r"[a-z0-9_]+(?:\.c|\.cpp|\.inc)", m.group(1)):
                files.add(re.sub(r"\.(c|cpp|inc)$", "", name))
        for clause in re.findall(r"(?:Replaces|Equivalent to)\b(.{0,400}?)(?:\.\s|\.$)",
                                 text, re.S):
            funcs.update(re.findall(r"Jpmcds\w+", clause))
    return files, funcs


def main():
    by_file = exported_by_file()
    if not by_file:
        print("no oracle archive -- build the debug preset with an upstream copy present")
        return
    claimed_files, ported = provenance()
    lines = {p.stem: sum(1 for _ in p.open(encoding="utf-8", errors="replace"))
             for p in list(UP.glob("*.c")) + list(UP.glob("*.cpp"))}

    rows = []
    for stem, n_lines in sorted(lines.items()):
        funcs = by_file.get(stem, set())
        countable = {f for f in funcs if f not in STRUCTURAL}
        done = countable & ported
        rows.append((stem, n_lines, funcs, countable, done))

    oos = [r for r in rows if r[0] in OUT_OF_SCOPE]
    scope = [r for r in rows if r[0] not in OUT_OF_SCOPE]
    started = [r for r in scope if r[0] in claimed_files]
    complete = [r for r in started if r[3] == r[4]]
    partial = [r for r in started if r[3] != r[4]]
    untouched = [r for r in scope if r[0] not in claimed_files]

    L = lambda rs: sum(r[1] for r in rs)
    F = lambda rs: sum(len(r[3]) for r in rs)
    D = lambda rs: sum(len(r[4]) for r in rs)

    print(f"{'':26} {'files':>6} {'funcs':>7} {'done':>6} {'lines':>7}")
    print(f"{'upstream lib/src':26} {len(rows):>6} {F(rows):>7} {D(rows):>6} {L(rows):>7}")
    print(f"{'  out of scope':26} {len(oos):>6} {F(oos):>7} {'-':>6} {L(oos):>7}")
    print(f"{'  IN SCOPE':26} {len(scope):>6} {F(scope):>7} {D(scope):>6} {L(scope):>7}")
    print(f"{'    complete':26} {len(complete):>6} {F(complete):>7} {D(complete):>6} {L(complete):>7}")
    print(f"{'    partial':26} {len(partial):>6} {F(partial):>7} {D(partial):>6} {L(partial):>7}")
    print(f"{'    not started':26} {len(untouched):>6} {F(untouched):>7} {'0':>6} {L(untouched):>7}")
    print()
    print(f"FUNCTIONS  {D(scope)}/{F(scope)} = {100*D(scope)/F(scope):.0f}%   <- the one to quote")
    print(f"LINES      in files that are function-complete: "
          f"{L(complete)}/{L(scope)} = {100*L(complete)/L(scope):.0f}%")
    print(f"FILES      {len(complete)}/{len(scope)} = {100*len(complete)/len(scope):.0f}%"
          f"   (coarsest; flatters most)")
    print()
    if partial:
        print("PARTIAL -- a header claims the file, these functions are not named:")
        for stem, n_lines, _funcs, countable, done in sorted(partial,
                                                             key=lambda r: len(r[3]) - len(r[4]),
                                                             reverse=True):
            missing = sorted(countable - done)
            print(f"  {stem:12} {len(done)}/{len(countable)}  {n_lines:>5} lines   "
                  f"{', '.join(missing)}")
        print()
    print("NOT STARTED, by size:")
    for stem, n_lines, _f, countable, _d in sorted(untouched, key=lambda r: r[1], reverse=True):
        print(f"  {stem:12} {len(countable):>2} fn {n_lines:>5} lines")


if __name__ == "__main__":
    main()
