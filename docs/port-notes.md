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
| `buscache.c:62` | `THoliday *cache = NULL;` — **no `static`** | external linkage, exported from the archive as `_cache`; a process-wide holiday cache with no locking. Two threads pricing with different calendars race. |
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
| 4 | calendars | `busday`, `buscache`, `badday` | kills the `cache` global; 2,253 lines, the biggest single layer |
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

Current coverage: 17 tests with an oracle present, 8 without, the latter
replaying 359 recorded `bsearch` cases.

Where the port deliberately differs from upstream, the difference is named in
a comment at the site and listed below.

### Deliberate differences so far

| site | upstream | port | why |
|---|---|---|---|
| `bsearch` run-of-equals loop | unbounded `while (x[lo] == x[hi]) hi++`, safe only by argument | bounded by `n` | upstream's safety is a comment; this makes it structural. Identical on all valid input. |
| `bsearch` index type | `int`, allows `hi = mid - 1` to go negative | `std::ptrdiff_t` | `std::size_t` would underflow here and read out of bounds. Signedness is load-bearing. |
| `bsearch` `skip` stride | byte stride plus a power-of-two shift optimisation | contiguous `std::span` | the 2009 multiply-to-shift trick is strictly worse than what the compiler now emits. A projection overload arrives when layer 1 reaches `interpc`. |
| order-check selection | `bSearchCheckOrder_g` global flag | `bracket` vs `bracket_checked` | removes process-wide state that changed every caller's behaviour |
| day count convention | `long method` carrying one of six `#define`d codes | `enum class DayCount` | any `long` was accepted at the call site and only rejected at the bottom of a `switch` |
| `JPMCDS_ACT_ACT` | `#define`d to the same value as `JPMCDS_ACT_365`, so the two are indistinguishable | `kActAct` is an alias of `DayCount::Act365`, not a separate enumerator | preserved, not fixed — splitting them would change results. The conflation is now visible in the type rather than buried in a macro. |
| ACT/365 leap weighting | walks years by adding 365 or 366 to a running date and re-deriving the year from it | intersects the period with each calendar year directly | same arithmetic, no running date to drift. Verified bit-identical (`EXPECT_DOUBLE_EQ`) across 3,456 pair/convention combinations and every start day of a full leap cycle. |
| `JpmcdsFormatDayCountConv` | returns `char *` into a string literal, and writes to the global error log as a side effect of formatting | returns `std::string_view`, logs nothing | writing through the returned pointer was undefined; a formatter should not have side effects |
| convention string parsing | substring matching, so `"GARBAGE365"` is ACT/365 and `"A360"` is ACT/360 | identical, deliberately | observable behaviour at a public API. Tightening it would be a breaking change disguised as a fix. A strict parser can be added alongside. |
| `"NONE"` round trip | parses to code `0`, which the formatter has no `case` for, so it formats back as `"unrecognized"` | identical, deliberately | an upstream asymmetry, recorded and tested rather than silently repaired |
| `TDateInterval.flag` | third field selecting what the interval is measured from | dropped | `JpmcdsMakeDateInterval` always sets it to 0 and nothing in the library reads it. A differential test asserts it stays 0, so if that ever changes we find out. |
| interval period types | fourteen documented; `JpmcdsMakeDateInterval` accepts thirteen; `JpmcdsDtFwdAny` can advance by seven | the seven advanceable ones | upstream lets you construct an interval it then refuses to use. A value you can build and not use is not a feature; the rest arrive with `schedule.c` if anything consumes them. |
| interval digit parsing | unbounded copy into a 32-byte stack buffer | bounded, rejects >31 digits | **the only place the port refuses to reproduce upstream behaviour**, because that behaviour is a stack buffer overflow. See UPSTREAM-4. |
| days-to-weeks formatting | week branch exists but is unreachable (missing `break`) | unreachable behaviour reproduced, dead intent not | `14D` formats as `"14D"`, never `"2W"`. It is observable output. See UPSTREAM-5. |
| `JpmcdsDate` failure | returns `-1`, a value in the date space | `std::optional<Date>` | `-1` was safe only because valid TDates are non-negative — exactly the coupling optional removes |
| `JpmcdsDaysInMonth` | `(long year, long month)`, month unvalidated, indexes a 13-element table | `days_in_month(std::chrono::year_month)` | a 13th month becomes unrepresentable instead of an out-of-bounds read |
| `gDateCacheArray` | 1,500-entry hardcoded table of month starts, 1995–2119, plus a slow path for anything outside | nothing | `std::chrono` covers the whole proleptic Gregorian range. 377 lines deleted, and the dates outside 1995–2119 stop taking a different code path |
| `TDate` arithmetic | `typedef long int`, so date + date compiles | `std::chrono::sys_days` | `sys_days + sys_days` does not compile; `sys_days - sys_days` yields `days` |

