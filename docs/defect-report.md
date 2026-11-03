# Defect report — ISDA CDS Standard Model 1.8.3

Seven defects found in the upstream C library while porting it to C++20, with
the evidence for each and what the port does about it.

- **Library examined:** ISDA CDS Standard Model, version 1.8.3 (C, 2009),
  obtained from cdsmodel.com under the ISDA CDS Standard Model Public License.
- **Found by:** differential testing against a C++20 port, with
  AddressSanitizer and UndefinedBehaviorSanitizer on the upstream build.
- **Reproduce the whole report:** `./build/debug/defect_evidence`
  (requires a local upstream copy; see README.md).
- **Published at:** <https://bdcbqa314159.github.io/cdsmodel/> — same content,
  rendered. `docs/index.html` is that page; update it alongside this file.
- **Migration context:** [port-notes.md](port-notes.md) carries the register
  and the surrounding plan.

## Register

| id | defect | severity | status in the port |
|---|---|---|---|
| UPSTREAM-1 | Month validated *after* it indexes a 13-element table | Low — UB on a reject path | Cannot occur |
| UPSTREAM-2 | `JPMCDS_DEFAULT` and `FAILURE` are both `-1` | Design | Cannot occur |
| UPSTREAM-3 | Lowercase `'y'` advances a date quarterly, not annually | **High** — silent mispricing | Cannot occur |
| UPSTREAM-4 | Stack buffer overflow from string input | **High** — memory safety | Fixed (deliberate divergence) |
| UPSTREAM-5 | Days never format as weeks (missing `break`) | Cosmetic | Reproduced deliberately |
| UPSTREAM-6 | Business-day adjustment never returns for a calendar with no business weekday | **High** — liveness | Cannot occur |
| UPSTREAM-7 | Date list truncation never examines index 0; two implementations fail in opposite directions | **High** — silently wrong | Not yet ported |

## What "fixed" means here, precisely

**Nothing is fixed in upstream.** `isda_cds_model_c_v1.8.3/` is never edited —
it is the numerical oracle the port is verified against, and editing it would
destroy the only independent check the project has. Every user of the official
C library still has all five defects.

Of the five, **three were not fixed so much as made unrepresentable**. No
patch was written for UPSTREAM-1, -2 or -3; the types stopped permitting them.
A month that cannot be 13. A result that cannot carry a sentinel. A period
type normalised at construction. That is the strongest single argument for the
port existing: three of five defects died as a side effect of choosing better
types, not as a side effect of looking for bugs.

**UPSTREAM-4 is the only defect the port consciously declines to reproduce.**
Faithfully reproducing a buffer overflow is not fidelity.

**UPSTREAM-5 is the only one deliberately kept**, because its output is
observable by callers; abbreviating `"14D"` to `"2W"` would be a behaviour
change dressed up as a fix.

---

## UPSTREAM-1 — Month validated after it indexes a 13-element table

**Location:** `lib/src/dateconv.c:657`, in `JpmcdsMDYToDate`.
**Severity:** Low. Undefined behaviour, on a path whose job is rejecting input.

### Mechanism

The day-range check indexes the month-length table before the month-range
check has validated `month`:

```c
/* Make sure day is in range. */
if (day >= 1 && day <= 28)
   /*EMPTY*/;
else if (day < 1 ||
    (isLeap ? day > leapDays[month] : day > (days[month])))   /* month unvalidated */
{ ... return FAILURE; }

/* Make sure month and year are in range. */
if (month < 1 || month > JPMCDS_MONTHS_PER_YEAR || ...)       /* too late */
{ ... return FAILURE; }
```

`days[]` and `leapDays[]` hold 13 elements, so `month == 13` reads one past
the end.

### Evidence

```
ERROR: AddressSanitizer: global-buffer-overflow
READ of size 4 at 0x000102977db4 thread T0
    #0 JpmcdsMDYToDate dateconv.c:658
0x000102977db4 is located 0 bytes after global variable 'leapDays' ... of size 52
```

### Reachability

All three conditions must hold:

- the year is outside 1995–2119 — inside that window `gDateCacheArray`'s fast
  path validates the month first and returns early, hiding the defect
- `day >= 29` — `day <= 28` short-circuits before the table lookup
- `month > 12`

The same validation logic appears in `JpmcdsNormalizeMDY` with the ordering
**correct**: it normalises `month` into 1–12 before indexing. One defect,
written twice, right in one copy.

### Status in the port

**Cannot occur.** `make_date` range-checks before constructing anything, and
`days_in_month` takes a `std::chrono::year_month`, so a 13th month cannot be
represented in a value that reaches a table.

