# Artifact #3 — the XLL foundation (ratified)

**Status:** ratified 2026-11-08 (Bernardo Cohen)
**Governing purpose:** Get a C++20 add-in loading in modern 64-bit Excel with
one function working end to end, so the pipe is proven before the 12-function
surface is designed on top of it (artifact #4).
**Inherits:** THE BAR — `CLAUDE.md`

## Reframe

The library is finished and packaged: 163/163 functions, 761 tests, installable
since artifact #02. **What is missing is the product** -- the XLL is how a desk
used this model, so the port is currently only half a replacement.

## Why this is a replacement and not an upgrade

| measured in `isda_cds_model_c_v1.8.3/excel/` | value |
|---|---|
| build configurations | `win32debug`, `win32release` — **no x64** |
| `XLOPER` uses / `XLOPER12` uses | **57 / 0** |
| registered functions | 18, of which **6 manage a global error log** |
| worst signatures | `UpfrontFlat`, `ParSpreadFlat` — **17 positional args** |
| object cache | process-wide `static TObject *`, `stricmp` string keys |
| `setjmp.h` | included; `setjmp`/`longjmp` never called |

**64-bit Excel cannot load a 32-bit XLL**, and `XLOPER` is the pre-2007 API:
no Unicode, 255-char strings, 65,536-row grids. No in-place upgrade exists --
only a new add-in that keeps the function names.

**Two-thirds of the parameter surface is repetition**, and that repeated block
is the conventions struct: upstream had nowhere to put it, with no aggregate in
C and only positional cells in a grid. The 17-argument signature is the absence
of a type. Fixing it is artifact #4; this slice builds the pipe it runs on.

## The SDK: required, never committed

The build uses the **official Microsoft Excel XLL SDK**. Hand-writing the
`XLOPER12` ABI was considered and rejected (D1): it trades a setup step for a
byte-exact struct layout whose failure mode is a crash inside Excel, and the
standard is the standard.

The SDK is Microsoft's, so it is gitignored exactly like the ISDA oracle, for
the same reason, and enforced the same way (`verify.py sdk-absent`). It goes in
`xlsdk/`, or anywhere `CDSMODEL_XLSDK_DIR` points. CMake searches for the four
files rather than assuming a layout, and names any that are missing.

**Same two-tier shape the port already has:** tier 2 needs the licence-gated
thing locally, tier 1 runs in CI without it.

## Architecture

Taken from `cpp_fdm_world`, which already solved this: the domain library knows
nothing of Excel, the SDK is wrapped in its own library, and the add-in is a
third layer on top. One change, and it is the reason the Acceptance table below
has oracles at all.

| layer | target | platform | role |
|---|---|---|---|
| `cdsmodel` | `cdsmodel` | all | the domain library, untouched by this slice |
| `xl/core` | `cdsmodel_xl_core` | **all** | `XlValue`, the registration table, dispatch, the labelled-range reader, `Expected` -> cell text |
| `xl/sdk` | `cdsmodel_xl_sdk` | Windows | `XLOPER12` <-> `XlValue`, SDK sources, temp memory |
| `xl/interface` | `cdsmodel_xll` | Windows | the five entry points; `SUFFIX ".xll"` |

**The seam is `XlValue`, and that is the one deviation from `cpp_fdm_world`.**
There, the whole Excel layer sits behind `#ifdef WIN32` -- right for that
project, wrong here, because it would put the marshalling, the dispatch table
and the labelled-range reader on the only machine that cannot run CI. Moving
the boundary one layer down means `xl/core` is ordinary C++ that compiles and
tests on every platform in the matrix, and only a thin adapter needs Windows.

## MUST — in scope, full discipline

1. The four layers above. `cdsmodel.xll` is a `SHARED` library with
   `SUFFIX ".xll"` (as in `cpp_fdm_world`), built only when the SDK is present
   and only on Windows.
2. The SDK's **four source files** are compiled from `CDSMODEL_XLSDK_DIR`
   (`xlcall.h/.cpp`, `framework.h/.c`) -- no import library, because
   `xlcall.cpp` resolves `MdCallBack12` from the running Excel process, so
   bitness is the compiler's decision and cannot be mismatched. The SDK is
   never committed: `verify.py sdk-absent`.
3. The five entry points, and **nothing else that touches Excel**.
4. A pure marshalling layer: `XLOPER12` <-> `Date`, `double`, `std::wstring`,
   2-D ranges, the missing/empty argument, and `Expected<T>` -> error text in
   the cell.
5. A **declarative** registration table carrying name, type codes, parameter
   names, per-parameter help and category. Upstream's `TFuncDef` had this right
   and `cpp_fdm_world` hand-writes each `Excel12f(xlfRegister, 0, 11, ...)`
   call instead -- fine for six functions, copy-paste for twenty-four. The
   table is the one genuinely good idea in the original XLL.
6. **One** function end to end -- `CdsPrice`, the original 16-argument form --
   proving registration, marshalling and dispatch work against real Excel
   before 11 more are built on the same pipe.
7. A handle registry for curves with explicit lifetime and case-exact keys.

## NOT-NOW — real, but deferred this cycle

- **The other 11 functions and the redesigned signatures -- artifact #4.**
  That is the design work: both registered forms, the labelled-range
  conventions block (D7), and holding the two forms to one answer. It is
  deliberately NOT a corner of the plumbing slice. The evidence it starts
  from is already measured here: 111 arguments over 8 functions, `Holidays` in
  8 of 8, four more conventions in 7 of 8.
- Splitting the library's `cdsmodel::Cds` type itself. A library API change,
  later again.
- ARM64 Excel; async/threaded registration; Ribbon UI; `IRTDServer`.
- `nanobind` -> PyPI, then any non-Excel host. Later, in that order.

## NEVER — out of scope

- **Committing `xlcall.h` / `xlcall32.lib`.** Microsoft's, and the repo is
  public. Enforced, not trusted: `verify.py sdk-absent`.
- **A 32-bit build, or any `XLOPER` (non-12) path.** No Excel 97 compatibility
  is wanted; supporting it doubles the marshalling surface to serve versions
  out of support.
- **A process-wide mutable object cache.** The thing 41 files of porting
  removed; reintroducing it at the top layer would undo that.
- Reproducing the global error log or the 6 functions that manage it.
- `setjmp`/`longjmp` anywhere. It skips destructors; that is the whole point
  of using C++20 here.

## Acceptance

An XLL only truly runs inside Excel, interactively, on Windows -- which is not
an oracle. So everything except the five entry points is ordinary C++ under
`ctest`, exercised through a **stub host** implementing the callback. A fake
Excel is a real oracle; opening the real one and looking is not.

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| C1 | THE XLL SHALL build for x64 under MSVC with `/W4 /WX` when the SDK is present; the SDK probe SHALL name every missing file when it is incomplete, on every platform | local build with the SDK; the probe's three branches (absent, partial, complete) are exercised in CI, which is why it runs even where no XLL can be built | 3 |
| C0 | `xl/core` SHALL compile and its tests pass on Windows, macOS and Linux with no SDK and no Excel | `ctest` in the existing matrix -- this is what D8 buys | 3 |
| C2 | `XLOPER12` round-trips every supported type without loss: `Date`, `double`, `std::string` (including >255 chars and non-ASCII), 2-D ranges, and the empty/missing argument | unit tests against the stub host; the >255-char and Unicode cases are exactly what `XLOPER` could not do | — |
| C3 | WHEN a library call returns an error, THE cell SHALL receive that error's own message as text, not `#VALUE!` and not a log entry | unit test asserting the string reaching the stub host equals `Expected`'s message | — |
| C4 | THE registration table SHALL describe every exported function, and its arity SHALL equal the dispatcher's | gate: table vs dispatch signatures compared mechanically, as `verify.py claims` does for the port | — |
| C5 | `CdsPrice` dispatches to the library and returns the value the equivalent `ctest` case pins | one test through the stub host, compared against an existing differential expectation | 3 |
| C6 | A curve handle SHALL resolve to the curve it named, and a stale handle SHALL report so rather than resolving to a different object | unit test: store, overwrite, resolve the old handle, require an error | — |
| C7 | IF the add-in is unloaded and reloaded, THEN no handle from the previous session SHALL resolve | `xlAutoClose` then `xlAutoOpen` against the stub host, asserting the registry is empty | — |
| C8 | Loading the real add-in in Excel registers all 12 functions and prices one trade | **UNCHECKABLE in CI** -- needs Excel on Windows, interactively. Partial evidence: C1 proves it links, C4 that the table is complete, C5 that dispatch is right. A manual smoke-test checklist is committed and its result recorded by hand | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D8 | `XlValue` splits the logic from the `XLOPER12` adapter | follow `cpp_fdm_world` and guard the whole Excel layer with `#ifdef WIN32` | Behind the guard, every decision the add-in makes would be verified on one machine by hand -- the situation `CLAUDE.md`'s two-tier contract exists to prevent. Costs one adapter, buys CI coverage | `XlValue` cannot carry some `XLOPER12` detail |
| D9 | Headers declare, sources define; no `using namespace std;` in a header | follow `cpp_fdm_world`'s `xlOper.h` | It works there because one TU includes it; across several TUs it is an ODR violation whose symptom is a duplicate-symbol link error pointing nowhere useful | -- |
| D10 | Explicit `XCHAR` conversion, not `mbstowcs` | `mbstowcs`, as `cpp_fdm_world` uses | Locale-dependent: the same workbook could marshal a path differently on two machines | -- |
| D1 | Use the official Excel XLL SDK, gitignored at `xlsdk/` | hand-write the `XLOPER12` ABI; vendor the SDK | **User's call, 2026-11-08.** Hand-writing it trades a setup step for a byte-exact layout whose failure mode is a crash in Excel. Upstream's vendored header is Excel 97-era anyway (`XLOPER12`: 0 matches, `far pascal`), so there was nothing to inherit | Microsoft stops shipping it |
| D2 | `XLOPER12` only; x64 only | support `XLOPER` and 32-bit too | Supporting the legacy pair doubles the marshalling surface to serve Excel versions out of support. The original's limits -- 255-char strings, 65k rows -- came from exactly that choice | someone needs 32-bit Excel |
| D3 | Five thin entry points; everything else pure and testable | a conventional XLL where logic lives in the `xlAuto*` functions | This is what makes C2-C7 runnable without Excel. It is the only reason this slice has an oracle at all | — |
| D4 | The 6 error-log functions are dropped | reproduce them over a new log | They exist because a failed call could only say FAILURE. `Expected<T>` carries the reason to the cell that asked | a user needs an audit trail across calls |
| D5 | Function names kept identical to the original | rename for clarity | Existing workbooks reference `CdsPrice`, `ParSpreads`. Keeping names is most of what "a complete replacement" means; the arguments change, the names must not | — |

## Open questions before ratification

1. ~~Does the 17-argument signature change in this slice?~~ **Withdrawn --
   it was a false dichotomy.** I posed it as compatibility versus usability;
   an XLL registers a table and can register both forms, so the answer is
   both (D6). Recorded because the question shaped an earlier draft of this
   spec and the reasoning should not vanish.
2. ~~What is a conventions handle?~~ **Resolved 2026-11-08: a labelled
   two-column range, read in place (D7).** No handle, no registry entry, no
   lifetime.
3. Non-blocking -- the SDK release to target. **Assumption: the Excel 2013
   XLL SDK**, the current one, which carries `XLOPER12` and an x64
   `xlcall32.lib`. The probe searches rather than assumes, so a different
   release that still provides those two files works unchanged.
4. Non-blocking -- `cdsmodel.xll` vs `cdsmodel64.xll`. **Assumption:
   `cdsmodel.xll`**, since no 32-bit build exists to disambiguate from.
5. Non-blocking -- C8's recorded result. **Assumption: `specs/reasoning/C8.md`**,
   with the date, Excel version, and the workbook's own verdict column.

## Progress

| ID | Status | Oracle result | Note |
|----|--------|---------------|------|
| C0 | todo | | the point of D8 |
| C1 | todo | | probe branches, all three |
| C2 | todo | | the ABI risk D1 takes on |
| C3 | todo | | artifact #01 reaching the cell |
| C4 | todo | | |
| C5 | todo | | |
| C6 | todo | | |
| C7 | todo | | |
| C8 | todo | | UNCHECKABLE in CI, by nature |
