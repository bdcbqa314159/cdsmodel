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
| 4 | calendars | `buscache` ✅, `busday` ✅, `badday` → layer 6 | **layer complete.** No global state left. `BusinessDaysDiff` transcribed, `AddBusinessDays` reimplemented — see "Transcribe or reimplement". `badday.c` holds `TBadDayList`, used only by the zero-curve swap code |
| 5 | intervals / schedules | `convert` ✅ (interval parts), `dtlist` ✅, `schedule`, `datelist`, `cxdatelist`, `stub` | `DateInterval` and the date list operations done. The date lists are the first module to **correct** upstream rather than reproduce it — see Policy |
| 6 | curves | `tcurve`, `zerocurve`, `cxzerocurve`, `zcall`, `zcswap`, `gtozc` | 3,330 lines; `TCurve` → `std::vector` |
| 7 | cashflows | `cashflow`, `streamcf`, `fltrate`, `cfinanci` | |
| 8 | CDS legs | `timeline` ✅, `contingentleg` ✅, `feeleg` ✅, `defaulted` | **both legs of a CDS price bit-identically.** Protection leg across 720 trades, premium leg across 576 configurations — see Decisions 9 and 10 in [journal.md](journal.md) |
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

Current coverage: 242 tests with an oracle present, 161 without, replaying
18,142 recorded cases. 600 of those are recorded *intent* rather than
upstream answers, for the date list module that corrects upstream. All of it also runs clean under
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
| the fee leg schedule | three parallel arrays (`accStartDates`, `accEndDates`, `payDates`) sharing one `nbDates`, with nothing tying them together | one `std::vector<FeePayment>` | a caller could hand upstream three arrays of different lengths and it would read past the end of two of them. The port makes a mismatched schedule unrepresentable. |
| the holiday cache | a process-wide linked list reached by name, with `JpmcdsHolidayListFromCache` returning a borrowed pointer into it | `Calendar` is a value the caller owns and copies | the headline fix of this layer, and it is not a lock — it is an owner. Nothing to invalidate, nothing to synchronise, and `JpmcdsHolidayEmptyCache` has no analogue because there is no cache to empty. |
| unsorted holiday lists | rejected by `verifyHolidayList` | rejected | the port does **not** quietly sort. `JpmcdsHolidayListIsHoliday` `bsearch`es the list, so upstream must reject unsorted input, and accepting what upstream refuses would be a behaviour change. |
| holidays falling on a weekend | stripped from the stored list by `verifyHolidayList`, so `is_holiday` reports false for a Saturday holiday the caller supplied | identical | observable through the stored list as well as the predicate |
| the built-in `"NONE"` calendar | means "no holiday dates", **not** "no non-business days" — Saturday and Sunday are still weekends | `Calendar::none()`, documented | the name is misleading enough to warrant a test asserting the behaviour against upstream |
| bad day convention | `long badDayConv` holding a character code, `((long)'F')` | `enum class BadDayConv : char` | same primitive-obsession fix; the character values are kept so interop is a cast |
| business-day rolling | unbounded `do { ... } while (aHoliday)` | bounded by `7 * (holidays + 2)`, derived | upstream never terminates for a calendar with no business weekday (UPSTREAM-6). The bound is **derived, and the first attempt at it was wrong** — see below. |
| business-day counting | two 7×7 lookup tables that are deliberately **not** negatives of each other | the same two tables, transcribed | the only module ported by transcription rather than reimplementation. See "Transcribe or reimplement" below. |
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

| UPSTREAM-7 | date list truncation ignored when fewer than two dates survive, in two implementations that fail in opposite directions (39% of a 600-case sweep) | **high** — silently wrong | not yet ported | the two upstream behaviours contradict each other, so "preserve upstream" is not available |

