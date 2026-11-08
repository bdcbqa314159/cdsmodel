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

Dependencies run **downward only**: `cdsmodel` knows nothing about Excel, and
`xl/core` knows nothing about `XLOPER12`. **Logic does not belong in `xl/sdk`
or `xl/interface`** -- those two are the only code CI cannot reach, so anything
decided there is verified on one machine by hand. If a marshalling rule is
worth a test, it goes in `xl/core`.

→ enforced by `python verify.py coverage`, whose `xl/core` floor is high
precisely because that is where the testable logic must live.

### Getting the SDK

The four files already exist on this machine, in
`~/work/throwAwayAfterUse/cpp_fdm_world/libraries/excel_sdk_api/` -- that copy
is the modern SDK (15 `XLOPER12`-era matches, no 16-bit `far`/`pascal`
leftovers). Copying them into `xlsdk/` is the whole setup; there is nothing to
download.

The SDK is **Microsoft's**, so it is gitignored and never committed -- the same
rule as the ISDA oracle, for the same reason, and this repo is public.

1. Download the **Excel 2013 XLL SDK** from Microsoft.
2. Put it in `xlsdk/`, or point `CDSMODEL_XLSDK_DIR` anywhere else.
3. Four files are needed, and CMake searches for them rather than assuming a
   layout: `xlcall.h`, `xlcall.cpp`, `framework.h`, `framework.c`.

There is **no import library**: `xlcall.cpp` resolves the callback with
`GetProcAddress(hmodule, "MdCallBack12")` from the running Excel process, so
bitness is the compiler's decision and cannot be mismatched. Build **x64** --
64-bit Excel cannot load a 32-bit add-in, which is why upstream's `win32`-only
XLL is unusable rather than merely old.

Configure reports what it found, or names each missing file:

    -- cdsmodel: Excel XLL SDK found
    -- cdsmodel: building cdsmodel.xll

→ enforced by `python verify.py sdk-absent`.

### READ THIS BEFORE THE FIRST BUILD ON WINDOWS

`xl/sdk` and `xl/interface` are the only code CI cannot compile -- they sit
behind a platform guard, so on macOS and Linux they vanish. **Typecheck them
before trusting them:**

    ./tools/typecheck_xl.sh

That compiles both against the REAL `xlcall.h` using a ten-typedef shim in
`tools/winshim` standing in for `<windows.h>`. It needs the SDK in `xlsdk/`
and nothing else. Run it after touching either file.

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

Then, by hand: load `cdsmodel.xll` in 64-bit Excel, open the workbook, let it
recalculate. Every row carries the value the C++ library computed and a verdict
column comparing it to what Excel got.

**Record the result in `specs/reasoning/C8.md`** with the date, the Excel
version, and the verdict column's contents. That file is the only evidence
tier 1 has for the add-in, so an empty or stale one means the add-in is
unverified -- say so plainly rather than implying otherwise.

### What the add-in exposes

Every function is registered **twice**: under its original name and argument
list, so existing workbooks keep working, and under a grouped form taking
labelled two-column ranges. Both read into the same four structs and then run
one code path, so they cannot disagree -- and a test compares them bitwise.

**Sixteen functions are registered**, covering all twelve upstream exposed:

    pricing     CdsPrice (16 args)   CDS.Price (5 args, grouped)
                ParSpreads   UpfrontFlat   ParSpreadFlat
    cash flows  DefaultAccrual   FeeLegFlows
    curves      IRZeroCurveMake   IRZeroCurveBuild   CleanSpreadCurveBuild
                DiscountFactor   DatesAndRates
    calendars   LoadHolidays   CDS.Calendar   CDS.CalendarFromFile
                CDS.CalendarDates
    other       CdsModelVersion

Five of those return a **grid** rather than a number, which is the newest and
least-tested part of the Windows side.

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

`clang-format` covers `include/`, `src/` and `tests/` only. Upstream C is never
reformatted — it is not ours, and a reformatted oracle is a modified oracle.
