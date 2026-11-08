# Artifact #2 — consumability (ratified)

**Status:** ratified 2026-11-08 (Bernardo Cohen)
**Governing purpose:** Make the library installable, discoverable and
includable, so nanobind and Cellwright consume a package rather than a source
tree.
**Inherits:** THE BAR — `CLAUDE.md`

## Reframe

**Nothing is broken.** 163/163 functions, 761 tests with the oracle, 445
without, eight gates green on three OSes. The library builds and is correct;
it simply cannot be *installed*. `CMakeLists.txt` already carries
`$<INSTALL_INTERFACE:include>` and a `cdsmodel::cdsmodel` alias, so the target
was written to be exported and then never was. This slice finishes a sentence
the build file started.

## The measurement

| | |
|---|---|
| `install(...)` rules in `CMakeLists.txt` | **0** |
| package config / version files | **0** |
| public headers | **45** |
| umbrella header | **none** — a consumer includes up to 45 things |
| per-header self-test (compiles each header alone) | already present, globbed |

The self-test glob matters: it means a new header is checked in isolation
automatically, and an umbrella header will be too, without new machinery.

## MUST — in scope, full discipline

1. `install(TARGETS ... EXPORT)` + `install(EXPORT ...)` + `install(DIRECTORY include/)`.
2. `cdsmodelConfig.cmake` via `configure_package_config_file`, and
   `cdsmodelConfigVersion.cmake` via `write_basic_package_version_file`.
3. `include/cdsmodel/cdsmodel.hpp` — one umbrella including every public header.
4. A gate that fails if the umbrella omits a public header, so it cannot drift.
5. Tests and the oracle stay gated on `PROJECT_IS_TOP_LEVEL`, so a consumer
   pulling this in by `FetchContent` builds neither.

## NOT-NOW — real, but deferred this cycle

- **Splitting `Cds` into conventions + contract** — see D4. It was on my own
  deferral list and the measurement does not support it yet.
- Giving `Cds` an enforced invariant — artifact #3, see D4.
- nanobind; Cellwright; a shared-library build; `SOVERSION`; CPack.
- Installing the oracle or anything derived from it. It is gitignored and
  licence-gated; an install tree must not carry it.

## NEVER — out of scope

- **Installing `isda_cds_model_c_v1.8.3/` or linking `cds_reference` into the
  exported target.** Rule 2 of `CLAUDE.md`: redistribution routes around the
  licence gate. The export set contains `cdsmodel` only.
- A header-only INTERFACE target. `src/version.cpp` is real, and a consumer
  should link one thing rather than discover it needs a TU.
- Vendoring GoogleTest into the install tree.

## Acceptance

The oracle ladder in `verification.md` is written for numerical results and has
no rung for "is this installable". The strongest available check is to **act as
the consumer**: install into a throwaway prefix and build a fresh project
against it, from outside the source tree. That is what B2 and B3 do, and it is
why they are the load-bearing criteria rather than inspection of the CMake.

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| B1 | `cmake --install` into an empty prefix produces the 45 headers, the static library, and both package files | script asserts each path exists; header count asserted exactly, not as a floor | — |
| B2 | A project outside the tree with only `find_package(cdsmodel REQUIRED)` + `target_link_libraries(app cdsmodel::cdsmodel)` configures, builds, and **runs**, pricing a CDS to a known value | temp-dir consumer build; compares against a value this suite already pins | consumer build |
| B3 | The same consumer works via `FetchContent_Declare(cdsmodel SOURCE_DIR ...)` without building our tests or needing the oracle | temp-dir consumer build with `CDSMODEL_UPSTREAM_DIR=/nonexistent`; assert no test target was built | consumer build |
| B4 | `find_package(cdsmodel 0.1.0)` succeeds and `find_package(cdsmodel 99.0)` fails | two configure runs, opposite outcomes asserted | — |
| B5 | A TU including **only** `<cdsmodel/cdsmodel.hpp>` can price a CDS and compiles under `-Wall -Wextra -Wpedantic` | compiled in B2's consumer, which includes nothing else | consumer build |
| B6 | THE umbrella SHALL include every public header; IF one is missing, THEN the build SHALL fail | `verify.py umbrella` diffs the include list against the directory | — |
| B7 | The install tree contains nothing derived from the upstream oracle | gate greps the staged tree for upstream filenames and for `isda_cds_model` | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D1 | Hand-written umbrella, checked by a gate | generated at configure time; no umbrella | A generated header is not greppable and does not appear in the source tree a reader browses. A gate gives the same guarantee without hiding the file | the header list changes often enough that maintaining it by hand annoys |
| D2 | Static library only | also build shared | Nothing asks for a shared build yet, and a shared build brings `SOVERSION` and symbol-visibility decisions that want their own slice | nanobind or Cellwright needs it |
| D3 | The consumer test lives in a script run by CI, not in `ctest` | a `ctest` fixture that installs and builds | Installing from inside the build that is being tested confuses the two trees. A separate script with its own prefix keeps "did it install" distinct from "does it work" | — |
| D4 | **`Cds` is not split in this slice, and the split is not the right next change** | split into conventions + contract as originally planned | My own deferral note said "split `Cds` into conventions + contract". Measured first: all 9 fields vary heavily across the suite (`day_count` 47 times, `coupon_interval` 31, `bad_day` 27), so the "conventions are shared, the contract varies" premise is not observable here. It may well be true of a *spreadsheet* caller — but that caller does not exist, and I have been wrong this week about building for absent consumers. What the measurement DID surface is a real defect: `Cds` is a raw aggregate with **no invariant**, so `end < start` is representable, and the same precondition is re-checked in `cds.hpp:145`, `cds.hpp:153` and `defaulted.hpp:114`. That duplication is exactly what `verify.py claims` exists to catch. The right next change is a validating factory returning `Expected<Cds>`, as artifact #3 | artifact #3 is written; or nanobind's API shows the split is needed |

## Open questions before ratification

1. Non-blocking — install prefix layout: `lib/cmake/cdsmodel/` is the
   conventional home for the config files. **Assumption: use it.**
2. Non-blocking — whether B2's consumer lives in `tests/consumer/` committed,
   or is written by the script into a temp dir. **Assumption: committed under
   `tests/consumer/`**, so a human can run it by hand when CI disagrees with
   their machine.

## Progress

| ID | Status | Oracle result | Note |
|----|--------|---------------|------|
| B1 | todo | | |
| B2 | todo | | the load-bearing one |
| B3 | todo | | |
| B4 | todo | | |
| B5 | todo | | |
| B6 | todo | | needs `verify.py umbrella` |
| B7 | todo | | rule 2 of CLAUDE.md, mechanically |