| UPSTREAM-8 | `!price > THRESHOLD` precedence bug disables a validation guard, four sites on the pricing path | low — a guard that does not guard; no mispricing demonstrated | **resolved** — the port guards a non-finite result instead of a proxy threshold | `1e-100` stood in for "the logarithm will misbehave"; with the integrals built that is directly testable. The honest guard and upstream's broken one fire on identical inputs across hazard 0.001 to 200. Applied at all four sites: two in the protection leg, two in the premium leg. |
| UPSTREAM-9 | an error message with two `%s` conversions formatted with one argument, so `vsprintf` dereferences an unpushed varargs slot | medium — UB on a reject path, reachable from any caller passing a malformed stub LENGTH field | **cannot occur** | `parse_stub_method` returns `nullopt` and formats nothing. Manifestation is caller-dependent: it faults in a minimal frame and not in the test binary, so the evidence is the source, not a crash |
| UPSTREAM-10 | `JpmcdsWriteToLog` formats with unbounded `vsprintf` into `char buffer[4096]`; 67 of 452 message sites interpolate a caller-supplied string | medium — deterministic caller-controlled stack overflow above ~3.9KB, but only once a host has called `JpmcdsErrMsgOn` | **cannot occur** | there is no global error log to format into; a failure is an empty `std::optional` and a caller string is never copied. Absent structurally, not patched |

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

### Transcribe or reimplement

Two modules faced the same choice and went opposite ways, which is worth
stating because the reasoning generalises.

**ACT/365 (`day_count.hpp`) was reimplemented.** Upstream walks years by
adding 365 or 366 to a running date and re-deriving the calendar year. The
port intersects the period with each calendar year instead. The *intent* was
recoverable — "days in leap years over 366, the rest over 365" — so a cleaner
implementation of that intent is a legitimate port, and equivalence was then
proven: bit-identical across 3,456 pair/convention combinations and every
start day of a full leap cycle.

**`BusinessDaysDiff` (`business_days.hpp`) was transcribed.** Upstream's own
comment says why:

> The values of the table were chosen so that the current ALIB behaviour was
> left unchanged. Note that because fwdDiffTable is not identical to
> (-1 x bwdDiffTable), swapping fromDate and toDate in the call to this
> function does not simply reverse the sign of the result.

There is no intent to recover. The answer is *defined by* the tables, and the
tables disagree with each other: forward Monday→Saturday is 4, backward is
−5. A cleaner implementation would be a different function. So the port
carries both tables verbatim, and a test asserts the asymmetry still exists
upstream — if a future release makes the tables mirror each other, that test
fails and the transcription can be revisited.

**`AddBusinessDays` was reimplemented**, which is the same file as
`BusinessDaysDiff` going the other way. Upstream splits it across three code
paths — a trivial case, a table-driven fast path for forwards under standard
weekends, and a general stepper — but those are optimisations of one idea:
step in the given direction counting business days. The port is a bounded
walk.

That claim was checked rather than assumed. Upstream's `offsetTable` gives
−1 for Saturday and −2 for Sunday at `n % 5 == 0`, normalising a weekend
start back to the preceding Friday, which looks like a contradiction until you
work it through: five business days from a Saturday is `Sat + 7 − 1 = Friday`,
and counting Mon–Fri from that Saturday also lands on Friday. The table
encodes the walk. The differential sweep then confirmed it across 28 start
days, 15 counts and 7 weekend sets.

**The rule:** reimplement when the intent is recoverable and equivalence can
be proven; transcribe when the behaviour *is* the specification. Guessing
which one you are in is how a port silently changes results. Both halves of
that rule now have a worked example in the same upstream file.

Practical consequence: `business_days_between` is **not antisymmetric**, so
the adversarial harness must not assume for it what it rightly assumes for
the day count conventions.

### Port-side findings

Defects in **this port**, found by auditing it rather than by comparing it to
the oracle. Numbered separately from the UPSTREAM series: these are ours.

