# Artifact #1 — error-channel (ratified, revision 3)

**Status:** ratified 2026-11-08 (Bernardo Cohen)
**Governing purpose:** Replace `std::optional` with an `Expected<T>` across the
whole API so a failed call says *why*, in C++20, using a taxonomy derived from
upstream's own diagnostics rather than invented.
**Inherits:** THE BAR — `CLAUDE.md` (the oracle rules, the two-tier
verification contract, the divergence policy)

## Reframe

**The optional-based port is not broken.** 163/163 in-scope functions, 747
tests green across debug/release/asan, 438 without the oracle, 40 defects
catalogued. Nothing here is a rescue; this slice spends information that did
not exist until the port was finished.

Revision history, because two of my own proposals were wrong and the record
should say so: **r1** migrated all 341 sites but moved the project to C++23 to
obtain `std::expected` — rejected, the standard is not a side effect of an
error slice. **r2** kept C++20 but narrowed to 46 sites — rejected on review;
the narrowing was unrequested and bought a mixed API. **r3 is r1's scope on
r2's standard.**

## The measurement this rests on

| quantity | value |
|---|---|
| `return std::nullopt` sites | 341 |
| — pure propagation (a callee already failed) | 149 (44%) |
| — genuine origins | 202 |
| upstream status checks (`== FAILURE` / `!= SUCCESS`) | **447** |
| — of those, branching on a *reason* | **0** |
| upstream `JpmcdsErrMsg` diagnostic sites | 452 |

Upstream emitted a message per failure and never once branched on one. The
error payload here has always been **text for a human**, not a code for a
`switch`. That is what sizes the taxonomy at four kinds, not 452, and rules out
a per-site enum.

The origin sites classify cleanly, which is what makes four kinds sufficient:

| origin shape | count | kind |
|---|---|---|
| range / ordering | 97 | `bad_argument` |
| empty input | 20 | `bad_argument` |
| size / count | 10 | `bad_argument` |
| non-finite, malformed aggregate | ~8 | `bad_argument` |
| unsupported value or enum code | 17 | `unsupported` |
| degenerate (`is_almost_zero`), no root at bounds | ~6 | `no_solution` |
| iterations exhausted | 1 | `no_convergence` |

The sharpest case is `root_find.hpp`, which admits the conflation in its own
doc comment — *"Returns nullopt both when the objective fails and when the
iteration count"* is exhausted — and separately on backwards bounds. Upstream
distinguishes all of them by message:

| upstream message (`rtbrent.c`) | kind |
|---|---|
| `Lower bound (…) >= higher bound (…)` | `bad_argument` |
| `Guess (…) is out of range […]` | `bad_argument` |
| `Function values at bounds imply no root exists` | `no_solution` |
| `Maximum number of iterations exceeded` | `no_convergence` |
| `Supplied function failed at point …` (8 sites) | *forwarded unchanged* |

Propagation is **not** a kind. A caller that forwards an error keeps the
original kind and message; inventing a wrapper kind at each layer is how the
innermost reason gets lost.

## MUST — in scope, full discipline

1. `cdsmodel::Expected<T>` — one C++20 header, a **strict API subset** of
   `std::expected` (`has_value`, `value`, `error`, `operator*`, `and_then`).
2. `Error` carrying a kind and a message with context. Four kinds:
   `bad_argument` · `unsupported` · `no_solution` · `no_convergence`.
3. All 341 sites migrated; `std::optional` gone from every public signature.
4. `root_find` distinguishes its outcomes rather than collapsing them.
5. Behaviour unchanged: the accepted/refused set of every function identical,
   proven by the existing differential and golden suites.

## NOT-NOW — real, but deferred this cycle

- nanobind mapping (kind → Python exception type).
- Cellwright mapping (kind → `#VALUE!` / `#NUM!` / `#N/A`).
- Swapping `cdsmodel::Expected` for `std::expected` — see D2's end date.
- Splitting `Cds` into conventions + contract; install/export; umbrella header.
- `std::source_location` in `Error`.

