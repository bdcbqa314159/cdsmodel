# Porting the ISDA CDS Standard Model 1.8.3 to C++20

Measured facts, the recurring legacy patterns, and the order of attack.
Everything here came from inspecting the upstream tree; nothing is assumed.

## What we are dealing with

| metric | value |
|---|---|
| upstream C | 21,377 lines, 54 `.c` + 1 `.cpp` |
| public headers | 49, flat in `lib/include/isda/` |
| distinct `Jpmcds*` entry points | 126 |
| functions returning `int` status | 98 |
| functions returning owning pointers | 64 |
| `goto done` occurrences | 472 |
| exported mutable globals | 1 (`cache`) |
| build system | 12 hand-written makefile fragments, 4 platforms |
| test suite shipped | none (the README references a `tests/` dir that is absent) |

## The six legacy patterns, and what replaces each

### 1. `goto done` as RAII — 472 sites
Every non-trivial function allocates, jumps to a single `done:` label on any
error, frees, and returns a status code. This is a correct and disciplined
idiom in C; it is also 472 opportunities to leak by adding an early return.

Replacement: destructors. The cleanup block disappears entirely rather than
being translated.

### 2. Status codes plus out-parameters — 98 functions
`int JpmcdsFoo(in..., double *out)` returning `SUCCESS`/`FAILURE` (0 / -1),
with the real answer written through a pointer. The caller cannot tell an
untouched out-param from a legitimately computed zero.

Replacement: return the value. `std::optional<T>` where failure carries no
information, `std::expected<T, Error>` where a diagnostic matters. The port
starts with `std::optional` and tightens to `std::expected` once the error
taxonomy is known — inventing it upfront would be guessing.

### 3. The preprocessor as a template engine — 2 files
`bsearch.inc` and `lintrp1.inc` are each `#include`d with `X_TYPE` `#define`d
to a concrete type, generating one copy of the algorithm per type. The
expansion is invisible to debuggers, to clangd, and to grep.

Replacement: one constrained template. Done for `bsearch` (see
`include/cdsmodel/bsearch.hpp`); `lintrp1` follows the same shape.

### 4. Mutable global state — the real hazard
| where | what | consequence |
|---|---|---|
| `buscache.c:62` | `THoliday *cache = NULL;` — **no `static`** | external linkage, exported from the archive as `_cache`; a process-wide holiday cache with no locking. Two threads pricing with different calendars race. Its lazy initialisation inside `holidayFind` is a second race: an unsynchronised `if (cache == NULL)` under which two threads both construct the built-ins and both assign. **Eliminated in the port** — see below. |
| `cerror.c` | 8 statics: `FILE*` log handle, path buffer, callback pointer, flags | logging is process-global; `JpmcdsErrMsgOff()` silences every caller at once |
| `bsearch.c:19` | `bSearchCheckOrder_g` | a global flag that silently switched every binary search between checked and unchecked |
| `yearfrac.c:172` | `static char privDccString[33]` | a function-local static scratch buffer inside `JpmcdsStringToDayCountConv`, so parsing a convention string is not thread-safe either |

Replacement: ownership. The cache becomes a value the caller holds and passes;
errors become return values; the flag becomes a choice of function. The
`bsearch` global is already gone.

This is the pattern that makes the library un-threadable today, and the one
with the most value in removing.

### 5. Hand-rolled allocation — `NEW` / `NEW_ARRAY` / `FREE`
Macros over `JpmcdsMallocSafe` / `JpmcdsFreeSafe`, 52 allocations against 74
frees. Replacement: `std::vector`, `std::string`, and ordinary value
semantics. `cmemory.c` is deleted, not ported.

### 6. Primitive obsession in the core types
| upstream | meaning | C++20 |
|---|---|---|
| `typedef long int TDate` | a day number | `std::chrono::sys_days` |
| `typedef int TBoolean` | a boolean | `bool` |
| `long dayCountConv` | an enum smuggled as a long | `enum class DayCount` |
| `char badDayConv` | an enum smuggled as a char (`'M'`, `'F'`, …) | `enum class BadDayConv` |
| `TCurve { int n; TRatePt *fArray; ... }` | owning array + count | `std::vector<RatePoint>` |

`TDate` is the highest-leverage one: it is the argument or return of most of
the 126 entry points, and `long` lets you add a date to a date. Its epoch must
be pinned by a differential test before anything above it is ported.

