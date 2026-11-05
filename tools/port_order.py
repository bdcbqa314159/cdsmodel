#!/usr/bin/env python3
"""Which upstream files can be ported next, and what is in the way.

The answer changes every slice, so this is a script rather than a note. Run it
before choosing the next module:

    python3 tools/port_order.py

A file is a LEAF when every Jpmcds* symbol its source references is defined
either in itself, in a file a port header already claims, or in one of the
files the port deliberately does not reproduce.

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
claimed = set()
for h in (ROOT/"include"/"cdsmodel").glob("*.hpp"):
    head = "".join(h.open(encoding="utf-8").readlines()[:8])
    m = re.search(r"Port of upstream ([a-z0-9_.+ ]+)", head)
    if m:
        for n in re.findall(r"[a-z0-9_]+(?:\.c|\.cpp|\.inc)", m.group(1)):
            claimed.add(re.sub(r"\.(c|cpp|inc)$","",n))

remaining = []
for src in sorted(UP.glob("*.c")) + sorted(UP.glob("*.cpp")):
    stem = src.stem
    if stem in claimed or stem in OUT_OF_SCOPE: continue
    text = src.read_text(encoding="utf-8", errors="replace")
    calls = {f for f in re.findall(r"Jpmcds\w+", text) if f in owner}
    blockers = collections.Counter()
    for f in calls:
        o = owner[f]
        if o != stem and o not in claimed and o not in OUT_OF_SCOPE:
            blockers[o] += 1
    remaining.append((stem, sum(1 for _ in src.open(errors="replace")), blockers))

print("REMAINING IN-SCOPE FILES, by how much is in their way:")
for stem, ln, blockers in sorted(remaining, key=lambda r: (len(r[2]), r[1])):
    if not blockers:
        print(f"  {stem:12} {ln:>5} lines   *** NO UNPORTED DEPENDENCIES -- a leaf ***")
    else:
        tot = ", ".join(f"{k}({v})" for k, v in blockers.most_common())
        print(f"  {stem:12} {ln:>5} lines   blocked by {len(blockers)}: {tot}")
