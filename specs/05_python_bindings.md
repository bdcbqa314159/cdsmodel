# Artifact #5 — the Python package (draft)

**Status:** ratified 2026-11-11 (Bernardo Cohen)
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
| where nanobind constructs a `call_guard` | **inside** the lambda wrapping the user function (`nb_func.h:132`); argument conversion produces that lambda's arguments and `from_cpp` runs on its result, so **both conversions hold the GIL**. D2 and D10 compose |
| throwing a plain C++ exception from inside the guard | converts correctly: the guard's destructor re-acquires the GIL during unwinding, then the translator runs. 200 alternating calls survived |
| dual-inherited exception, built and raised | `except ValueError` caught it **and** `isinstance(e, BadArgument)` was True — D3 proven, not assumed |
| `nb::exception<T>` | requires `T` to expose `what()`, so the carrier must derive from `std::exception` |
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
- **Pickling the value objects.** It matters for `multiprocessing`, but D10's
  GIL release makes threads the first answer for a book of trades, and pickling
  adds a versioned format to maintain. Named here rather than left silent.
- `FeeLeg` and `ContingentLeg` as bound types — see D15.

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
| P18 | Failure and date conversion remain correct under repeated calls with the GIL released | a loop alternating a failing call and a date-returning call several hundred times, asserting no crash and the right type each time. A single call proves nothing here: GIL misuse corrupts state rather than failing immediately | — |
| P5 | `except ValueError` catches a bad argument, and `except CdsModelError` catches everything | two tests per exception class, on the base and on the builtin | — |
| P6 | A curve outlives the Python name that built it, and cannot be mutated | hold a curve, rebind the source, price again; `setattr` raises | — |
| P7 | `mypy --strict` passes over the test suite using only the shipped stubs | CI step; proves `py.typed` and `.pyi` are real, not decoration | — |
| P8 | THE package SHALL expose exactly the documented function list | a test comparing `dir(cdsmodel)` against a committed manifest, so an accidental export fails | — |
| P9 | `pip install` from a built sdist works with no compiler flags and no SDK | CI builds the sdist and installs it in a clean venv | 3 |
| P10 | Publishing uses OIDC with no stored secret | the workflow is inspected for `password:`; absence asserted | — |
| P11 | THE sdist and the wheel SHALL contain no upstream ISDA source and no Excel SDK — **and SHALL contain our own sources** | both artifacts are built, unpacked, and grepped: upstream filenames and the ISDA licence string absent, `src/cds_price.cpp` present. The positive half is what stops an empty archive passing | 3 |
| P12 | `cdsmodel.__version__`, the CMake project version and C++ `version()` agree | one test comparing all three | — |
| P14 | `pip install` from the sdist succeeds **with no network access** | build the sdist, install it in a clean venv with the index and network blocked; no FetchContent may run | 3 |
| P16 | Every emitted enum value is a valid, non-keyword Python identifier | a test importing the module and checking each enum member name with `str.isidentifier()` and `keyword.iskeyword()`. `Weekend.None` is the case that motivated it, and it fails at import, so the test must run on names rather than attribute access | — |
| P17 | Every value object has a `__repr__` naming its type and fields | a test asserting `repr()` of each bound type contains the type name and every field name — so a Jupyter user sees `TradeTerms(start=..., end=...)` rather than `<cdsmodel._core.TradeTerms object at 0x...>` | — |
| P15 | No public callable takes more than five parameters, with `Conventions` the single named exemption at six | a test walking every public signature with `inspect`, asserting arity ≤ 5 against an explicit one-entry exemption list. A *new* callable at six fails; widening `Conventions` to seven fails. Naming the exemption rather than raising the limit is what keeps the gate able to fail | — |
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
| D11a | **The cap counts what a caller must pass, not defaulted parameters. `Conventions` is the one named exemption** | count every parameter, splitting `Conventions` into nested `Schedule` and `Accrual` blocks | **Ratified 2026-11-11 (Bernardo Cohen)**, choosing the flat form over the strict split. All six fields are defaulted, so a caller writes `Conventions()` and overrides what they need — zero required arguments. It matches `xl/core`'s `Conventions` block field for field, so one mental model serves Excel and Python, which is the whole point of reusing the blocks. The strict split would have cost nesting at the call site for the commonest case — changing one day count — to satisfy a count no caller experiences. The exemption is **named individually in the gate, never granted by pattern**, exactly as the error channel names its two D7 exemptions | a seventh field is proposed, which is the signal that the concept really has split |
| D20 | **The guarded body touches no Python API; failure leaves it as a plain C++ exception and a translator converts it** | throw `nb::python_error`, or set the error with `PyErr_SetString` at the failure site | D10 releases the GIL around the call, and calling any Python API without the GIL is undefined behaviour — a crash that would appear only under load, long after the code looked fine. Measured: a plain C++ exception thrown inside the guard unwinds past the guard's destructor, which re-acquires the GIL, and only then does the translator run. The obvious implementation is the unsafe one, which is why this is a decision rather than a note | D10 is withdrawn |
| D21 | **Four exception types, one translator that switches on `ErrorKind`** | one `nb::exception<T>` per kind, each with its own auto-registered translator | `nb::exception<T>` is keyed on the *C++* type, and all four kinds share one carrier, so four of them cannot discriminate. Building the four Python types explicitly and switching inside a single translator keeps D5 intact — the kind and the message pass through together, unwrapped and unrenamed | a fifth kind appears |
| D15 | **Bind the result types as readable objects, not tuples** | return tuples or dicts | `flow.date` survives a field being added; `flow[0]` does not. `CashFlow` and `DefaultAccrual` are the two the twelve functions return. `FeeLeg` and `ContingentLeg` stay NOT-NOW because `cds_fee_leg` is not among the XLL's twelve | a caller needs the legs themselves |
| D16 | **Expose only the seven enums reachable from the bound surface** | expose all seventeen | The other ten are implementation — interpolation metrics, stub location, date-list keep policy. Exposing them would invite callers to depend on internals the port is free to change, and would make D4's twelve-function scope meaningless | a newly bound function needs one |
| D17 | **Enum values are emitted UPPER_CASE, because two of them are Python keywords** | transliterate the C++ names directly | Measured: `Weekend::None` and `BadDayConv::None` would be `Weekend.None`, which does not parse. A blanket UPPER_CASE rule fixes both without a special case per name, and `NONE` reads correctly beside `SATURDAY`. P16 checks every emitted name is a valid identifier and not a keyword, so a future enum value called `class` or `lambda` fails a test rather than the import | — |
| D18 | **Binding functions take `std::vector`; `std::span` never crosses the boundary** | write a `span` caster | A span is a borrowed view, and a caster for one would have to keep the owner alive across the call — exactly the lifetime bug class P6 exists to prevent. Taking a vector by value at the boundary and passing a span inward costs one copy on a call that is about to do far more work | a profile shows the copy mattering |
| D19 | **`ZeroCurve.from_points` takes parallel date and rate lists, not `RatePoint`s** | expose `RatePoint` | It keeps one type off the public surface for no loss: a caller building a curve has two columns, not a list of pairs. `DatesAndRates` in the XLL already returns exactly those two columns, so the shapes match | a caller wants to round-trip `points()` back in |
| D32 | **`ParSpreads` is not bound as its own function** | mirror the XLL's grid form | In Excel a grid function exists because a cell cannot loop; in Python `[par_spread(TradeTerms(start=s, end=e, coupon_rate=0), ...) for e in ends]` is one line. Worse, the XLL's form deliberately puts a failed maturity's *message* in its row so the rest still price — which in Python means `list[float | str]`, a type no caller wants to handle and no stub can describe usefully. `par_spread` covers the computation; the loop belongs to the caller, who can then choose whether one bad maturity stops the batch | a vectorised `numpy` form is added, which is already NOT-NOW |
| D33 | **A third market block, `CurveMarket`, rather than an optional field** | make `spread` optional on `MarketData` | `clean_spread_curve` needs a discount curve and a recovery rate and *produces* the spread curve, so `MarketData` cannot describe its inputs and `FlatMarket`'s single `quote` does not apply — the coupons live on the benchmarks. Making `MarketData.spread` optional would have avoided a type at the cost of a field that is required in two uses out of three and ignored in the other, which is a runtime check where a type does the job | the three collapse to one shape |
| D31 | **`add_subdirectory(xl/core)` is guarded on presence, not on an option** | include `xl/` in the sdist; or gate on `CDSMODEL_BUILD_PYTHON` | The sdist omits `xl/` deliberately — D23 makes them peers, and a wheel has no use for the Excel layer — so an unguarded `add_subdirectory` made the source distribution impossible to configure. A presence check is the honest form: it skips when the directory genuinely is not there and says so, rather than tying the Excel layer's existence to a Python flag it has nothing to do with. Including `xl/` instead would ship 280K of code a wheel never compiles | the sdist starts shipping `xl/` for some other reason |
| D30 | **Platform guards in the suite use `sys.platform`, not `hasattr`** | `hasattr(time, "tzset")`, which is what the TZ sweep originally used | Both are correct at runtime; only `sys.platform` is understood by a type checker, which treats the guarded branch as unreachable on the platform that lacks the symbol. With `hasattr`, `time.tzset` read as an attribute error on Windows — caught by `mypy --strict` on the Windows runner, which is precisely what P7 exists to catch. `mypy --platform win32` reproduces it locally in seconds instead of waiting three minutes for the runner | — |
| D29 | **The `type: ignore` comments in the suite are evidence, not debt** | annotate around them, or drop the tests | Two tests deliberately pass positional arguments to assert a `TypeError`. `mypy` flags both — which is the stub proving keyword-only is enforced at type-check time as well as at runtime, so the suppression records that both halves hold. Logged here because the project's ledger rule covers every suppression, and because a reader who finds `# type: ignore` in a strict-typed suite deserves to know which kind it is | the tests stop asserting a runtime `TypeError` |
| D28 | **P3 compares exactly within a platform and tolerantly across one; CI regenerates the fixtures per runner** | one committed fixture compared exactly everywhere, as the spec originally said | Drift, and measured on CI rather than predicted. `exp`, `log` and `pow` are not correctly rounded in IEEE 754 and the library calls them 24 times, so glibc and macOS libm differ in the last bit: a price of `0.08687005698251757` on Linux against `...758` on macOS, one ULP, neither wrong. No care in the bindings could reconcile that, and a single committed fixture compared exactly would fail on two platforms out of three forever. Regenerating per runner keeps the comparison **exact everywhere it runs in CI**, which is what P3 is for; a fixture carried across platforms falls back to the project's 1e-12 recorded-vector tier, states that it has done so, and was checked to still catch a 1e-6 error. A CI step asserts the stamp matches the runner, so a silently skipped regeneration cannot downgrade the check | the library stops calling libm transcendentals, which it will not |
| D27 | **A second named cap exemption, `SwapConventions`, and an exemption must now prove it requires nothing** | a single exemption; splitting `SwapConventions` into three | Seven fields, every one defaulted — the same shape as `Conventions`, so the same argument applies. But two exemptions start to look like a pattern, and `CLAUDE.md` says exemptions are named individually and never granted by pattern. So P15 gained a second test: **every exempt callable must have zero required parameters.** That is what D11a actually argued — six defaulted convention fields are not six things a caller juggles — and it stops the list becoming a way to smuggle a wide signature past the cap. Verified to fail both ways: naming a callable with required arguments, and naming one that does not exist | a third exemption appears, which would be the signal the cap is wrong rather than the signatures |
| D25 | **`Weekend` is `nb::is_flag()` without `nb::is_arithmetic()`** | both; or a plain enum | A plain enum makes `SATURDAY | SUNDAY` a `TypeError` in Python where the same expression is idiomatic in C++, and the library's `Standard` *is* `Saturday|Sunday`. Adding `is_arithmetic` on top permits implicit int conversion, which collapses the repr to a bare `96` instead of `Weekend.STANDARD` — the unreadable-object problem P17 exists to prevent | a caller needs the integer value, which `int(...)` already gives |
| D26 | **Reading a holiday file happens in Python, not C++** | mirror `xl/core`'s `read_calendar_file` | `parse_holiday_file` takes contents, not a path (artifact #4, D4), so the IO belongs to the caller either way. Doing it in Python is both less code and safer: `std::ifstream` *opens* a directory and reads zero bytes, and an empty holiday file is legitimate, so a C++ reader without an is-regular-file check hands back a working calendar with no holidays — the same silent-wrong-calendar failure as UPSTREAM-41, which `xl/core` guards against explicitly. `Path.read_text()` raises `IsADirectoryError`. A Python caller also expects `FileNotFoundError` rather than a library error type | the parser ever needs to stream a file too large to hold |
| D24 | **P15's arity oracle parses the docstring, not `inspect.signature`** | `inspect` as the spec originally said | Drift: `inspect.signature` reports `(*args, **kwargs)` for a nanobind function and raises outright for a method, so the oracle as written could not fail. nanobind puts the real signature on the first line of `__doc__`, which is where its own stub generator reads it. The parser counts top-level commas only, so `Sequence[datetime.date]` is one parameter, and it reads **every** overload line rather than the first — nanobind lists them separately, and checking only line one let a wide second overload through | stubs land (P7), which would be a structured source |
| D23 | **The Python layer mirrors `xl/core`'s blocks but defines its own types** | bind `cdsmodel::xl`'s structs directly | Drift against D11, found when binding the first one. `cdsmodel::xl::TradeTerms` includes `xl/value.hpp` and `xl/handles.hpp`, so binding it would make the Python layer depend on the Excel layer — which the layering in `CLAUDE.md` forbids, and would drag `XlValue` and the handle registries into a wheel that has no use for either. Python sits *beside* `xl/core`: both depend only on `cdsmodel`. The grouping stays identical so one mental model serves both, but a field added to one must be added to the other by hand | the blocks are promoted into `cdsmodel` proper, which is the real fix and a library change |
| | | | → enforced by `python verify.py python-layer`, which refuses an `#include` of `cdsmodel/xl/` and a link against `cdsmodel_xl_*`, and refuses to pass on an empty tree |
| D22 | **`build-dir = "build/wheel/{wheel_tag}"`, not `build/{wheel_tag}"`** | scikit-build-core's documented default | CMakePresets already own `build/debug` and `build/release`. A wheel tree beside them interleaves with the project's own, and cleaning one takes out the other — which happened once during P1 and made `verify.py tests` report that no tests ran at all | the presets move |
| D14 | **The sdist ships only what builds the extension, and builds with `CDSMODEL_BUILD_TESTS=OFF`** | ship the repo as-is | Nothing gitignored is needed, and the library is *already proven* to build with no oracle — CI does it on every commit with `-DCDSMODEL_UPSTREAM_DIR=/nonexistent`, which is what the two-tier contract was for. Beyond that, `CDSMODEL_BUILD_TESTS` defaults to `PROJECT_IS_TOP_LEVEL`, which is true under scikit-build-core, so an untouched build would FetchContent GoogleTest and need network — `pip install` from the sdist would fail behind a firewall. With tests off, `tests/` (6.6M of 8.9M) is not needed either | a user wants to run the tier-1 vectors from an installed sdist |
| D12 | **`__version__` single-sourced from `CMakeLists.txt`** | a literal in `__init__.py` | `version.hpp` already exposes `version()` and `upstream_version()`, and CMake already carries `project(cdsmodel VERSION …)`. A literal in `__init__.py` would be a fourth copy and the one most likely to drift. scikit-build-core reads the version out of CMakeLists as dynamic metadata | — |
| D8 | Trusted Publishing, no API token | a `PYPI_API_TOKEN` secret | A long-lived token in repository secrets is a credential that can leak and does not expire. OIDC issues one scoped to the workflow run. P10 asserts no `password:` appears | PyPI changes the mechanism |

