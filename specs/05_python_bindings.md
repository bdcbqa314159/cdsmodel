# Artifact #5 — the Python package (draft)

**Status:** draft
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
| nanobind | **3.1.0** |
| stable-ABI floor, **linked mode** | **cp312** — a CPython limit, not nanobind's |
| the same floor in pybind11 | **also 3.12** (`Py_LIMITED_API >= 0x030C0000`) |
| stable-ABI floor, nanobind split mode | **3.10** — an escape hatch pybind11 lacks |
| `sys_days` through nanobind's stock chrono caster | returns `datetime.datetime`, **not** `datetime.date` |
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
2. **A `Date` caster to and from `datetime.date`**, not the stock chrono one.
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
- Vendoring the ISDA oracle in the sdist. The package ships the port only.

## Acceptance

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| P1 | A `cp312-abi3` wheel builds on all three OSes and imports | `cibuildwheel`; the tag is asserted, not eyeballed | 3 |
| P2 | WHEN a `datetime.date` goes in, THEN a `datetime.date` comes out — never a `datetime` | round-trip test over a swept date range, `type()` asserted | — |
| P3 | Every price the package returns equals the C++ value **bit for bit** | a generated fixture of inputs and `%.17g` outputs, produced by a C++ program linking the same library; pytest compares exactly | 3 |
| P4 | IF a call fails, THEN it raises the mapped exception with the library's own message | one test per `ErrorKind`, asserting both the class and that the message names the offending value | — |
| P5 | `except ValueError` catches a bad argument, and `except CdsModelError` catches everything | two tests per exception class, on the base and on the builtin | — |
| P6 | A curve outlives the Python name that built it, and cannot be mutated | hold a curve, rebind the source, price again; `setattr` raises | — |
| P7 | `mypy --strict` passes over the test suite using only the shipped stubs | CI step; proves `py.typed` and `.pyi` are real, not decoration | — |
| P8 | THE package SHALL expose exactly the documented function list | a test comparing `dir(cdsmodel)` against a committed manifest, so an accidental export fails | — |
| P9 | `pip install` from a built sdist works with no compiler flags and no SDK | CI builds the sdist and installs it in a clean venv | 3 |
| P10 | Publishing uses OIDC with no stored secret | the workflow is inspected for `password:`; absence asserted | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D1 | **Python ≥ 3.12, `cp312-abi3`, linked mode** | split mode for a 3.10 floor; per-version wheels for a 3.11 floor; pybind11 | Three wheels total instead of twelve, covering 3.12, 3.13, 3.14 and later. The floor is CPython's and pybind11 shares it exactly, so it is not a library-choice question. 3.10 went end-of-life last month and 3.11 follows in under a year, so the floor costs us no supported version | a user needs 3.11 — then D1a, which is a config change, not a rewrite |
| D1a | **Fallback if 3.11 is ever needed: drop the stable ABI, build per-version wheels** | split mode | Split mode reaches 3.10 but splits the extension into two shared objects, so the packaging gets harder exactly where it is already hardest. Plain per-version wheels are what every binding library did before abi3 existed; `cibuildwheel` makes twelve wheels as automatically as three. The cost is CI minutes and release artifacts, not design | a real 3.11 user appears |
| D2 | A custom `Date` caster | nanobind's `stl/chrono.h` | Measured: the stock caster returns `datetime.datetime`, so `price(start=date(...))` would hand back a datetime at midnight. A CDS has no time of day — the library refuses a fractional Excel serial for the same reason — and a type that is almost right is worse than one that is wrong loudly | the library grows intraday semantics |
| D3 | One exception class per `ErrorKind`, each deriving from a builtin too | one `CdsModelError`; return a result object | Artifact #01's four kinds exist because upstream's 452 messages collapse to four *kinds*; this is the consumer that finally branches on them. Dual inheritance means `except ValueError` works for a caller who expects Pythonic behaviour and `except CdsModelError` for one who wants the library's | a fifth kind appears |
| D4 | Expose the XLL's twelve functions, not all 163 | expose everything; expose a minimal four | The XLL surface is **evidence**: it is what a desk used for seventeen years. The other 150 are schedule generation, interpolation and date arithmetic — implementation of those twelve | a user asks for one, which is a better signal than my guess |
| D5 | `scikit-build-core`, not `setuptools` | `setuptools` + custom build_ext; `meson-python` | It is what nanobind's own documentation builds against, it reads `py-api` straight into the stable-ABI setting, and the proof build worked first time | — |
| D6 | The Python tests own the BOUNDARY only | mirror the C++ suite in pytest | A pytest that recomputes a price is a weaker copy of a test that already runs against the oracle. What Python can break is conversion, lifetime and error mapping — so that is what P2 to P8 test. P3 is the one exception, and it compares against C++ output rather than recomputing | — |
| D7 | Fixtures are generated by a C++ program, not hand-written | hand-written expected values; a Python reimplementation | Same rule the XLL workbook follows: no number in a test is typed by hand. The generator links the same library the extension does, so a divergence is a real divergence | — |
| D8 | Trusted Publishing, no API token | a `PYPI_API_TOKEN` secret | A long-lived token in repository secrets is a credential that can leak and does not expire. OIDC issues one scoped to the workflow run. P10 asserts no `password:` appears | PyPI changes the mechanism |

## Open questions before ratification

1. ~~BLOCKING — the package name.~~ **Closed.** Both PyPI endpoints return
   404 for `cdsmodel` (`/pypi/cdsmodel/json` and `/simple/cdsmodel/`), so the
   name is unclaimed. It matches the C++ namespace; no fallback needed.
2. Non-blocking — where `python/` sits. **Assumption: a `python/`
   subdirectory** with its own `pyproject.toml`, so the C++ build is not
   forced to know about Python and `cibuildwheel` has one root.
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