## Dependency layering (fan-in, measured)

`cerror.h` 45 · `macros.h` 42 · `convert.h` 26 · `cgeneral.h` 24 ·
`ldate.h` 22 · `tcurve.h` 16 · `bastypes.h` 16 · `date_sup.h` 13 ·
`cmemory.h` 12 · `datelist.h` 11

The two headers almost everything depends on, `cerror.h` and `cmemory.h`, are
both infrastructure that the C++ port **deletes** rather than ports. That is
the lucky part of this migration: the widest dependencies are the ones that
evaporate.

## Order of attack

Bottom-up, each layer fully differential-tested against `cds_reference`
before the next begins. The build stays green and hybrid at every commit —
there is never a big-bang integration.

| # | layer | upstream files | notes |
|---|---|---|---|
| 0 | harness | — | ✅ done: CMake, GoogleTest, two-tier verification, oracle optional |
| 1 | search / interp | `bsearch` ✅, `lintrp1`, `linterpc`, `interpc` | `interpc` needs a strided overload — one call site, `interpc.c:560`, `sizeof(TRatePt)` |
| 2 | dates | `cdate` ✅, `dateconv` ✅, `ldate` ✅ (except calendar-dependent parts), `date_sup`, `dateadj` | epoch pinned: TDate 0 = 1601-01-01, proleptic Gregorian, verified every 13th day to 2200 |
| 3 | day counts | `ldate` (DCF part) ✅, `yearfrac` ✅, `convert` | layer complete except `convert.c`, which is date intervals rather than day counts and moves to layer 5 |
| 4 | calendars | `buscache` ✅ (type + predicates), `busday`, `badday` | **the `cache` global is gone** — a calendar is now a value the caller owns. 2,350 lines, the biggest single layer; business-day arithmetic and bad-day conventions still to come |
| 5 | intervals / schedules | `convert` ✅ (interval parts), `schedule`, `datelist`, `dtlist`, `cxdatelist`, `stub` | `DateInterval` done — the type the whole schedule layer is built on |
| 6 | curves | `tcurve`, `zerocurve`, `cxzerocurve`, `zcall`, `zcswap`, `gtozc` | 3,330 lines; `TCurve` → `std::vector` |
| 7 | cashflows | `cashflow`, `streamcf`, `fltrate`, `cfinanci` | |
| 8 | CDS legs | `feeleg`, `contingentleg`, `timeline`, `defaulted` | |
| 9 | solver | `rtbrent` | callback → template parameter / `std::function` |
| 10 | top-level API | `cds`, `cdsone`, `cdsbootstrap` | the surface users actually call |
| 11 | deleted outright | `cmemory`, `cerror`, `lprintf`, `lscanf`, `cfileio`, `strutil` | ~2,900 lines replaced by the standard library |

Layer 11 is worth stating plainly: roughly **14% of the codebase is
infrastructure that C++20 provides for free**. The migration gets smaller as
it proceeds.

## Verification standard

A module is ported when it is differentially equal to its ancestor, not when
it looks right.

The upstream sources cannot be redistributed — cdsmodel.com gates them behind
accepting the licence and supplying an email address — so verification runs
in two tiers.

**Tier 2, live differential (local only).** Both libraries are linked into
`cdsmodel_differential_tests`. Assertions call the port and the C side by
side, in-process, and compare. The oracle cannot drift because it is not a
recorded value — it is the original code, executing. `oracle_example`
additionally pins upstream's own example output byte-exactly.

**Tier 1, recorded vectors (everywhere, including CI).** The differential
sweep's answers are generated into `tests/golden/<module>_cases.hpp` by
`tools/gen_<module>_golden.cpp` and replayed by `tests/<module>_test.cpp`.
Licence section 4(b) exempts "data derived from calculations that have been
attained by using the ISDA CDS Standard Model", so the numbers ship even
though the code that produced them does not. Committing the generator keeps
them reproducible rather than magic.

A module needs **both**. Tier 2 alone is verified only on the machine that
has the oracle; tier 1 alone can go stale against a behaviour nobody swept.

Current coverage: 142 tests with an oracle present, 95 without, the latter
replaying 6,631 recorded reference cases. All of it also runs clean under
AddressSanitizer and UndefinedBehaviorSanitizer, with UBSan findings **fatal**
for port code (see PORT-5 below).