## Observation — two error kinds are unobservable from Python

Three separate attempts, across every solver-backed path the surface has:

| path | what came back |
|---|---|
| `ZeroCurve.build`, absurd swap rate | `bad_argument: the swap benchmarks could not be bootstrapped` |
| `par_spread`, hazard to 5000 | a finite number |
| `par_spread_flat`, a root find, quote 1e9 | `bad_argument: the contingent leg integral is not finite` |
| `par_spread_flat`, recovery 1.0 | `bad_argument: guess (inf) is out of range [0, 1e10]` |

The last is the clearest: the solver validates its bracket **before** searching,
so an unreachable target is reported as a bad argument rather than as a
failure to find a root. Combined with `add_swaps` returning `bool` — which
destroys the kind before any caller could forward it — `no_root` and
`no_convergence` appear genuinely unreachable through the twelve functions.



`no_root` and `no_convergence` are not reachable from any of the twelve bound
functions, on the evidence so far. One path provably destroys them (below).
The others were probed and refused earlier, or returned a finite answer.

That is worth stating rather than leaving as an unticked box, because it
bears on whether a four-kind taxonomy earns its keep *at this boundary*. The
taxonomy is justified in artifact #01 by the C++ call sites, and
`A6EveryKindIsReachable` proves all four are reachable **in C++**. Reachable
in C++ and reachable through twelve public functions are different claims,
and only the first is established.

