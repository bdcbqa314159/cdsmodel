# cdsmodel — the law for this repo

Guardrails, not suggestions. When a change conflicts with this file, this file
wins. Narrow by design: everything here is enforceable, and every line is
checked by a gate named beside it. Rationale lives in `specs/` and
`docs/port-notes.md`; how I work generally lives outside this repo.

## The oracle — two inviolable rules

`isda_cds_model_c_v1.8.3/` is a local copy of the upstream ISDA CDS Standard
Model. It is the project's only independent check on 163 ported functions.

1. **It is never edited.** Compiler problems are solved with CMake flags, never
   by touching a source file. Editing it destroys the only independent check
   the project has.
   → enforced by `python verify.py upstream-unmodified` (132 files byte-compared
   against a recorded manifest).

2. **It is never committed.** Upstream is gated behind accepting the ISDA
   licence and giving an email address on cdsmodel.com; redistributing it here
   routes around that. It is gitignored and stays that way.
   → enforced by `python verify.py upstream-absent`.

Computed output under `tests/golden/` **is** committable — licence section 4(b)
exempts derived data. Each generator says so in its header.

## The verification contract

CI has no oracle, because of rule 2. A module tested only against upstream is
therefore verified **on one laptop and nowhere else.** So every module needs
both:

- a **tier-2** differential test that compiles upstream into the same binary and
  sweeps the input domain against it (local only); and
- that sweep **recorded into tier-1 vectors** under `tests/golden/`, replayed
  without the oracle.

→ enforced by `python verify.py golden` (every vector set paired with its
generator) and `python verify.py tests` (a floor on the count collected with no
oracle present — raise it when a module lands, never lower it to go green).

The build must be green **both** ways before a commit:
`ctest --preset debug` and the same with `-DCDSMODEL_UPSTREAM_DIR=/nonexistent`.

## Divergence from upstream

The port reproduces upstream's behaviour, including its defects. Correcting is
the exception and needs a reason: reproduce when the implementation is
self-consistent and only its documentation disagrees; correcting is available
only when the code contradicts *itself*, so there is no single behaviour to
preserve.

Any deliberate difference gets all three of: a comment at the site, a row in
the "Deliberate differences" table in `docs/port-notes.md`, and a test
asserting it from **both** sides — so an upstream release that changes the
behaviour fails a test instead of drifting silently.

A known divergence is never frozen into a recorded vector set. Golden files are
what the port is checked against; putting a divergence there converts a
documented difference into a silent one.

→ enforced by `python verify.py reports` (every catalogued defect present on all
three surfaces) and `python verify.py claims` (no upstream function claimed by
two headers).

## The error channel

A function reports failure by returning `cdsmodel::Expected<T>` carrying a
kind and a message that names the offending value. **No namespace-scope
function in `include/cdsmodel/` returns `std::optional`.**

`std::optional` remains correct where `nullopt` means *legitimately absent* --
an optional parameter, a struct field with no value yet, a lookup miss. The
test is semantic: **absent is `optional`, failed is `Expected`.** Exemptions
are named individually in the gate, never granted by pattern.

Propagation is not a kind. A caller forwarding a callee's failure passes the
original kind and message through unchanged; inventing a wrapper at each layer
buries the one reason the caller needs.

`Expected<T>` is a C++20 stand-in whose API is a **strict subset** of
`std::expected`'s, and it may not grow a member `std::expected` lacks -- that
subset is what lets it become a type alias and a deletion if this project ever
moves to C++23 for its own reasons.

→ enforced by `python verify.py error-channel` (return types, named
exemptions, and a floor on DISTINCT error messages -- a non-empty check alone
would pass 341 copies of "invalid input").

Rationale and the measurements behind the four kinds: `specs/01_error_channel.md`.

## The Excel add-in

`cdsmodel.xll` is built **only on Windows, only with the Excel XLL SDK
present.** A session on any other machine builds and tests everything except
the add-in itself, and that is by design, not a limitation to work around.

### Four layers, and what may depend on what

    cdsmodel          the domain library            all platforms
    xl/core           XlValue, dispatch, readers    all platforms
    xl/sdk            XLOPER12 <-> XlValue          Windows + SDK
    xl/interface      cdsmodel.xll                  Windows + SDK
    python            the nanobind extension        all platforms

`python/` is a **peer of `xl/core`, not a layer above it**: both bind the same
`cdsmodel` functions, and neither may depend on the other. It mirrors
`xl/core`'s four argument blocks by hand rather than including them, because
those types carry `XlValue` and the handle registries.
→ enforced by `python verify.py python-layer`.

Dependencies run **downward only**: `cdsmodel` knows nothing about Excel, and
`xl/core` knows nothing about `XLOPER12`. **Logic does not belong in `xl/sdk`
or `xl/interface`** -- those two are the only code CI cannot reach, so anything
decided there is verified on one machine by hand. If a marshalling rule is
worth a test, it goes in `xl/core`.

→ enforced by `python verify.py coverage`, whose `xl/core` floor is high
precisely because that is where the testable logic must live.

### Getting the SDK