Where the port deliberately differs from upstream, the difference is named in
a comment at the site and listed below.

### Deliberate differences so far

| site | upstream | port | why |
|---|---|---|---|
| `bsearch` run-of-equals loop | unbounded `while (x[lo] == x[hi]) hi++`, safe only by argument | bounded by `n` | upstream's safety is a comment; this makes it structural. Identical on all valid input. |
| `bsearch` index type | `int`, allows `hi = mid - 1` to go negative | `std::ptrdiff_t` | `std::size_t` would underflow here and read out of bounds. Signedness is load-bearing. |
| `bsearch` `skip` stride | byte stride plus a power-of-two shift optimisation | contiguous `std::span` | the 2009 multiply-to-shift trick is strictly worse than what the compiler now emits. A projection overload arrives when layer 1 reaches `interpc`. |
| order-check selection | `bSearchCheckOrder_g` global flag | `bracket` vs `bracket_checked` | removes process-wide state that changed every caller's behaviour |
| the holiday cache | a process-wide linked list reached by name, with `JpmcdsHolidayListFromCache` returning a borrowed pointer into it | `Calendar` is a value the caller owns and copies | the headline fix of this layer, and it is not a lock — it is an owner. Nothing to invalidate, nothing to synchronise, and `JpmcdsHolidayEmptyCache` has no analogue because there is no cache to empty. |
| unsorted holiday lists | rejected by `verifyHolidayList` | rejected | the port does **not** quietly sort. `JpmcdsHolidayListIsHoliday` `bsearch`es the list, so upstream must reject unsorted input, and accepting what upstream refuses would be a behaviour change. |
| holidays falling on a weekend | stripped from the stored list by `verifyHolidayList`, so `is_holiday` reports false for a Saturday holiday the caller supplied | identical | observable through the stored list as well as the predicate |
| the built-in `"NONE"` calendar | means "no holiday dates", **not** "no non-business days" — Saturday and Sunday are still weekends | `Calendar::none()`, documented | the name is misleading enough to warrant a test asserting the behaviour against upstream |
| day count convention | `long method` carrying one of six `#define`d codes | `enum class DayCount` | any `long` was accepted at the call site and only rejected at the bottom of a `switch` |
| `JPMCDS_ACT_ACT` | `#define`d to the same value as `JPMCDS_ACT_365`, so the two are indistinguishable | `kActAct` is an alias of `DayCount::Act365`, not a separate enumerator | preserved, not fixed — splitting them would change results. The conflation is now visible in the type rather than buried in a macro. |
| ACT/365 leap weighting | walks years by adding 365 or 366 to a running date and re-deriving the year from it | intersects the period with each calendar year directly | same arithmetic, no running date to drift. Verified bit-identical (`EXPECT_DOUBLE_EQ`) across 3,456 pair/convention combinations and every start day of a full leap cycle. |
| `JpmcdsFormatDayCountConv` | returns `char *` into a string literal, and writes to the global error log as a side effect of formatting | returns `std::string_view`, logs nothing | writing through the returned pointer was undefined; a formatter should not have side effects |
| convention string parsing | substring matching, so `"GARBAGE365"` is ACT/365 and `"A360"` is ACT/360 | identical, deliberately | observable behaviour at a public API. Tightening it would be a breaking change disguised as a fix. A strict parser can be added alongside. |
| `"NONE"` round trip | parses to code `0`, which the formatter has no `case` for, so it formats back as `"unrecognized"` | identical, deliberately | an upstream asymmetry, recorded and tested rather than silently repaired |
| `TDateInterval.flag` | third field selecting what the interval is measured from | dropped | `JpmcdsMakeDateInterval` always sets it to 0 and nothing in the library reads it. A differential test asserts it stays 0, so if that ever changes we find out. |
| interval period types | fourteen documented; `JpmcdsMakeDateInterval` accepts thirteen; `JpmcdsDtFwdAny` can advance by seven | the seven advanceable ones | upstream lets you construct an interval it then refuses to use. A value you can build and not use is not a feature; the rest arrive with `schedule.c` if anything consumes them. |
| interval digit parsing | unbounded copy into a 32-byte stack buffer | bounded, rejects >31 digits | a refusal to reproduce upstream behaviour, because that behaviour is a stack buffer overflow. See UPSTREAM-4. |
| interval digit conversion | `atoi`, whose overflow is undefined; `"9999999999M"` becomes 1410065407 months and a 20-digit run becomes `prd = -1` (also its `FAILURE` value) | rejects any run that would not fit an `int` | the second refusal to reproduce undefined behaviour. **Found by auditing the port, not by differential testing** — both sides were producing wrapped garbage, so they agreed. See "Auditing the port itself" below. |
| days-to-weeks formatting | week branch exists but is unreachable (missing `break`) | unreachable behaviour reproduced, dead intent not | `14D` formats as `"14D"`, never `"2W"`. It is observable output. See UPSTREAM-5. |
| `JpmcdsDate` failure | returns `-1`, a value in the date space | `std::optional<Date>` | `-1` was safe only because valid TDates are non-negative — exactly the coupling optional removes |
| `JpmcdsDaysInMonth` | `(long year, long month)`, month unvalidated, indexes a 13-element table | `days_in_month(std::chrono::year_month)` | a 13th month becomes unrepresentable instead of an out-of-bounds read |
| `gDateCacheArray` | 1,500-entry hardcoded table of month starts, 1995–2119, plus a slow path for anything outside | nothing | `std::chrono` covers the whole proleptic Gregorian range. 377 lines deleted, and the dates outside 1995–2119 stop taking a different code path |
| `TDate` arithmetic | `typedef long int`, so date + date compiles | `std::chrono::sys_days` | `sys_days + sys_days` does not compile; `sys_days - sys_days` yields `days` |