The classes are still bound and still tested (P5), because the alternative —
binding two of four and leaving a caller unable to write
`except cdsmodel.NoRoot` — would be worse than binding four and having two
rarely fire.

## Observation — the solver's reason is lost before the caller

`build_ir_zero_curve` reports every failure as `bad_argument`, including the
case where the swap bootstrap exhausts its solver:

    absurd swap rate -> bad_argument: the swap benchmarks could not be
                        bootstrapped onto the curve

That is not D5 being broken by its caller. `add_swaps` returns **`bool`**, so
the kind and the message are destroyed before any caller could forward them —
there is nothing left to propagate, and the caller must invent a message to
have one at all.

It is worth naming because it is the same diagnostic loss artifact #01 was
created to fix. That spec's decisive case was `root_find.hpp` conflating
backwards-bounds, objective-failure and iterations-exhausted into one
`nullopt`; here the conflation survives one layer up, in a `bool`-returning
path the migration did not cover. The error-channel gate does not catch it
because it bans `std::optional`, not `bool`.

Not fixed here: it is a library change, not a binding one. Recorded so that
P4's two remaining kinds are known to be unreachable through this path rather
than assumed reachable and quietly missed.

## Open questions before ratification

0. ~~Non-blocking — the cap reading.~~ **Closed 2026-11-11 by D11a**: flat
   `Conventions` at six defaulted fields, one named exemption in P15.
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
| P1 | **pass** (macOS; 3-OS pending CI) | `cdsmodel-0.1.0-cp312-abi3-macosx_26_0_arm64.whl` builds, installs into a clean venv and imports. Wheel holds 8 entries — the package only | two bugs caught by the *import* half of the oracle: a missing `nanobind/stl/string.h` (`ImportError: std::bad_cast`) and the library's own install rules staging `include/`, `lib/libcdsmodel.a` and `lib/cmake/` into the wheel, which would have landed in site-packages |
| P2 | **pass** (macOS; 3-OS on CI) | 48 tests: ten dates x four zones, both directions. All zones agree. `type(x) is datetime.date` asserted rather than `isinstance`, since `datetime` is a *subclass* of `date` and would slip past | the TZ sweep is POSIX-only -- `time.tzset()` has no Windows equivalent, so the Windows leg checks value and type without varying the zone, and the test says so |
| P3 | **pass** | **216 prices and 216 par spreads bitwise equal to the C++ values**, plus 20 fee-leg flows and a returned struct, on each of the three platforms — CI regenerates the fixtures per runner (D28). Verified by perturbing one fixture a **single ULP** (rel 1.4e-16) and watching it fail; the tolerant fallback was verified to engage, to say so, and still to catch a 1e-6 error | links `cdsmodel` only, never the oracle: this asks whether the binding is transparent, not whether the port matches upstream |
| P4 | partial — **and now believed unclosable** | `bad_argument` and `unsupported` raised and distinguished across every bound function | **`no_root` and `no_convergence` appear unreachable through the bound surface.** Tried: a zero-length trade (refused earlier as `bad_argument`), recovery of 1.0 (returns 0.0), and hazard rates to 5000 (returns a finite spread). Either the inputs that reach them are more exotic than a user would write, or the two kinds are genuinely unobservable here — see the note below, since one path provably destroys them |
| P5 | **pass** | all four classes subclass both `CdsModelError` and their builtin; a failure is caught either way, and `except Unsupported` does not catch a `bad_argument` | a taxonomy that cannot be branched on is not a taxonomy |
| P6 | **pass** | a curve outlives the lists and the names that built it (checked after `gc.collect()`), a calendar outlives the `Conventions` that held it, returned flows are independent copies, and `base_date`, `basis` and `day_count` all refuse assignment | the XLL needed a registry, a mutex and `shared_ptr<const T>` for this; Python holds the object, so the stale-handle class does not arise — but the guarantee that replaces it is checked, not assumed |
| P7 | **pass** | `mypy --strict` clean over 8 test files on all three platforms, resolving every symbol through the shipped `_core.pyi` and `py.typed`. Proven real: removing `py.typed` breaks it, and a deliberately wrong call is caught (`Argument "start" has incompatible type "str"; expected "date"`) | the first run passed **vacuously** — the wheel had no `.pyi`, so every symbol was `Any`. Shipping the stub then revealed two genuine errors in the suite, and the Windows runner a third |
| P8 | **pass** | `__all__` compared against a committed manifest of ten names | a new export must be added deliberately |
| P9 | **pass** | `pip install` from the sdist into a clean venv builds and computes. **It did not, first time**: `add_subdirectory(xl/core)` was unguarded while `xl/` is excluded from the sdist, so the source distribution could not configure at all | nothing before this had ever built the tree without `xl/` present |
| P10 | todo | | |
| P11 | **pass** | `verify.py sdist` — 79 files, no ISDA or Microsoft sources, all 10 required entries present. Verified to fail both ways: a leaked oracle file, and an sdist missing `src/` | the positive half is what stops an empty archive passing |
| P12 | **pass** | `cdsmodel.__version__` is `0.1.0`, read out of `CMakeLists.txt` by scikit-build-core and out of the C++ `version()` by the module; the test compares all three | `__upstream_version__` reports `1.8.3` |
| P13 | **pass** | 400 prices across 4 threads identical to serial; 400 concurrent reads of one shared curve identical; 300 tasks interleaving failure and success yield exactly one distinct price | measured speedup is **1.15x** for an 11us price and **2.61x** for a 34us curve build — dispatch overhead dominates the cheap call. Not asserted, as the spec said: it would be flaky, and safety is the property that matters |
| P14 | **pass** | installs with `--no-index --no-build-isolation` and prices a trade. Confirms D14: with `CDSMODEL_BUILD_TESTS=OFF` the build never reaches GoogleTest's `FetchContent` | the case that matters on a desk behind a proxy |
| P15 | **pass** | 21 bound signatures checked. Max 5 (`ZeroCurve.from_points`); `Conventions.__init__` at 6 is the single named exemption, and the gate reports it as exempt rather than ignoring it. Verified to fail by lowering the cap to 4 | `inspect.signature` is useless on nanobind callables — see D24 |
| P16 | **pass** | `DayCount` and `Weekend` members all identifiers, non-keywords, UPPER_CASE. **`Weekend::None` is the real collision**, now reachable as `Weekend.NONE` | `None`, `class` and `Standard` all refused by the same check |
| P17 | **pass** | all eleven bound types repr their contents, and all nine enums repr as `DayCount.ACT_360` rather than an int | was partial while `ZeroCurve`, `TradeTerms`, `Calendar`, `Weekend` reprs | one more type bound; `TradeTerms` reprs as `TradeTerms(start=2024-03-20, end=2029-06-20, coupon_rate=0.01)` | one type bound so far; the criterion covers all of them |
| P18 | **pass** | 300 interleaved failing and succeeding calls, discount factor identical throughout | GIL misuse corrupts rather than fails, so one call would prove nothing |

