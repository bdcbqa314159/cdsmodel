# cdsmodel

A C++20 port of the **ISDA CDS Standard Model** (version 1.8.3), with Python
bindings and a cross-platform spreadsheet interface — replacing a 2009 C
codebase that only ever shipped a Windows-only Excel XLL.

> This application is based on the ISDA CDS Standard Model (version 1.8.3),
> developed and supported in collaboration with Markit Group Ltd.

ISDA and Markit are not affiliated with this project and do not endorse it.
See [NOTICE.md](NOTICE.md) for the full attribution and licence position.

📋 **[Defect report](https://bdcbqa314159.github.io/cdsmodel/)** — five defects
found in the original library while porting it, with sanitizer traces and the
fix status of each.

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
with upstream present: 219 tests  (tier 1 + tier 2 + end-to-end example)
without upstream:      146 tests  (tier 1 only, replaying 15,982 recorded cases)
```

Both configurations also run clean under AddressSanitizer and
UndefinedBehaviorSanitizer (`--preset asan`). That combination has found seven
genuine defects in the original model so far, including a stack buffer
overflow reachable from string input, an interval type whose meaning changes
with letter case, and an adjustment that never returns — see "Upstream defects
found" in [docs/port-notes.md](docs/port-notes.md).

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
| C++20 port | layers 1-4 complete; vertical slice in progress — zero curve and integration timeline done |
| Python bindings (nanobind) | not started |
| Cross-platform spreadsheet interface | not started |

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
docs/port-notes.md         migration analysis and plan
isda_cds_model_c_v1.8.3/   your own upstream copy -- GITIGNORED, never committed
```

## Licence

The port and its tooling are MIT ([LICENSE](LICENSE)). The upstream sources
under `isda_cds_model_c_v1.8.3/` remain under the ISDA CDS Standard Model
Public License, which permits derivative works under a licence of your choice
(§2(d), §2(e)) subject to the attribution above.