## Upstream defects found

Porting against a live oracle under sanitizers surfaces bugs in the original.
**None of them are fixed in upstream, and none ever will be here**: editing
`isda_cds_model_c_v1.8.3/` would destroy the only independent check the
project has. Anyone using the official C library still has all of these.

### Register

| id | defect | severity | in the port | how |
|---|---|---|---|---|
| [UPSTREAM-1](#upstream-1--out-of-bounds-read-in-jpmcdsmdytodate-dateconvc657) | month validated *after* it indexes a 13-element table | low — UB on a reject path | cannot occur | validation precedes lookup, and `std::chrono::month` makes a 13th month unrepresentable |
| [UPSTREAM-2](#upstream-2--jpmcds_default-collides-with-failure) | `JPMCDS_DEFAULT` == `FAILURE` == `-1` | design | cannot occur | `std::optional` throughout; no sentinel inside the value space |
| [UPSTREAM-3](#upstream-3--lowercase-y-advances-a-date-quarterly-not-annually) | lowercase `'y'` advances quarterly, not annually | **high** — silent mispricing | cannot occur | `make_date_interval` upper-cases and normalises before any arithmetic |
| [UPSTREAM-4](#upstream-4--stack-buffer-overflow-in-jpmcdsstringtodateinterval) | stack buffer overflow from string input | **high** — memory safety | **fixed, deliberate divergence** | digit count bounded; input over 31 digits rejected |
| [UPSTREAM-5](#upstream-5--days-never-format-as-weeks-missing-break) | days never format as weeks (missing `break`) | cosmetic | **reproduced on purpose** | observable output; fidelity beats tidiness |

Three of these were not *fixed* so much as made unrepresentable. Nobody wrote
a patch for UPSTREAM-1, -2 or -3 — the types stopped permitting them. A month
that cannot be 13, a result that cannot carry a sentinel, a period type
normalised at construction. That is the strongest single argument for the port
existing: three of five defects died as a side effect of choosing better types.

UPSTREAM-4 is the only defect the port consciously declines to reproduce.
Faithfully reproducing a buffer overflow is not fidelity.

UPSTREAM-5 is the only one deliberately kept, because its output is observable
by callers and abbreviating `"14D"` to `"2W"` would be a behaviour change
dressed up as a fix.

### Not yet reported to ISDA

Each entry below carries a reproducer, a sanitizer trace where relevant, and
its reachability conditions, so the register is the raw material for a report.
Reporting has not happened yet. UPSTREAM-3 and UPSTREAM-4 are the two worth
leading with: a silent annual-to-quarterly substitution, and a stack write
driven by user-entered spreadsheet input.

### Full write-ups

Each defect's mechanism, evidence, sanitizer trace, reachability conditions
and fix status live in **[defect-report.md](defect-report.md)**, which is the
canonical account. It is published at
<https://bdcbqa314159.github.io/cdsmodel/>.

Keeping the detail in one place means the register above is a summary and not
a second, drifting copy. `python verify.py reports` checks that every id named
here appears in both the report and the published page.

## Auditing the port itself

Differential testing proves the port agrees with the oracle on the inputs
swept. It cannot prove three other things, so those need separate attention:

1. **Agreement on undefined behaviour is not agreement.** Where both
   implementations are undefined, a differential test passes while both
   produce garbage. This is how the `atoi` overflow above survived: the port
   accumulated digits into an `int` with signed overflow, upstream did the
   same through `atoi`, and the suite was green because neither side was
   tested with a ten-digit interval. UBSan on the *port* found it in seconds;
   no amount of differential testing would have.
2. **Coverage is only what was swept.** 6,631 recorded cases sounds like a
   lot, but every one of them was chosen by hand. The gaps are invisible by
   construction.
3. **Transitive includes.** A public header that compiles only because some
   other header was included first is broken for any consumer who includes it
   alone. The `cdsmodel_header_selftest` target in `CMakeLists.txt` compiles
   each public header on its own to catch this. Note it does **not** catch
   much on Apple libc++, which satisfies most of these transitively — its
   value is on the Windows and Linux legs of CI.

`tests/adversarial_test.cpp` is the harness for the first two. It asserts
**invariants** rather than agreement — totality, round trips, postconditions —
and feeds hostile input rather than plausible input, from a fixed seed so a
failure is reproducible and the suite cannot go intermittently red.

### Port-side findings

Defects in **this port**, found by auditing it rather than by comparing it to
the oracle. Numbered separately from the UPSTREAM series: these are ours.

| id | defect | found by | status |
|---|---|---|---|
| PORT-1 | signed overflow accumulating interval digits (`periods * 10 + digit`); `"9999999999M"` was UB that happened to yield 1410065407 | UBSan, run by hand on the port | fixed — check before multiplying |
| PORT-2 | signed overflow *scaling* interval periods (`periods * 12` for `'Y'`, `* 28` for `'U'`) — a different site from PORT-1 | the harness | fixed — `detail::scale` rejects |
| PORT-3 | signed overflow in `normalize_ymd` (`month - 1` at `INT_MIN`, `year + carry`) and in `advance`'s days path (`sys_days + days{periods}`) | the harness | fixed — arithmetic widened once and range-checked, rather than guarded per operation |
| PORT-4 | `from_tdate` narrowed silently outside an undocumented domain: `from_tdate(5'000'000'000)` round-tripped to 705032704 | probing extremes after PORT-3 | fixed — `kMinTDate`/`kMaxTDate`/`is_representable_tdate`, derived from the epoch |
| PORT-5 | **UBSan findings were not fatal in CI**, so every pure-UB finding had been silently ignored since the sanitizer job was added | noticed when PORT-2 "passed" while printing a runtime error | fixed — `CDSMODEL_FATAL_UBSAN`, on for the asan preset |

**PORT-5 is the one that mattered most.** UndefinedBehaviorSanitizer prints
and continues by default, exiting 0. The sanitizer job only ever failed on
AddressSanitizer errors, which is why UPSTREAM-1 and UPSTREAM-4 surfaced —
both are *also* ASan errors — while pure undefined behaviour like signed
overflow was invisible. PORT-1 through PORT-4 were all sitting in a green
suite. The flag is applied to the port and its tests only, never to
`cds_reference`: upstream's undefined behaviour is catalogued, not fixed, and
must stay observable without aborting the run.

Two things worth taking from this:

* **A sanitizer that cannot fail the build is documentation, not a gate.**
  Check that a new sanitizer actually aborts before trusting it.
* **PORT-2 and PORT-3 are in code written to fix PORT-1.** Guarding
  individual arithmetic operations is easy to get subtly incomplete; the
  version that holds up widens once and range-checks at the boundary.

## Not doing

- Not touching `isda_cds_model_c_v1.8.3/`. It is the oracle; editing it
  destroys the only independent check we have.
- Not committing it either. It is gitignored. Anyone reproducing this work
  fetches their own copy from cdsmodel.com under the terms ISDA sets.
- Not porting the Excel XLL (`excel/`). It is Win32-only by construction and
  the cross-platform spreadsheet layer replaces it.
- Not optimising anything before it is correct and differentially equal.