Tests: `DateDifferential.MakeDateMatchesUpstreamIncludingRejections` (sweeps
months 0–12 only, since there is no defined behaviour at 13 to compare
against), `DateMonth.DaysInMonthIsTypeSafeWhereUpstreamWasNot`.

---

## UPSTREAM-2 — `JPMCDS_DEFAULT` and `FAILURE` are both `-1`

**Location:** `lib/include/isda/cgeneral.h:64` and `:56`.
**Severity:** Design. No current caller is broken by it.

### Mechanism

```c
#define FAILURE        -1
#define JPMCDS_DEFAULT -1
```

A day count code of `-1` means both "the default convention" and "this call
failed". The same pattern appears in `JpmcdsDate`, which returns `FAILURE` as
a `TDate`.

### Evidence

```
  JPMCDS_DEFAULT = -1
  FAILURE        = -1
  equal?         = YES

Upstream parse of "Default": rc=SUCCESS, code=-1
Upstream JpmcdsDate(1600,1,1) (an invalid date): -1
```

A caller holding a `long` cannot distinguish the two.

### Status in the port

**Cannot occur.** Success and value are separate channels:

```
Port parse of "Default": has_value=true, value=-1
Port make_date(1600,1,1):  has_value=false
```

`-1` is an ordinary value in the port, not a signal. This is the reason the
port returns `std::optional` throughout rather than reserving values inside
the result space.

Test: `DayCountStringParse.NonCalculableKeywordsParseButAreNotConventions`.

---

## UPSTREAM-3 — Lowercase `'y'` advances a date quarterly, not annually

**Location:** `lib/src/ldate.c`, in `JpmcdsDtFwdAny`.
**Severity:** High. A silent 12-month-to-3-month substitution is a mispricing.

### Mechanism

Inside `case 'Y':` the month multiplier is chosen like this:

```c
if (upperPrdTyp == 'M')
    intval.prd = interval->prd;
else if (upperPrdTyp == 'A' || interval->prd_typ == 'Y')   /* raw, not upper-cased */
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_YEAR;
else if (upperPrdTyp == 'S')
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_SEMI;
else
    intval.prd = interval->prd * JPMCDS_MONTHS_PER_QUARTER;  /* 'y' lands here */
```

The second test checks the **upper-cased** type for `'A'` but the **raw** type
for `'Y'`. A lowercase `'y'` matches neither, falls to the `else`, and is
multiplied by 3 instead of 12.

### Evidence

Start date 2008-01-03, one period, measured against the live library:

```
  type   upstream       months advanced
  'Y'    2009-01-03     12
  'y'    2008-04-03     3
  'A'    2009-01-03     12
  'a'    2009-01-03     12
  'M'    2008-02-03     1
  'm'    2008-02-03     1
  'S'    2008-07-03     6
  's'    2008-07-03     6
  'Q'    2008-04-03     3
  'q'    2008-04-03     3
```

`'y'` is the only period type in the library whose meaning depends on letter
case.

### Reachability

Not reachable through `JpmcdsMakeDateInterval` or
`JpmcdsStringToDateInterval`, both of which upper-case first and normalise
`'Y'` to months. It bites hand-constructed `TDateInterval` values — which is
precisely the audience upstream's own comment names, a few lines above the
defect:

> This code is left here for people who have set up their own TDateIntervals
> without calling JpmcdsMakeDateInterval or one of the supplied macros.

### Status in the port

**Cannot occur.** `make_date_interval` upper-cases and normalises to days or
months before any arithmetic, so no non-canonical period type survives to the
calculation:

```
Port: make_date_interval(1,'y') == make_date_interval(1,'Y') -> true
```

Tests: `DateIntervalDifferential.Upstream3_LowercaseYAdvancesQuarterlyNotAnnually`
(asserts the defect still exists upstream, so it fails if a future release
fixes it), `DateIntervalMake.IsCaseInsensitiveIncludingY`.

---

## UPSTREAM-4 — Stack buffer overflow from string input

**Location:** `lib/src/convert.c:288`, in `JpmcdsStringToDateInterval`.
**Severity:** High. Memory safety, driven directly by caller-supplied strings.

### Mechanism

The digit-copy loop has no bounds check against its fixed stack buffer:

```c
#define MAX_DIGITS 32
char numberBuff[MAX_DIGITS];
char *nump = numberBuff;
...
while (isdigit((int)*inp))
    *nump++ = *inp++;          /* unbounded */
```

Any interval string with more than 31 digits writes past the buffer.

### Evidence

```
ERROR: AddressSanitizer: stack-buffer-overflow
WRITE of size 1 at 0x00016b2e1e60 thread T0
    #0 JpmcdsStringToDateInterval convert.c:289
  This frame has 1 object(s):
    [32, 64) 'numberBuff' (line 275) <== Memory access at offset 64 overflows this variable
```