## The interface

Every callable sized against the cap. Nothing exceeds five.

### Value objects — the four blocks, shared with `xl/core`

    Valuation(today, cash_settle, step_in)                              3
    TradeTerms(start, end, coupon_rate)                                 3
    Conventions(pay_accrual_on_default, coupon_interval, stub,
                day_count, bad_day, holidays)                           6, all defaulted
    MarketData(discount, spread, recovery)                              3
    FlatMarket(discount, quote, recovery)                               3   <- D11b

`FlatMarket` is the block the XLL leaves loose. `quote` is a par spread for
`upfront` and an upfront charge for `par_spread_flat` -- the two are inverses,
and naming the field for its role rather than its meaning is what lets one
block serve both.

### Pricing

    price(trade, valuation, conventions, market, clean=True)            5
    par_spreads(trade, valuation, conventions, market)                  4
    upfront(trade, valuation, conventions, flat_market, clean=True)     5
    par_spread_flat(trade, valuation, conventions, flat_market,
                    clean=True)                                         5
    fee_leg_flows(trade, valuation, conventions)                        3
    default_accrual(trade, valuation, conventions, market)              4

### Curves and calendars

    ZeroCurve.from_points(dates, rates, basis, day_count)               4
    ZeroCurve.build(valuation, instruments, conventions, basis)         4
    ZeroCurve.discount_factor(date)                                     1
    ZeroCurve.points()        -> list[tuple[date, float]]               0
    clean_spread_curve(valuation, benchmarks, conventions, market)      4
    Calendar.from_dates(dates, weekends)                                2
    Calendar.from_file(path)                                            1
    Calendar.dates(start, end)                                          2