## Upstream defects found

Porting against a live oracle under sanitizers surfaces bugs in the original.
These are reported here, not fixed in place — upstream stays untouched.

### UPSTREAM-1 — out-of-bounds read in `JpmcdsMDYToDate` (dateconv.c:657)

The day-range check indexes the month-length table *before* the month-range
check validates `month`:

```c
    /* Make sure day is in range. */
    if (day >= 1 && day <= 28)
       /*EMPTY*/;
    else if (day < 1 ||
        (isLeap ? day > leapDays[month] : day > (days[month])))   /* <-- month unvalidated */
    { ... return FAILURE; }

    /* Make sure month and year are in range. */
    if (month < 1 || month > JPMCDS_MONTHS_PER_YEAR || ...)        /* <-- too late */
```

`days[]` and `leapDays[]` hold 13 elements, so `month == 13` reads one past
the end. Found by AddressSanitizer plus UBSan via
`DateDifferential.MakeDateMatchesUpstreamIncludingRejections`:

```
dateconv.c:658:25: runtime error: index 13 out of bounds for type 'int[13]'
AddressSanitizer: global-buffer-overflow
  0 bytes after global variable 'leapDays' ... of size 52
    #0 JpmcdsMDYToDate dateconv.c:658
    #1 JpmcdsDate      dateconv.c:480
```

Reachable when **all three** hold:

- the year is outside 1995–2119 (inside that window `gDateCacheArray`'s fast
  path validates month first and returns early, so the bug is hidden)
- `day >= 29` (`day <= 28` short-circuits before the table lookup)
- `month > 12`

Impact is low in practice — it is a read of adjacent static data, and the
function returns `FAILURE` either way on current compilers — but it is
undefined behaviour on a validation path that exists to reject bad input.

The same validation logic appears in `JpmcdsNormalizeMDY` with the ordering
**correct**: it normalises `month` into 1–12 before indexing. One bug, written
twice, right in one copy.

The port is structurally immune: `make_date` range-checks before constructing,
and `std::chrono::month` cannot hold 13 in a way that reaches a table.

Worth sending to ISDA once there is something to send it with.

### UPSTREAM-2 — `JPMCDS_DEFAULT` collides with `FAILURE`

`cgeneral.h:64` defines `JPMCDS_DEFAULT` as `-1`. `cgeneral.h:56` defines
`FAILURE` as `-1`. They are the same value.

So a day count code of `-1` means both "the default convention" and "this
call failed", and `JpmcdsStringToDayCountConv("Default", &type)` writes a
value indistinguishable from an error sentinel. The same pattern appears in
`JpmcdsDate`, which returns `FAILURE` as a `TDate`.

Not a live bug — no current caller tests a parsed code against `FAILURE` —
but it is the reason the port returns `std::optional` everywhere rather than
reserving values inside the result space.

### UPSTREAM-3 — lowercase `'y'` advances a date quarterly, not annually

`ldate.c`, `JpmcdsDtFwdAny`. Inside `case 'Y':` the month multiplier is
chosen like this:

```c
if (upperPrdTyp == 'M')
    intval.prd = interval->prd;
else if (upperPrdTyp == 'A' || interval->prd_typ == 'Y')   /* <-- raw, not upper */
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_YEAR;
else if (upperPrdTyp == 'S')
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_SEMI;
else
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_QUARTER;  /* <-- lands here */
```

The second test checks the **upper-cased** type for `'A'` but the **raw**
type for `'Y'`. A lowercase `'y'` therefore matches neither, falls to the
`else`, and is multiplied by 3 instead of 12.