The SDK is Microsoft's -- `xlcall.h` carries *"Copyright (c) 1997 - 2010
Microsoft Corporation"* -- and its licence (the Excel 2013 XLL SDK terms)
**forbids redistributing any of its files** (clause 2.b). So it is gitignored
and never committed; this repo is public. Each machine supplies its own copy,
and `python verify.py sdk-absent` enforces that nothing leaks in. Same rule, and
same reason, as the ISDA oracle.

Two further licence points shape how we use it:

- `xlcall.h` and `src/xlcall.cpp` **may not be modified** (clause 2.a.i) -- they
  are staged **byte-for-byte**.
- The **sample** framework (`framewrk.*`, `MemoryManager.*`, `MemoryPool.*`)
  **may** be modified (clause 2.a.ii), but its *executable code* is left exactly
  as Microsoft ships it. The only touch to `framework.c` is its `#include`
  lines: un-inline `xlcall.cpp` (the pristine sample `#include`s it, which would
  double-define every callback) and repoint the framework / memory-manager
  includes at the names we stage them under. The 8-bit `Excel4` helpers, which a
  64-bit link cannot resolve, are **not** compiled out -- they are left intact
  and `xl/sdk/src/excel4_stub.cpp` (ours) defines the two symbols they reference,
  so the workaround sits on our side of the seam, not in Microsoft's file.

**Eight files** are compiled, not four -- `framework.c` pulls in the memory
manager:

    xlcall.h  xlcall.cpp  framework.h  framework.c
    MemoryManager.h  MemoryManager.cpp  MemoryPool.h  MemoryPool.cpp

The easiest way to stage them from your own download:

    python tools/setup_xlsdk.py  /path/to/extracted/Excel2013XLLSDK

It copies `xlcall.*` and the memory manager pristine, repoints `framework.c`'s
`#include` lines (its code untouched), and writes all eight into `xlsdk/`
(gitignored). Then `cmake --preset release` configures and builds the add-in.

You can also populate `xlsdk/` (or `CDSMODEL_XLSDK_DIR`) by hand; CMake searches
recursively for `xlcall.h/.cpp` and `framework.h/.c` by name, and if it finds a
`framework.c` that still `#include`s `xlcall.cpp` inline it compiles `xlcall.cpp`
once rather than twice. Configure prints what it found:

    -- cdsmodel: Excel XLL SDK found
    -- cdsmodel: building cdsmodel.xll

### What this repo builds, and where

Four targets, in dependency order. Nothing below depends on anything above it.

    include/cdsmodel/        cdsmodel              all platforms
      src/                                         the ported library, 163 functions

    xl/core/                 cdsmodel_xl_core      all platforms
      include/cdsmodel/xl/                         XlValue, the registration
      src/                                         table, dispatch, readers,
      tests/                                       the handle registries

    xl/sdk/                  cdsmodel_xl_sdk       Windows + SDK
      include/cdsmodel/xl/oper.hpp                 XLOPER12 <-> XlValue, and
      src/oper.cpp                                 the SDK's own files compiled
      src/excel4_stub.cpp                          in; the stub (ours) defines
                                                   Excel4/Excel4v so the pristine
                                                   sample links unedited

    xl/interface/            cdsmodel_xll          Windows + SDK
      src/exports.cpp                              the five entry points plus
                                                   one wrapper per function;
                                                   SUFFIX ".xll"

Supporting directories:

    xlsdk/                   the Excel SDK -- GITIGNORED, see above
    tools/winshim/           a ten-typedef <windows.h> stand-in, for
                             tools/typecheck_xl.sh only. Not a compatibility
                             layer and nothing links it
    tests/xll/smoke.xlsx     the C8 workbook, self-contained
    specs/reasoning/C8.md    where the Excel result gets recorded

### READ THIS BEFORE THE FIRST BUILD ON WINDOWS

`xl/sdk` and `xl/interface` are the only code CI cannot compile -- they sit
behind a platform guard, so on macOS and Linux they vanish. **Typecheck them
before trusting them:**

    ./tools/typecheck_xl.sh

That compiles both against the REAL `xlcall.h` using a ten-typedef shim in
`tools/winshim` standing in for `<windows.h>`. It needs the SDK in `xlsdk/`
and nothing else. Run it after touching either file.

On Windows, `tools/typecheck_xl.ps1` is the native counterpart: it syntax-checks
the same two files with `clang++` against the **real** `<windows.h>`, so no shim
is involved and it catches strictly more. The full `cmake --preset release`
build is still authoritative; the script is the quick inner-loop gate.

It found four bugs the first time it ran, each of which would have failed a
Windows build: a missing `<windows.h>` include before `xlcall.h`;
`static_cast` where `GetTempMemory`'s `LPSTR` return needs `reinterpret_cast`;
a `CallContext` built with one registry when it needs two; and **thirteen of
sixteen registered functions with no exported wrapper at all.**

**What it still cannot catch**, and what the first real build is therefore for:

1. The `XLOPER12` ABI's actual layout and the calling conventions. The shim
   defines `pascal`, `_cdecl` and `CALLBACK` as nothing, so a convention
   mismatch is invisible to it.
