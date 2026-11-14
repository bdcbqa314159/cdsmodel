# cdsmodel

A C++20 port of the **ISDA CDS Standard Model** (version 1.8.3), with Python
bindings and a cross-platform spreadsheet interface — replacing a 2009 C
codebase that only ever shipped a Windows-only Excel XLL.

> This application is based on the ISDA CDS Standard Model (version 1.8.3),
> developed and supported in collaboration with Markit Group Ltd.

ISDA and Markit are not affiliated with this project and do not endorse it.
See [NOTICE.md](NOTICE.md) for the full attribution and licence position.

📋 **[Defect report](https://bdcbqa314159.github.io/cdsmodel/)** — **forty-one**
defects found in the original library while porting it, with sanitizer traces,
reproducers and the fix status of each. One returns a **negative cash amount**
on a defaulted-CDS settlement and reports success. Another accepts
`03/15/aaaa` as **15 March 2000**.

## Why

The official model is the market standard for CDS pricing and the reference
implementation every participant settles against. It is also 21,000 lines of
C from 2009: four hand-written makefiles per platform, no test suite, a
process-wide mutable holiday cache with external linkage, and a distribution
whose only user interface is an Excel add-in that cannot run on macOS or
Linux.

This port keeps the numbers and modernises everything around them.

## Obtaining the reference implementation

**This repository does not contain the upstream ISDA C sources, by design.**

They are distributed only from [cdsmodel.com](https://www.cdsmodel.com/) after
you accept the ISDA CDS Standard Model Public License and supply an email
address. Redistributing them here would route around that gate, so each
developer fetches their own copy:

1. Download `isda_cds_model_c_v1.8.3` from cdsmodel.com, accepting the terms.
2. Unpack it into the repository root, or anywhere you like.
3. If you put it elsewhere, point the build at it:
   `cmake --preset debug -DCDSMODEL_UPSTREAM_DIR=/path/to/isda_cds_model_c_v1.8.3`

The build works either way — see below for what changes.

## How correctness is established

Two tiers, because the oracle cannot be redistributed.

**Tier 1 — recorded vectors. Always runs, including in CI.**
Reference answers were computed from the upstream oracle and committed to
`tests/golden/`. Section 4(b) of the upstream licence explicitly exempts
"data derived from calculations that have been attained by using the ISDA CDS
Standard Model" from its distribution terms, so these numbers are free to
ship even though the code that produced them is not. `tools/` holds the
generators, so the vectors are reproducible rather than magic numbers.

**Tier 2 — live differential. Runs only where a local upstream copy exists.**
The pristine C is compiled, untouched, into `cds_reference` and linked into
the **same test binary** as the port. Tests call both implementations side by
side and compare in-process:

```cpp
const auto ported    = cdsmodel::bracket(view, desired);
const auto reference = reference_bracket(view, desired);   // the 2009 C, live
ASSERT_EQ(ported->lo, reference->lo);
```

Here the oracle is not a recorded value that can go stale — it is the
original implementation, executing. A module counts as ported only once it is
differentially equal to its ancestor across a swept input domain, and the
sweep is then recorded into tier 1 so CI inherits the guarantee.

```
with upstream present: 827 tests  (tier 1 + tier 2 + end-to-end example)
without upstream:      504 tests  (tier 1 only, replaying 44 recorded vector sets)
```

Both configurations also run clean under AddressSanitizer and
UndefinedBehaviorSanitizer (`--preset asan`). That combination has found
**forty-one** genuine defects in the original model, including two separate stack
buffer overflows reachable from string input, an unparseable year silently
replaced with 2000, an interval type whose meaning changes with letter case,
and an adjustment that never returns — see
[the full report](https://bdcbqa314159.github.io/cdsmodel/) or "Upstream defects
found" in [docs/port-notes.md](docs/port-notes.md).

Thirty-nine are reproduced faithfully, because a port that silently fixes
things is no longer a port. **Two** are corrected, and in both cases the
reasoning is written down next to the code: one where upstream's own
implementation contradicts itself eleven lines apart, and one where reproducing
the behaviour would mean reproducing a silently wrong holiday calendar.

## Build

Requires CMake >= 3.28 and a C++20 compiler. GoogleTest is fetched
automatically.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (non-Windows). Configure output states
plainly whether the oracle was found.

## Status

| component | state |
|---|---|
| CMake build, 3-OS CI, differential test harness | working |
| **the port** | **complete** — 168 upstream functions, verified against the original under **two** compilers |
| ISDA's published example | **reproduced**, on three platforms, with no upstream copy present |
| **Excel add-in** (`cdsmodel.xll`) | **complete and run in Excel** — 24 functions, 64-bit, both signature forms agreeing bitwise |
| **Python package** (`cdsmodel`, nanobind) | **complete** — 40 names, wheels for Linux, macOS and Windows |
| Cross-platform spreadsheet interface | not started |

**895 tests pass with a local upstream copy as the oracle, 572 without one**,
plus 577 tests over the Python package. 41 defects in the original library are
catalogued, each present at the site, in `docs/port-notes.md`, and in a test
that asserts it from both sides.

The three upfront charges printed by upstream's own example program are
reproduced to the precision they are printed at — the only externally
published numbers in the distribution, and the only reference in this project
that is not either upstream's observed behaviour or a property argued from
first principles.

### What verification means here, and where it stops

The differential suites compile the original C into the same binary and
compare the port against it with `EXPECT_EQ` on raw doubles — bit-exact, not
approximate. That needs the upstream sources, which are licence-gated and
never committed, so **those suites run locally and not in CI**. Every sweep is
therefore recorded into vectors under `tests/golden/`, which CI replays with
no oracle present; that is the 572.

Two things are deliberately not claimed:

- **The Excel add-in is evidenced by a recorded run, not by an assertion.** An
  XLL only truly runs inside Excel, which is not an oracle. Two runs are
  recorded in `specs/reasoning/C8.md` with dates, Excel version and verdicts.
- **The Python package does not re-test the numerics.** The C++ suites own
  that, against an oracle Python cannot see. What the Python suite proves is
  that the numbers cross the boundary unchanged: 216 prices and 216 par
  spreads compared **bit for bit** against values emitted by a C++ program
  linking the same library.

## Installing the Python package

    pip install cdsmodel          # once published

One `cp312-abi3` wheel per platform, covering Python 3.12 and later. Building
from the source distribution needs no network and no Excel SDK:

    pip install --no-index --no-build-isolation cdsmodel-*.tar.gz

The migration plan, the measured legacy-pattern inventory, and every
deliberate deviation from upstream behaviour are documented in
[docs/port-notes.md](docs/port-notes.md).
[docs/journal.md](docs/journal.md) records how the port was made — the
decisions, the alternatives rejected, and what went wrong — organised by
decision rather than by date. Defects found in the original
library are catalogued in [docs/defect-report.md](docs/defect-report.md), and
published at
[bdcbqa314159.github.io/cdsmodel](https://bdcbqa314159.github.io/cdsmodel/).

## Layout

```
include/cdsmodel/          public headers for the port
src/                       port implementation
tests/                     GoogleTest suite (tier 1 always, tier 2 with oracle)
tests/golden/              recorded reference vectors -- generated, committed
tools/                     generators that produce tests/golden/ from the oracle
specs/                     the contracts this was built against
docs/port-notes.md         migration analysis and plan

xl/core/                   Excel logic -- all platforms, fully tested
xl/sdk/                    XLOPER12 <-> XlValue -- Windows + SDK only
xl/interface/              cdsmodel.xll itself -- Windows + SDK only
tests/xll/smoke.xlsx       the Excel smoke workbook, self-contained

isda_cds_model_c_v1.8.3/   your own upstream copy -- GITIGNORED, never committed
xlsdk/                     your own Excel XLL SDK copy -- GITIGNORED, same reason
```

The `xl/` split exists so that everything deciding anything is testable off
Windows. `xl/core` holds the marshalling rules, the registration table, the
dispatchers and the handle registries, and CI runs its tests on three
platforms. Only the thin `XLOPER12` adapter and the five Excel entry points
need Windows, which is also the only code CI cannot compile -- so
`tools/typecheck_xl.sh` checks it against the real `xlcall.h` using a
ten-typedef shim.

## Building the Excel add-in

Needs Windows, MSVC, and the Microsoft Excel XLL SDK. Four files are read --
`xlcall.h`, `xlcall.cpp`, `framework.h`, `framework.c` -- and **the layout
does not matter**: CMake searches recursively, so the extracted SDK bundle
works unchanged.

```
xlsdk/Excel2013XLLSDK/INCLUDE/xlcall.h     <- or just the four files flat
xlsdk/Excel2013XLLSDK/SRC/xlcall.cpp          in xlsdk/; both work
...
```

There is no import library to link: `xlcall.cpp` resolves `MdCallBack12` from
the running Excel process, so the bundle's `LIB/` and `SAMPLES/` are unused
and bitness is the compiler's decision.

```
cmake --preset release
cmake --build --preset release --target cdsmodel_xll
python tools/gen_xll_workbook.py        # writes tests/xll/smoke.xlsx
```

Then load `cdsmodel.xll` in 64-bit Excel and open the workbook: it builds its
own curves and reports PASS/FAIL per row against values the C++ library
computed. See `CLAUDE.md` for what to expect on a first build, and
`specs/reasoning/C8.md` for where to record the result.

## Licence

The port and its tooling are MIT ([LICENSE](LICENSE)). The upstream sources
under `isda_cds_model_c_v1.8.3/` remain under the ISDA CDS Standard Model
Public License, which permits derivative works under a licence of your choice
(§2(d), §2(e)) subject to the attribution above.