## NEVER — out of scope

- **A C++23 bump to obtain `std::expected`.** The project's identity is a C++20
  port; a language-version change is a decision on its own merits, not a side
  effect of an error-handling slice.
- **A per-site or per-function error enum.** 447 upstream checks, 0 reason
  branches: a `switch` nobody writes.
- **Exceptions on the core API.** Both future consumers want an inspectable
  value, not a throw crossing a language boundary.
- **Upstream's process-wide error log.** Mutable global state is what this port
  removed; re-adding it to carry messages would undo that.

## Acceptance

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| A1 | `Expected<T>` compiles under `-std=c++20 -Wall -Wextra -Wpedantic` on all three CI compilers, and its API is a strict subset of `std::expected`'s | CI matrix green; a test asserting the identical call sequence compiles against `std::expected` under `-std=c++23` locally | 3 |
| A2 | THE migration SHALL NOT change which inputs are accepted or refused, for any function | the full existing suite: 747 with oracle, 438 without, three presets, zero diffs | 3 |
| A3 | WHEN bounds are reversed, THE solver SHALL report `bad_argument`; WHEN the bounds do not bracket, `no_solution`; WHEN iterations are exhausted, `no_convergence` | differential test against `rtbrent.c`, asserting upstream FAILS in each and the port's kind matches the upstream *message* at that site | 3 |
| A4 | IF a callee fails, THEN the caller SHALL forward that error with its kind and message unchanged | test on the real 3-deep chain `cdsone` → `bootstrap` → `root_find`, asserting the innermost message reaches the outermost caller | — |
| A5 | No **namespace-scope** function in `include/cdsmodel/` returns `std::optional` | new `verify.py optional` gate over declarations at namespace scope; function-local lambdas and `optional` parameters/fields are out of its scope — see D7 | — |
| A6 | Every kind is reachable from a real call; none is dead | assert all 4 observed across the suite | — |
| A7 | Every one of the 202 origin sites carries a message naming the offending value, not a bare kind | gate counting `Error{` constructions with an empty message; assert 0 | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D1 | All 341 sites in one pass | 46 on the solver path, deferring 295 (r2) | **User's call, 2026-11-08.** One mechanical pass beats two, and a mixed API — some `Expected`, some `optional` — is a wart every caller sees until the second pass lands. The counter-argument (295 signatures before a consumer branches) loses because the signatures are mechanical, while the wart is not | — |
| D2 | C++20 with a minimal `cdsmodel::Expected<T>` | bump to C++23; `std::variant<T, Error>`; `optional` + out-param | The bump was mine, not the project's, and nothing measured requires it. A strict-subset API gives this debt a **defined end date**: the day the project moves to C++23 for its own reasons, it becomes a type alias and a deletion. `variant` has no `and_then` and 149 propagation sites want one. The out-param is upstream's pattern, which this port removed | The project moves to C++23 for an unrelated reason |
| D3 | `Error` holds an owned `std::string` | `string_view` into static text; fixed buffer; kind only | 452 upstream messages interpolate runtime values (`"Guess (%2.6e) is out of range"`). Context is the whole value | The allocation appears in a profile on a hot path |
| D4 | Message text follows upstream's wording where upstream has one | fresh wording | Keeps the port diffable against the oracle and preserves field-recognisable diagnostics | — |
| D5 | Propagation forwards the original error unchanged; it is not a kind | a `propagated` kind wrapping an inner error | 8 of 12 solver diagnostics are the same forwarded message. A wrapper kind at each of 149 layers buries the one reason the caller needs | A caller needs the forwarding chain, not just the origin |
| D7 | `std::optional` stays legal wherever `nullopt` means **legitimately absent**; `Expected` is for **failed**. The test is semantic, not syntactic |  convert every `optional` to `Expected`; leave A5 as first written | **Drift, 2026-11-08.** A5 as ratified said "no function returns `std::optional`", and a lambda is a function. But `date_from_string`'s `to_month_name` returns `nullopt` to mean *"not a month name — try the other interpretation"*, which is a parse alternative, not a failure. Converting it would have required inventing error messages for non-errors. Measured: 9 function-local lambdas converted, of which 7 are genuine errors (4 solver objectives, 2 overflow refusals, 1 unsupported conversion) and 2 are parse predicates. Found again at class scope while migrating: `ZeroCurveBuilder::find_exact` returns "no exact date match", which is a lookup miss like `map::find`, not a failure — so the rule had to be restated semantically rather than as "lambdas are exempt". Reverted: 2 parse predicates + `find_exact` | a parse predicate's "did not apply" needs to reach a caller, at which point it is an error after all |
| D6 | A5 becomes a repo `CLAUDE.md` rule once this lands | leave it spec-level | `CLAUDE.md` now exists (promoted 2026-11-08) and this is exactly its tier: machine-checked, permanent, project-wide | at step 5, per the method |

