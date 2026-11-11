# Artifact #5 — the Python package (draft)

**Status:** draft
**Licence position:** settled — MIT, per `NOTICE.md`; the ISDA licence permits
derivative works under a licence of the author's choice. Not an open question.
**Governing purpose:** Publish `cdsmodel` on PyPI as a typed, wheel-installed
package, exposing the surface a desk actually used for seventeen years.
**Inherits:** THE BAR — `CLAUDE.md`

## Reframe

Nothing is broken and nothing needs porting. The library is complete and
oracle-verified, and this slice adds a second caller to an API that already
has one. **The binding layer therefore owns no numerics** — if a Python test
is checking arithmetic, it is in the wrong file.

## What was measured before designing

| | |
|---|---|
| C8, the add-in in real Excel | **PASS 2026-11-11** — 24/24 registered, every row matched, both forms agreed (Excel 16.0 build 20527, x64) |
| grouped XLL forms | **11**, all wide functions covered; 8 are within a five-argument cap, 3 are not |
| nanobind | **3.1.0** |
| stable-ABI floor, **linked mode** | **cp312** — a CPython limit, not nanobind's |
| the same floor in pybind11 | **also 3.12** (`Py_LIMITED_API >= 0x030C0000`) |
| stable-ABI floor, nanobind split mode | **3.10** — an escape hatch pybind11 lacks |
| `sys_days` through nanobind's stock chrono caster | returns `datetime.datetime`, **not** `datetime.date` |
| nanobind's use of the datetime C API | **none** — no `PyDateTime_IMPORT`, no `PyDateTimeAPI` |
| casters `chrono.h` ships | exactly two: durations, and `time_point`. **`year_month_day` is not mentioned anywhere in nanobind** |
| direction of the gap | **input is already correct** — a Python `date` is accepted. No caster ever *returns* one |
| `date(2024,3,15) == datetime(2024,3,15,0,0)` | **`False`**, silently. Ordering and subtraction raise `TypeError`; set intersection is empty |
| the stock caster's `from_cpp` | routes through `system_clock::to_time_t` and `localtime_r`; `from_python` through `mktime` — **local time on both sides** |
| stock caster, `TZ=UTC` / `Europe/London` | `2024-03-15 00:00:00` — right day, wrong type |
| stock caster, `TZ=America/New_York` | **`2024-03-14 20:00:00` — the wrong day** |
| stock caster, `TZ=Pacific/Auckland`, round trip | **`2024-03-14 13:00:00` — the wrong day** |
| what the Python build actually needs | `include/` 624K + `src/` 12K. `tests/` alone is **6.6M**, mostly golden vectors |
| `CDSMODEL_BUILD_TESTS` default | `PROJECT_IS_TOP_LEVEL` — **true** under scikit-build-core, so a default sdist build would FetchContent GoogleTest and need network |
| scikit-build-core's sdist rule | **`.gitignore`-driven.** A decoy `isda_cds_model_c_v1.8.3/` and `xlsdk/` were both excluded; `include/`, `src/` and `tests/golden/` were kept |
| what that rule also does | a file that is untracked but **not** ignored **is** shipped — a stray `log` landed in the test tarball |
| a caster built on `year_month_day` | `2024-03-15`, type `date`, correct in all four zones; compiles and runs under `STABLE_ABI` |
| proof build | `proof-0.0.1-cp312-abi3-macosx_26_0_arm64.whl`, imports and runs |

The stable-ABI floor shapes everything else, and it belongs to **CPython**,
not to the binding library. nanobind's CMake gives the reason in a comment:

> *Linked-mode stable ABI builds require CPython >= 3.12 (the backend itself
> then compiles under `Py_LIMITED_API`)*

pybind11 has the identical floor — its `PYBIND11_STABLE_ABI_VERSION` defaults
to 3.12 and it requires `Py_LIMITED_API >= 0x030C0000`, which *is* 3.12 — and
it only gained stable-ABI support in 3.0, where nanobind has had it longer and
additionally offers split mode down to 3.10. So the floor is not a reason to
prefer one over the other, and switching would buy a worse one.

## MUST — in scope, full discipline

1. `python/` holding a `src`-layout package: `cdsmodel/` with `_core` (the
   extension), `__init__.py`, `_core.pyi`, and `py.typed`.
2. **A `Date` caster to and from `datetime.date`**, not the stock chrono one,
   and **`nanobind/stl/chrono.h` included nowhere in the project** — it is not
   merely redundant for our type, it is measurably wrong for it.
3. `Expected<T>` → raised exceptions, one class per `ErrorKind`, each also
   deriving from the builtin a Python caller would expect.
4. The twelve functions upstream's XLL exposed, plus the types they need.
5. `pytest` suite testing the BINDING: conversion, error mapping, ownership,
   and agreement with the C++ values. Not the arithmetic.