Curves and calendars are **classes, not handles**. The XLL needs a registry
because a cell cannot hold a curve; Python holds the object directly, so the
whole stale-handle problem does not exist here. `shared_ptr<const T>` already
makes them immutable, which is what lets them be shared safely (P6).

### Objects a caller receives

    CashFlow      date, amount            returned by fee_leg_flows()
    DefaultAccrual days, amount           returned by default_accrual()

Both are readable value objects, not tuples -- `flow.date` rather than
`flow[0]`. `CashFlow` already has a defaulted `operator==`, so Python equality
comes free and correct rather than being written by hand.

### Smaller value objects a caller constructs

    DateInterval(periods, unit)                                         2
    StubMethod(position, length)                                        2

Both already carry a defaulted `operator==`. `StubMethod` exists precisely
because upstream's `TStubMethod` was two bare booleans that could be swapped
silently; keeping it a two-field object in Python preserves that protection.

### Enums

Seven of the library's seventeen are reachable from the bound surface and are
exposed: `DayCount`, `BadDayConv`, `PeriodUnit`, `Weekend`, `StubPosition`,
`StubLength`, `InstrumentKind`. The other ten are internal. `ErrorKind` is
deliberately **not** exposed -- D3 turns it into exception classes.

**`Weekend::None` and `BadDayConv::None` cannot keep their names**:
`Weekend.None` is a syntax error in Python. Every enum value is emitted
UPPER_CASE, so these become `Weekend.NONE` and `BadDayConv.NONE`.