Measured boundary:

```
  digits         upstream               port
  5              accepted               accepted
  10             accepted               accepted
  31             accepted               accepted
  32             overflow               rejected
  40             overflow               rejected
  64             overflow               rejected
```

### Reachability

Interval strings (`"1Y"`, `"3M"`, `"6M"`) are exactly what the shipped Excel
add-in reads from user-entered cells, so the input is attacker-influenced in
the most ordinary deployment of this library.

### Status in the port

**Fixed, by deliberate divergence.** `parse_date_interval` bounds the digit
count and rejects anything longer. This is the only place the port does not
reproduce upstream behaviour.

An executable reproducer ships in the repository rather than as prose, as
`DISABLED_Upstream4_LongDigitRunOverflowsStackBuffer`. It is disabled because
it crashes the sanitizer build on purpose:

```sh
./build/asan/cdsmodel_differential_tests \
    --gtest_also_run_disabled_tests --gtest_filter='*Upstream4*'
```

Test: `DateIntervalParse.RejectsDigitRunsThatWouldOverflowUpstreamsBuffer`.

---

## UPSTREAM-7 — Date list truncation never examines index 0

**Location:** `lib/src/dtlist.c:592` (`JpmcdsJpmcdsDateListTruncate`) and
`lib/src/cxdatelist.c:220` (`JpmcdsDateListTruncate`) — two independent
implementations with the same defect.
**Severity:** High. Silently wrong results, and one variant is reachable from
the pricing path.

### Mechanism

Both scan backwards for the truncation point:

```c
    truncatePt = -1;                       /* dtlist.c */
    for (i = numItems - 1; i > 0; i--)     /* i > 0, so index 0 is never tested */
    {
        if (dateList->fArray[i] < truncationDate) { truncatePt = i; break; }
        if (inclusive && dateList->fArray[i] == truncationDate) { truncatePt = i; break; }
    }
    size = truncatePt + 1;
```

The loop condition is `i > 0` rather than `i >= 0`, so element 0 is never
examined. When element 0 is the only one that qualifies, the sentinel value
survives — and the two files chose **different sentinels**, so they fail in
opposite directions:

| file | sentinel | size when index 0 is the answer | effect |
|---|---|---|---|
| `dtlist.c` | `truncatePt = -1` | `0` | returns an empty list — data loss |
| `cxdatelist.c` | `truncatePt = numItems - 1` | `numItems` | returns everything — no truncation at all |

### Evidence

Keeping dates before the cut, measured against both implementations:

```
  input / cut                want           dtlist.c         cxdatelist.c
                                            (sentinel -1)    (sentinel n-1)
  [1,2] @2                   [1]            []               [1,2]
  [10,30] @30                [10]           []               [10,30]
  [5] @9                     [5]            []               [5]
  [9,10] @9 inclusive        [9]            []               [9,10]
  [1,2,3] @3                 [1,2]          [1,2]            [1,2]
```

Both are correct whenever two or more elements survive, which is why this
has not been noticed: the failure needs the answer to be exactly the first
element.

Reproduce with `./build/debug/defect_evidence u7`.

### Reachability

`JpmcdsDateListTruncate` (the `cxdatelist.c` variant) is called from
`timeline.c`, which builds the integration timeline for the fee and
contingent legs — so it sits on the CDS pricing path. `timeline.c` truncates
at the start and end dates with `inclusive = TRUE`.

**What is established and what is not.** The function misbehaves: that is
measured above. Whether a CDS price actually changes is **not** established
here — it would require the qualifying element to land at index 0 of a
timeline, and that needs a test at the timeline level. That test belongs with
layer 8, where `timeline.c` is ported. Recording the finding now so it is not
rediscovered then.

`JpmcdsSubtractDateLists` also depends on the `dtlist.c` variant and inherits
its data loss.

### Status in the port

**Not yet ported.** This is the first finding where the port cannot simply
"preserve upstream behaviour", because there are two upstream behaviours and
they contradict each other. The choice needs deciding rather than defaulting
— see the open question in `docs/port-notes.md`.

---

## UPSTREAM-6 — Business-day adjustment never returns

**Location:** `lib/src/busday.c`, `getNextBusDate`, reached from
`JpmcdsHolidayListBusinessDay` and `JpmcdsMultiHolidayListBusinessDay`.
**Severity:** High. A hang rather than a wrong answer.

### Mechanism

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
`FALSE` and the loop never exits.

Such a calendar is **accepted**: `JpmcdsHolidayListNewGeneral(NULL, 0x7F)`
returns a valid `THolidayList`, because `verifyHolidayList` has no opinion on
the weekend mask.