| id | defect | found by | status |
|---|---|---|---|
| PORT-1 | signed overflow accumulating interval digits (`periods * 10 + digit`); `"9999999999M"` was UB that happened to yield 1410065407 | UBSan, run by hand on the port | fixed — check before multiplying |
| PORT-2 | signed overflow *scaling* interval periods (`periods * 12` for `'Y'`, `* 28` for `'U'`) — a different site from PORT-1 | the harness | fixed — `detail::scale` rejects |
| PORT-3 | signed overflow in `normalize_ymd` (`month - 1` at `INT_MIN`, `year + carry`) and in `advance`'s days path (`sys_days + days{periods}`) | the harness | fixed — arithmetic widened once and range-checked, rather than guarded per operation |
| PORT-4 | `from_tdate` narrowed silently outside an undocumented domain: `from_tdate(5'000'000'000)` round-tripped to 705032704 on macOS | probing extremes after PORT-3 | fixed — `kMinTDate`/`kMaxTDate`/`is_representable_tdate`, derived from the epoch |
| PORT-6 | the first test for PORT-4 asserted macOS's domain as a literal, and failed on both other platforms for two *different* reasons | the 3-OS CI matrix | fixed — the test asserts properties of the bounds, never numbers |
| PORT-7 | `add_business_days` negated `LONG_MIN` to get a magnitude, which is undefined and silently returned the input unchanged | the test written to check absurd counts | fixed — rejected explicitly before any negation |
| PORT-8 | the same function computed its "room available" as `to_tdate(from) - kMinTDate` in a signed type, which overflows where `kMinTDate` sits near the minimum of `days::rep`. Ordinary **backward** offsets were rejected | the Linux leg of CI — macOS and Windows both passed | fixed — computed unsigned, which is exact because `kMinTDate <= t <= kMaxTDate`, plus a stated offset limit |
| PORT-9 | `cds_contingent_leg` omitted the one-day shift that `JpmcdsContingentLegMake` applies when `protectStart` is set, costing exactly one day of protection — a relative price error around 2e-5 | the FIRST test to compare a composed price. The protection leg's own differential suite could not see it: that suite fed a pre-built leg to both sides, so leg CONSTRUCTION was never compared | fixed — the shift applied in the factory, plus a construction differential test so the gap is closed at its source rather than only one layer up |
| PORT-5 | **UBSan findings were not fatal in CI**, so every pure-UB finding had been silently ignored since the sanitizer job was added | noticed when PORT-2 "passed" while printing a runtime error | fixed — `CDSMODEL_FATAL_UBSAN`, on for the asan preset |

**The upfront charge completes the pricing chain, and reproduces ISDA's own
published numbers.** The example program shipped in the distribution prints
three upfront charges; the port matches all three, in CI, with no oracle. Its
discount curve comes from `JpmcdsBuildIRZeroCurve` -- the single entry point of
the unported interest-rate stack -- so the curve is recorded as input data
(derived data, licence 4(b)), which is legitimate because the CDS model takes a
discount curve as an input.

**A caveat worth publishing, not a defect.** The upfront charge SATURATES in
the quoted spread: measured on a five-year contract at recovery 0.4,
`d(charge)/d(spread)` falls from 4.98 at 25bps to 0.22 at 3600bps, 0.053 at
5000bps, and **-0.008 at 7200bps**. Past roughly 5000bps the map is not
injective, so `JpmcdsCdsoneSpread` cannot uniquely invert a charge at
distressed quote levels and no implementation could. The port therefore
asserts the contract the solver actually guarantees -- the returned spread
reproduces the target charge to within `f_tolerance` -- and keeps the
spread-recovers-spread round trip as a separate test restricted below 1000bps,
where the derivative exceeds 2. Nothing in upstream documents this.