### Errors

    CdsModelError(Exception)
      BadArgument(CdsModelError, ValueError)
      Unsupported(CdsModelError, NotImplementedError)
      NoRoot(CdsModelError, ArithmeticError)
      NoConvergence(CdsModelError, ArithmeticError)

One class per `ErrorKind`, each also deriving from the builtin a Python caller
would reach for first (D3). The message is passed through unchanged -- D5 says
propagation is not a kind, and that holds across the language boundary too.

### Layout

    pyproject.toml                  repo root, so the sdist can reach src/ (D9)
    python/
      CMakeLists.txt                nanobind_add_module(_core STABLE_ABI NB_STATIC)
      src/date_caster.hpp           D2 -- via year_month_day, no time zone
      src/errors.hpp                D3 -- ErrorKind -> exception type
      src/types.cpp                 the blocks, the enums, the curves
      src/pricing.cpp               the twelve functions
      cdsmodel/__init__.py          re-exports, __version__ from CMake (D12)
      cdsmodel/py.typed
      cdsmodel/_core.pyi            generated by nanobind.stubgen, committed
    tests/python/
      test_dates.py                 P2 -- swept under four TZ values
      test_errors.py                P4, P5
      test_values.py                P3 -- against C++-generated fixtures
      test_surface.py               P8, P15 -- exports and arity
