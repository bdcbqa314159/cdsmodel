#!/usr/bin/env python3
"""Which upstream files can be ported next, and what is in the way.

The answer changes every slice, so this is a script rather than a note. Run it
before choosing the next module:

    python3 tools/port_order.py

A file is a LEAF when every Jpmcds* symbol its source references is either
defined in itself, named in some port header's "Replaces ..." clause, in a file
the port does not reproduce at all, or in the STRUCTURAL set below.

Reported blockers are a CEILING, not a floor: a `file:function` blocker can
also mean the port already covers that function under a different name and the
header simply never said so. The fix for a false positive is to make the header
claim it -- which is the provenance mechanism verify.py already enforces --
rather than to special-case it here. Over-reporting costs a minute of checking;
under-reporting costs a wasted slice, which is why this script is function-level
at all: the file-level version called zr2coup a leaf when it needs three
unported tcurve.c functions.

Why the symbol table and not a regex over the sources: a regex for function
definitions undercounted three files in a row -- one function in zcall.c, two
in cashflow.c, four in datelist.c -- every time because the return type sat on
its own line. `nm` on the oracle archive has no such ambiguity. It needs a
local upstream copy and a built debug tree; without one it prints nothing
rather than guessing.
"""
import re, subprocess, pathlib, collections
ROOT = pathlib.Path("/Users/bernardocohen/work/my_ISDA_CDS_model")
UP = ROOT/"isda_cds_model_c_v1.8.3"/"lib"/"src"
out = subprocess.run(["nm","-g","-A",str(ROOT/"build/debug/libcds_reference.a")],
                     capture_output=True,text=True).stdout
owner = {}
for line in out.splitlines():
    m = re.match(r"^.*?:([^:]+)\.o:\s+\S+\s+T\s+_(Jpmcds\w+)$", line)
    if m: owner[m.group(2)] = re.sub(r"\.(c|cpp)$","",m.group(1))

OUT_OF_SCOPE = {"cerror","lscanf","lprintf","cfileio","strutil","cmemory","version"}

# Functions the port replaces STRUCTURALLY rather than one-for-one, so they can
# never block a file. Allocation and lifetime become std::vector and value
# semantics; error text becomes std::optional, so there is nothing to format.
# Listed by category rather than per function, because the decision was
# categorical -- see the headers' "no port needed" notes.
STRUCTURAL = {
    # allocation and lifetime
    "JpmcdsFreeCFL", "JpmcdsNewEmptyCFL", "JpmcdsFreeTCurve", "JpmcdsNewTCurve",
    "JpmcdsCopyCurve", "JpmcdsFreeDateList", "JpmcdsNewEmptyDateList",
    "JpmcdsCopyDateList", "JpmcdsZCFree", "JpmcdsZCMake", "JpmcdsSwapDatesFree",
    "JpmcdsSwapDatesNewEmpty", "JpmcdsFreeHoliday", "JpmcdsHolidayListDelete",
    "JpmcdsFreeCouponDateList", "JpmcdsNewEmptyCouponDateList",
    "JpmcdsNewEmptyTStreamFixed", "JpmcdsNewEmptyTStreamFloat", "JpmcdsFreeTStreamFixed", "JpmcdsFreeTStreamFloat",
    # error reporting
    "JpmcdsErrMsg", "JpmcdsErrMsgFailure", "JpmcdsFormatDate",
    "JpmcdsFormatDayCountConv", "JpmcdsFormatDateInterval",
    # mutable global state, deliberately not reproduced -- see UPSTREAM-13
    "JpmcdsBSearchCheckOrder",
    # the holiday-file reader and its process-wide cache. The port takes
    # calendars as values, so where they come from is the caller's choice.
    "JpmcdsHolidayLoadFromDisk", "JpmcdsHolidayListFromCache",
    "JpmcdsHolidayListAddToCache", "JpmcdsHolidayEmptyCache",
    "JpmcdsHolidayListRead", "JpmcdsHolidayListNewGeneral",
}
claimed = set()
ported_funcs = set()
for h in (ROOT/"include"/"cdsmodel").glob("*.hpp"):
    text = h.read_text(encoding="utf-8")
    head = "".join(text.splitlines(keepends=True)[:8])
    m = re.search(r"Port of upstream ([a-z0-9_.+ ]+)", head)
    if m:
        for n in re.findall(r"[a-z0-9_]+(?:\.c|\.cpp|\.inc)", m.group(1)):
            claimed.add(re.sub(r"\.(c|cpp|inc)$","",n))
    # FUNCTION level, which is what actually decides whether a file can be
    # ported. The first version of this script only tracked which FILES a port
    # header claimed, and called zr2coup a leaf when it needs three unported
    # tcurve.c functions -- a header claiming a file does not mean every
    # function in it is done. Only names inside a "Replaces ..." clause count,
    # so prose naming a function as missing is never miscounted as present.
    # Two phrasings are in use: "Replaces X" and "Equivalent to upstream X".
    # Both are provenance claims, and only counting the first undercounted
    # bsearch.hpp, which covers JpmcdsBSearchLong under the second.
    for clause in re.findall(r"(?:Replaces|Equivalent to)\b(.{0,400}?)(?:\.\s|\.$)",
                             text, re.S):
        ported_funcs.update(re.findall(r"Jpmcds\w+", clause))

remaining = []
for src in sorted(UP.glob("*.c")) + sorted(UP.glob("*.cpp")):
    stem = src.stem
    if stem in claimed or stem in OUT_OF_SCOPE: continue
    text = src.read_text(encoding="utf-8", errors="replace")
    calls = {f for f in re.findall(r"Jpmcds\w+", text) if f in owner}
    blockers = collections.Counter()
    for f in calls:
        o = owner[f]
        if o == stem or o in OUT_OF_SCOPE or f in ported_funcs or f in STRUCTURAL:
            continue
        # The file may be claimed while this particular function is not.
        blockers[o if o not in claimed else f"{o}:{f}"] += 1
    remaining.append((stem, sum(1 for _ in src.open(errors="replace")), blockers))

print("REMAINING IN-SCOPE FILES, by how much is in their way:")
for stem, ln, blockers in sorted(remaining, key=lambda r: (len(r[2]), r[1])):
    if not blockers:
        print(f"  {stem:12} {ln:>5} lines   *** NO UNPORTED DEPENDENCIES -- a leaf ***")
    else:
        tot = ", ".join(f"{k}({v})" for k, v in blockers.most_common())
        print(f"  {stem:12} {ln:>5} lines   blocked by {len(blockers)}: {tot}")