`UpfrontDates` exists because this is the published entry point and upstream
passes four adjacent `TDate`s that do different jobs. Two are independently
load-bearing and easy to conflate: the benchmark CDS start (which implies the
flat curve) and the traded contract start (upstream's comment: "cds can start
from past"). A test asserts moving either changes the answer. Also preserved:
upstream's `payAccruedAtStart` is passed straight through as `isPriceClean`,
two names suggesting opposite polarity that the code treats as identical.

**The bootstrap is the first module verified contract-first.** Its defining
property -- a curve bootstrapped from quoted benchmarks reprices those
benchmarks to zero -- is asserted before any comparison against upstream,
needs no oracle, and holds on every platform. 1,488 repricings within 1e-9.
The differential comparison sits behind it and is still bit-exact: 588 solved
rates equal to the last bit.

Recorded-rate tolerance is **1e-9 absolute, derived**: the solve stops at
`|pv| <= 1e-10`, two platforms may return different spreads both satisfying
that bounded by `2e-10 / |dpv/ds|`, the risky duration is at least about 1 for
the shortest benchmark, and the continuous-to-annual conversion amplifies by at
most about 1.7. Deliberately not the relative 1e-12 of accumulating values: a
solver's output is set by where it stopped, not by how the arithmetic rounded.

**The feasible region is part of the contract.** About a tenth of the
realistic quote space cannot be bootstrapped at high recovery -- at recovery
0.8 a distressed ladder solves four nodes (hazards 0.287, 0.326, 0.375, 0.542)
and fails the fifth, because reaching a 7-year 800bp quote would need a
forward hazard the curve cannot deliver arbitrage-free. Upstream refuses
exactly the same combinations and the 32 infeasible cases are recorded with
`n == 0` rather than omitted. Unlike the root finder's convergence verdict
this one IS portable: "does a root exist in [0, 1e10]" is a property of the
market data, where "did this converge in 100 steps" was a property of the path.

Measured permissiveness, not defects: `guess = coupon/(1-recovery)` is
unguarded, and `recovery == 1` with a zero coupon produces a NaN guess that
passes the solver's bounds check (neither comparison holds for a NaN) and then
fails to bracket -- no wrong answer. Negative recovery is **accepted**, since
an LGD above 1 is coherent arithmetic; measured at -0.5 and -2.0, both
bootstrap with hazards falling as LGD rises.

**The root finder is where the three-tier floating-point standard was expected
to break. The root tier held; the claim that the iteration PATH is portable
did not, and CI is what established both.**

Roots: every smooth objective agrees to relative 1e-12 across macOS, Linux and
Windows. No fourth tier.

Paths: 146 of 10,296 recorded cases take a different number of objective
evaluations on Linux and Windows than on macOS -- including pure polynomials
with no libm in them, worst ratio 1.93x. The cause is floating-point
contraction, which re-rounds the middle of an expression; `-ffp-contract=fast`
is the GCC and Clang default. A single-platform one-ULP probe cannot model
that, passed on all three platforms, and was mistaken for evidence of
reproducibility. The recorded count is therefore asserted in-process (tier 2,
where it is exact) and only as a 4x band in CI.

Resolution: an objective with plateaus determines its root only to the plateau
width. `Quantised` truncates output to 1e-6 and has derivative about 2.83 at
its root, so any x within 3.5e-7 gives an identical value; platforms disagreed
by up to 6.3e-9, well inside one step, and neither is wrong. Objectives declare
a `root_resolution()` and replay widens to it.

The replay suite also verifies the **solver's own contract** --
`|f(root)| <= f_tolerance`, root inside the bracket -- which needs no oracle
and no recorded number, and is the property a caller actually depends on.

**PORT-9 is the most instructive of the nine, because of what did not catch
it.** The protection leg had a passing differential suite with a bit-exact
sweep behind it. That suite built a `TContingentLeg` by hand on the upstream
side and a `ContingentLeg` by hand on the port side, then compared the PV. It
verified the integral thoroughly and verified the **constructor** not at all —
and upstream's constructor lives in `cx.c`, a different file from the PV
routine in `contingentleg.c`, and applies a one-day shift that the port had
skipped.

The generalisation is worth more than the fix: **a differential test that
hands the same pre-built intermediate to both sides has verified nothing about
how that intermediate is produced.** Every such test in this port was written
that way for a reason — building the intermediate by hand is how you get a
controlled sweep — so the gap is structural, not careless. The remedy is to
add an explicit construction comparison wherever a factory exists, which is
now done for the protection leg and the premium leg schedule.

It also says something about the composition layer. `cds.c` contains almost no
arithmetic; it builds two legs and subtracts. That made it look like the
lowest-risk slice in the project, and it immediately found a defect that four
earlier slices had not. **Composition is not glue — it is the first place the
pieces are required to agree about anything.**

**PORT-5 is the one that mattered most.** UndefinedBehaviorSanitizer prints
and continues by default, exiting 0. The sanitizer job only ever failed on
AddressSanitizer errors, which is why UPSTREAM-1 and UPSTREAM-4 surfaced —
both are *also* ASan errors — while pure undefined behaviour like signed
overflow was invisible. PORT-1 through PORT-4 were all sitting in a green
suite. The flag is applied to the port and its tests only, never to
`cds_reference`: upstream's undefined behaviour is catalogued, not fixed, and
must stay observable without aborting the run.

**PORT-6 and PORT-8 are why the three-OS matrix is not ceremony.** Both were
invisible on macOS. PORT-8 is the sharper of the two: it was not a test
asserting the wrong literal but a *functional* bug in the library, rejecting
ordinary backward business-day offsets, and it reached a passing local suite
and a passing Windows build before Linux caught it. The platform difference
in `std::chrono::days::rep` has now produced three separate defects
(PORT-4, PORT-6, PORT-8), which is a strong argument for treating the
representable range as a first-class part of the interface rather than an
implementation detail.

 The representable
domain varies in two independent ways: `std::chrono::days::rep` is 32-bit on
libc++ and 64-bit on libstdc++, and `long` — hence `TDate` — is 32-bit on
Windows and 64-bit elsewhere. Upstream's `typedef long int TDate` inherits
that second one, so **the model's date type is not a stable width across
platforms.** Harmless for day numbers around 150,000, and worth remembering at
any ABI boundary: the C shim and the Python bindings both cross one.

Three things worth taking from this:

* **A sanitizer that cannot fail the build is documentation, not a gate.**
  Check that a new sanitizer actually aborts before trusting it.
* **PORT-2 and PORT-3 are in code written to fix PORT-1.** Guarding
  individual arithmetic operations is easy to get subtly incomplete; the
  version that holds up widens once and range-checks at the boundary.
* **A test that asserts a platform's answer as a literal is a portability
  bug.** PORT-6 was in the test written to prove PORT-4 fixed.
* **Replacing an unbounded loop with a bounded one means getting the bound
  right.** The first version of `roll_to_business_day`'s budget charged one
  day per holiday, which is wrong whenever fewer than seven weekdays trade: a
  holiday on the only trading weekday costs a full week to skip. The
  differential sweep caught it at `weekends = 0x7E`. A guessed bound is a
  hang with extra steps.

### UPSTREAM-6 — unbounded loop for a calendar with no business weekday

`busday.c`, `getNextBusDate`, reached from `JpmcdsHolidayListBusinessDay` and
`JpmcdsMultiHolidayListBusinessDay`.

The walker steps one day at a time with no iteration bound:

```c
    aHoliday = TRUE;
    do
    {   /* Check if it's a holiday first. */
        ...
            if (JPMCDS_IS_WEEKEND (curDate, hl->weekends))
                 curDate += direction;
            else
                 aHoliday = FALSE;
        ...
    }
    while ( aHoliday == TRUE );
```

If the calendar's weekend set covers all seven days, `aHoliday` never becomes
`FALSE` and the loop never exits. Such a calendar is **accepted** —
`JpmcdsHolidayListNewGeneral(NULL, 0x7F)` returns a valid `THolidayList`,
since `verifyHolidayList` has no opinion on the weekend mask.

Measured:

```
calendar with weekends=0x7F constructed: accepted
calling JpmcdsHolidayListBusinessDay with FOLLOW...
--- killed after 5s ---
```

Severity: a liveness failure rather than memory corruption, and it needs an
unusual calendar — all seven weekend bits, which upstream provides no named
constant for. But accepted input that never returns is a real defect, and it
is the kind that takes a pricing service down rather than returning a wrong
number. As it spins, `curDate += direction` would eventually overflow `TDate`
too; the hang simply arrives first.

The port keeps construction permissive, because the predicates are perfectly
well defined for such a calendar (everything is a non-business day), and
makes `adjust` return `nullopt`. `Calendar::allows_business_days()` exposes
the condition so a caller can distinguish "no business day exists" from other
failures.

Only half of this is differentially testable. A test can assert that upstream
still *accepts* the calendar — `BadDayDifferential.Upstream6_AllWeekendCalendarIsAcceptedByUpstream`
does, and will fail if a future release rejects it — but it cannot assert the
hang, because the test would never finish.

### Policy — when the port diverges from upstream

Decided after UPSTREAM-7 forced the question, and applied retroactively to
everything already done. The port had been making this call case by case; the
cases turned out to follow one rule.

**Preserve** when the behaviour is a *convention*: observable at a public API,
not wrong so much as particular, and a caller could reasonably depend on it.

- `"14D"` never abbreviating to `"2W"` (UPSTREAM-5) is a formatting choice.
- The convention-string parser accepting `"GARBAGE365"` is leniency.
- The asymmetric business-day tables **are** the ALIB definition — there is no
  truth they are failing to match.

**Fix** when the behaviour is wrong *by its own contract*: undefined
behaviour, non-termination, or a result that contradicts the documented
intent.

- UPSTREAM-4, a stack buffer overflow. Reproducing it is not fidelity.
- UPSTREAM-6, a loop that never returns.
- UPSTREAM-1, -2, -3, made unrepresentable by the type system rather than
  patched.
- UPSTREAM-7, where both implementations contradict their own header *and*
  each other, so there is no single upstream behaviour available to preserve.

**The condition attached to every fix.** A divergence must be enumerable and
its consequence **measured, not assumed**:

1. a row in "Deliberate differences" above, naming the site;
2. a test asserting the defect still exists upstream, so a future upstream
   release that fixes it makes the divergence visible instead of silent;
3. where the function is on the pricing path, a test that **prices both ways
   and reports whether the number moves**.

Point 3 is the one that is easy to skip and matters most.

**The first such debt has now been paid.** UPSTREAM-7's correction was owed a
measurement because `timeline.c` calls the defective truncation. The answer
is that the correction is **free**, and the reason is structural rather than
statistical: both truncations run *after* the start and end dates have been
merged into the list, and the contract requires `end > start`, so at least two
distinct dates always survive — which is exactly the regime where upstream is
correct. Confirmed over 96 windows plus 8 deliberately minimal two-date
timelines, all matching exactly.

That is the shape a discharged debt should take: a reason, not just a green
test. A test tells you it did not happen in the cases you tried; the argument
tells you it cannot happen.

Still outstanding: no *other* fix has been shown to change a price, and the
port still does not compute one. The contingent leg integral is next.

**Why fix at all.** A port that faithfully reproduces defects is a
translation. A port that corrects them, enumerates the corrections, and
measures their effect is a contribution. The second is the one worth
offering.

## Not doing

- Not touching `isda_cds_model_c_v1.8.3/`. It is the oracle; editing it
  destroys the only independent check we have.
- Not committing it either. It is gitignored. Anyone reproducing this work
  fetches their own copy from cdsmodel.com under the terms ISDA sets.
- Not porting the Excel XLL (`excel/`). It is Win32-only by construction and
  the cross-platform spreadsheet layer replaces it.
- Not optimising anything before it is correct and differentially equal.