2. Whether Excel accepts what `to_oper` builds -- especially the
   `xltypeMulti` grid path used by `ParSpreads`, `DatesAndRates`,
   `FeeLegFlows`, `DefaultAccrual` and `CDS.CalendarDates`.
3. `xlAutoFree12` does nothing, because this add-in returns only temp memory.
   If Excel crashes on a returned array, that assumption is where to look.

**Fix in `xl/sdk`, not in `xl/core`.** If a fix seems to need a change above
that line, say so rather than making it: the seam is what keeps the logic
testable, and CI cannot see anything on the Windows side of it.

### Producing and recording the add-in's output

An XLL only truly runs inside Excel, interactively, which is not an oracle. So
the result is recorded rather than asserted, and the workbook reports its own
verdict:

    cmake --preset release
    cmake --build --preset release --target cdsmodel_xll
    python tools/gen_xll_workbook.py          # writes tests/xll/smoke.xlsx

Or drive it: `pwsh tools/run_c8.ps1` registers the add-in in 64-bit Excel over
COM, sweeps every function name for `#NAME?`, recalculates the workbook and
prints the Verdict and Forms columns, exiting non-zero if anything fails to
register or any row is not PASS. That is the same Excel calculation engine and
the same add-in; scripting it only makes the evidence reproducible.

By hand works too: load `cdsmodel.xll` in 64-bit Excel, open the workbook, let
it recalculate. Every row carries the value the C++ library computed and a
verdict column comparing it to what Excel got.

**Record the result in `specs/reasoning/C8.md`** with the date, the Excel
version, and the verdict column's contents. That file is the only evidence
tier 1 has for the add-in, so an empty or stale one means the add-in is
unverified -- say so plainly rather than implying otherwise.

### Recording a run

`specs/reasoning/C8.md` is the only evidence tier 1 has for the add-in, so
**every run is appended, not overwritten.** Two are recorded: 2026-11-11 and
2026-11-14. One run is evidence; a series is a habit, and the file is where
the difference shows.

A session starting on Windows has no memory of the last one, so anything it
discovers belongs in this file or in a spec rather than in a transcript. The
rule that matters most on a cold start: **do not mark a criterion passing
without the oracle result pasted into the spec's Progress table.** "It built"
is not a result.

### What the add-in exposes

`CdsPrice` is registered **twice**: in its original sixteen-argument form, so
existing workbooks keep working, and as `CDS.Price`, taking five labelled
two-column ranges. Both read into the same four structs and then run one code
path, so they cannot disagree -- and a test compares them bitwise.

**Every wide upstream function now has a grouped form too.** The seven that were
once flat-only -- `UpfrontFlat` and `ParSpreadFlat` at seventeen arguments,
`CleanSpreadCurveBuild` at sixteen, `ParSpreads` at thirteen, `IRZeroCurveBuild`
at twelve, `DefaultAccrual` at eleven, `FeeLegFlows` at nine -- each gained a
`CDS.`-prefixed twin taking labelled ranges, reading through the same
`read_block` the four reader structs expose. The flat forms stay registered so
existing workbooks keep working.

**Twenty-four functions are registered**, covering all twelve upstream exposed:

    pricing     CdsPrice (16 args)   CDS.Price (5 args, grouped)
                ParSpreads   CDS.ParSpreads
                UpfrontFlat   CDS.Upfront
                ParSpreadFlat   CDS.ParSpreadFlat
    cash flows  DefaultAccrual   CDS.DefaultAccrual
                FeeLegFlows   CDS.FeeLegFlows
    curves      IRZeroCurveMake   IRZeroCurveBuild   CDS.IRZeroCurve
                CleanSpreadCurveBuild   CDS.SpreadCurve
                DiscountFactor   DatesAndRates
    calendars   LoadHolidays   CDS.Calendar   CDS.CalendarFromFile
                CDS.CalendarDates
    other       CdsModelVersion

Eight of those return a **grid** rather than a number -- `ParSpreads`,
`DatesAndRates`, `FeeLegFlows`, `DefaultAccrual`, `CDS.CalendarDates` and the
grouped twins `CDS.ParSpreads`, `CDS.FeeLegFlows`, `CDS.DefaultAccrual` -- the
newest and least-tested part of the Windows side.

**A failed call returns the library's own message as text in the cell.** Not
`#VALUE!`, and not a log entry a user has to go and read -- upstream needed six
of its eighteen functions to manage that log, and all six are gone.

Adding a function means adding a row to the table in `xl/core/src/functions.cpp`
and a three-line wrapper in `xl/interface/src/exports.cpp`. Registration walks
the table, so the name, type codes, parameter names, help and category cannot
drift apart.

→ enforced by `python verify.py coverage` and the `Registration` tests, which
assert the arity the type codes declare equals the documented parameter count.

## Formatting

`clang-format` covers our own C++ only: `include/`, `src/`, `tests/`, `tools/`
and `python/src/`. Upstream C is never reformatted — it is not ours, and a
reformatted oracle is a modified oracle. The Excel SDK is excluded for the same
reason, and `xl/` is excluded because it was not in scope when the gate was
written.
