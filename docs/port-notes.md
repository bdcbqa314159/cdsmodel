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

**All twelve layers are complete**: 163 of 163 in-scope functions, 41 of 41
files, plus three deliberate non-ports listed above. The ✅ marks below are
kept at file granularity because that is the level the notes were written at
and the level `tools/port_progress.py` reports; the per-layer notes record why
each layer was shaped the way it was, which is still the useful part.

| # | layer | upstream files | notes |
|---|---|---|---|
| 0 | harness | — | ✅ done: CMake, GoogleTest, two-tier verification, oracle optional |
| 1 | search / interp | `bsearch` ✅, `lintrp1` ✅, `linterpc` ✅, `interpc` ✅ | `interpc` needs a strided overload — one call site, `interpc.c:560`, `sizeof(TRatePt)` |
| 2 | dates | `cdate` ✅, `dateconv` ✅, `ldate` ✅, `date_sup` ✅, `dateadj` ✅ | epoch pinned: TDate 0 = 1601-01-01, proleptic Gregorian, verified every 13th day to 2200 |
| 3 | day counts | `ldate` (DCF part) ✅, `yearfrac` ✅, `convert` ✅ | layer complete except `convert.c`, which is date intervals rather than day counts and moves to layer 5 |
| 4 | calendars | `buscache` ✅, `busday` ✅, `badday` ✅ | **layer complete.** No global state left. `BusinessDaysDiff` transcribed, `AddBusinessDays` reimplemented — see "Transcribe or reimplement". `badday.c` holds `TBadDayList`, used only by the zero-curve swap code |
| 5 | intervals / schedules | `convert` ✅, `dtlist` ✅, `schedule` ✅, `datelist` ✅, `cxdatelist` ✅, `stub` ✅ | `DateInterval` and the date list operations done. The date lists are the first module to **correct** upstream rather than reproduce it — see Policy |
| 6 | curves | `tcurve` ✅, `zerocurve` ✅, `cxzerocurve` ✅, `zcall` ✅, `zcswap` ✅, `gtozc` ✅ | 3,330 lines; `TCurve` → `std::vector`. `JpmcdsMakeTCurveNoRateCheck` is a deliberate non-port; its TDate-0 terminator is a deliberate difference |
| 7 | cashflows | `cashflow` ✅, `streamcf` ✅, `fltrate` ✅, `cfinanci` ✅ | |
| 8 | CDS legs | `timeline` ✅, `contingentleg` ✅, `feeleg` ✅, `defaulted` ✅ | **both legs of a CDS price bit-identically.** Protection leg across 720 trades, premium leg across 576 configurations — see Decisions 9 and 10 in [journal.md](journal.md) |
| 9 | solver | `rtbrent` ✅ | callback → template parameter. Solver outputs compare at the stopping tolerance, not in ULPs |
| 10 | top-level API | `cds` ✅, `cdsone` ✅, `cdsbootstrap` ✅ | the surface users actually call |
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

### Deliberate non-ports

Three functions are counted out of the port's denominator rather than left
undone. `tools/port_progress.py` holds the list; the reasons belong here,
because a number that excludes something needs its exclusions readable.

| function | file | why not |
|---|---|---|
| `JpmcdsHolidayLoadFromDisk` | `buscache.c` | **partially reversed by artifact #4.** The objection was the process-wide cache, not the reading, and that distinction has now been acted on: `parse_holiday_file` reads the format and returns a `Calendar` by value, with no cache and no global. What remains a non-port is the cache itself and the by-name lookup it served; the add-in keeps an owned registry instead, where a stale handle is detected rather than silently resolved |
| `JpmcdsStringToDayCountConv` | `yearfrac.c` | parses into a `static char privDccString[33]`, a function-local static scratch buffer (see the mutable-state table above). The port's day-count parsing went in with the other parsers and takes a `string_view` |
| `JpmcdsMakeTCurveNoRateCheck` | `tcurve.c` | **its entire purpose is to skip the check `ZeroCurve::create` exists to perform.** `JpmcdsMakeTCurve` delegates to it and adds the rate check; porting the unchecked variant would reintroduce the silent-NaN hazard behind a second door. Its TDate-0 terminator is documented as a deliberate difference below and as a second reachability path in UPSTREAM-36 |

A non-port is not a gap, but it is also not free: each one is a function a
caller of the upstream library could reach and a caller of this port cannot.
All three are cases where the thing that would be ported is the defect.

### Deliberate differences so far