### Evidence

```
calendar with weekends=0x7F constructed: accepted
calling JpmcdsHolidayListBusinessDay with FOLLOW...
--- killed after 5s ---
```

### Reachability

Needs all seven weekend bits set, for which upstream provides no named
constant — a caller would have to OR them, or compute the mask. So it is a
robustness failure rather than a likely production path. But accepted input
that never returns is the kind of defect that takes a pricing service down
rather than returning a wrong number. As it spins, `curDate += direction`
would eventually overflow `TDate` as well; the hang simply arrives first.

### Status in the port

**Cannot occur.** `roll_to_business_day` returns `nullopt` when any calendar
has no business weekday, and otherwise bounds its loop at
`7 * (holidays + 2)` — a ceiling derived from the fact that stored holidays
are always business weekdays and reaching the next one costs at most seven
days.

Construction stays permissive, matching upstream, because the predicates are
perfectly well defined for such a calendar: everything is a non-business day.
`Calendar::allows_business_days()` exposes the condition so a caller can tell
"no business day exists" from other failures.

Only half of this is differentially testable. A test asserts that upstream
still *accepts* the calendar, and will fail if a future release rejects it —
but it cannot assert the hang, because the test would never finish.

Tests: `BadDayDifferential.Upstream6_AllWeekendCalendarIsAcceptedByUpstream`,
`BadDayAdjust.AllWeekendCalendarDeclinesInsteadOfLooping`,
`BadDayAdjust.TerminatesWithOnlyOneBusinessWeekdayAndManyHolidays`.

---

## UPSTREAM-5 — Days never format as weeks (missing `break`)

**Location:** `lib/src/convert.c`, in `JpmcdsFormatDateInterval`.
**Severity:** Cosmetic. Output is correct, just never abbreviated.

### Mechanism

The `'D'` case computes a week form and then falls through into `default:`,
which overwrites both variables:

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
works.

### Evidence

```
  stored     upstream       port       intended
  12M        1A             1A         same
  18M        3S             3S         same
  6M         1S             1S         same
  7D         7D             7D         1W
  14D        14D            14D        2W
  28D        28D            28D        4W
```

### Status in the port

**Reproduced deliberately.** This is observable output; abbreviating `"14D"`
to `"2W"` would change behaviour for every caller that formats an interval.
The port reproduces the behaviour and does not implement the unreachable
intent.

Tests: `DateIntervalDifferential.Upstream5_DaysNeverFormatAsWeeks`,
`DateIntervalFormat.DaysNeverCollapseToWeeks`.

---

## Also noted — thread safety

Not defects in the sense above, but the reason the library cannot be used
concurrently. Catalogued in `docs/port-notes.md`:

| location | state |
|---|---|
| `buscache.c:62` | `THoliday *cache = NULL;` — **no `static`**, so it has external linkage and is exported from the archive as `_cache`. A process-wide holiday cache with no locking. |
| `cerror.c` | eight statics including a `FILE *` log handle and a callback pointer |
| `bsearch.c:19` | `bSearchCheckOrder_g`, a global flag that silently switched every binary search in the process between checked and unchecked |
| `yearfrac.c:172` | `static char privDccString[33]`, a function-local scratch buffer in the convention-string parser |
| `convert.c` | `static char format[2][16]` with a toggling index, so `JpmcdsFormatDateInterval`'s result is invalidated by the second subsequent call |

The port holds no mutable global state. `bSearchCheckOrder_g` is gone (the
caller chooses `bracket` or `bracket_checked`), and the holiday cache is now
gone too: a calendar is a value the caller owns and copies. That is not a lock
but an owner — there is nothing left to invalidate or synchronise, and
`JpmcdsHolidayEmptyCache` has no analogue because there is no cache to empty.

## Method

The upstream C is compiled untouched into a static library and linked into the
**same test binary** as the port. Tests call both implementations side by side
and compare in-process, so the oracle is not a recorded value that can go
stale — it is the original code, executing.

All of it runs under AddressSanitizer and UndefinedBehaviorSanitizer. That
pairing is what surfaced UPSTREAM-1 and UPSTREAM-4; neither changes the
library's return value, so neither is visible from output alone.

Current coverage: 102 tests with a local oracle present, 61 without, replaying
4,225 recorded reference cases.

## Scope

- Covers only the parts of the library ported so far: binary search, dates,
  day counts, and date intervals — about 2,000 of 21,775 upstream lines.
- Calendars, schedules, curves, cashflows, the CDS legs, the solver and the
  top-level pricing API have not been examined yet.
- No attempt was made to assess exploitability of UPSTREAM-4 beyond
  confirming the out-of-bounds write.