## Open questions before ratification

1. ~~Scope: 341 or 46?~~ **Resolved 2026-11-08: all 341 (D1).**
2. ~~C++23 or C++20?~~ **Resolved: C++20 (D2).**
3. Non-blocking — `Expected<T>` in `cdsmodel::` or `cdsmodel::detail::`.
   **Assumption: `cdsmodel::`**, since it appears in public return types and
   hiding it in `detail` would misstate its visibility.
4. Non-blocking — `source_location` now or with nanobind. **Assumption:
   later**, nothing formats a traceback yet.

## Progress

| ID | Status | Oracle result | Note |
|----|--------|---------------|------|
| A1 | **pass** | 7/7 unit tests; the identical call sequence compiles against BOTH `cdsmodel::Expected` and `std::expected` at `-std=c++23` | the subset property is what gives D2's debt its end date |
| A2 | **pass** | 760/760 debug · 754/754 release · 754/754 asan, all with the oracle; **445/445 without it** (floor 438 → 445) | zero behavioural diffs across 341 migrated sites |
| A3 | **pass** | reversed bounds → `bad_argument`, no bracket → `no_root`, iterations → `no_convergence`, each matched to the message `rtbrent.c` logs at that site; all three messages mutually distinct | the conflation named in `root_find.hpp`'s own doc comment is gone |
| A4 | **pass** | objective's `unsupported` + "the curve has no point at 2029-06-01" arrives at `root_find_brent`'s caller byte-identical; 3-deep curve refusal names ACT/365F at the surface | D5 held: no wrapper kind invented |
| A5 | **pass** | `verify.py error-channel`: no namespace-scope function returns `std::optional`, with **2 named D7 exemptions** | exemptions listed individually, so a third is a deliberate act |
| A6 | **pass** | all 4 kinds produced by real calls in one test; none dead | |
| A7 | **pass** | 194 errors, **171 distinct messages** (88%), 0 empty | the distinct count is asserted, not just non-emptiness |

### What the slice actually cost

341 sites. 131 propagation sites forwarded mechanically, 198 origins authored
by hand, 12 in `root_find` done first to set the standard.

**Five false positives from my own mechanical pass**, each caught by the
compiler rather than by review, and each the same underlying mistake — a
syntactic rule standing in for a semantic one:

| what the pass did | why it was wrong |
|---|---|
| converted `to_month_name` / `to_number` | parse alternatives, not failures (D7) |
| converted `find_exact`, `MatchRange::exact` | lookup misses, like `map::find` (D7) |
| protected `leg_pv` and `counted` as fields | both are LOCALS of the same name |
| converted `Outcome::root` in the differential harness | holds the UPSTREAM result, which has no message to carry |
| rewrote `if (!bracketed)` as `unexpected(bracketed.error())` | `bracketed` is a `bool` |

The lesson is D7's final form: **the distinction is semantic, not syntactic.**
A rule phrased as "lambdas are exempt" was wrong twice before it was phrased
as "`optional` means absent, `Expected` means failed".