| site | upstream | port | why |
|---|---|---|---|
| `split_bad_day_and_stub_pos` | validates the stub position via `JpmcdsStubPosMake` and assigns the bad day convention from `value & 0xFF` **unchecked**, though `JpmcdsBadDayConvValid` is in the same library | refuses a convention it cannot represent, at the split | `BadDayConv` is a four-valued enum, so an arbitrary byte is not a value it can hold. Both reject the same inputs overall -- `JpmcdsBusinessDay` validates the convention before looking at the date, measured on a business day and a weekend alike -- but the port rejects earlier, so a caller that splits and never adjusts a date sees SUCCESS from upstream and failure from the port. |
| `date_from_string` year token | a bare `atol`, so an unparseable year silently becomes 2000 (UPSTREAM-39) | refuses any year token containing a non-digit | the only **correction** in the port. The month and day fields in the same function do detect an unparseable token, so the code is internally inconsistent and there is no single behaviour to reproduce. Divergence asserted from both sides; excluded from the recorded vectors on purpose |
| `date_from_string` month name | lowercases three bytes unconditionally, into the caller's own buffer (UPSTREAM-38) | checks the length first, lowercases into a local array | closes a stack overflow, and stops mutating a string the caller only asked to have parsed |
| `ZeroCurve::create` point list | `JpmcdsMakeTCurveNoRateCheck` stops at the first point whose date is TDate 0, making the length argument an upper bound; a leading 0 yields a **zero-point curve** | copies the whole `span` | TDate 0 is 1601-01-01, a real date a caller may hold, so honouring it as a terminator would silently truncate a valid curve. Measured: date 0 terminates, date **1 fails**, so the header's "a 1 or 0 in the date field" is wrong. The empty-curve case this enables is a second reachability path for UPSTREAM-36 |
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
| the zero-period schedule | `JpmcdsNewCouponDatesSwap` returns one, reported SUCCESS, and its own validator rejects it (UPSTREAM-29) | refused | there is no single upstream behaviour worth preserving here: the constructor and the validator disagree about the same value, so reproducing one means contradicting the other. Refusing is what makes `CouponSchedule::periods` non-empty by construction. Pinned without the oracle — the vectors record the zero-period cases and the replay asserts the port declines them. |
| `TCouponDates.exDividendDate` | a fifth date in the struct that no line in the library ever reads or writes | dropped | always zero, i.e. 1601-01-01, in every schedule upstream has produced. Same treatment as `TDateInterval.flag`. |
| the long-stub period-type guard | `prd_typ != 'I' && != 'E' && != 'J'` | no expression at all | dead code upstream: those three types are accepted by `JpmcdsMakeDateInterval` and then refused by `JpmcdsDtFwdAny`, which runs first in the stepping loop. In the port they are not representable — `make_date_interval` returns nullopt for them — so there is nothing to guard against. |
| the unadjusted-date buffer | sized from `startDate..matDate`, filled from the roll date (UPSTREAM-30) | `std::vector`, plus the caller's bound checked at the callee too | the sizing mismatch is not observable behaviour, it is a heap overflow. The bound is checked in both places so the public entry point is as safe as the internal one. |
| `JpmcdsFloatRateSet` | a function whose entire body is six assignments and a macro expansion, returning SUCCESS unconditionally and validating nothing — not the day count convention, though `JpmcdsDayCountValid` is in the same library and `yearfrac.h` is already included | gone; `FloatRate` is an aggregate | a constructor that cannot fail and only assigns is aggregate initialisation. The positional signature took `matInterval` and `payInterval` adjacently, both `TDateInterval*`, so transposing them compiled silently; designated initialisers name them. |
| `JpmcdsFloatRateSet`'s only call site | `streamcf.c:481` fills a local `TFloatRate floatRateDef; /* Used for convexity adjustment */` and **nothing reads it** — the adjustment does not exist in this version, so `matInterval` is computed only to feed a dead store, with spread and weight hardcoded 0 and 1 rather than the real ones | no `FloatRate` is built there at all | dead code computes no wrong answer, so not a defect. The latent trap is specific: `floatRateDef` is a STACK struct and the setter never assigns `rateType`, so restoring the adjustment would read an indeterminate field — and neither ASan nor UBSan sees an uninitialised stack read, which is the one memory class this project's harness is blind to. |
| `JpmcdsCdsFeeLegFlows`'s deferral | a note in `cds.hpp` said the function "needs cashflow.c, which is layer 7 and not on the pricing path" | ported | the reason was true when written and **expired** once `cashflow.c` was ported in full, several slices earlier. Nothing noticed: `port_progress.py`'s mentioned-but-not-claimed check is what surfaced it. A deferral with an expired reason reads exactly like a live one, which is the argument for machine-checked provenance over a comment. |
| `JpmcdsCdsFeeLegFlows`'s hardcoded inputs | declares `payAccOnDefault = 1` and `protectStart = TRUE` as locals and passes them to `JpmcdsCdsFeeLegMake`, so a caller cannot set either | `cds_fee_leg_flows` takes a `Cds`, whose defaults ARE those values | a caller wanting upstream's behaviour passes a default-constructed `Cds`; one wanting something else can say so. Note that `pay_accrual_on_default` cannot affect the ANSWER here — these are the flows that occur when there is no default — which is asserted rather than left to a reader's inference. |
| **three operations shipped twice** | upstream exports and publicly declares two of each: `JpmcdsJpmcdsDateListAddDates` (dtlist.h:92) vs `JpmcdsDateListAddDates` (cxdatelist.h:45); `JpmcdsJpmcdsDateListTruncate` vs `JpmcdsDateListTruncate`; `JpmcdsDatesFromCurve` (tcurve.c) vs `JpmcdsNewDateListFromTCurve` (datelist.c). **The doubled prefix is upstream's own** — a renaming pass ran twice over `dtlist.c` and once over `cxdatelist.c` | one function each, claimed against both names | the pairs are not copies: the four-parameter add takes an `allowDuplicates` flag and the three-parameter one is that flag set FALSE, measured on `{100,200,300}` + `{200,400}` → `100 200 200 300 400` vs `100 200 300 400`. So `add_dates(…, false)` IS the cxdatelist entry point, and a differential test sweeps the equivalence rather than resting on the one example that suggested it. `cxdatelist.c` is a partial re-implementation of `dtlist.c` under slightly different names, and a caller choosing by autocomplete gets whichever. |
| the NULL-list branch of `JpmcdsDateListAddDates` | accepts `dl == NULL` and copies the added array **verbatim**, skipping the dedupe the function's own name promises | a span has no null state, so the merge runs and dedupes | measured: three dates with a duplicate come back as three. The port refuses the input instead, because `date_list.hpp` already declares the stricter contract for this module. |
| `JpmcdsDateListAddDates`'s third branch | `dl->fNumItems == 0 && numItems == 0` sits after `numItems <= 0`, which already caught it | not reproduced | dead branch. It also declares `TDateList tmp = {0, NULL};`, never assigns it, and calls `FREE(tmp.fArray)` on a provably-NULL pointer — the **fourth** always-NULL free in this port. |
| `JpmcdsExtendDateList`'s new slots | come from `JpmcdsMallocSafe`, which memsets, so an extended list holds **TDate 0 = 1601-01-01** — a real date, not an obviously-unset one | `std::vector::resize` grows to default-constructed `Date`, which is **1970-01-01** | the two disagree about what an unset date is, and NEITHER value is invalid. `Date` is `std::chrono::sys_days`, so a default-constructed one is the UNIX epoch, 134,774 days after the ISDA epoch. This reaches beyond `resize`: the port's aggregates (`CouponDates`, `FeePayment`, `SwapDate`) all default-initialise their `Date` members, so 1970-01-01 is the port's silent "unset" throughout. Every constructor sets every field, which is why nothing has depended on it — recorded rather than assumed harmless. I asserted the two agreed and the test said otherwise. |
| `JpmcdsDateListAddDatesFreeOld` | frees the caller's list **even when the add fails**, so a failed call loses the input and returns NULL | gone; `add_dates` returns a value | upstream's own comment documents it ("gets freed"), so it is a contract rather than a defect. Returning a value removes the question. |
| `JpmcdsYear`'s return | `return 0;` -- the only function in `ldate.c` that does not write `return SUCCESS;`. The two are identical (`SUCCESS` is `#define`d to 0) | `year_of` returns the year | a style inconsistency rather than a defect, recorded because it makes a reader stop and check. |
| `JpmcdsIsLeap` vs `JPMCDS_IS_LEAP` | upstream has a MACRO taking a year and a FUNCTION taking a date; the function routes through `JpmcdsDateToMDY` to get what the macro wants | `is_leap(year)` and `is_leap_year(date)`, the second composed from the first | expressed as the composition it is. Upstream's function implements the Gregorian rule with three nested `if`s whose "no" branches are empty statements (`; /* not divisible by 4 */`) — correct, and reads like a trap. |
| `JpmcdsDaysLeftThisYear` | the name promises calendar days and the answer does not deliver them: it takes a day count convention and delegates to `JpmcdsDaysDiff`, so under 30/360 the result is a 30/360 difference | reproduced, and placed in `day_count.hpp` rather than `date.hpp` because its answer depends on a convention | measured: from 1 January an ACT convention sees 365 or 366 and 30/360 sees 360. I expected 31 December to be the clean illustration — one day under ACT, zero under 30/360 — and it is NOT: 30/360 USA adjusts D1 from 31 to 30, so both give 1 and the conventions AGREE exactly where I expected them to diverge. |
| the four `ldate.c` year helpers | serve `JpmcdsDayCountFraction`'s ACT/ACT branch, which walks whole years adding 365 or 366 | ported, though nothing in the port needs them | the port's `year_fraction` intersects the period with each calendar year instead, which is why these four stayed unclaimed long after `day_count.hpp` was finished. Ported because all four are declared in `ldate.h` and are therefore public API. Their tier-1 tests check them against the **Gregorian rule itself** rather than against recorded upstream answers — a higher rung on the oracle ladder, available only because functions this small have a closed form. |
| `JpmcdsFeeLegMake` | defined in `cx.c` with external linkage, declared in **no header** (`cx.h` declares `MakeEmpty` and `Free` and skips it), and called from nowhere — it appears exactly twice in the whole tree: its own definition and its own `routine[]` string | gone; `FeeLeg` is built by aggregate initialisation | the only constructor that copies caller-supplied arrays exists, works, and is unreachable without hand-writing its prototype. The library's actual way to build a fee leg is `JpmcdsFeeLegMakeEmpty(n)` then poking the three parallel arrays directly, which `cds.c` does at lines 166 and 192. |
| `JpmcdsFeeLegMake`'s count | copies each array only `if (nbDates > 0 && array != NULL)` and otherwise stores NULL, while `nbDates` keeps whatever was passed — so `(3, NULL, ends, pays)` returns SUCCESS with a count of 3 and a NULL array, and `JpmcdsFeeLegPV` reads `fl->accStartDates[0]` whenever `nbDates > 1`. A negative count is stored as given. MEASURED | unrepresentable | **not filed as a defect**, because no header declares the function: a caller cannot invoke it without guessing its signature, and the register's rule is that reachability bounds severity. `std::vector<FeePayment>` cannot have a length that disagrees with itself. Pinned by a differential test that reaches upstream through a hand-written prototype — the only way any caller could. |
| `JpmcdsContingentLegMake`'s validation order | `REQUIRE (p->endDate > p->startDate)` runs AFTER the `protectStart` shift, so **the flag decides whether a zero-length leg is legal**: with it the stored start is `start - 1` and `end == start` passes, without it the same input is refused | reproduced | measured on both sides and asserted as an exact count asymmetry (60 accepted with, 48 without, over the same grid). Not an accident of ordering but the right answer — protecting on the start date makes a single-day contract meaningful. A port that validated before shifting would pass every case except the zero-length ones. |
| the `protectStart` shift's lower bound | plain `TDate` arithmetic with none, so `JpmcdsContingentLegMake(0, ...)` with `protectStart` stores a start date of **-1** — a negative TDate, which no valid date is. MEASURED | nothing to refuse | `Date` is a `std::chrono::sys_days`, so a day before the epoch is an earlier date rather than an invalid one, and there is no sentinel to collide with. The port does not pretend to a check it has no need for. |
| `JpmcdsContingentLegFree` | does not exist — `TContingentLeg` has a constructor and no destructor, while `TFeeLeg` has both, so a caller frees one with `FREE` and the other with a named function | both are values | asymmetric in upstream and invisible in the port. |
| three output pointers | `exact`, `loBound`, `hiBound`, with the documented invariant `hiBound - loBound - 1` = the number of matches and `[loBound+1, hiBound-1]` all matches | one `MatchRange{first, count}` | the invariant IS the answer, so returning it makes `hiBound - loBound - 1 == count` a tautology rather than a promise each caller re-derives. Upstream's `loBound == -1` and `hiBound == arraySize` are not "absent" — they are one past each end and load-bearing for the subtraction — so a half-open range needs no sentinel. |
| `exact` | documented as "the index of A value which is equal", and the indefinite article is load-bearing: the code picks whichever of the two bracketing indices matches, which need not be the first of a run. MEASURED — `{10,10,20,30}` seeking 10 gives `exact == 1` while the first match is 0 | `MatchRange::exact()` returns the FIRST | a stronger guarantee, derivable from the range. The differential test asserts upstream's `exact` lies WITHIN the port's range rather than equalling its start, because equalling it is something upstream never promised. |
| the "x array not in increasing order" branch | reached when the `arraySize + 1` iteration cap expires | reproduced, as a `nullopt` | and it **never fired** across an exhaustive sweep of every unsorted array of length 2–4 over 0..3. The cap is `arraySize+1` iterations on a halving search, so it is a runaway backstop rather than a detector; disorder produces a confident wrong answer, which is exactly what the function's own documentation says. Dead code, not a broken check — so not filed. I wrote the test asserting at least one unsorted array would be refused, and it failed. |
| `JpmcdsErrMsg("%s: x array not in increasing order.n", routine)` | `.n` for `.\n` — the log line never ends | the port has no log | not filed: a cosmetic defect with no behavioural consequence, and this register is for things that change an answer or touch memory. Recorded because a reader comparing the two files will notice, and the reason for the omission should be written down rather than inferred. |
| `void *interpData  /* (I) Interpolation data-not used */` | the third annotated dead parameter, and the second annotated **at the declaration** rather than at a call site | not a parameter | five files now carry the parameters of features that were removed; two of them say so. |
| clamping vs the stored date | the clamp applies to the date the interpolation is PERFORMED at; the date STORED in the new curve is the one the caller asked for. So a date past the old curve's end comes back carrying the final rate **at its own date** — a flat extension, not an extrapolation | reproduced, with the two dates in separate vectors so they cannot be conflated | a tier-1 property asserts the stored dates are exactly the requested ones, which a port that clamped both would fail while still producing plausible rates. |
| a decreasing requested-date list | upstream builds a curve whose dates DECREASE and reports success | refused | `ZeroCurve::create` requires strictly increasing dates, a zero_curve-slice decision. Not recorded in the vectors — there is no answer to replay — so it is pinned by a differential test instead. |
| three parallel arrays and one count | `JpmcdsBuildIRZeroCurve` takes `char *instrNames`, `TDate *dates`, `double *rates` and a single `long nInstr` covering all three, so a caller can hand it arrays of different lengths and it reads past the end of two of them | one `std::span<const InstrumentQuote>` | the same shape the fee leg had and the same fix. No test for it: there is no second length to disagree with, and a test could only assert that a struct has the members it is declared with. |
| an empty instrument list | `nInstr == 0` reaches `NEW_ARRAY(TDate, 0)`, which is `JpmcdsMallocSafe(0)`, which returns NULL **by design** with the message "Number of bytes (0) must be at least 1" — so the caller is told about bytes rather than about instruments | refused, as an empty list | not filed as a defect: the outcome is correct and only the diagnostic is wrong, and a diagnostic is not something an `optional`-returning port can reproduce or improve on. Measured so the agreement on the OUTCOME is recorded. |
| `fwdLength = '3'; /* not used */` | the only place in the library where the dead parameter is annotated as dead **at the call site** | not a parameter | four files in a row — `fltrate`, `streamcf`, `zcswap`, `gtozc` — carry the parameters of a feature that was removed; here somebody noticed and wrote it down rather than deleting it. |
| `fwdLength` | a `char` parameter of `JpmcdsZCSwaps`, "For fwd smoothing, length of fwds", appearing three times in the file — declaration, definition, and the call that passes it to the validator — and **read by none of them** | not a parameter | forward smoothing was removed and its parameter stayed. |
| a stub point inside the cash range | `JpmcdsZCCash` merges the input curve back with `JpmcdsZCAddPrefixCurve` (points BEFORE the cash curve's first date) and `JpmcdsZCAddSuffixCurve` (points AFTER its last). Nothing copies the middle, so a caller's curve point falling inside the span of the cash quotes is silently dropped | reproduced | asserted both ways by a tier-1 property test: outside the range the point must survive, inside it must not. The first version of that test claimed every stub point survives and the recorded data refuted it. |
| the cash curve's terms | `ZC_DEFAULT_BASIS 1` and `JPMCDS_ACT_365F`, hardcoded, ignoring the input curve's own basis and day count | reproduced | the merge routes each point through `JpmcdsZCAddGeneralRate`, which converts, so the result is consistent — but a caller's curve in other terms comes back expressed in these. `JpmcdsZCSwaps` has no such constant and keeps the input curve's terms, so the two entry points in one file differ on this. |
| the two validators' view of equal dates | `CheckZCCashInputs` tests `dateArr[i-1] > dateArr[i]`, permitting equal dates; `CheckZCSwapsInputs` tests `dates[i+1] <= dates[i]`, rejecting them | both reproduced | two validators in one file disagreeing about the same question. The cash path's duplicates are refused later by the curve itself, so the two paths agree on acceptance by accident rather than by rule. |
| the solve target | `&zc->rate[zc->numItems-1]`, assuming the point just added is last (UPSTREAM-33) | the index of the added point, looked up by date | `set_rate(index, rate)` recomputes the cached discount, so the rate/discount invariant cannot drift either — upstream mutates `fArray[last].fRate` in a snapshot and recomputes the discount separately. |
| `JpmcdsZCAddSwaps`'s three dead locals | `TSwapDates *newDates` ("Swap dates w/ synthetics"), `double *newRates`, `TBoolean useFastZC` — none ever read; `newDates` never even freed, `newRates` freed at `done:` while provably still NULL | not reproduced | a synthetic-swap feature was removed and its variables stayed. Same shape as `streamcf.c`'s vestigial interface. |
| `X_TOLERANCE = DBL_MAX` | the solver's x-criterion, set to the largest double | reproduced exactly | an idiom, not a mistake: Brent's acceptance test is `|f| <= fTol && |dx| <= xTol`, so DBL_MAX says "judge convergence on f alone". It also switches OFF the quirk recorded in `root_find.hpp`, where the initial guess is accepted on proximity to either bound — with DBL_MAX that disjunction is always true and acceptance reduces to `|y0| <= fTol`, which is the sensible rule. The one caller that sets this parameter sets it to the value that neutralises the quirk. |
| the zero-rate special case | `JpmcdsZCGetSwapCFL` returns one flow of 1.0 at the adjusted maturity and no coupons, via an early `return cfl` rather than a jump to `done:` | reproduced | the early return is load bearing, not a shortcut: `status` is still FAILURE at that point, so routing through the shared exit would free the list and return NULL. Worth recording because it is the one degenerate input in this file somebody thought about — see UPSTREAM-32 for the one they did not. |
| `TBadDayList *` + `long badDayConv` | two unrelated parameters, with `JpmcdsZCAdjustDate` branching on which is PRESENT rather than consulting both (UPSTREAM-26) | one `SwapBadDayRule` with an `optional` table | grouping says they are one decision; the `optional` says which. Empty and absent stay different inputs, because upstream branches on the pointer and not the count. |
| `TSwapType` | one enumerator, `JPMCDS_SINGLE_REFIX`, guarded in three places by the one-sided `if (swapType > JPMCDS_SINGLE_REFIX)` | dropped | a field with one possible value is not a choice. The guard holds only because an all-non-negative enum gets an unsigned compatible type in C — in C++ clang picks signed, so the test file and the oracle it links against disagree about the same enum. A guard correct by implementation-defined signedness is not one to carry forward. |
| `TStreamFloat.firstCoupon` / `.finalCoupon` | `TStubRates`, and NEITHER FIELD IS EVER READ: `stubInterval` is passed to `GetFloatStubRate` which ignores it, `stubRate` is not even passed (`zcswap.c:615,618` set it to 0.0 and nothing reads it back) | dropped from `FloatStream` | so a caller's floating-leg stub rates are silently ignored, while the FIXED leg genuinely honours `firstCouponRate`/`finalCouponRate`. A differential test sweeps both fields over values that would move the answer if read — huge rates, one-day and ten-year intervals — and requires every flow to stay put. The omission is a measurement, not an assertion. |
| `TStreamFloat.averSoFar` / `.compSoFar` | read, passed down two levels, and landing in `GetFloatStubRate` parameters it never uses | dropped | swept in the same test. `GetFloatStubRate` takes eleven parameters and uses five; `spotOffset`, `averSoFar`, `compSoFar`, `stubInterval`, `rateBadDayConv` and `holidayFile` are all dead. |
| the fixed path's `valueDate` | `JpmcdsNewStreamFixedCFL` takes it, passes it to `JpmcdsGetFixedCouponRates`, which passes it to `CheckFixedStreamDetails`, which never reads it | not a parameter | dead through the entire path. A differential test varies it across a century, including dates long after maturity, and requires the cash flows not to move. |
| the one-period `TWO_STUB` collision | the fixed path writes `couponRates[n-1] = final` then `couponRates[0] = first`, so FIRST wins; the float path runs `GetFrontStub` then `GetBackStub` into `amounts[0]`, so BACK wins | both reproduced, in upstream's order | opposite precedence from the same stub location in the same file, on a schedule `JpmcdsCouponDateListCheck` accepts and `JpmcdsNewEmptyCouponDateList` makes constructible. Picking a winner would be a convention decision, not a porting one — the UPSTREAM-24 and UPSTREAM-27 reasoning. |
| the float leg's schedule validation | `CheckFixedStreamDetails` calls `JpmcdsCouponDateListCheck`; there is no `CheckFloatStreamDetails` and `JpmcdsNewStreamFloatCFLGen` validates nothing | `CouponSchedule` is non-empty by construction | which is how UPSTREAM-29's zero-period schedule reaches `zcswap.c`'s `fArray[fNumItems-1]` reads on the float leg specifically. The port has no unvalidated path because there is no invalid schedule to pass. |
| `floatRateDef`'s convexity adjustment | a local `TFloatRate` filled by `JpmcdsFloatRateSet` and never read, with `matInterval` computed only to feed it | no `FloatRate` built there | already recorded above against `fltrate.c`; noted here because this is the site. |
| `TFloatRate.weight` | documented "Multiplied by rate", written by both constructions (`fltrate.c:44`, `zcswap.c:625`), and read by no line in the library — `streamcf.c:286` passes the stream's `spread` to the cash-flow generator and not the weight | kept, with the comment saying so | unlike `rateType` this IS a field callers fill in, so dropping it would change their code. A floating cash flow is `notional * yearFrac * (rate + spread)` with the weight absent whatever it was set to. |
| `TFloatRate.rateType` | documented with real semantics — "payInterval is ONLY used if rateType = JPMCDS_SIMPLE_BASIS" — never assigned by its only constructor, and never read anywhere in the library | dropped | NOT a defect: an uninitialised field matters when something reads it, and nothing does. Both in-library constructions leave it indeterminate (`streamcf.c:481` via the setter, `zcswap.c:621-625` by hand). What is worth recording is that `streamcf.c:264` reads `payInterval` with no test on `rateType` at all, so the documented condition is simply false — the UPSTREAM-26 shape without the consequence. |
| the spot offset | `JPMCDS_SET_ADJ_INTERVAL_DAYS` builds it as `'D'` with `isBusDays = CALENDAR`, `holidayFile = NULL` and `badDayConv = NONE`, so despite the macro's name a two-day spot offset from a Friday reset is the Sunday | `calendar_day_offset`, reproduced exactly | the offset is the caller's to choose and `AdjustedOffset` can express all four bases. The struct's documentation says the holiday file and bad day convention "are used to adjust dates used to compute the rate", and every in-library caller sets both to the values meaning *do not adjust*. A tier-2 test asserts the spot date still lands on weekends, so a drift to business days fails loudly. |
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
| UPSTREAM-11 | `JpmcdsDefaultAccrual` locates the coupon period by the TRADE date and then accrues to the EVENT date, guarding `edd >= startDate` but never `edd >= accStartDates[i]` | **high** — a negative cash amount returned as SUCCESS; 62% of a settlement-lag grid differs from asking on the event date, 60% negative | **fixed, deliberate divergence** | locate the period containing the EVENT date. The accrued coupon at a credit event cannot depend on when someone asks, and upstream's does. One date changed; the arithmetic is untouched |
| UPSTREAM-12 | `JpmcdsDateIntervalToYears` case `'U'` has no `break` and falls into the error branch, so a lunar-month interval computes 28/365 correctly and then returns FAILURE | low — reachable only through a hand-built `TDateInterval`, since `JpmcdsMakeDateInterval` normalises `'U'` to 28 days. Upstream explicitly supports hand-built intervals for other letters | **cannot occur** | `make_date_interval` normalises at construction, so no `DateInterval` carries the letter the bug keys on; equivalence of the normalised quotient is checked bit-for-bit, not argued |
| UPSTREAM-13 | `JpmcdsLinInterpLongPoint1Discontinuous` clears the process-wide `bSearchCheckOrder_g` and returns on two error paths without restoring it | medium — one failed call disables sorted-input validation for every binary search in the process; no in-library caller and no public declaration, but exported and in the XLL surface | **cannot occur** | `bracket` and `bracket_checked` are two functions and there is no global. A taste judgement from slice 1 that turned out to prevent a real defect |
| UPSTREAM-14 | `bondDaysDiff` / `bondEDaysDiff` discard `JpmcdsDaysDiff`'s status over an uninitialised local; it fails before the 1601 epoch without writing the out param | medium — interpolating at a pre-epoch abscissa returns stack garbage (-667794 on a 0.01–0.02 curve) and reports SUCCESS. An uninitialised read, so ASan is blind to it | **cannot occur, and the port is correct where upstream is garbage** | the status is checked; `std::chrono` has no 1601 epoch so the difference is well defined. Affected combinations are omitted from the recorded vectors |
| UPSTREAM-15 | `checkTCurve` promises to "guarantee we can generate positive discount factors" but accepts a 30/360 curve and an empty curve, neither of which `JpmcdsZeroPrice` can price | medium — a silent NaN where an error belongs, from a standard day count a validating constructor accepted; same no-error-channel problem as UPSTREAM-2 | **cannot occur** | `ZeroCurve::create` rejects at construction what cannot be priced, which is why it is stricter than `checkTCurve` and why `zero_price` needs no NaN sentinel. Pinned by a tier-1 invariant over 810 discount factors: never NaN |
| UPSTREAM-16 | `JpmcdsZCAddGenRate` short-circuits when the rate's terms match the curve's and then falls through, adding the point a second time by another route | medium — usually swallowed by the duplicate check, but a date equal to the value date makes the second route fail AFTER the first committed, so the function reports FAILURE with the curve mutated | **cannot occur** | the port returns after the matching case, as the missing `return` would have. Every route commits only after all its conversions succeed, asserted as a property |
| UPSTREAM-17 | `JpmcdsZCComputeDiscount`'s fast path guards `rate >= -1.0` and computes `pow(1+rate, ...)`, so at exactly -1.0 it returns an infinity where the general path it stands in for rejects the input | medium — an infinite discount factor stored in the curve and served by `JpmcdsZCDiscountFactor`, reported as SUCCESS. Two paths for one quantity, and the fast one is tested first | **cannot occur** | the fast path is reproduced but taken only where it agrees with the general one; the boundary falls through and is refused. `ZeroCurve::create` refuses `rate <= -basis` as well, so the curve is protected twice |
| UPSTREAM-18 | `JpmcdsZCFwdDiscountFactor` detects a non-positive discount factor at the start of the forward, writes the error message, and returns SUCCESS with a fabricated `*discount = 1.0` | medium — 1.0 means no discounting, so `JpmcdsZCForwardRate` reports a forward rate of exactly zero. Reachable by underflow at a continuous rate above ~24 over 30y; the output stops looking wrong as the input gets worse | **cannot occur** | `forward_discount_factor` returns nullopt where upstream fabricates. The underflow itself is reproduced — the zero discount factor is arithmetic, not a defect |
| UPSTREAM-19 | TWO SITES. (a) the guard `if (discount<=0.0) return FAILURE` in `JpmcdsZCForwardRate` — and the identical one in `JpmcdsDiscountToRate` — cannot see NaN, since every comparison with NaN is false | medium — a NaN forward rate returned as SUCCESS. Reached from two legitimately infinite discount factors (UPSTREAM-15 settled that the overflow is a value) whose ratio is indeterminate | **cannot occur** | guard written as `isfinite` in `discount_to_rate`, so no caller can leak a NaN; tier-1 property: no evaluation returns NaN, while infinity stays permitted. (b) `JpmcdsForwardFromZCurve` in zr2fwd.c divides the two discount factors with NO guard at all — both `0/0` and `inf/inf` reach the conversion and come back as a NaN rate reported SUCCESS. One fix covered both sites, so it is one defect at two entry points rather than two defects |
| UPSTREAM-20 | `JpmcdsMergeCFL`'s header documents one precondition (non-NULL, and it is checked) and omits the one the algorithm actually requires: both inputs ordered by date | low — unordered input yields neither a sorted list nor a stable interleaving, returned as success. One call site in the library, passing ordered schedules, so it is reachable only through the public header | **cannot occur** | `merge_cash_flows` refuses unless both inputs are non-decreasing by date |
| UPSTREAM-21 | `JpmcdsZCAddCashFlowList` reads `cfl->fArray[cfl->fNumItems-1]` in its first statement and again in its `done:` handler, neither guarded | medium — `JpmcdsNewEmptyCFL(0)` gives `fNumItems=0` with `fArray=NULL`, so both are `NULL[-1]`. Measured SIGSEGV (139). With an explicit date the function detects its own error and then crashes reporting it, so the error cannot be received | **cannot occur** | an empty span is refused before anything reads from it; the live reproducer is a `DISABLED_` test plus `defect_evidence u21 --unsafe` |
| UPSTREAM-22 | when the final payment falls exactly on the curve's last date, every branch declines and the function returns SUCCESS having changed nothing | medium — the caller is told the instrument was bootstrapped; the curve lacks the date it asked for and no rate was solved, so the next instrument bootstraps against a curve one point shorter than the caller believes | **cannot occur** | `add_cash_flow_list` returns false when nothing is uncovered |
| UPSTREAM-23 | `JpmcdsZCAddCFLPoints` filters its insertions through `if (rate >= 0.0)` with no else, in a function whose comment says it exists to ensure every uncovered cash flow date is in the curve | low — on a negative curve it places 1 of 5 dates instead of 2, and the cliff is at exactly zero (a rate of -2e-13 drops what +2e-13 keeps). Tried and FAILED to show a pricing consequence: inserting a point at its own interpolated rate is idempotent under both interpolation schemes, so the fixed point survives | **not reproduced** | the port places every date; the golden vectors record which dates upstream dropped, and the replay asserts the difference is exactly those |
| UPSTREAM-24 | `JpmcdsNewDateListExtendedRoll`'s header says the roll date is used when ROLL-to-maturity is an integral number of intervals; the code tests START-to-maturity, so the roll date is consulted when there is no stub and dropped when there is one | low — inert in exactly the documented case. Reachable from `cashflow.c:230`, which passes startDate AS the roll date: that flips backward generation to forward, and the two disagree where month arithmetic clamps. Measured 7 of 378 integral-span cases, every one a 29 February start | **reproduced deliberately** | the code is self-consistent and only the documentation disagrees, so the policy's correction clause does not apply. A roll date decides accrual; reinterpreting it on the strength of a comment is not a porting decision |
| UPSTREAM-25 | `JpmcdsNewCFLFromDL3` detects a stub by comparing the instrument's dates against the grid, without checking the grid is long enough to have a surrounding period | medium — with `numDates == 1` the front branch reads `dates[1]` and WRITES `cfl[1]`, both past a one-element allocation, and the back branch reads `dates[-1]`. Both confirmed by ASan. The unsanitised front case returns NULL only because the garbage it read failed a comparison | **cannot occur** | the grid is a `std::span` and one shorter than two dates is refused when a stub was detected. Reachable only through the public header — `JpmcdsMakeCFL` builds its own grid and never produces one date with a stub |
| UPSTREAM-26 | `JpmcdsZCAdjustDate` branches on the PRESENCE of a bad day list, so with one supplied it never consults `badDayConv` or the holiday file — and the list returns its input unchanged for any date it does not mention | low — a Saturday absent from the table comes back a Saturday, in a function documented as making a date "fall on a business day". An EMPTY list does it to every date, because the test is on the pointer not on `count` | **reproduced** | the two modes are intended as alternatives and an incomplete table is the caller's business. The port fixes the SIGNATURE instead: `std::optional<std::span<const BadDayPair>>` says "either this or the convention". The optional is required rather than stylistic — no-table and empty-table are different inputs with different results |
| UPSTREAM-27 | `SetPrevDateAndOnCycle` assumes "we can only be on cycle if date is not on or after the 29th of the month", which `JpmcdsCountDates` contradicts | low — suppresses 600 of 7,296 genuinely on-cycle combinations, and because the flag selects forward-from-value-date vs backward-from-maturity, 14 of those get a previous coupon date one day out. All 14 have a 29 February value date; the backward route returns a date not on the schedule | **reproduced** | moving a swap's previous coupon date is a convention decision, not a porting one — same reasoning as UPSTREAM-24. Pinned both ways: on_cycle true implies an exact interval count, and false does NOT imply the converse |
| UPSTREAM-28 | `JpmcdsInterpRateExact` saves and disables the `bSearchCheckOrder` global, restores it at `done:`, and has two `return SUCCESS` statements in between — the two extrapolation clamps | medium — every extrapolation leaves order validation OFF process-wide and permanently. Demonstrated: an unsorted array the checked search rejected passes after one extrapolating call. Same global as UPSTREAM-13 with reachability inverted — that leaks on error paths, this on success paths, so between them the flag is never trustworthy | **cannot occur** | there is no global: `bracket` and `bracket_checked` are separate functions and the caller picks. One decision has now pre-empted two defects in two files, which is the argument for removing mutable global state rather than auditing its users |
| UPSTREAM-29 | `JpmcdsNewCouponDatesSwap` returns a non-NULL `TCouponDateList` with `fNumItems == 0` — a value its own `JpmcdsCouponDateListCheck` rejects with "schedule must have at least one coupon period" | medium — `CheckArgsSwapMakeSchedule` rejects `startDate > matDate` but not equality (and its message, "maturity date is on or before start date", does not describe the condition it guards), and the empty-accrual-period removal then decrements `fNumItems` to zero. NOT confined to equality: a Saturday-to-Sunday span under FOLLOW collapses the same way. `zcswap.c` reads `floatDL->fArray[floatDL->fNumItems-1]` at lines 725, 743 and 744 with no emptiness check and no validator call, making those `fArray[-1]`; ASan reports heap-use-after-free, because the 40 bytes before that array are `adjStreamSched->fArray`, freed at `schedule.c:345` on the way out. Reachable because `zcswap.c` passes `zc->valueDate` as the start date, so a benchmark maturing on the curve's value date suffices. `streamcf.c:1066` does validate, and is safe | **cannot occur** | `coupon_schedule` refuses whenever every period collapses, so `CouponSchedule::periods` is non-empty by construction and `is_well_formed` is true of everything the port builds. Guarded on the OUTCOME rather than on `start == maturity`, because adjustment reaches it from a non-degenerate span too. The zero-period cases are recorded in the tier-1 vectors as `accepted` with `period_count == 0`, and the replay asserts the refusal |
| UPSTREAM-30 | `JpmcdsGenerateUnadjDates` sizes its scratch array from `JpmcdsCountDates(startDate, matDate, couponInterval) + 3` and then steps the generating loop from the ROLL date | medium — a roll date outside `[startDate, matDate]` makes the stepped span longer than the span the array was sized for, and `dateArr[nDates++]` at `schedule.c:761` writes past the end. ASan: heap-buffer-overflow. The function validates none of its nine arguments. `JpmcdsNewCouponDatesSwap` bounds both roll dates before calling and within that bound the stepped span is always the shorter one, so the library's own path is safe — but the function is declared in the public `schedule.h` and has **no caller inside the library at all**, so it exists precisely for the external callers who get no checking | **cannot occur** | a `std::vector` removes the sizing question, and the bound upstream applies in the caller is checked in `generate_unadjusted_dates` as well, so neither entry point can overflow. The overflowing upstream call is a `DISABLED_` test plus `defect_evidence u30 --unsafe` |
| UPSTREAM-31 | `GetFloatStubRate` declares `double rate;`, assigns it only inside its one success branch, and writes `*stubRate = rate;` **after** the `done:` label every failure path jumps to | low — a failure copies an indeterminate double to the caller. The precondition is measured, not inferred: `JpmcdsForwardFromZCurve` leaves its output untouched on failure (a sentinel survives the call), and a real back-stub schedule reaches it. No consequence escapes — both callers test the status before using the value. The second site, the `default:` branch where `rate` is never assigned on any path, is unreachable only because `TSwapType`'s single enumerator makes its underlying type unsigned **in C**, so the one-sided guard `swapType > JPMCDS_SINGLE_REFIX` catches negatives by accident. In C++ the same enum is signed, so the two languages sharing the header disagree | **cannot occur** | `detail::stub_payment` RETURNS its value, so a failure path has no output to write and the shape is unavailable. This is the first instance of the blind spot the harness has documented from the start: ASan does not instrument uninitialised reads, UBSan has no check for them, there is no MSan job, and single-file analysis cannot see it either because the precondition lives in `zr2fwd.c` — both `-Wconditional-uninitialized` and `clang --analyze` on `streamcf.c` report nothing |
| UPSTREAM-32 | `JpmcdsZCGetSwapCFL` special-cases the degenerate RATE, with a comment, and leaves the degenerate MATURITY unguarded two statements later: `cfl->fArray[cfl->fNumItems-1].fAmount += 1.0;` | medium — with `maturity == valueDate` the chain is `JpmcdsNewDateList` → 1 date, `JpmcdsNewPayDates` → **0** (it drops the first element and decrements the count), `JpmcdsNewEmptyCFL(0)` → `fArray == NULL`, so the `+=` is `NULL[-1].fAmount += 1.0` — a read-modify-**WRITE** through a null-derived pointer, not merely a read like UPSTREAM-21. Both sanitizers catch it with different diagnostics: UBSan *"applying non-zero offset 18446744073709551600 to null pointer"* at `zcswutil.c:96` (that is −16, one `TCashFlow`) and ASan `SEGV on 0xfffffffffffffff8` (−8, the offset of `.fAmount` inside `fArray[-1]`). Reachable from `zcswap.c:475`, which passes `zc->valueDate` and `matDate` with no comparison between them anywhere in that file — the same trigger as UPSTREAM-29 reaching a different defect | **cannot occur** | `swap_cash_flows` refuses an empty coupon-date list, guarding on the OUTCOME rather than on `maturity == value_date`, since substitution and bad-day adjustment can also empty a list. The live write is a `DISABLED_` test plus `defect_evidence u32 --unsafe` |
| UPSTREAM-33 | `JpmcdsZCValueFixFltSwap` adds a placeholder point for the benchmark it is about to solve and then solves into `&zc->rate[zc->numItems-1]` — which is the point just added only when that point is the LATEST in the curve | low — `JpmcdsZCAddRateAndDiscount` inserts in SORTED position, so for a benchmark inside an existing curve the solver varies a different, already-solved point and leaves the new one at the placeholder. Measured with controls: 1,400 swept combinations with the benchmark inside the curve ALL fail, because varying a distant zero rate barely moves a shorter swap's present value and Brent cannot bracket; positive controls with the benchmark beyond the curve's end succeed, so the failures are the misdirection and not the fixture. **Tried and FAILED to produce a silently wrong curve** — the observable consequence is a misleading "Root finder failed" where the real problem is the benchmark's position, which is why this is low rather than high. Reachable from `JpmcdsZCAddSwaps`'s else-branch and from the exported function directly | **cannot occur** | `value_fixed_float_swap` looks the added point up by date and solves for that INDEX, so the solve targets what it added whatever the order. A tier-1 test asserts the port solves exactly the input upstream cannot, and that the pre-existing far point is untouched |
| UPSTREAM-34 | TWO SITES. Both static validators in `gtozc.c` detect `zeroCurve == NULL`, log it, set `status = FAILURE`, and **do not return** — then dereference it: `CheckZCCashInputs` at `gtozc.c:345` (`dateArr[i] <= zeroCurve->fBaseDate`) and `CheckZCSwapsInputs` at `gtozc.c:464` (`JpmcdsIsBusinessDay(zeroCurve->fBaseDate, ...)`, which is **not** inside a count-guarded loop) | medium — a validator that reports a NULL pointer and then reads through it. Both sanitizers: UBSan *"member access within null pointer of type 'TCurve'"*, ASan `SEGV on 0x10`. A zero instrument count short-circuits before validation, so the NULL is harmless there; any positive count reaches it. One edit — an early return — fixes both sites, so one number, the UPSTREAM-19 counting rule | **cannot occur** | both port functions take `const ZeroCurve &`, so the input does not exist. The lesson is narrower than "return early": the ACCUMULATING validator style is worth keeping — `stream_cash_flows.hpp`'s `is_well_formed` does exactly that safely because every check there is independent — and an accumulating validator is safe only when no check guards another. Live reproducers are two `DISABLED_` tests plus `defect_evidence u34 --unsafe` |
| UPSTREAM-35 | `zerocurve.c` sorts its benchmark list with `instr = toupper(instrNames[i]);` where `instrNames` is a `char *` | low — C requires `toupper`'s argument to be representable as `unsigned char` or equal `EOF`, and a high-bit byte in a signed `char` is neither. Measured: `(char)0xE9` reaches `toupper` as −23, with `EOF` being −1. Of the 128 high-bit bytes exactly ONE is legal by accident — `0xFF` is −1, which collides with `EOF`. No consequence demonstrated: the instrument check rejects the character immediately after, and the linked C library pads its table for exactly this mistake. UBSan has no check for it, so like UPSTREAM-31 it is undefined behaviour the harness cannot see — unlike UPSTREAM-31 it is one of the best-known traps in C, with a one-token fix | **cannot occur** | `instrument_kind_from_code` compares the four characters directly — no `toupper`, so no argument contract to violate and no LOCALE to consult either, which `toupper` does: under a locale where `'s'` does not uppercase to `'S'` upstream's parse changes. `InstrumentKind` is an enum, so a quote cannot carry an unparsed letter. A tier-1 test walks all 256 bytes and requires exactly four to be accepted |
| UPSTREAM-36 | `JpmcdsLinInterpCurve`'s clamping loop reads `oldCurve->fArray[oldCurve->fNumItems-1]` and `fArray[0]` with no emptiness check | medium — `JpmcdsNewTCurve` accepts a count of ZERO (only a negative one is refused) and leaves `fArray` NULL, so both become null-derived reads. UBSan: *"applying non-zero offset 18446744073709551600 to null pointer"* at `linterpc.c:65` (that is −16, one `TRatePt`); ASan: `SEGV on 0xfffffffffffffff0`; exit 134. And a zero-point curve is not hypothetical — `zerocurve.c` builds one as its FIRST act. The function has **no caller inside the library at all** (it appears once as `TCurveInterpFunc JpmcdsLinInterpCurve;` in `interp.h`, a typedef declaration and not a call), so like UPSTREAM-30 its only possible callers are the external ones that get no checking. **THE FOURTH unguarded `fArray[fNumItems-1]` in this register** — after UPSTREAM-21 (read), UPSTREAM-29 (the zero-period list that enables one) and UPSTREAM-32 (read-modify-write). At four occurrences it is an idiom the codebase trusts, not four accidents | **cannot occur** | `ZeroCurve` is non-empty by construction, so `points().front()` and `points().back()` are total. Live reproducer is a `DISABLED_` test plus `defect_evidence u36 --unsafe` |
| UPSTREAM-37 | `JpmcdsBinarySearchLong`'s main path guards both optional outputs — `if (loBound != NULL)` and `if (hiBound != NULL)` — and its **three early-return paths write through them unconditionally** | medium — the guard IS the documentation: it tells a caller NULL is supported, and the in-range case honours it. A value outside the array, or an array of one element, then stores through NULL. UBSan: *"store to null pointer of type 'long'"* at `cxbsearch.c:84`; ASan: `SEGV on 0x0`; exit 134. Both in-library callers (`cxzerocurve.c:103`, `feeleg.c:569`) pass non-NULL for all three, so it is reachable only through the public header — which is also the only place anyone would read the guard and conclude NULL was allowed. **Not a missing check but an INCONSISTENT one**, forty lines apart in one function, and that is worse: a missing check is a precondition a caller can be told about, while a check on one path and not another is an invitation the code does not honour | **cannot occur** | `match_range` RETURNS a range, so there are no output pointers to guard on any path. Three live reproducers as `DISABLED_` tests plus `defect_evidence u37 --unsafe` |
| UPSTREAM-38 | `NStringToMonth` lowercases `cp[0]`, `cp[1]`, `cp[2]` **unconditionally, with read-modify-writes**, however short the token is, and no length check precedes them | high — a 32-byte stack buffer, a non-numeric second token, and a ~29-character first token put the token at `dateCopy[30]` and `cp[2]` at `[32]`. ASan: `stack-buffer-overflow` at `convert.c:395`, frame `[32, 64) 'dateCopy'`, *"Memory access at offset 64 overflows this variable"*, reached from the public `JpmcdsStringToDate` at `convert.c:119`. The **second** overflow in `convert.c`, 225 lines from UPSTREAM-4, and the contrast is the lesson: UPSTREAM-4 is an unbounded copy in, this is a **bounded** copy followed by an unchecked read past the result. The bound does not help because the overflow is downstream of it | **cannot occur** | the token is a `string_view` and `to_month_name` refuses any size but 3 **before** reading a character; the lowercasing writes into a local array, so the port also does not mutate the caller's text. `DISABLED_` reproducer plus `defect_evidence u38 --unsafe` |
| UPSTREAM-39 | the month and day are parsed with a **checked** `JpmcdsSscanf`; eleven lines later the year is parsed with a bare `atol`, whose 0 return is indistinguishable from a parsed zero and which stops silently at the first non-digit | high — `03/15/aaaa` is accepted as **2000**-03-15, `15-Mar-twenty` as 2000-03-15, `03/15/99bogus` as 1999-03-15. An unparseable month is refused, an unparseable day is refused, an unparseable year is **invented**: one function, three fields, two policies. Severity exceeds the mechanism because a crash gets investigated and a date does not — every downstream accrual is then computed correctly from a wrong date | **corrected** | the only correction in the port, justified because the code disagrees with *itself* rather than with its documentation, so there is no single upstream behaviour to preserve. `date_from_string` refuses a year token containing a non-digit; the divergence is asserted from **both** sides, and the recorded vectors exclude these inputs so a documented difference cannot become a silent one |
| UPSTREAM-40 | `JpmcdsSscanf` accumulates digits as `value = value * 10 + (*cp - '0')` with no range check | low-medium — signed overflow is UB, so a 19-or-more-digit number has no defined result. UBSan: *"signed integer overflow: 1111111111111111111 * 10 cannot be represented in type 'long'"* at `lscanf.c:486`. `lscanf.c` is **out of scope** for the port, but this is reachable from the in-scope public `JpmcdsStringToDate`, which is why it is filed: the register's subject is upstream's defects, not the port's surface | **not applicable** | no `scanf` reimplementation exists; the equivalent arithmetic in `date_from_string` range-checks before each multiply. Found by the same probe as UPSTREAM-38 — same input, different file, mechanism and fix |

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
| PORT-10 | the defaulted-accrual amount was written `fraction * coupon_rate * notional`, where upstream computes `fraction * (coupon_rate * notional)` -- one ULP apart, 3333.333333333333 against 3333.3333333333335 | the differential suite, immediately, on the one day count where the difference was representable | fixed -- association matched to upstream, with the measured values in a comment at the site |
| PORT-11 | `ZeroCurve::create` had no rate check, so a rate at or below `-basis` was accepted and `zero_price` then returned inf or NaN silently — upstream's `checkTCurve` calls `JpmcdsRateValid` on every point | comparing CONSTRUCTION against upstream, which no earlier test did: every curve differential test built a valid curve on both sides, so the constructor's rejection behaviour was never compared. The third time that shape has hidden a defect, after PORT-9 | fixed — `rate_is_valid` applied per point in `create`, matching upstream's rule exactly across a 66-case accept/reject sweep |
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

**PORT-9 is the most instructive of the ten, because of what did not catch
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