6. `cibuildwheel` on Linux, macOS and Windows; one `cp312-abi3` wheel per
   platform; tested against 3.12, 3.13 and 3.14.
7. Publishing by PyPI **Trusted Publishing** (OIDC), no API token anywhere.

## NOT-NOW — real, but deferred this cycle

- The ~150 internal functions: date arithmetic, schedules, interpolation,
  the solver. They are implementation, and the XLL surface is the evidence
  for what a caller wants.
- `numpy` array overloads for curve construction and vectorised pricing.
- Free-threaded (3.13t+) wheels; split mode; a 3.11 floor.
- `conda-forge`; stub-completeness gating; async anything.

## NEVER — out of scope

- **Re-testing the numerics in Python.** 863 C++ tests own that, against an
  oracle Python cannot see. A pytest that recomputes a price is a second,
  weaker copy of a test that already exists.
- Exposing `Expected<T>` itself. Python has exceptions; a caller should never
  write `if result.has_value()`.
- Mutable curve or calendar objects. Both are immutable in the library and
  must stay so across the boundary, or the handle-staleness class of bug
  arrives in a new language.
- Vendoring the ISDA oracle, or the Excel SDK, in the sdist or the wheel.
  The package ships the port only. This is rule 2 of `CLAUDE.md` applied to a
  redistribution channel far more consequential than git.
- Including `nanobind/stl/chrono.h`. See D2a: with it absent, a TU that forgets
  our caster fails loudly; with it present, the failure is silent.