Measured against the live library, start date 2008-01-03, one period:

| type | result | months advanced |
|---|---|---|
| `'Y'` | 2009-01-03 | 12 |
| `'y'` | 2008-04-03 | **3** |
| `'A'` / `'a'` | 2009-01-03 | 12 |
| every other type | case-insensitive | — |

`'y'` is the only period type in the library whose meaning depends on case.

Not reachable through `JpmcdsMakeDateInterval` or
`JpmcdsStringToDateInterval`, both of which upper-case first and normalise
`'Y'` to months. It bites only hand-constructed `TDateInterval` values — which
is precisely the audience upstream's own comment names: *"This code is left
here for people who have set up their own TDateIntervals without calling
JpmcdsMakeDateInterval."*

An annual interval silently becoming quarterly is a real mispricing, so this
is the most consequential finding so far in financial terms.

The port is structurally immune: `make_date_interval` upper-cases and
normalises to days or months, so no non-canonical type survives to the
arithmetic.

### UPSTREAM-4 — stack buffer overflow in `JpmcdsStringToDateInterval`

`convert.c:288`. The digit-copy loop has no bounds check against its fixed
32-byte stack buffer:

```c
#define MAX_DIGITS 32
char numberBuff[MAX_DIGITS];
char *nump = numberBuff;
...
while (isdigit((int)*inp))
    *nump++ = *inp++;          /* unbounded */
```

Any interval string with more than 32 digits writes past the buffer.
Confirmed with AddressSanitizer:

```
ERROR: AddressSanitizer: stack-buffer-overflow
WRITE of size 1 at 0x00016fcc6180 thread T0
    #0 JpmcdsStringToDateInterval convert.c:288
  This frame has 1 object(s):
    [32, 64) 'numberBuff' (line 275) <== Memory access at offset 64 overflows this variable
```

This is the most serious finding: a write past a stack buffer driven directly
by string input, and interval strings are exactly what the Excel add-in takes
from user-entered cells.

An executable reproducer lives in
`tests/date_interval_differential_test.cpp` as
`DISABLED_Upstream4_LongDigitRunOverflowsStackBuffer`. It is disabled because
it crashes the sanitizer build on purpose; run it with
`--gtest_also_run_disabled_tests --gtest_filter='*Upstream4*'`.

**This is the one behaviour the port does not reproduce.** `parse_date_interval`
bounds the digit count and rejects longer input. Faithfully reproducing a
buffer overflow is not fidelity.

### UPSTREAM-5 — days never format as weeks (missing `break`)

`convert.c`, `JpmcdsFormatDateInterval`. The `'D'` case computes a week form
and then falls through into `default:`, which overwrites both variables:

```c
case 'D':
    if (interval->prd % JPMCDS_DAYS_PER_WEEK == 0)
    {
        periodType = 'W';                     /* computed... */
        numPeriods = interval->prd/JPMCDS_DAYS_PER_WEEK;
    }
    ...
    /* no break */
default:
    periodType = interval->prd_typ;           /* ...then discarded */
    numPeriods = interval->prd;
```

The `'M'` case directly above it does have its `break`, so month collapsing
works. Measured:

| stored | formats as | intended |
|---|---|---|
| 12M | `1A` | `1A` |
| 18M | `3S` | `3S` |
| 7D | `7D` | `1W` |
| 14D | `14D` | `2W` |

Harmless — output is still correct, just not abbreviated — but the week
branch is unreachable code. Reproduced in the port, since it is observable
output; the unreachable intent is not.

Also noted while here: `JpmcdsFormatDateInterval` returns a pointer into a
toggling pair of `static char format[2][16]` buffers, so its result is
invalidated by the second subsequent call and is not thread-safe. The port
returns a value.

## Not doing

- Not touching `isda_cds_model_c_v1.8.3/`. It is the oracle; editing it
  destroys the only independent check we have.
- Not committing it either. It is gitignored. Anyone reproducing this work
  fetches their own copy from cdsmodel.com under the terms ISDA sets.
- Not porting the Excel XLL (`excel/`). It is Win32-only by construction and
  the cross-platform spreadsheet layer replaces it.
- Not optimising anything before it is correct and differentially equal.