## Acceptance

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| P1 | A `cp312-abi3` wheel builds on all three OSes and imports | `cibuildwheel`; the tag is asserted, not eyeballed | 3 |
| P2 | WHEN a `datetime.date` goes in, THEN the same `datetime.date` comes out — same day, and never a `datetime` | round-trip over a swept date range **under `TZ` set to UTC, America/New_York, Europe/London and Pacific/Auckland**, asserting both the value and `type()`. The western zones are the positive control: the stock caster passes in UTC and London and fails in New York, so a UTC-only test would measure the fixture rather than the code | — |
| P3 | Every price the package returns equals the C++ value **bit for bit** | a generated fixture of inputs and `%.17g` outputs, produced by a C++ program linking the same library; pytest compares exactly | 3 |
| P4 | IF a call fails, THEN it raises the mapped exception with the library's own message | one test per `ErrorKind`, asserting both the class and that the message names the offending value | — |
| P5 | `except ValueError` catches a bad argument, and `except CdsModelError` catches everything | two tests per exception class, on the base and on the builtin | — |
| P6 | A curve outlives the Python name that built it, and cannot be mutated | hold a curve, rebind the source, price again; `setattr` raises | — |
| P7 | `mypy --strict` passes over the test suite using only the shipped stubs | CI step; proves `py.typed` and `.pyi` are real, not decoration | — |
| P8 | THE package SHALL expose exactly the documented function list | a test comparing `dir(cdsmodel)` against a committed manifest, so an accidental export fails | — |
| P9 | `pip install` from a built sdist works with no compiler flags and no SDK | CI builds the sdist and installs it in a clean venv | 3 |
| P10 | Publishing uses OIDC with no stored secret | the workflow is inspected for `password:`; absence asserted | — |
| P11 | THE sdist and the wheel SHALL contain no upstream ISDA source and no Excel SDK — **and SHALL contain our own sources** | both artifacts are built, unpacked, and grepped: upstream filenames and the ISDA licence string absent, `src/cds_price.cpp` present. The positive half is what stops an empty archive passing | 3 |
| P12 | `cdsmodel.__version__`, the CMake project version and C++ `version()` agree | one test comparing all three | — |
| P14 | `pip install` from the sdist succeeds **with no network access** | build the sdist, install it in a clean venv with the index and network blocked; no FetchContent may run | 3 |
| P15 | No public callable takes more than five parameters | a test walking the module's signatures with `inspect`, asserting arity ≤ 5 — so a future function cannot quietly breach the cap | — |
| P13 | Pricing the same inputs from four threads gives results identical to serial | threaded test; the GIL is released around the C++ call. Speedup is deliberately **not** asserted — it would be flaky, and safety is the property that matters | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D1 | **Python ≥ 3.12, `cp312-abi3`, linked mode** | split mode for a 3.10 floor; per-version wheels for a 3.11 floor; pybind11 | Three wheels total instead of twelve, covering 3.12, 3.13, 3.14 and later. The floor is CPython's and pybind11 shares it exactly, so it is not a library-choice question. 3.10 went end-of-life last month and 3.11 follows in under a year, so the floor costs us no supported version | a user needs 3.11 — then D1a, which is a config change, not a rewrite |
| D1a | **Fallback if 3.11 is ever needed: drop the stable ABI, build per-version wheels** | split mode | Split mode reaches 3.10 but splits the extension into two shared objects, so the packaging gets harder exactly where it is already hardest. Plain per-version wheels are what every binding library did before abi3 existed; `cibuildwheel` makes twelve wheels as automatically as three. The cost is CI minutes and release artifacts, not design | a real 3.11 user appears |
| D2 | A custom `Date` caster, in the binding layer only, converting via `std::chrono::year_month_day` | nanobind's `stl/chrono.h`; changing the library's `Date` type | **The stock caster returns the wrong day.** It converts through `localtime_r` outbound and `mktime` inbound, and a `sys_days` is midnight UTC, so anywhere west of UTC midnight UTC is yesterday: measured as `2024-03-14` in New York and, on a round trip, in Auckland. It is correct in UTC and London only — so it would pass here and corrupt dates for a user in New York, which is the same silent-landmine shape as the three epoch bugs and the `"Q"`/`"U"` type codes. The wrong *type* is the lesser half, though real on its own: `date == datetime` is `False` with no error, so `if maturity == my_maturity` is silently never true. Converting via `year_month_day` is pure civil-calendar arithmetic — no `std::tm`, no `mktime`, no time zone — and was measured correct in all four zones. **Nothing in `include/` or `src/` changes**: `sys_days` is the right type for a day count, and `year_month_day` would not help as a boundary type because nanobind does not know it either, while costing the cheap day arithmetic the port uses throughout | nanobind ships a calendar-type caster of its own |
| D3 | One exception class per `ErrorKind`, each deriving from a builtin too | one `CdsModelError`; return a result object | Artifact #01's four kinds exist because upstream's 452 messages collapse to four *kinds*; this is the consumer that finally branches on them. Dual inheritance means `except ValueError` works for a caller who expects Pythonic behaviour and `except CdsModelError` for one who wants the library's | a fifth kind appears |
| D4 | Expose the XLL's twelve functions, not all 163 | expose everything; expose a minimal four | The XLL surface is **evidence**: it is what a desk used for seventeen years. The other 150 are schedule generation, interpolation and date arithmetic — implementation of those twelve | a user asks for one, which is a better signal than my guess |
| D5 | `scikit-build-core`, not `setuptools` | `setuptools` + custom build_ext; `meson-python` | It is what nanobind's own documentation builds against, it reads `py-api` straight into the stable-ABI setting, and the proof build worked first time | — |
| D6 | The Python tests own the BOUNDARY only | mirror the C++ suite in pytest | A pytest that recomputes a price is a weaker copy of a test that already runs against the oracle. What Python can break is conversion, lifetime and error mapping — so that is what P2 to P8 test. P3 is the one exception, and it compares against C++ output rather than recomputing | — |
| D7 | Fixtures are generated by a C++ program, not hand-written | hand-written expected values; a Python reimplementation | Same rule the XLL workbook follows: no number in a test is typed by hand. The generator links the same library the extension does, so a divergence is a real divergence | — |
| D2a | **`chrono.h` is not included anywhere, recorded by a gate** | include it and rely on the full specialisation winning | Not a precaution so much as declining a header we have no use for: the library exposes no durations and no timestamps at its boundary, so `chrono.h` buys nothing. Keeping it out also removes a silent failure mode — with both present, a translation unit that sees only `chrono.h`'s generic `time_point` caster converts dates differently from one that sees ours, and the compiler is not required to diagnose it. With it absent there is only one caster, so there is nothing to disagree with, and a file that forgets our header fails loudly on its first call instead | a duration or timestamp ever crosses the boundary |
| D9a | **Explicit `sdist.exclude` for the oracle and the SDK, on top of `.gitignore`** | rely on the gitignore alone | Measured: scikit-build-core excludes gitignored paths, so the default behaviour is already correct and `verify.py upstream-absent` already gates the gitignore entry. But the mechanism is *ignore*-driven rather than *tracked*-driven, so anything untracked and unignored ships — a stray `log` file proved it. Naming the two sensitive directories explicitly means the licence rule does not rest on one line of `.gitignore` staying put | — |
| D9 | **pyproject.toml at the repo root, not in `python/`** | a `python/` subdirectory, as this spec originally assumed | Drift, caught at design time. An sdist is rooted at its `pyproject.toml`, so a `python/` subdirectory cannot reach `../include` and `../src` — P9 would be unsatisfiable. Root placement fixes that and makes P11 mandatory rather than incidental, because the oracle then sits inside the sdist root | scikit-build-core supports parent-directory sdist inclusion |
| D10 | **Release the GIL around every C++ call** | hold it, as nanobind does by default | Conversion happens before the guard and after it, so the C++ side touches no Python object; curves and calendars are already `shared_ptr<const T>` from the audit, so there is nothing mutable to race on. It lets a user price a book across threads, and retrofitting it later means touching every binding | a callback into Python is ever added — then the guard must go |
| D11 | **A hard cap of five parameters per callable, met by reusing `xl/core`'s four blocks** | mirror the C++ positional signatures; invent a Python-specific grouping | `cds_price` takes nine. The four blocks `xl/core` already defines — `Valuation`, `TradeTerms`, `Conventions`, `MarketData` — plus `clean` is exactly five, which is also `CDS.Price`'s arity. So the grouped form built for Excel *is* the Python signature, and one design answers both callers rather than two that can drift. Keyword-only on top, because `today`, `cash_settle` and `step_in` are three adjacent same-typed dates where a positional swap is silent | the deferred `Cds` split lands, which would cut it further |
| D11b | **Python groups the three loose market values that the XLL leaves flat** | copy each `CDS.` form's arity verbatim | Drift, found when the Windows session landed all eleven grouped forms on 2026-11-11. D11 claimed the Excel grouped form *is* the Python signature; that holds for `CDS.Price` at five and fails for three: `CDS.Upfront` and `CDS.ParSpreadFlat` at seven, `CDS.SpreadCurve` at six. The cause is structural rather than arbitrary — those take a flat par spread or an upfront charge instead of a spread *curve*, so `Discount Curve`, `Market Par Spread`/`Upfront Charge` and `Recovery Rate` sit loose where `CDS.Price` has one `MarketData` block. Grouping them the same way puts all three at exactly five. The blocks stay shared; Python simply applies the existing principle where the XLL stopped. The XLL keeps its arities — it has no cap, and it is now verified in Excel, so changing it would mean re-running C8 | the XLL adopts the same block, which would restore one arity per function |
| D11a | **The cap counts what a caller must pass, not defaulted parameters** | count every parameter | `Conventions` has six fields, which would breach a strict reading. All six are defaulted, so a caller writes `Conventions()` and overrides what they need — zero required. Splitting the six would scatter one concept: they are precisely the values a desk sets once, which is why upstream made you retype them into all eight priced functions | the user reads the cap strictly, in which case `Conventions` splits along schedule vs accrual |
| D14 | **The sdist ships only what builds the extension, and builds with `CDSMODEL_BUILD_TESTS=OFF`** | ship the repo as-is | Nothing gitignored is needed, and the library is *already proven* to build with no oracle — CI does it on every commit with `-DCDSMODEL_UPSTREAM_DIR=/nonexistent`, which is what the two-tier contract was for. Beyond that, `CDSMODEL_BUILD_TESTS` defaults to `PROJECT_IS_TOP_LEVEL`, which is true under scikit-build-core, so an untouched build would FetchContent GoogleTest and need network — `pip install` from the sdist would fail behind a firewall. With tests off, `tests/` (6.6M of 8.9M) is not needed either | a user wants to run the tier-1 vectors from an installed sdist |
| D12 | **`__version__` single-sourced from `CMakeLists.txt`** | a literal in `__init__.py` | `version.hpp` already exposes `version()` and `upstream_version()`, and CMake already carries `project(cdsmodel VERSION …)`. A literal in `__init__.py` would be a fourth copy and the one most likely to drift. scikit-build-core reads the version out of CMakeLists as dynamic metadata | — |
| D8 | Trusted Publishing, no API token | a `PYPI_API_TOKEN` secret | A long-lived token in repository secrets is a credential that can leak and does not expire. OIDC issues one scoped to the workflow run. P10 asserts no `password:` appears | PyPI changes the mechanism |

## Open questions before ratification

1. ~~BLOCKING — the package name.~~ **Closed.** Both PyPI endpoints return
   404 for `cdsmodel` (`/pypi/cdsmodel/json` and `/simple/cdsmodel/`), so the
   name is unclaimed. It matches the C++ namespace; no fallback needed.
2. ~~Non-blocking — where `python/` sits.~~ **Closed by D9**: the root, because
   an sdist cannot reach above its own `pyproject.toml`.
3. Non-blocking — whether stubs are generated or hand-written.
   **Assumption: generated** by `nanobind.stubgen` at build time and
   committed, so a diff shows when the API changes.

## Progress

| ID | Status | Oracle result | Note |
|----|--------|---------------|------|
| P1 | todo | | a proof wheel already built locally |
| P2 | todo | | D2's caster |
| P3 | todo | | the binding criterion |
| P4 | todo | | |
| P5 | todo | | |
| P6 | todo | | |
| P7 | todo | | |
| P8 | todo | | |
| P9 | todo | | |
| P10 | todo | | |
| P11 | todo | | the licence criterion |
| P12 | todo | | |
| P13 | todo | | |
| P14 | todo | | the firewall case |
| P15 | todo | | the cap, machine-checked |
