# Defect report — ISDA CDS Standard Model 1.8.3

Eight defects found in the upstream C library while porting it to C++20, with
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
| UPSTREAM-7 | Date list truncation ignored when fewer than two dates survive; two implementations fail in opposite directions | **High** — silently wrong | Fixed (documented intent) |
| UPSTREAM-8 | `!price > THRESHOLD` precedence bug disables a validation guard, in four places | Low — a guard that does not guard | Resolved: the port guards the real failure |

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

## UPSTREAM-8 — A validation guard that validates almost nothing

**Location:** `lib/src/contingentleg.c:90` and `:97`, `lib/src/feeleg.c:101`
and `:108` — the same line written four times.
**Severity:** Low. A guard that does not guard, rather than a wrong number.

### Mechanism

```c
if (!JpmcdsZeroPrice(spreadCurve, cl->endDate) > JPMCDS_LOG0_THRESHOLD)
{
    JpmcdsErrMsg("%s: Default hazard rate is not defined at maturity %s!", ...);
    goto done;
}
```

`!` binds tighter than `>`, so this is `(!price) > THRESHOLD` — a `0` or `1`
compared against `1e-100`. It is true exactly when `price == 0.0`.

The intent was plainly `!(price > THRESHOLD)`: reject a survival probability
too small to take a logarithm of. The macro is even named
`JPMCDS_LOG0_THRESHOLD`, "Used for avoiding LOG0 error".

So the guard rejects only an *exactly zero* price, and lets through every
tiny-but-nonzero one it was written to catch.

### Evidence

Ten-year flat curves, varying the hazard rate:

```
hazard rate    survival(T)    as-written as-intended JpmcdsContingentLegPV
0.01           0.905          passes    passes    pv=494850
0.5            0.00671        passes    passes    pv=5.63225e+06
5              1.85e-22       passes    passes    pv=5.96421e+06
20             1.17e-87       passes    passes    pv=5.99101e+06
50             4.72e-218      passes    fires     pv=5.9964e+06
80             0              fires     fires     REJECTED
200            0              fires     fires     REJECTED
```

Reproduce with `./build/debug/defect_evidence u8`.

### Why the severity is low, stated carefully

The window where written and intended behaviour differ is survival in
`(0, 1e-100]` — hazard rates above roughly 23 for a ten-year maturity. That
is not a credit; it is a number that has underflowed.

And in that window the computation **does not misbehave**. At a hazard rate
of 50 it returns 5.9964e6 against a notional of 1e7 and recovery of 0.4,
which is `notional * (1 - R) = 6e6` to four figures — the correct limit for a
credit certain to default. The guard was defensive, the defect disabled the
defence, and the computation turned out not to need it.

So this is **not** a demonstrated mispricing, and it is recorded as a guard
defect rather than a numerical one. Worth knowing precisely because the
tempting write-up — "precedence bug on the pricing path" — would overstate
it.

### Status in the port: resolved, by measurement

**Option 3 — the port guards the real failure rather than a proxy.** It
computes, and rejects a result that is not finite.

`1e-100` was only ever a stand-in for "the logarithm will misbehave". With the
integral built, that condition is directly testable, and the measurement
settles it:

- down to survival probabilities around `1e-300` the logarithms stay finite,
  the integral converges on `notional * (1 - recovery)`, and the answer is
  correct;
- only an **exactly zero** survival probability gives `log(0) = -inf` and
  hence a NaN.

So the honest guard and upstream's broken one fire on exactly the same inputs.
Verified across hazard rates from 0.001 to 200, including the boundary at 70,
74 and 80 where survival crosses from tiny to underflowed: the port and
upstream agree on validity everywhere, and on value bit-for-bit.

The port therefore keeps upstream's behaviour while stating a condition that
is true. Implementing the author's literal intent would instead have made the
port refuse to price cases upstream prices correctly — which is why
"fix the contract violation" was the wrong reflex here, and why the decision
waited for a measurement.

---

## UPSTREAM-7 — Date list truncation is ignored when fewer than two dates survive

**Location:** `lib/src/dtlist.c:592` (`JpmcdsJpmcdsDateListTruncate`) and
`lib/src/cxdatelist.c:220` (`JpmcdsDateListTruncate`) — two independent
implementations with the same defect and opposite failure modes.
**Severity:** High. Silently wrong results on 39% of a swept domain, and one
variant is reachable from the pricing path.

### Mechanism

Both scan for the truncation point without examining one end:

```c
    truncatePt = -1;                       /* dtlist.c   */
    truncatePt = numItems - 1;             /* cxdatelist.c -- different sentinel */
    for (i = numItems - 1; i > 0; i--)     /* i > 0, so index 0 is never tested */
    {
        if (dateList->fArray[i] < truncationDate) { truncatePt = i; break; }
        if (inclusive && dateList->fArray[i] == truncationDate) { truncatePt = i; break; }
    }
    size = truncatePt + 1;
```

The loop condition is `i > 0` rather than `i >= 0`, so the scan can fail to
find any qualifying element. When it does, the sentinel survives — and the two
files chose **different sentinels**, so they fail in opposite directions.

The consequence is wider than element 0, which an earlier version of this
report understated. The scan fails to find anything whenever **fewer than two
dates should survive**, so:

| file | sentinel | result when the scan finds nothing | effect |
|---|---|---|---|
| `dtlist.c` | `-1` | `size = 0` | returns an empty list — data loss |
| `cxdatelist.c` | `numItems - 1` | `size = numItems` | **the truncation is ignored entirely** |

### Evidence

Keeping dates before the cut, against `cxdatelist.c` — the variant on the
pricing path:

```
  keep-before                  want         cxdatelist.c
  [5,6,7] @3                   []           [5,6,7]      <- ignored entirely
  [5,6,7] @5                   []           [5,6,7]      <- ignored entirely
  [5,6,7] @6                   [5]          [5,6,7]      <- ignored entirely
  [5,6,7] @7                   [5,6]        [5,6]        correct
  [5,6,7] @9                   [5,6,7]      [5,6,7]      correct
```

The forward branch fails too, when the answer is empty:

```
  [5,6,7] @9 keep-after        []           [5,6,7]
```

And the two implementations against each other:

```
  input / cut                want           dtlist.c         cxdatelist.c
                                            (sentinel -1)    (sentinel n-1)
  [1,2] @2                   [1]            []               [1,2]
  [10,30] @30                [10]           []               [10,30]
  [5] @9                     [5]            []               [5]
  [9,10] @9 inclusive        [9]            []               [9,10]
  [1,2,3] @3                 [1,2]          [1,2]            [1,2]
```

Over a 600-case sweep across six lists, every cut position and both
directions, upstream disagrees with the documented intent on **234 cases —
39%**. Both are correct whenever two or more dates survive, which is the
common case and the reason this has gone unnoticed.

Reproduce with `./build/debug/defect_evidence u7`.

### Reachability

`JpmcdsDateListTruncate` (the `cxdatelist.c` variant) is called from
`timeline.c`, which builds the integration timeline for the fee and
contingent legs — so it sits on the CDS pricing path. `timeline.c` truncates
at the start and end dates with `inclusive = TRUE`.

**Price impact: measured, and it is none.** `timeline.c` is the only caller
on the pricing path, and it cannot reach the defect. Both of its truncations
run *after* the start and end dates have been merged into the date list, and
the contract requires `end > start`, so at least two distinct dates always
survive — which is precisely the regime where upstream's truncation is
correct. Confirmed over 96 integration windows plus 8 deliberately minimal
two-date timelines, every one matching the corrected port exactly.

So the correction is free. Note the form of that answer: the test says it did
not happen in the cases tried, while the structural argument says it cannot
happen.

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

## UPSTREAM-9 — An error message formatted with one argument too few

**Location:** `lib/src/stub.c:173`, inside `JpmcdsStringToStubMethod`.
**Severity:** Medium. Undefined behaviour on a reject path, reachable from any
caller that passes a stub string with a malformed LENGTH field.

### Mechanism

One message is declared with **two** `%s` conversions:

```c
static char invalidFormatMsg[]  = "%s: Invalid stub method format %s. "
    "Format is LOCATION/TYPE/[LENGTH].\n" ...
```

The bad-LOCATION branch passes both arguments:

```c
JpmcdsErrMsg (invalidFormatMsg, routine, name);
```

The bad-LENGTH branch, twelve lines below it, passes one:

```c
JpmcdsErrMsg(invalidFormatMsg, routine);
```

So `vsprintf` reaches for a second argument that was never pushed, takes
whatever occupies that varargs slot, and dereferences it as a string.

Any input whose LENGTH field is present but does not begin with `s`, `S`, `l`
or `L` reaches it — including `"f/"`, where the field is present but empty.

### Evidence

```
AddressSanitizer: stack-buffer-underflow, READ of size 5
  #0 strlen                      <- consuming the second %s
  #3 __vsprintf_chk
  #5 JpmcdsErrMsgV
  #7 JpmcdsStringToStubMethod+0x120
```

Reproduce with `./build/debug/defect_evidence u9 --unsafe`.

### On manifestation, stated carefully

The read is undefined whenever the branch is taken, but whether it **faults**
depends on what the caller left in that varargs slot. Measured on arm64 macOS,
it faults in a minimal standalone frame and does *not* fault inside
`cdsmodel_differential_tests` or inside `defect_evidence`.

A dedicated reproducer target was written and then deleted, because it stopped
faulting once it was a CMake target rather than a scratch file — the frame
layout changed and the slot stopped holding a poisoned address.

So the evidence here is the source, not a crash. Behaviour that appears in one
caller and not another is what undefined behaviour looks like; that is not
grounds for calling it benign, and not grounds for claiming a reliable crash
either. See UPSTREAM-10 for the mechanism that makes it dangerous.

### A documentation divergence found alongside it

The same message documents the format as `LOCATION/TYPE/[LENGTH]` — three
fields. The parser calls `JpmcdsStringParser(buf, "/", &lengthStr)`, which
splits on a *single* `/` and reads field two as LENGTH. There is no TYPE field,
and a third field is silently ignored: `"f/n/l"` is read as LENGTH `'n'` and
rejected without ever reaching the `'l'`.

The code is the specification; the message describes an interface that was
never implemented.

### In the port

Not reproduced. `parse_stub_method` returns `std::nullopt` and formats
nothing, so the defect cannot occur. The port also drops upstream's
`toupper(*locStr)` call — whose domain excludes negative values, i.e. any byte
above `0x7F` where `char` is signed — in favour of comparing both cases
directly.

The two preserved behaviours are kept deliberately: single-character field
matching (`"fromage/sausage"` parses as front/short), and the `"f"` succeeds /
`"f/"` fails asymmetry, which comes from upstream distinguishing a missing
field (null pointer) from an empty one (`""`).

## UPSTREAM-10 — Unbounded `vsprintf` into a 4096-byte stack buffer

**Location:** `lib/src/cerror.c:533`, `JpmcdsWriteToLog`. Every formatted
error message in the library passes through this one function.
**Severity:** Medium. Deterministic, caller-controlled stack overflow —
limited only by requiring the host to have enabled error messages.

### Mechanism

```c
static int JpmcdsWriteToLog(TBoolean formatted, char *format, va_list parminfo)
{
    char buffer[JPMCDS_ERR_MSG_BUFFER];   /* 4096 */
    ...
    if (formatted)
    {
        vsprintf(buffer, format, parminfo);
    }
```

`vsprintf`, not `vsnprintf`. Of the 452 `JpmcdsErrMsg` call sites, **67**
interpolate a caller-supplied string after the routine name — day count
strings, date interval strings, stub strings, calendar and file names. A
caller passing roughly 4KB or more of string therefore writes past the end of
this buffer with content it chose.

### Evidence

Measured threshold, two independent public entry points, messages enabled. The
message prefix is about 200 characters, so the cliff lands where prefix plus
argument reaches 4096:

```
length   JpmcdsStringToStubMethod      JpmcdsStringToDayCountConv
1000     returns FAILURE               returns FAILURE
3800     returns FAILURE               returns FAILURE
3900     ABORT (fortify check fires)   ABORT (fortify check fires)
4200     ABORT                         ABORT
9000     ABORT                         ABORT
```

Survives at 3800, dies at 3900, identically from both entry points. On a
fortified libc the overflow is detected and the process aborts; on an
unfortified build it is a plain stack smash with caller-controlled bytes.

Reproduce with `./build/debug/defect_evidence u10 --unsafe` (exits 133).

### Why Medium and not High

The formatting is only reached once a host has called `JpmcdsErrMsgOn`.
Messages are off by default, and nothing in the shipped distribution turns
them on — with messages off, `JpmcdsErrMsgV` returns before reaching
`JpmcdsWriteToLog` and a 50,000-character argument is harmless.

So it is latent in the library and live in any host that enabled diagnostics —
which is exactly the configuration someone debugging a pricing discrepancy
would be running, and the configuration an Excel add-in surfacing error text
would want.

### Relationship to UPSTREAM-9

This is the mechanism that makes UPSTREAM-9 more than a cosmetic slip: the
stray pointer U9 produces is dereferenced by this same `vsprintf`. U9 supplies
a bad pointer; U10 is the unbounded write it feeds.

Finding them in that order was luck. U9 was visible by reading `stub.c` while
porting the stub parser; U10 only appeared because U9's reproducer behaved
inconsistently and the question "why does this sometimes fault" led into
`cerror.c`. An inconsistent reproducer was the more useful signal than a
reliable one would have been.

### In the port

No equivalent exists, and not because it was fixed. There is no global error
log to format into: a failure is an empty `std::optional`, and a
caller-supplied string is never copied anywhere. The defect is absent
structurally rather than patched — the fifth time in this port that a defect
died from the shape of the code rather than from a correction.

## UPSTREAM-11 — A negative accrual, returned as SUCCESS

**Location:** `lib/src/defaulted.c:68-80`, inside `JpmcdsDefaultAccrual`.
**Severity:** High. A wrong cash amount on a settlement calculation, reported
as success, on realistic inputs. Not undefined behaviour, not a crash, not an
error path — the only defect in this register that silently returns the wrong
money.

### Mechanism

The function locates the coupon period by the **trade** date and then accrues
to the **event** date:

```c
while (i < fl->nbDates)
{
    if (fl->accStartDates[i] <= tradeDate && tradeDate < fl->accEndDates[i])
    {
        *accrualDays = edd + 1.0 - fl->accStartDates[i];
        if (JpmcdsDayCountFraction(fl->accStartDates[i], edd + 1, paymentDcc,
                                   defaultAccrual) != SUCCESS)
            goto done;
        *defaultAccrual *= couponRate * notional;
```

It guards `edd >= startDate`, but never `edd >= accStartDates[i]`. So as soon
as the trade date crosses a coupon boundary that the event date did not,
`edd + 1.0 - accStartDates[i]` goes negative — and `status = SUCCESS` is set
on the next line.

### Evidence

Quarterly from 20 March 2024, credit event 1 August 2024, notional 1e7, coupon
100bp, ACT/360. The event sits in the 20 June – 20 September period, so 43
days are owed. Varying only **when the question is asked**:

```
  trade date       upstream   accrual days   amount           port
  event+0          SUCCESS    43             11944.4          43 days
  event+14         SUCCESS    43             11944.4          43 days
  event+45         SUCCESS    43             11944.4          43 days
  event+50         SUCCESS    -49            -13611.1         43 days
  event+90         SUCCESS    -49            -13611.1         43 days
  event+365        SUCCESS    -322           -89444.4         43 days
```

The flip lands exactly where the trade date passes 20 September.

Reproduce with `./build/debug/defect_evidence u11`.

### Scope

Event dates across April to December 2024, settlement lags 0 to 120 days:

```
  2250 cases, 1399 (62%) differ from asking on the event date,
  1345 (60%) return a NEGATIVE accrual.
```

### Why High, and why the sign is not the argument

A negative accrual is obviously wrong, but a caller might catch it. The real
problem is the dependence: **the coupon accrued at a credit event cannot
depend on when someone asks.** Upstream's answer does, and in the window
before the sign flips it is wrong without being obviously wrong.

Settlement after a CDS auction routinely lands weeks after the event
determination date. A quarterly coupon boundary sits every 13 weeks. The
affected regime is not an edge case; it is the ordinary one.

### In the port

**Corrected.** One date changed: `default_accrual` locates the period
containing the **event** date rather than the trade date. Then the period
start precedes the event by construction, the result is always positive, and
it no longer varies with the trade date. The arithmetic, the conventions and
the `trade_date < event` gate are all upstream's, untouched.

A second behavioural change follows from the same root cause and is recorded
rather than absorbed: upstream **fails** when the trade date lies past the
final accrual end, because no period contains it, even though the event sits
comfortably inside the schedule and the debt is well defined. Locating by the
event date means a late settlement still returns what is owed. That is
intended — a debt does not lapse because the question was asked late — but it
is a divergence, not just a sign fix.

Because this module corrects rather than reproduces, its recorded vectors are
the **documented intent** with a flag recording whether upstream agreed at
generation time: 859 of 1,302 agreed, 443 did not. If a regeneration turns
those all true, upstream fixed it and the correction should be revisited. The
differential suite asserts the disagreement still exists, so that cannot pass
unnoticed.

## UPSTREAM-12 — A lunar-month interval computes correctly, then fails

**Location:** `lib/src/date_sup.c`, inside `JpmcdsDateIntervalToYears`.
**Severity:** Low, and the reachability is why — see below.

### Mechanism

```c
case 'U':
    *years = (double)interval->prd *
             (double)JPMCDS_DAYS_PER_LUNAR_MONTH/JPMCDS_DAYS_PER_YEAR;
default:
    JpmcdsErrMsg("%s: unknown interval type %c.\n", routine, interval->prd_typ);
    return FAILURE;
```

`case 'U'` has no `break`, so it falls into `default:`. Second of its family
after UPSTREAM-5.

### Evidence

```
type   rc         years          note
A      SUCCESS    1              annual
M      SUCCESS    0.08333333333  monthly
D      SUCCESS    0.002739726027 daily
U      FAILURE    0.07671232877  LUNAR MONTH
Z      FAILURE    -999           genuinely unknown

28/365 = 0.07671232877  <- what 'U' computes before falling through
```

The distinguishing detail is the last row. A genuinely unrecognised letter
leaves the output untouched at its sentinel; `'U'` writes the correct value and
*then* returns FAILURE. The computation ran; only the `break` is gone.

Reproduce with `./build/debug/defect_evidence u12`.

### Reachability, stated precisely

This is low severity for one reason, and the first draft of this report
overstated it before the reachability was measured.

`JpmcdsMakeDateInterval('U')` normalises to **28 days**, so an interval built
the way callers build one arrives at the ACT/365F branch and works. The missing
`break` is reachable only through a hand-constructed `TDateInterval`.

That is not a hypothetical. Upstream contemplates hand-built intervals
explicitly: the switch in `JpmcdsDtFwdAny` retains code for `'A'`, `'S'` and
`'Q'` with the comment *"left here for people who have set up their own
TDateIntervals without calling JpmcdsMakeDateInterval"*. It extends that
courtesy to `'W'` and not to `'U'` — so a hand-built lunar interval is refused
by `JpmcdsDtFwdAny` and mishandled by `JpmcdsDateIntervalToYears`.

It propagates as far as it can: `JpmcdsDateIntervalToFreq` and
`JpmcdsCountDates` both consume this function, so neither works for a
hand-built lunar interval.

### In the port

**Cannot occur.** `make_date_interval('U')` yields `{28 * periods, Days}`, so
no `DateInterval` ever carries the unit letter the bug keys on, and
`interval_to_years` has two total branches where upstream has eleven partial
ones.

The equivalence that makes this safe is checked rather than argued: for every
letter the parser accepts, the normalised interval's year fraction is compared
bit-for-bit against what upstream computes from an interval built through its
own constructor. Normalisation divides numerator and denominator by the same
factor — `'Q'` with 5 periods becomes 15 months, and 15/12 is exactly 5/4 — so
the correctly-rounded quotient is unchanged, across 120 recorded cases.

## UPSTREAM-13 — A leaked global that disables validation process-wide

**Location:** `lib/src/lintrp1.inc`, in the body expanded as
`JpmcdsLinInterpLongPoint1Discontinuous`.
**Severity:** Medium. One failed call permanently disables an input validation
for every binary search in the process.

### Mechanism

Upstream cannot say "search without checking order" per call, because that
choice lives in a mutable global: `bSearchCheckOrder_g` in `bsearch.c`,
default TRUE, toggled by `JpmcdsBSearchCheckOrder` and consulted by **both**
`JpmcdsBSearchDouble` and `JpmcdsBSearchLong` before validating that their
input array is sorted.

The discontinuous interpolator legitimately needs the unchecked search, since
repeated abscissae *are* the discontinuities. So it clears the global, does its
own non-decreasing check, searches, and restores it:

```c
checkOrder = JpmcdsBSearchCheckOrder(FALSE);
if (checkOrder)
{
    for (idx = 0; idx < N-1; idx++)
        if (XARRAY(idx) > XARRAY(idx+1))
        {
            JpmcdsErrMsg(...);
            return FAILURE;                     /* leaks */
        }
}
if (SEARCH_ROUTINE_NAME(xDesired, x, xskip, N, &lo, &hi) == FAILURE)
    goto done;                                  /* leaks */
JpmcdsBSearchCheckOrder(checkOrder);            /* only reached on success */
```

Both error paths leave before the restore.

### Evidence

```
bSearchCheckOrder_g: process-wide static, default TRUE, consulted by
JpmcdsBSearchLong and JpmcdsBSearchDouble before validating order.

  baseline:                      JpmcdsBSearchLong(unordered) -> FAILURE (validated)
  one failed call ...            -> FAILURE
  after that single failed call: JpmcdsBSearchLong(unordered) -> SUCCESS  *** VALIDATION IS OFF ***
  current global state:          FALSE (leaked)
```

Reproduce with `./build/debug/defect_evidence u13`.

### Scope and severity

After one failed call, every binary search in the process stops checking that
its domain is sorted — including inside the zero curve and the whole CDS
pricing path. A binary search over an unsorted array does not fail; it returns
a bracket that is simply wrong, and the interpolation built on it is silently
wrong too.

Medium rather than high, for reachability rather than consequence. The function
has **no caller inside the library** and **no declaration in any public
header** — but it is an exported symbol, and it is in the XLL's surface, so a
spreadsheet can reach it. And producing a wrong number afterwards needs a later
unsorted array, which curves built through `JpmcdsMakeTCurve` are not.

The clearest statement of the problem is operational: the differential suite
for this module has to restore the global in `TearDown`, or one test corrupts
every test after it.

### In the port

**Cannot occur.** `bsearch.hpp` offers `bracket` and `bracket_checked` as two
functions and has no global at all.

That was a taste judgement made in the very first slice of this port, whose
comment read *"process-wide state that silently changed the behaviour of every
caller"*. UPSTREAM-13 is that sentence turning out to be literally true — not
through explicit toggling, but through a leak.

## UPSTREAM-14 — Bond metrics return stack garbage before the epoch

**Location:** `lib/src/yearfrac.c`, the static helpers `bondDaysDiff` and
`bondEDaysDiff` behind `JpmcdsDayCountToMetricFunc`.
**Severity:** Medium. A catastrophically wrong interpolation, reported as
success, from an uninitialised read.

### Mechanism

```c
static double bondDaysDiff(double startDate, double endDate)
{
    long days;                                        /* uninitialised */
    (void)JpmcdsDaysDiff((TDate)startDate, (TDate)endDate,
                         JPMCDS_B30_360, &days);
    return ((double)days);                            /* garbage if it failed */
}
```

The status is discarded. `JpmcdsDaysDiff` fails for a TDate before the 1601
epoch and **does not write its out parameter**, so the metric returns whatever
was on the stack.

### Evidence

```
DaysDiff(-50  , 100, 30/360) -> FAILURE  days = -123456789 <- OUT PARAM NEVER WRITTEN
DaysDiff(-1   , 100, 30/360) -> FAILURE  days = -123456789 <- OUT PARAM NEVER WRITTEN
DaysDiff(0    , 100, 30/360) -> SUCCESS  days = 100
DaysDiff(1    , 100, 30/360) -> SUCCESS  days = 99

metric(-50, 100) -> 6101050560          <- uninitialised stack read

interpolate at -50 with the 30/360 metric -> -667794.1351
```

The curve interpolated is `f = {0.01, 0.02}`. `JpmcdsLinInterpLongPoint1`
reports SUCCESS.

**The value is not stable, and that is the point.** A second binary built from
the same sources gave `metric(-50, 100) = 4` and an interpolated
**-619018.2639**. Whatever occupies that stack slot is what the caller gets,
so any single figure quoted here is one observation rather than the answer.

Reproduce with `./build/debug/defect_evidence u14`.

### Two things worth noting about how it was found

It is an **uninitialised read**, which AddressSanitizer cannot detect —
MemorySanitizer would, and this project does not run one. So the sanitizer job
is green on it.

And it was found by the **golden generator's full cross-product** of abscissae
against metrics, not by the differential suite, whose sweeps used realistic
dates for each metric. A generator that sweeps everything found what targeted
sweeps missed.

### In the port

**Cannot occur**, and the port additionally diverges by being *correct*.

`metric_distance` checks the status it is given, so a failure becomes
`nullopt`. In practice the branch rarely fires: `std::chrono` has no 1601
epoch, so a pre-epoch abscissa is simply a date in 1600 and the 30/360
difference is well defined. The port returns a sensible extrapolation —
slightly below the first node's value — where upstream returns garbage.

The recorded vectors deliberately **omit** the affected combinations. Recording
the output of undefined behaviour would enshrine it, which is the same rule
that kept UPSTREAM-4's buffer overflow out of the vectors.

## UPSTREAM-15 — A validating constructor that admits unpriceable curves

**Location:** `lib/src/tcurve.c` — `checkTCurve`, reached through
`JpmcdsMakeTCurve`.
**Severity:** Medium. A silent NaN where an error belongs, from inputs a
careful caller would consider reasonable.

### Mechanism

`checkTCurve`'s stated purpose is *"to guarantee we can generate positive
discount factors"*. It checks the day count with `JpmcdsDayCountValid`, which
correctly accepts 30/360 — a thoroughly standard convention — and it accepts
an empty curve, since its only size test is `fNumItems < 0`.

Neither can be priced. `JpmcdsZeroPrice` routes through
`JpmcdsConvertCompoundRate`, which supports ACT/365F and ACT/360 and rejects
everything else explicitly. And `JpmcdsZeroPrice` returns a bare `double`, so
it has nowhere to report the failure.

### Evidence

```
ACT/365F   construct=accepted  JpmcdsZeroPrice=0.7833167239
30/360     construct=accepted  JpmcdsZeroPrice=nan
30E/360    construct=accepted  JpmcdsZeroPrice=nan
empty      construct=accepted  JpmcdsZeroPrice=nan
```

Reproduce with `./build/debug/defect_evidence u15`.

So a caller picks a standard day count, a constructor named `Make…` with an
explicit validation pass accepts it, and every discount factor afterwards is
NaN. This is the same problem as UPSTREAM-2 in a different disguise: there is
no error channel, so the error becomes a number.

### The distinction that matters

Infinity is **not** part of this finding, and conflating the two would
overstate it. A continuous-basis rate of −400 over three years is `exp(1200)`,
which overflows to `+inf` in upstream and in the port alike — both accept the
curve and both return the same infinity. That is a *value*: the answer is
enormous. NaN is not a value; here it is an error channel.

### In the port

**Cannot occur.** `ZeroCurve::create` rejects at construction what cannot be
priced: a non-empty curve is required, and the day count must be one the
conversion supports. That is why `create` is deliberately stricter than
`checkTCurve` on those two counts, and why `zero_price` can return a plain
`double` with no NaN sentinel — every curve that exists can be priced.

The guarantee is pinned as a tier-1 property over 810 discount factors drawn
from the curves `create` accepts: **never NaN**, non-negative, and finite for
any rate a market could quote.

### A note on what else was dropped

`JpmcdsMakeTCurveNoRateCheck` treats a `TDate` of 0 as an array terminator —
*"This allows spreadsheet users to select a big range of dates, but use less
than the whole range by terminating with a 1 or 0."* Measured: five dates with
a zero at index 2 produce a curve of **two** points.

Not reproduced. The port takes a `std::span`, so the length is explicit and a
terminator has nothing to mean — and date 0 is a legitimate date, 1601-01-01,
that a span-based caller could reasonably include. The same input is refused
outright as out of order rather than silently truncated.

## UPSTREAM-16 — A failure that leaves the curve modified

**Location:** `lib/src/zcall.c`, `JpmcdsZCAddGenRate`.
**Severity:** Medium. Partial mutation on an error path — the function reports
failure having already committed a point.

### Mechanism

The function short-circuits when the rate's terms already match the curve's,
and then forgets to stop:

```c
if (basis==zc->basis && dayCountConv==zc->dayCountConv)
{
     if (JpmcdsZCAddRate(zc,date,rate) == FAILURE)
         goto done;
}
/* no return -- falls through */
if (JpmcdsRateToDiscount(rate, zc->valueDate, date, dayCountConv, basis, &df) == FAILURE)
    goto done;
if (JpmcdsZCAddDiscountFactor(zc,date,df) == FAILURE)
    goto done;
```

So the matching case adds the point and then adds it **again** by a second
route.

### Why it is usually harmless

The duplicate detection in `JpmcdsZCAddRateAndDiscount` sees the same date,
compares the rate against the stored one within a hardcoded `1e-7`, and
returns success without doing anything. The second route's rate has survived a
round trip through a discount factor, so it lands well inside that window.
Wasted work, not a wrong answer.

### Why it is not always harmless

A date equal to the curve's value date makes the second route fail:
`JpmcdsDiscountToRate` rejects a zero-length period. The function then returns
FAILURE — after the first route already committed the point.

```
date == valueDate : rc=FAILURE  numItems=1   *** curve MUTATED ***
   orphaned point: date=154497 rate=0.05 discount=1
```

Reproduce with `./build/debug/defect_evidence u16`.

A caller that discards the curve on failure is unaffected. One that retries,
or inspects the error and carries on, now holds a point it never successfully
added — and `numItems` says it is there.

### In the port

**Cannot occur.** `add_general_rate` returns after the matching case, which is
what the missing `return` would have done. That removes the duplicated work
as well, and the rate at the value date now simply succeeds — a rate over no
time discounts by nothing, which is a perfectly good point. The only reason
upstream failed was the redundant second route.

More generally, every route into the builder commits only after all of its
conversions have succeeded, so no failure anywhere can leave a point behind.
That is asserted as a property rather than inferred.

## UPSTREAM-17 — An optimisation that changes the answer at its boundary

**Location:** `lib/src/zcall.c`, `JpmcdsZCComputeDiscount`.
**Severity:** Medium. An infinite discount factor stored in a curve, reported
as success.

### Mechanism

A fast path for the common case, whose own comment calls it "a special
optimization":

```c
if (zc->basis == 1              &&
    rate      >= -1.0           &&
    date      >= zc->valueDate  &&
    (zc->dayCountConv == JPMCDS_ACT_365F || zc->dayCountConv == JPMCDS_ACT_360))
{
    *discount = pow(1+rate, (zc->valueDate - date) /
                    (zc->dayCountConv == JPMCDS_ACT_360 ? 360.0 : 365.0));
    return SUCCESS;
}
```

The guard is `rate >= -1.0`. At exactly `-1.0` the base is zero and the
exponent negative, so `pow` returns an infinity — while the general path it
stands in for **rejects** the same input, because
`JpmcdsRateToDiscountYearFrac` refuses a base at or below zero.

### Evidence

```
rate         ZCComputeDiscount          JpmcdsRateToDiscount
-0.9999999   1.0923357970930543e+56     1.0923357970930543e+56
-1.0         inf                        FAILURE
-1.5         FAILURE                    FAILURE
```

And the infinity is not merely returned; it is stored and then served:

```
JpmcdsZCAddRate(curve, date, -1.0) -> SUCCESS
  stored: date=157419 rate=-1 discount=inf
  JpmcdsZCDiscountFactor at that date -> SUCCESS  inf
```

Reproduce with `./build/debug/defect_evidence u17`.

Two code paths for one quantity, disagreeing at the boundary, and the fast one
wins because it is tested first. The guard wants `rate > -1.0`.

### In the port

**Cannot occur.** The fast path is reproduced — it is not merely an
optimisation, since `(valueDate - date)/365.0` and
`-1.0 * ((date - valueDate)/365.0)` are the same double only because negation
is exact — but it is taken only where it agrees with the general path, so the
boundary case falls through and is refused.

Pinned both ways: a test asserts the two paths agree for every rate inside the
boundary, and that nothing infinite can be stored. `ZeroCurve::create` would
also refuse `rate <= -basis` downstream (that was PORT-11), so the curve is
protected twice over.

## UPSTREAM-18 — An error that is logged, then reported as success

**Location:** `lib/src/zcall.c`, `JpmcdsZCFwdDiscountFactor`.
**Severity:** Medium. A fabricated answer returned with a success status, on a
path the function has already identified as broken.

### Mechanism

```c
*discount = 1.0;
if (discount1<=0.0)                 /* bad discount factor */
{
    JpmcdsErrMsg("JpmcdsZCFwdDiscountFactor: bad discount at start of fwd\n");
}
else
{
    *discount = discount2 / discount1;
}

return SUCCESS;
```

There is no missing error channel here. The function returns `int`, every
caller checks it, and the condition is detected well enough to write a message
about. The status simply does not match what the function found.

And 1.0 is the worst available stand-in: a forward discount factor of 1.0 means
*no discounting over this period*, so `JpmcdsZCForwardRate` converts it to a
forward rate of exactly **zero**.

### Evidence

Reachable by underflow. Continuous basis, a 30-year point, sweeping the rate:

```
rate=20   df1=1.70976e-261  fwd=2.06115e-09  fwd rate=4.85165e+08
rate=23   df1=1.31182e-300  fwd=1.02619e-10  fwd rate=9.7448e+09
rate=24   df1=1.20094e-313  fwd=4.11398e-11  fwd rate=2.43073e+10   <- subnormal
rate=25   df1=0             fwd=1            fwd rate=0             <- fabricated
rate=30   df1=0             fwd=1            fwd rate=0             <- fabricated
```

Reproduce with `./build/debug/defect_evidence u18`.

Note the direction of travel. As the input deteriorates the output stops
*looking* wrong: 2.4e10 is visibly nonsense and any sanity check would catch
it, 0 is not. The failure mode improves in appearance exactly as the situation
gets worse.

The defect is basis-specific. Annual compounding never reaches it — `(1+r)^-t`
decays polynomially in `r`, so even a rate of 10,000% over 30 years gives
`6.7e-61`, nowhere near underflow. Continuous compounding decays exponentially
and crosses the cliff at a rate of about 24 over 30 years.

A 2500% continuous rate is not a market quote. It is what a diverged bootstrap
produces — precisely the moment a caller needs the failure reported rather
than smoothed over.

### In the port

**Cannot occur.** `forward_discount_factor` returns nullopt where upstream
fabricates. The underflow itself is reproduced exactly — the port returns the
same zero discount factor, because that part is arithmetic and not a defect;
only the fabricated forward is refused.

## UPSTREAM-19 — The guard against a bad discount factor cannot see NaN

**Location:** `lib/src/zcall.c`, `JpmcdsZCForwardRate`; and
`lib/src/cfinanci.cpp`, `JpmcdsDiscountToRate`.
**Severity:** Medium. A NaN rate returned as success.

### Mechanism

```c
if (discount<=0.0)                  /* bad discount factor */
{
    JpmcdsErrMsg("JpmcdsZCForwardRate: bad discount for forward\n");
    return FAILURE;
}
```

`NaN <= 0.0` is false. The one check whose entire job is to stop an unusable
discount factor is written as the one comparison every NaN passes. The NaN goes
into `JpmcdsDiscountToRate`, which guards the same way — `if (discount <= 0.0)`
— and comes back out as a NaN rate with a SUCCESS status.

### Where the NaN comes from, and why that matters

Continuous basis, a 30-year horizon, a large **negative** rate overflows the
discount factor:

```
rate=-20   df1=5.84877e+260  df2=2.83762e+269  fwd=4.85165e+08  rate=-1
rate=-23   df1=7.62298e+299  df2=inf           fwd=inf          rate=-1
rate=-24   df1=inf           df2=inf           fwd=nan          rate=nan
rate=-100  df1=inf           df2=inf           fwd=nan          rate=nan
```

Reproduce with `./build/debug/defect_evidence u19`.

Both discount factors are legitimately infinite. That is the same overflow
UPSTREAM-15 concluded is a **value** and not a finding, since the answer really
is that large, and the port returns the same infinities. The defect is
downstream of it: the ratio of two infinities is indeterminate, and nothing
stops the result.

So the lineage is UPSTREAM-15's accepted infinity feeding UPSTREAM-19's blind
guard. A correct value flows into a check that cannot represent what happens
when two of them are combined — the same relationship UPSTREAM-9 has with
UPSTREAM-10, where one supplies the bad input and the other fails to contain
it.

### In the port

**Cannot occur.** `forward_discount_factor` rejects any non-finite result, and
`discount_to_rate` was tightened to `!(discount > 0.0) || !std::isfinite(...)`
— upstream's guard written so that it means what it was for.

Fixed at the conversion rather than at each caller, because a NaN discount
factor is not a rate no matter who asks. The tier-1 property is the general
form: across every basis, day count and rate in the sweep, **no evaluation
returns NaN**. Infinity is still permitted, since UPSTREAM-15 settled that it
is a value.

### A second site: `JpmcdsForwardFromZCurve`

The same defect is reachable from `zr2fwd.c`, where there is no guard at all:

```c
forwardDiscount = endDiscount/startDiscount;

if (JpmcdsDiscountToRate(forwardDiscount, startDate, endDate,
                      dayCountConv, basis, rate) == FAILURE)
```

That is strictly less protection than `JpmcdsZCFwdDiscountFactor` has --
there, at least, `if (discount1 <= 0.0)` is tested, badly (UPSTREAM-18), but
tested. Here everything rests on `JpmcdsDiscountToRate`, whose guard is the one
above. Measured, continuous basis over 30 years:

```
rate   disc(start)     disc(end)       fwd disc   rc        forward rate
0.03   0.406302        0.394294        0.970446   SUCCESS   0.0304545
24     1.20094e-313    4.94066e-324    4.11e-11   SUCCESS   2.43073e+10
25     0               0               nan        SUCCESS   nan
-24    inf             inf             nan        SUCCESS   nan
```

Both ends of the range produce a NaN rate reported as success: `0/0` where the
factors underflow and `inf/inf` where they overflow.

Recorded here rather than as a separate entry. The mechanism is the same blind
guard, the consequence is the same NaN-as-success, and the fix was the same one
line in `discount_to_rate` -- the port needed no new work to be immune at this
site. Giving it its own number would make the register count entry points
rather than defects.

## UPSTREAM-20 — The precondition the documentation does not mention

**Location:** `lib/src/cashflow.c`, `JpmcdsMergeCFL`.
**Severity:** Low. Unreachable from inside upstream; reachable through the
public header.

### Mechanism

The header states the contract in one sentence:

> Merges two cash flow lists. The input cash flow lists cannot be NULL.

That precondition is real, and it is checked — on both arguments.

The other one is not stated and not checked. The algorithm is a single forward
pass that advances whichever side carries the earlier date, so it is a merge
only if both inputs are already ordered by date. Nothing verifies that, and
nothing in the documentation asks the caller to.

### Evidence

With `a = [40000, 20000, 30000]` and `b = [25000, 15000]`:

```
MergeCFL -> fNumItems=5
    [0] date=25000  amount=10
    [1] date=15000  amount=20
    [2] date=40000  amount=1
    [3] date=20000  amount=2
    [4] date=30000  amount=3
```

Reproduce with `./build/debug/defect_evidence u20`.

Every element is present, so nothing is lost — but the output is not sorted,
and it is not a stable interleaving of the inputs either. It is an artefact of
where the comparison happened to stop. A caller who reads the header, satisfies
the one documented precondition, and passes lists in whatever order they
arrived has no way to learn the result is meaningless.

### Why the severity is low

Exactly one call site exists in the library: `zcswap.c:804`, merging a fixed
and a floating schedule, both generated in date order. So this is unreachable
from inside upstream and reachable only by an external caller of the public
header — the same tier as UPSTREAM-12.

### In the port

**Cannot occur.** `merge_cash_flows` returns nullopt unless both inputs are
ordered. Checking a precondition the documentation forgot is the move
`ZeroCurve::create` makes over `checkTCurve` (UPSTREAM-15): refuse at the
boundary what cannot produce a meaningful answer, so the result type never has
to carry a "this is nonsense" value.

Non-decreasing, not strictly increasing — a list may legitimately carry two
payments on one date, and upstream's merge passes such a pair through
uncoalesced. Both forms represent the same cash flow, so neither is wrong.

### What this function is NOT

The first hypothesis here was different, and it was wrong. `JpmcdsNewEmptyCFL`
sets `fNumItems` to the count it was asked for and allocates through
`NEW_ARRAY`, hence `JpmcdsMallocSafe`, whose allocation line reads:

```c
ptr = malloc(theSize);              /* Don't use calloc due to RS6000. */
```

On that line alone, a public function named "NewEmpty" returns *n* cash flows
of heap garbage. Four separate probes said otherwise — a size-matched recycled
block, `MallocPreScribble=1`, and an ASan build all came back zeroed — and the
reason is the next branch of the same function:

```c
/* Clear out the memory for safety. */
(void)memset((char *)ptr, (size_t)0, theSize);
```

The comment about calloc means "we reproduce calloc by hand", not "we skip the
zeroing". **No defect**, and the negative result is worth more than the finding
would have been: every `NEW` and `NEW_ARRAY` in the library returns zeroed
memory, so upstream cannot have an uninitialised-*heap* read anywhere. The one
uninitialised read in this register, UPSTREAM-14, is a stack local — where no
allocator intervenes.

One detail does deserve recording. `fDate = 0` is not a null in upstream's
encoding; TDate 0 is **1601-01-01**, a date like any other. A list from
`JpmcdsNewEmptyCFL(n)` that a caller under-fills does not look uninitialised —
it looks like *n* deliberate payments of zero in 1601. Garbage might be caught
downstream; that will not be. The port has no equivalent, because its `Date`
has no zero and length is the number of payments present.

## UPSTREAM-21 — The error path dereferences a null pointer

**Location:** `lib/src/zcall.c`, `JpmcdsZCAddCashFlowList`.
**Severity:** Medium. Deterministic SIGSEGV on an empty cash flow list, and the
crash is in the handler that exists to report the error.

### Mechanism

Two reads of the last element, neither guarded:

```c
if (date==0L)                       /* add at last c.f. if not set up */
{
    date = cfl->fArray[ cfl->fNumItems-1].fDate;
}
...
done:
    if (status == FAILURE)
        JpmcdsErrMsg("%s: Failed for CFL maturing %s.\n", routine,
                     JpmcdsFormatDate(cfl->fArray[cfl->fNumItems-1].fDate));
```

`JpmcdsNewEmptyCFL(0)` — a public constructor — returns `fNumItems = 0` with
`fArray = NULL`, so both reads are `NULL[-1]`.

### Evidence

```
empty CFL: fNumItems=0 fArray=0x0
calling with date=0 ...
exit status: 139          (SIGSEGV)
```

And with an explicit `date`, which skips the first read entirely:

```
explicit date, empty CFL -> exit status: 139
```

Reproduce with `./build/debug/defect_evidence u21 --unsafe`.

The second measurement is the one that matters. Given an explicit date, the
function reaches its own `"no cash flows in list beyond ZeroCurve. Therefore,
nothing to add."` branch — the error is detected, the branch is written, the
message is composed — and then `done:` dereferences the empty list while
formatting it. **The diagnostic path for an empty list is the one path that
cannot survive an empty list**, so there is no way for a caller to receive the
error the function was written to report.

### In the port

**Cannot occur.** An empty span is refused before anything reads from it, and
nothing formats a payment that is not there. The differential test for this one
is `DISABLED_` — it takes the process with it — which is the same arrangement
UPSTREAM-4 and UPSTREAM-9 use.

## UPSTREAM-22 — Success with nothing added

**Location:** `lib/src/zcall.c`, `JpmcdsZCAddCashFlowList`.
**Severity:** Medium. The function reports that an instrument was bootstrapped
when it did nothing at all.

### Mechanism

When the final payment falls exactly on the curve's last date, every branch
declines in turn:

 * the guard rejects a final payment *earlier* than the curve's last date
   (`fArray[firstUncovered].fDate < lastZCDate`), so equality passes it;
 * the backward search for the first uncovered payment then finds none, and
   `firstUncovered` is left at `fNumItems`;
 * `firstUncovered == cfl->fNumItems-1` is false, so the single-payment branch
   is skipped;
 * `firstUncovered < cfl->fNumItems` is false, so the root-finding branch is
   skipped — and so is the `JpmcdsZCAddCFLPoints` call guarded by the same
   test;
 * `status = SUCCESS`.

### Evidence

A 3-point curve ending 2027-01-01, a list ending on that same date, asked to
add a point at 2030-01-01:

```
rc=SUCCESS   numItems 3 -> 3   point at 2030 present? NO
```

Reproduce with `./build/debug/defect_evidence u22`.

The caller is told the instrument was bootstrapped. The curve does not contain
the date it asked for, and no rate was solved for anything. The next instrument
then bootstraps against a curve one point shorter than the caller believes,
which is how a silent no-op becomes a wrong curve rather than a missing point.

### In the port

**Cannot occur.** `add_cash_flow_list` returns false when there is nothing
uncovered to solve for. If no rate was found, the instrument was not added, and
saying so is the only honest answer.

## UPSTREAM-23 — A sign test that silently drops curve points

**Location:** `lib/src/zcall.c`, `JpmcdsZCAddCFLPoints`.
**Severity:** Low — but only because the consequence turned out to be smaller
than it looks. See "what it does not do" below.

### Mechanism

```c
if (rate >= 0.0)
{
    dates[n] = d;
    rates[n++] = rate;
}
```

No `else`, no message, no status. A cash flow date whose interpolated rate is
negative is not added — in a function whose own comment is *"Ensure all
uncovered cash flow dates in curve. ... This will make sure they can be priced
appropriately by the curve."*

### Evidence

A five-payment zero-coupon bond over a curve at −0.5%, varying only the price:

```
price=0.97  solved rate +0.0044     dates placed 2 of 5
price=1.00  solved rate −2.2e-13    dates placed 1 of 5
price=1.06  solved rate −0.0083     dates placed 1 of 5
price=1.20  solved rate −0.0257     dates placed 1 of 5
```

Reproduce with `./build/debug/defect_evidence u23`.

The cliff is at exactly zero. A solved rate of −2e-13 drops four points that a
rate of +2e-13 would have kept, so the curve's *shape* turns on the sign of a
number that is, at that magnitude, noise.

Negative zero rates are not exotic — EUR, CHF and JPY curves were negative for
most of a decade after 2014 — so this is reachable from ordinary market data
rather than from a pathological input.

### What it does not do, having tried to show that it does

The expected consequence was a broken bootstrap: points missing means the
instrument no longer reprices to its own input. **It does not.** Measured under
linear interpolation and under flat forwards, with 1 of 5 dates placed, the
instrument still reprices to within the solver's f-tolerance:

```
NEGATIVE curve, price>par     dates placed 1/5   input 1.040000 -> 1.0400000000
NEGATIVE curve, deep premium  dates placed 1/5   input 1.080000 -> 1.0800000000
```

The reason is that inserting a point at its own interpolated rate is idempotent
for both schemes. Linear interpolation puts the new point exactly on the line;
flat forwards is log-linear in the discount factor, which puts it exactly on
that curve. Splitting an interval at a point already on the path leaves the
path alone.

So the finding is about data, not prices: a caller who inspects the finished
curve sees a different number of points depending on the sign of the rates, for
no documented reason, and the `>= 0.0` test is dead weight that happens to be
visible.

### In the port

The port reproduces no filter at all — every missing date gets its interpolated
rate, which is what the comment describes. The divergence is recorded as data
rather than prose: the golden vectors carry the exact dates upstream dropped,
and the replay asserts that the port's curve differs from upstream's by exactly
those dates and nothing else. Twelve of the sixty recorded bootstraps diverge
this way, and that count is asserted too, so a change in upstream's behaviour
surfaces as a failure rather than as fewer checks.

## UPSTREAM-24 — The roll date is consulted in the opposite case to the one documented

**Location:** `lib/src/datelist.c`, `JpmcdsNewDateListExtendedRoll`.
**Severity:** Low. A schedule generated from the wrong end, in a window one
day wide.

### Mechanism

The header states the rule:

> If rollDate is non-zero and stubAtEnd is False, then the rollDate is used to
> generate cash flow dates. First starting from roll-date we see if we can
> count forward an integral number of intervals to the maturity date. If this
> is so then we can generate our datelist by counting forward from the
> rolldate.

The code tests something else:

```c
if (JpmcdsCountDates(startDate, maturityDate, interval, &numIntervals, &extraDays) != SUCCESS)
    goto done;

if (extraDays == 0)
{
    /* We can use the roll date - so we count forward from this day */
```

`startDate` to maturity, not `rollDate` to maturity. The difference inverts
the feature:

 * `extraDays == 0` means start-to-maturity is a whole number of intervals —
   **there is no stub**, which is the case where a roll date has nothing to
   contribute. The roll date is consulted here.
 * `extraDays != 0` means there **is** a stub, the only case where a roll date
   could supply a grid that lands on maturity. The roll date is ignored here,
   and the function falls back to plain `JpmcdsNewDateListExtended`.

### Evidence

Quarterly, roll 2024-01-15, maturity 2027-01-15 (exactly twelve quarters from
the roll), start 2024-02-20 (mid-period) — the case the documentation
describes:

```
roll  -> mat   intervals=12  extraDays=0     INTEGRAL
start -> mat   intervals=11  extraDays=56    has a stub

with the roll date   n=13: 154511 154602 ... 155607
with rollDate = 0    n=13: 154511 154602 ... 155607
=> the roll date CHANGED NOTHING
```

Reproduce with `./build/debug/defect_evidence u24`.

Conversely, when start-to-maturity *is* integral the roll date does take
effect — and introduces a stub, producing a list that begins before the start
date and ends past the maturity.

### Reachability, and a claim withdrawn

The library's only call site is `cashflow.c:230`, which passes **startDate as
the roll date**. The obvious reading is that this makes the mechanism inert:
`count_dates(roll, start)` is trivially integral when the two are equal, so
`firstDate` becomes `startDate`. Three hand-picked probes agreed, and the first
draft of this record said so.

The exhaustive sweep found the counterexample. The roll branch ends in
`NewDateListExtended(first, maturity, interval, TRUE)` — generating **forward**
from the start — where `rollDate = 0` generates **backward** from maturity.
Those agree only where month arithmetic does not clamp, and clamping is not
invertible: 29 February plus twelve months is 28 February, but 28 February
minus twelve months is 28 February, not the 29th.

Measured over every integral-span combination of start and maturity
day-of-month, monthly through annual:

```
2024-02-29  2027-02-28  iv=1   DIFFER: first 154556 vs 154555, n=37 vs 37
2024-02-29  2027-02-28  iv=3   DIFFER: first 154556 vs 154555, n=13 vs 13
...
7 of 378 integral-span cases differ (1.9%)
```

Every one starts on 29 February. So this **is** reachable from inside upstream
— a leap-day start whose maturity falls on the clamped day gets a
forward-generated schedule where a back-stub request asked for a backward one.
Still low severity, because the window is that narrow and the schedules differ
by a day at one end rather than in length. But "inert" was wrong, and only the
sweep found it.

### In the port — reproduced, deliberately

Unlike `date_list.hpp`, which corrects upstream, this module does not. The
policy in `docs/port-notes.md` permits a correction where there is no single
upstream behaviour to preserve — UPSTREAM-7, where two implementations
contradicted each other and the header. That is not the situation here: the
code is self-consistent and deterministic, and only the documentation disagrees
with it.

A roll date decides coupon dates, coupon dates decide accrual, and accrual
decides money. Reinterpreting a roll-date convention on the strength of a
comment is not a porting decision. PORT-9 is the standing reminder of what
happens when a porter picks a winner between two disagreeing sources; this is
the same hazard with the sources swapped.

The behaviour is reproduced and the disagreement is pinned by a test that
asserts the roll date is inert in the documented case, effective in the
undocumented one, and that every `roll == start` difference involves a 29
February start.

## UPSTREAM-25 — A heap buffer overflow on a one-date schedule

**Location:** `lib/src/cashflow.c`, `JpmcdsNewCFLFromDL3`.
**Severity:** Medium. Out-of-bounds read in both directions and a latent
out-of-bounds write, reachable only through the public header.

### Mechanism

The function detects a stub by comparing the instrument's dates against the
grid it was handed:

```c
frontStub = ( startDate > dates->fArray[0] );
backStub = ( maturityDate < dates->fArray[numDates-1] );
```

Neither branch checks that the grid is long enough to have a surrounding
period. With `numDates == 1` the front branch indexes past the end:

```c
JpmcdsStubPayment(accrualDates->fArray[0],
                  accrualDates->fArray[1],      /* index 1 of a 1-element list */
                  accrualStart,
                  accrualDates->fArray[1],
                  rate, couponDayCountConv,
                  &cfl->fArray[1].fAmount)      /* WRITE past a 1-element array */
```

and the back branch indexes before it, `accrualDates->fArray[numDates-2]`
being `fArray[-1]`.

### Evidence

```
front stub: ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...0f8
            READ of size 8    #0 JpmcdsNewCFLFromDL3
            allocated by      #2 JpmcdsNewEmptyDateList

back stub:  ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...0e8
            READ of size 8    #0 JpmcdsNewCFLFromDL3
```

Reproduce with `./build/debug/defect_evidence u25 --unsafe`.

Note the two addresses: `...0f8` is past the one-element allocation and
`...0e8` is before it. Both directions are real.

In the unsanitised build the front case happens to return NULL — the garbage
it read as `nextCouponDate` failed `JpmcdsStubPayment`'s
`prevCouponDate >= nextCouponDate` check. **That is luck, not safety.** A
garbage value on the other side of that comparison lets the function proceed to
the out-of-bounds *write*, which ASan never gets to report because it aborts on
the read first.

### Reachability

`JpmcdsMakeCFL` builds its own grid, and `JpmcdsNewDateListExtended` never
returns a single date together with a stub — it anchors the grid on whichever
end the stub is not at, so one of the two comparisons is always false. Measured:

```
stubAtEnd=0: first=154261 last=155722 -> frontStub=1 backStub=0
stubAtEnd=1: first=154547 last=156008 -> frontStub=0 backStub=1
```

But `JpmcdsNewCFLFromDL3` is declared in `cashflow.h` and takes the grid as an
argument, so a caller supplying its own date list reaches it directly. That is
why this is medium rather than high: unreachable from inside upstream,
reachable from outside it.

### In the port

**Cannot occur.** The grid arrives as a `std::span`, and a grid shorter than
two dates is refused when a stub was detected. There is nothing to compute: a
stub payment needs the period surrounding it, and a one-date grid has none.

The live reproducer is a `DISABLED_` test — ASan aborts the process — which is
the arrangement UPSTREAM-4, UPSTREAM-9 and UPSTREAM-21 already use.

### A second finding in the same function, not filed

The long-stub coalescing block runs two branches in sequence, and the first
reassigns the pointer the second reads from:

```c
if ( frontStub )
{
    datesCopy->fArray[0] = dates->fArray[0];
    memcpy(&datesCopy->fArray[1], &dates->fArray[2], (numDates - 2) * sizeof(TDate));
    dates = datesCopy;          /* simply overwrite pointer, hiding original */
    --numDates;
}
if (backStub)
{
    memcpy(&datesCopy->fArray[0], &dates->fArray[0], (numDates - 2) * sizeof(TDate));
```

With both stubs present, `dates == datesCopy` by the time the second `memcpy`
runs, so source and destination are the same pointer. C11 7.24.1p2 makes
copying between overlapping objects undefined, and identical objects overlap.

Not filed as a defect. The result is correct — five grid dates coalesce to
three flows, as intended — and no implementation mishandles `memcpy(p, p, n)`.
Recorded because a compiler entitled to assume `restrict` on those parameters
would be within its rights to make it stop working.

Worth noting for the harness rather than for upstream: **neither
AddressSanitizer nor UndefinedBehaviorSanitizer reports it.** The project
already records that ASan cannot see uninitialised reads; this is a second
class it cannot see, in the same function where it caught the overflow above.

## UPSTREAM-26 — A substitution table silently disables the convention

**Location:** `lib/src/zcswdate.c`, `JpmcdsZCAdjustDate`.
**Severity:** Low. The function's documented purpose is not achieved on one of
its two paths, for dates the table does not mention.

### Mechanism

The function branches on the *presence* of a bad day list, not on its contents:

```c
if (badDayList != NULL)
{
    *adjDate = JpmcdsBad2GoodBadDayList(badDayList, date);
    return SUCCESS;
}
else
{
    if (JpmcdsBusinessDay(date, badDayConv, holidayFile, adjDate) == FAILURE)
        goto done;
}
```

So when a list is supplied, `badDayConv` and `holidayFile` are never consulted.
And `JpmcdsBad2GoodBadDayList` returns its input unchanged for any date the
list does not contain — see UPSTREAM-25's neighbour, `badday.c`, where that is
the documented behaviour.

The function is documented as *"Adjusts date to fall on a business day, if
necesssary"*. On the list path it does not.

### Evidence

With a table containing one Saturday:

```
input                                    adjusted to
Saturday, no list, conv=F                154504   (moved to Monday)
Saturday, in the list, conv=F            154504   (from the list)
another Saturday, no list, conv=F        154511   (moved)
another Saturday, list given, conv=F     154509   <- STILL A SATURDAY
```

Reproduce with `./build/debug/defect_evidence u26`.

The second Saturday is absent from the table, so it comes back untouched even
though a convention was supplied that would have moved it.

An **empty** table does this to every date, because the test is on the pointer
rather than on `count`:

```
no list (NULL),   conv=F -> 154504
EMPTY list,       conv=F -> 154502   <- unchanged, still a Saturday
```

### Why it is low, and what the port does instead

The two modes are evidently intended as alternatives — a caller supplies either
a complete substitution table or a convention — and a table that omits a bad
day is incomplete, which is the caller's business. What is wrong is the
documented purpose, not the code.

So the port reproduces the behaviour and fixes the *signature* instead.
`zc_adjust_date` takes `std::optional<std::span<const BadDayPair>>`, which says
"either a table or the convention" where upstream's two unrelated parameters
say "both".

That measurement is also why the parameter is an `optional` rather than a plain
span. "No table" and "a table with nothing in it" are genuinely different
inputs with different results, and a span alone would have merged them —
silently changing what an empty table means. This is the one case so far where
a parameter had to be *less* convenient to stay faithful.

## UPSTREAM-27 — A day-of-month heuristic that suppresses real cycles

**Location:** `lib/src/zcswdate.c`, the file-local `SetPrevDateAndOnCycle`.
**Severity:** Low. A wrong flag in 8% of month-end cases, and a previous coupon
date off by one day in a narrow subset of those.

### Mechanism

The on-cycle decision is made in two steps, and the first is a guess:

```c
if ((valueMDY.day <= 28) && (origMDY.day <= 28))
{
    /*
     * We assume we can only be on cycle if date is not on or after
     * the 29th of the month.
     */
    JpmcdsCountDates(valueDate, origDate, &interval, &numIntervals, &extraDays);
    *onCycle = (extraDays == 0);
}
else
{
    *onCycle = FALSE;
}
```

The assumption is false. `JpmcdsCountDates` reports an exact whole number of
intervals for month-end dates perfectly well:

```
day 29: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
day 30: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
day 31: CountDates says extraDays=0   so genuinely ON cycle; heuristic says OFF
```

### Evidence, and the two consequences

The reported `onCycle` flag is wrong for those dates. Swept over every month,
the days 26 to 31, and four frequencies: the heuristic suppresses **600 of
7,296** genuinely on-cycle combinations.

And because the flag chooses how `previous` is computed — forward from the
value date when on cycle, backward from the maturity when not — the suppression
can change the date:

```
freq=1   2024-02-29 -> 2025-02-28 : on-cycle 154556, off-cycle 154555  DIFFER by 1
freq=2   2024-02-29 -> 2027-02-28 : on-cycle 155468, off-cycle 155467  DIFFER by 1
...
14 of the 600 give a different previous coupon date (2.3%)
```

Reproduce with `./build/debug/defect_evidence u27`.

Every one has a **29 February value date** — the same clamping asymmetry as
UPSTREAM-24, where forward from the 29th and backward from the 28th do not meet
because month arithmetic is not invertible.

Worth being precise about which answer is which. In the annual case the forward
route is `valueDate + 0 intervals`, the value date itself, which is by
definition on the schedule. The backward route gives the day before it — a date
the schedule does not contain. So on these fourteen the heuristic does not
merely lose precision; it picks the route that returns a date that is not a
coupon date.

### In the port — reproduced, deliberately

Deciding that a swap's previous coupon date should move by a day is a
convention question, not a porting one. Same reasoning as UPSTREAM-24, same
conclusion: the behaviour is reproduced and the disagreement is recorded.

The heuristic is pinned from both sides. One test asserts that an `on_cycle`
flag of true really does mean a whole number of intervals — checked against
`count_dates`, not against upstream — and another asserts that a false flag
does **not** imply the converse, which is this defect stated as a property.

### One thing the port did not need to reproduce

`numIntervals` is declared uninitialised and read only in the on-cycle branch.
That is safe, because `*onCycle` can only be true where `JpmcdsCountDates` has
just set it — but the safety is an argument about control flow rather than a
property of the code, which is exactly the shape UPSTREAM-14 is. Here the
argument happens to hold, and the port keeps the variable initialised anyway.

## UPSTREAM-28 — A process-wide flag left disabled on the success paths

**Location:** `lib/src/interpc.c`, `JpmcdsInterpRateExact`.
**Severity:** Medium. Order validation silently disabled process-wide and
permanently, by ordinary extrapolation.

### Mechanism

The function opens by disabling the binary search's order checking, saving the
previous value to restore later:

```c
/*
** Since we are using the general interpolation functions, but we
** know that the TCurve should have been constructed properly,
** we will turn off order checking in the binary search routines.
**
** This might well produce a dramatic improvement in all the ALIB
** zero curve analytics!
*/
TBoolean checkOrder = JpmcdsBSearchCheckOrder (FALSE);
```

and restores it at the `done:` label:

```c
done:
    /* Re-set the global check order flag. */
    JpmcdsBSearchCheckOrder (checkOrder);
```

Between those two points sit **two `return SUCCESS` statements** — the clamps
for a date past the end of the curve and a date before its start:

```c
if (desiredDate > curve->fArray[curveLen-1].fDate)
{
    *rate = curve->fArray[curveLen-1].fRate;
    return SUCCESS;                       /* bypasses done: */
}
else if (desiredDate < curve->fArray[0].fDate)
{
    *rate = curve->fArray[0].fRate;
    return SUCCESS;                       /* bypasses done: */
}
```

### Evidence

```
after calling with...                        order checking
(initial state)                              ON
a date inside the curve                      ON
a date AFTER the curve (extrapolation)       OFF  <- LEAKED
a date BEFORE the curve (extrapolation)      OFF  <- LEAKED
an empty curve (an error path)                ON
```

Reproduce with `./build/debug/defect_evidence u28`.

And the consequence, in two calls:

```
order checking ON : JpmcdsBSearchDouble(unsorted) -> FAILURE
after one extrapolation: same call               -> SUCCESS  <- no longer validated
```

An unsorted array that the checked search correctly rejected now passes. The
precondition `JpmcdsBSearchDouble` and `JpmcdsBSearchLong` exist to verify is
simply no longer verified, anywhere in the process, for the rest of its life.

### Its relationship to UPSTREAM-13

Same global, same failure mode, **inverted reachability**. UPSTREAM-13 leaks
`bSearchCheckOrder_g` on two *error* paths in `lintrp1.inc`. This leaks it on
two *success* paths, and extrapolating off the end of a curve is ordinary use
rather than a fault — a swap maturing beyond the last curve point hits it on
every valuation.

Note the last row of the measurement: here the genuine error path reaches
`done:` and restores correctly, which is the exact opposite of UPSTREAM-13. Two
functions share one global and leak it under complementary conditions, so
between them there is no state of the program in which the flag can be trusted.

The comment is worth keeping too. The mechanism was introduced as a performance
speculation — *"This might well produce a dramatic improvement in all the ALIB
zero curve analytics!"* — and the speculation is what created the leak. The
optimisation it describes is to skip a validation; the bug is that it skips it
for everyone, forever.

### In the port

**Cannot occur**, structurally rather than carefully: there is no global.
`bracket` and `bracket_checked` are separate functions and the caller picks
which to use, which is the decision recorded in `bsearch.hpp` when UPSTREAM-13
was filed. A flag that does not exist cannot be left in the wrong state.

So one decision has now pre-empted two defects, in two files, found two slices
apart. That is the argument for removing mutable global state rather than
auditing its users: the second defect needed no new work to be immune to.

The differential test for this one resets the flag in `SetUp` and `TearDown`,
because the function under test leaks it — without that, test *order* would
decide what the checked searches do for every other test in the binary.

## UPSTREAM-29 — A schedule the library returns and its own validator rejects

**Location:** `lib/src/schedule.c`, `JpmcdsNewCouponDatesSwap` and
`CheckArgsSwapMakeSchedule`. Consequence in `lib/src/zcswap.c`, lines 725, 743
and 744.
**Severity:** Medium. A structurally invalid schedule returned as SUCCESS, and
an out-of-bounds heap read in the only part of the library that consumes it
without validating.

### Mechanism

`CheckArgsSwapMakeSchedule` opens with

```c
if (startDate > matDate)
{
    JpmcdsErrMsg("%s: maturity date (%s) is on or before start date (%s)\n", ...
```

Two things are wrong with those four lines together. The message says *on or
before* while the condition says *after*, so equality is accepted; and the
message is the only documentation of the intent, so the gap is invisible unless
you read both.

Equality then produces one unadjusted period of zero length. The
empty-accrual-period removal loop — introduced to handle exactly this, with the
comment *"Thanks to Neill Penney for the solution"* — drops it:

```c
if ( adjStreamSched->fArray[i].accrueStartDate !=
     adjStreamSched->fArray[i].accrueEndDate )
{
    streamSched->fArray[j] = adjStreamSched->fArray[i];
    j++;
}
else
{
   if ( i==0 )
   {
      ...
      streamSched->fNumItems--;
```

With one period there is nothing left, and `fNumItems` reaches zero. The
consistency check that follows compares `j` against `fNumItems` — both zero, so
it passes. `retCode = SUCCESS`, and a non-NULL list comes back.

That list is rejected by the library's own validator for this exact type:

```c
if (schedule->fNumItems < 1)
{
    JpmcdsErrMsg("%s: schedule must have at least one coupon period"
              " but has only %d.\n", ...
    return FAILURE;
```

So `JpmcdsNewCouponDatesSwap` and `JpmcdsCouponDateListCheck` disagree about
the same value. One of them is wrong, and nothing in the library says which.

### Not confined to the degenerate input

The obvious reading is that this needs `startDate == matDate` — a zero-tenor
swap, which no sane caller builds. It does not. Any span whose every accrual
period collapses *under bad-day adjustment* arrives at the same place, and a
weekend does it:

| input | periods | stubLocation | validator |
|---|---|---|---|
| start == maturity, front stub | 0 | NO_STUB | FAILURE |
| start == maturity, back stub | 0 | BACK_STUB | FAILURE |
| 2024-03-16 → 2024-03-17 (Sat → Sun), FOLLOW | 0 | NO_STUB | FAILURE |

Both dates in the third row adjust forward to Monday the 18th, so the single
period has `accrueStartDate == accrueEndDate` and is removed. This matters for
severity: a one-day span over a weekend is not obviously invalid input, and the
port's guard therefore tests the *outcome* rather than the input.

The `stubLocation` column is a second finding inside the first. At `i == 0` the
removal maps `FRONT_STUB` to `NO_STUB` and `TWO_STUB` to `BACK_STUB`, but
leaves `BACK_STUB` alone — correctly, since dropping the front period does not
remove a stub at the back. With no periods left at all, the schedule still
reports one. Note also that upstream's `i == 0` arm is an `else if` chain ahead
of its `i == fNumItems-1` arm, so for a single-period schedule the front arm
wins and the back mapping never runs.

Both mapping arms read `adjStreamSched->stubLocation`, the original value,
rather than the one being written. When the front and the back period both
collapse, `FRONT -> NONE` and then `TWO -> FRONT` are evaluated against the
same original, landing on `FRONT` where composing the two would have given
`NONE`. The port reproduces that, with a comment, because it is the behaviour.

### Why it matters

`zcswap.c` consumes the schedule without validating it:

```c
   (floatDL->fArray[floatDL->fNumItems-1].resetDate,          /* line 725 */
   maxDate = MAX(indexMatDate, floatDL->fArray[floatDL->fNumItems-1].payDate);
   minDate = MIN(indexMatDate, floatDL->fArray[floatDL->fNumItems-1].payDate);
```

There is no emptiness check and no call to `JpmcdsCouponDateListCheck` on
either schedule it builds. With `fNumItems == 0` all three are `fArray[-1]`.

The reachability is ordinary rather than exotic: `zcswap.c` passes
`zc->valueDate` as the start date, so a benchmark instrument maturing on the
curve's own value date is sufficient. `streamcf.c:1066` *does* call the
validator, and is safe.

Under ASan the read reports as

```
ERROR: AddressSanitizer: heap-use-after-free
  ... freed by thread T0 here:
      JpmcdsFreeCouponDateList schedule.c:631
      JpmcdsNewCouponDatesSwap schedule.c:345
```

rather than as a buffer underflow, because the 40 bytes immediately before
`streamSched->fArray` are `adjStreamSched->fArray` — the scratch list the
function allocates at `schedule.c:115` and frees at `:345` on its way out,
including on the success path (the `error:` label is reached after
`retCode = SUCCESS`, so it is a cleanup label, not an error one). Both labels
are accurate. The fault is the negative index; which block it lands in is the
allocator's choice.

Reproduce with `defect_evidence u29`, and `defect_evidence u29 --unsafe` under
the asan preset for the trace.

### In the port

**Cannot occur.** `coupon_schedule` returns `std::nullopt` when the removal
leaves no periods, so `CouponSchedule::periods` is non-empty by construction
and `is_well_formed` — the port's `JpmcdsCouponDateListCheck` — is true of
every schedule the port builds. The guard is on the outcome, not on
`start == maturity`, because the weekend case above shows the input test is the
wrong test.

This is a **deliberate difference**, and it is one of the cases the port's
policy says to correct rather than reproduce: there is no single upstream
behaviour to preserve when the constructor and the validator contradict each
other about the same value. Reproducing either means contradicting the other.

It is pinned without the oracle. The zero-period cases are recorded in
`tests/golden/coupon_schedule_cases.hpp` as `accepted == true` with
`period_count == 0`, and the tier-1 replay asserts the port declines exactly
those four and no others — so if the refusal is ever removed, or widened, CI
says so on a machine with no upstream copy.

## UPSTREAM-30 — A buffer sized from one span and filled from another

**Location:** `lib/src/schedule.c`, `JpmcdsGenerateUnadjDates`.
**Severity:** Medium. Heap buffer overflow at a public entry point with no
argument validation.

### Mechanism

The function sizes its scratch array from the span it was asked about:

```c
if (JpmcdsCountDates(startDate, matDate, couponInterval, &numPeriods, &dummyInt)==FAILURE)
    goto done;
numPeriods = numPeriods + 3;
dateArr = NEW_ARRAY(TDate,numPeriods);
```

and then steps the generating loop from somewhere else entirely:

```c
if (firstRollDate > 0)
{
    beginDate = firstRollDate;
    endDate   = matDate;
    ...
    dateArr[nDates++] = unadjustedDate;        /* line 761 */
```

`startDate..matDate` sized the array; `firstRollDate..matDate` fills it. A roll
date before `startDate` makes the second span the longer one, and the `+ 3` of
slack is not a bound on the difference. The `lastRollDate` branch has the mirror
problem with a roll date after `matDate`.

Measured, with a one-month interval:

| span | by | dates |
|---|---|---|
| sizing: 2024-01-01 → 2024-07-01 | 1M | ~9 slots allocated |
| stepping: 1990-01-01 → 2024-07-01 | 1M | ~415 written |

```
ERROR: AddressSanitizer: heap-buffer-overflow
SUMMARY: AddressSanitizer: heap-buffer-overflow
  schedule.c:761 in JpmcdsGenerateUnadjDates
```

The function validates none of its nine arguments. It does not call
`CheckArgsSwapMakeSchedule`, and nothing in its own body bounds the roll dates.

### Reachability, and why it is still a defect

`JpmcdsNewCouponDatesSwap` bounds both roll dates before calling:

```c
if (firstRollDate < startDate) { ... return FAILURE; }
if (firstRollDate > matDate)   { ... return FAILURE; }
```

and within `[startDate, matDate]` the stepped span is always a sub-span of the
sizing span, so the library's own path is safe. That is the whole of the
protection: a check in the caller, which the callee neither performs nor
documents as a precondition.

`JpmcdsGenerateUnadjDates` is declared in the public `lib/include/isda/schedule.h`
and has **no caller inside the library at all**. It is not an internal helper
that happens to be exported; it is an exported function whose only possible
callers are external ones, and those are exactly the callers that get no
argument checking. The in-library safety says how bad this is, not whether it
exists — the same conclusion UPSTREAM-25 reached, where reasoning from
in-library reachability to function safety nearly dropped a confirmed heap
overflow.

Reproduce with `defect_evidence u30`, and `defect_evidence u30 --unsafe` under
the asan preset to execute the overflow.

### In the port

**Cannot occur**, twice over. `generate_unadjusted_dates` accumulates into a
`std::vector`, so there is no array to size and no sizing to get wrong. And the
bound upstream applies in its caller is applied *here* as well, in the function
that needs it, so the public entry point is as safe as the internal one —
`coupon_schedule` checks it too, since checking a precondition in both places
costs two comparisons and removes the question of which one is responsible.

The overflowing upstream call is kept as a `DISABLED_` test alongside the
differential suite: recorded, never executed, because running it is undefined
behaviour and a recorded vector derived from it would be a committed file
produced by UB.

## UPSTREAM-31 — An output written after the error label, from a variable never set

**Location:** `lib/src/streamcf.c`, `GetFloatStubRate`.
**Severity:** Low. Undefined behaviour on a reachable path, with no observable
consequence — and invisible to every tool in this project's harness.

### Mechanism

```c
static int GetFloatStubRate(...)
{
    int           status = FAILURE;     /* Until proven successful */
    double        rate;                 /* never assigned here */
    ...
    switch (swapType)
    {
      case JPMCDS_SINGLE_REFIX:
        if (JpmcdsForwardFromZCurve(..., &rate) == FAILURE)
            goto done;
        break;

      default:
        JpmcdsErrMsg("%s: unknown swap type. Can't calculate stub rate.\n", routine);
        goto done;
    }

    status = SUCCESS;

 done:
   if (status == FAILURE)
       JpmcdsErrMsg("%s: Failed to calculate back stub rate.\n", routine);

    *stubRate = rate;

    return status;
}
```

The output write sits **after** the label, so it executes on every path — and
on the failure paths `rate` was never assigned. Reading an indeterminate
`double` is undefined behaviour, regardless of what the caller then does with
the value.

This is the same shape as `schedule.c`'s `error:` label, which is also reached
on success — a single exit block doing cleanup for both outcomes. There the
shared block was harmless; here it writes an output.

### The precondition, measured

The read is only indeterminate if `JpmcdsForwardFromZCurve` leaves its output
alone when it fails. It does — all three of its failure paths `goto done`
without touching `*rate`:

```c
if (JpmcdsDiscountDate(startDate, zc, interpType, &startDiscount) == FAILURE)
    goto done;
if (JpmcdsDiscountDate(endDate, zc, interpType, &endDiscount) == FAILURE)
    goto done;
forwardDiscount = endDiscount/startDiscount;
if (JpmcdsDiscountToRate(forwardDiscount, startDate, endDate,
                      dayCountConv, basis, rate) == FAILURE)
    goto done;
```

Confirmed by sweeping forward computations with a sentinel in the output, so an
unwritten result is visible. On a curve with one point at an extreme rate, over
200 six-month forward periods:

| outcome | count |
|---|---|
| ordinary success | 160 |
| **NaN returned as SUCCESS** | **39** |
| FAILURE returned | 1 |
| …of those, output left untouched | 1 |

The failure is narrow: it needs `startDiscount > 0` with `endDiscount` exactly
`0`, so `forwardDiscount` is `0` and `JpmcdsDiscountToRate`'s `discount <= 0`
guard fires. One rung further out and both discounts underflow, giving `0/0`,
and then the guard cannot see the NaN and the function returns it as SUCCESS —
which is UPSTREAM-19's second site, incidentally quantified here at 39 of 200.

### Reachability

A back-stub schedule whose final accrual period lands in that window hands
`GetBackStub` exactly such a period, and `GetFloatStubRate` calls the forward
on the period's own accrual dates:

```
schedule: 323 periods, stubLocation 1 (BACK_STUB)
stub period accrues 15 Jan 2185 -> 15 Jun 2185
forward on that period: FAILURE, output LEFT UNTOUCHED
```

The inputs are absurd — a 160-year swap on a curve quoting a rate of 100 — but
they come in through the public API and nothing rejects them.

The `default:` branch is a second site of the same write, and there `rate` is
never assigned on **any** path, not merely on an error one. It is unreachable,
for a reason worth recording: `TSwapType` has a single enumerator, so in C its
compatible type is unsigned, `(TSwapType)(-1)` is 4294967295, and the
one-sided guard `if (swapType > JPMCDS_SINGLE_REFIX)` — where that constant is
`0` — catches negatives by accident. In **C++** clang gives the same enum a
signed type, so a C++ translation unit and the C library sharing
`schedule.h` disagree about it. The comparison happens on the C side, so the C
answer governs; but a guard that is correct only through implementation-defined
enum signedness, differing between the two languages that share its header, is
not protection anyone should rely on.

### Why this one could not have been caught

Every other memory finding in this register was caught by a sanitizer. This one
cannot be, and the reasons compound:

- **ASan** does not instrument uninitialised reads at all. It tracks
  addressability, not initialisedness.
- **UBSan** has no check for them either.
- **MemorySanitizer** would catch it. This project has no MSan job — it is not
  available on the development platform, and the gap has been recorded in the
  port notes since the harness was set up.
- **Single-file static analysis cannot see it.** Whether `rate` is indeterminate
  depends on `JpmcdsForwardFromZCurve`'s failure behaviour, which lives in
  `zr2fwd.c`. From inside `streamcf.c`, the call takes `&rate` and might write
  it. Measured: `clang -Wconditional-uninitialized -Wuninitialized` on
  `streamcf.c` reports nothing, and `clang --analyze` with
  `core.uninitialized` reports nothing. Both are **correct** — the information
  they would need is in another translation unit.

So this is the first concrete instance of a blind spot that had until now been
a stated limitation with no example: an uninitialised **stack** read, in a
cross-translation-unit shape. It was found by reading, and the reading was then
confirmed by measuring the precondition rather than asserting it.

### Consequence

None observable. `GetFrontStub` and `GetBackStub` both test the status before
touching the value:

```c
if (GetFloatStubRate(..., &stubRate) == FAILURE)
{
    JpmcdsErrMsg("%s: failed to calculate floating front stub.\n", routine);
    return FAILURE;
}
stubRate += stream->floatingRate->spread;
```

so the indeterminate double is written and then discarded. Nothing wrong
reaches a price. The read is undefined behaviour all the same, and a compiler
is entitled to assume it does not happen.

Reproduce with `defect_evidence u31`. There is no `--unsafe` mode and no
`DISABLED_` test: unlike UPSTREAM-21, 25 and 30, running this path is not
dangerous, because the damage is confined to a value nobody reads.

### In the port

**Cannot occur.** `detail::stub_payment` returns `std::optional<double>`
rather than writing through an out-pointer, so a failure path has no output to
write and the shape is unavailable. This is the general argument for returning
values instead of filling them in: a function that returns cannot write a
result it did not compute.

## UPSTREAM-32 — The bizarre rate got a guard; the bizarre maturity did not

**Location:** `lib/src/zcswutil.c:96`, `JpmcdsZCGetSwapCFL`. Reachable from
`lib/src/zcswap.c:475`.
**Severity:** Medium. A read-modify-write through a null-derived pointer,
reachable from the curve bootstrap.

### Mechanism

The function opens by special-casing a degenerate input, and says so:

```c
/* Bizarre rate==0.0 case
 */
if (rate==0.0)
{
    cfl = JpmcdsNewEmptyCFL(1);        /* simply cash flow of 1.0 at date */
    if (cfl == NULL)
        goto done;
    cfl->fArray[0].fAmount = 1;
    if (JpmcdsZCAdjustDate(matDate, badDayList, badDayConv, holidayFile,
                        &cfl->fArray[0].fDate) == FAILURE)
        goto done;

    return cfl;
}
```

That path is carefully done. Even its `return cfl` rather than a jump to
`done:` is deliberate: `status` is still `FAILURE` at that point, so routing
through the shared exit would free the list and return NULL. Somebody thought
about it.

The other degenerate input gets nothing. With `matDate == valueDate`:

| step | result |
|---|---|
| `JpmcdsNewDateList(v, v, 6M)` | **1** date |
| `JpmcdsNewPayDates(v, v, 6M)` | **0** — it drops the first element and decrements the count |
| `JpmcdsNewEmptyCFL(0)` | `fNumItems == 0`, `fArray == NULL` |
| the coupon loop | does not run |

and then, three statements from the end:

```c
cfl->fArray[cfl->fNumItems-1].fAmount += 1.0;   /* add principal */
```

which is `NULL[-1].fAmount += 1.0`. Note that this is a read-modify-**write**,
not merely a read as in UPSTREAM-21 — the `+=` loads and stores.

`JpmcdsNewPayDates` is the step that makes this possible, and it is worth
quoting, because the emptying is not an accident of allocation:

```c
/* Now remove startDate, and move all dates back by one.
 */
for (idx=0; idx < payDates->fNumItems-1; idx++)
{
    payDates->fArray[idx] = payDates->fArray[idx+1];
}
payDates->fNumItems--;
```

With one date the loop body never executes and the count goes to zero. The
value date is correctly excluded from a list of payment dates; the case where
it is the *only* date is not considered.

### Both sanitizers, two diagnostics

```
zcswutil.c:96:4: runtime error: applying non-zero offset
                 18446744073709551600 to null pointer
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior zcswutil.c:96:4

==22701==ERROR: AddressSanitizer: SEGV on unknown address 0xfffffffffffffff8
SUMMARY: AddressSanitizer: SEGV zcswutil.c:96 in JpmcdsZCGetSwapCFL
```

The two numbers are the same bug seen from two distances.
`18446744073709551600` is `-16` as an unsigned 64-bit value, and `sizeof
TCashFlow` is 16 — one element backwards. `0xfffffffffffffff8` is `-8`, the
offset of `.fAmount` within that element, which is the address actually
touched.

This is the opposite situation from UPSTREAM-31, filed one slice earlier: there
the harness could see nothing, here two tools independently report it.

### Reachability

`zcswap.c:475`:

```c
cfl = JpmcdsZCGetSwapCFL(zc->valueDate,
                         matDate, isEndStub,
                         rate,
                        &ivl,
                         fixDayCountConv,
```

There is **no comparison between `matDate` and `zc->valueDate` anywhere in
`zcswap.c`**. So a swap benchmark maturing on the curve's own value date is
sufficient — the same trigger as UPSTREAM-29, arriving at a different defect in
a different file. A benchmark list with a zero-tenor swap in it is malformed
input, but it is input the library accepts rather than rejects.

The zero-rate path is immune, which is the shape of the whole finding: the
input somebody wrote a comment about is handled, and the one two statements
later is not.

Reproduce with `defect_evidence u32`, and `defect_evidence u32 --unsafe` under
the asan preset for the traces. The live call is also a `DISABLED_` test in the
differential suite, never enabled.

### In the port

**Cannot occur.** `swap_cash_flows` refuses when the coupon-date list is empty:

```cpp
if (dates->empty()) {
  return std::nullopt;
}
```

The guard is on the **outcome**, not on `maturity == value_date`. That is the
lesson UPSTREAM-29 taught two slices ago — there the input test looked
sufficient and a Saturday-to-Sunday span under FOLLOW reached the same place
with `start < maturity`. Here the same reasoning applies for a different
reason: the substitution table and the bad-day adjustment both run between the
date generation and the flow construction, and `fix_bad_dates` is documented as
able to collapse two dates onto one.

`swap_coupon_dates` is *not* changed — it still returns the empty list, because
that is upstream's answer and the date generation is faithful. The refusal
belongs where the empty list becomes a problem.

## UPSTREAM-33 — The root finder is pointed at the wrong curve point

**Location:** `lib/src/zcswap.c`, `JpmcdsZCValueFixFltSwap`.
**Severity:** Low. A misdirected solve whose measured consequence is a
misleading failure rather than a wrong number — and the report says so rather
than implying a danger it could not demonstrate.

### Mechanism

The function adds a placeholder point for the benchmark it is about to solve:

```c
if (JpmcdsZCAddRate(zc, maxDate, rate) == FAILURE)
    goto done;
```

and then hands the root finder the address of the **last** slot:

```c
if (JpmcdsRootFindBrent(JpmcdsObjFuncPVtheSwap, &funcParams,
                     LOWER_BOUND, UPPER_BOUND, MAX_ITERATIONS, rate,
                     INITIAL_X_STEP, INITIAL_F_DERIV,
                     X_TOLERANCE, F_TOLERANCE,
                     &zc->rate[zc->numItems-1]) == FAILURE)
```

`zc->numItems-1` is the point just added only if that point is the latest date
in the curve. `JpmcdsZCAddRateAndDiscount` does not append unconditionally — it
inserts in sorted position:

```c
if (zc->numItems==0 || zc->date[zc->numItems-1]<date)
{
    zc->date[zc->numItems] = date;      /* insert at end */
    ...
}
else
{
    int i = zc->numItems - 1;           /* loops backwards, until before date */
```

So for a benchmark whose `maxDate` falls inside the existing curve, the solver
varies a different, already-solved point, and the point it was supposed to
solve keeps the placeholder `rate` it was seeded with. The discount
recomputation two statements later uses the same wrong index:

```c
if (JpmcdsZCComputeDiscount(zc,
                      zc->date[zc->numItems-1],
                      zc->rate[zc->numItems-1],
                      &zc->discount[zc->numItems-1])==FAILURE)
```

Demonstrated directly — a curve with points at 2026 and 2035, then
`AddRate(2027, 0.045)`:

```
    [0] 20260115  rate 0.0250
    [1] 20270115  rate 0.0450
    [2] 20350115  rate 0.0400   <-- the slot the solver writes
```

The point just added is at index 1; the solver writes index 2.

### Consequence, measured with controls

A sweep of 1,400 combinations — far-point year, far-point rate, benchmark year,
benchmark rate, both interpolation types — with the benchmark inside the
existing curve. **Every one fails.** Varying a distant zero rate barely moves a
shorter swap's present value, so Brent cannot bracket a root in `[-0.99, 100]`
within 50 iterations and the function reports *"Root finder failed"*.

Positive controls were essential here, because "it always fails" is also what a
broken fixture looks like:

| case | result |
|---|---|
| control: no far point, 5y benchmark | SUCCESS |
| control: far point 2026, 5y benchmark | SUCCESS |
| control: far point 2027, 8y benchmark | SUCCESS |
| INSIDE: far point 2035, 5y benchmark | FAILURE |
| INSIDE: far point 2030, 5y benchmark | FAILURE |
| INSIDE: far point 2029, 4y benchmark | FAILURE |

The controls succeed and the inside-the-curve cases all fail, so the failures
are the misdirection and not the setup.

**I tried and failed to produce a silently wrong curve.** Across the whole
sweep there was no case where the solve converged while varying the wrong
point. So the observable consequence is a misleading diagnostic: the caller is
told the root finder failed when the real problem is that the benchmark is not
the longest instrument so far. That is why this is filed low. The same
reasoning as UPSTREAM-23, where a pricing consequence was looked for and not
found, and the register said so.

### Reachability

Two ways in:

- `JpmcdsZCAddSwaps` has an **else** branch that calls `JpmcdsZCAddSwap` for
  benchmarks *not* beyond the curve's end — which is exactly the condition that
  triggers this — and passes `discZC` through, so `JpmcdsZCValueFixFltSwap` is
  reached whenever a discount curve is supplied;
- `JpmcdsZCValueFixFltSwap` is itself declared in `zcprvt.h` and exported.

A benchmark strip that is sorted and extends monotonically past the stub curve
never hits it. One that overlaps an existing curve does.

Reproduce with `defect_evidence u33`. No `--unsafe` mode: nothing here is
undefined behaviour, and the sweep is safe to run.

### In the port

**Cannot occur.** `value_fixed_float_swap` looks the added point up by date and
solves for that **index**:

```cpp
if (!curve.add_rate(max_date, rate)) {
  return false;
}
const std::optional<std::size_t> solved_index = curve.find_exact(max_date);
```

so the solve targets what it added whatever the insertion order. A tier-1 test
asserts the port solves exactly the input upstream cannot, and that the
pre-existing far point is left at the rate it had.

There is a second benefit, not the point of the fix but worth recording: the
port sets the rate through `ZeroCurveBuilder::set_rate`, which recomputes the
cached discount factor, so the rate/discount invariant cannot drift. Upstream
mutates `fArray[last].fRate` in its snapshot `TCurve` and recomputes the
discount in a separate call against the live `ZCurve` — two places holding the
same quantity, kept in step by hand.

## UPSTREAM-34 — A validator that reports a NULL pointer and then dereferences it

**Location:** `lib/src/gtozc.c`, `CheckZCCashInputs` (line 345) and
`CheckZCSwapsInputs` (line 464).
**Severity:** Medium. A null dereference inside the code that detected the
null.

### Mechanism

Both static validators in `gtozc.c` **accumulate** failures rather than
returning at the first one:

```c
static int CheckZCCashInputs(...)
{
    int   status = SUCCESS;     /* Until proven a failure */

    if(zeroCurve == NULL)
    {
        JpmcdsErrMsg("%s: ZeroCurve is NULL\n", routine);
        status = FAILURE;
    }

    if (numRates < 0) { ...; status = FAILURE; }
    if (JpmcdsDayCountValid(routine,dayCountConv)==FAILURE) { ...; status = FAILURE; }

    for (i=0 ;i<numRates ;i++ )
    {
       ...
       if (dateArr[i] <= zeroCurve->fBaseDate)
```

That style is *good design*. It reports every problem with the input instead of
only the earliest, which is strictly more useful to a caller — and this report
has praised the same pattern elsewhere, in `streamcf.c`'s
`CheckFixedStreamDetails`.

The defect is narrower than the style. **One of the accumulated checks is a
precondition for a later one.** The NULL test sets a flag and carries on, and
the loop then reads `zeroCurve->fBaseDate`.

### Two sites, one mechanism, one fix

`CheckZCSwapsInputs` has the identical shape, and is worse: its dereference is
not inside a loop whose count could be zero.

```c
if(JpmcdsIsBusinessDay(zeroCurve->fBaseDate, holidayFile, &isBusDay)==FAILURE)
```

That runs whatever the instrument count is.

| site | function | the dereference |
|---|---|---|
| gtozc.c:345 | `CheckZCCashInputs` | `dateArr[i] <= zeroCurve->fBaseDate` |
| gtozc.c:464 | `CheckZCSwapsInputs` | `JpmcdsIsBusinessDay(zeroCurve->fBaseDate, ...)` |

One edit — an early return once the curve is known to be NULL — fixes both. So
this is one defect at two sites rather than two defects, the same counting rule
UPSTREAM-19 established.

### What is and is not reachable

A zero instrument count short-circuits before any validation runs:

```c
if (numRates == 0)    /* nothing to add */
{
   tcCash = JpmcdsCopyCurve (zeroCurve);
   return tcCash;
}
```

and `JpmcdsCopyCurve` tolerates a NULL, so `JpmcdsZCCash(NULL, …, 0, …)`
returns NULL cleanly. Measured. Any positive count reaches the validator.

Under the asan preset, both sanitizers report it:

```
gtozc.c:345:37: runtime error: member access within null pointer
                of type 'TCurve'
ERROR: AddressSanitizer: SEGV on unknown address 0x000000000010
SUMMARY: AddressSanitizer: SEGV gtozc.c:345 in CheckZCCashInputs

gtozc.c:464:34: runtime error: member access within null pointer
                of type 'TCurve'
SUMMARY: AddressSanitizer: SEGV gtozc.c:464 in CheckZCSwapsInputs
```

Note the address: `0x10` is the offset of `fBaseDate` within `TCurve`, read
from a null base.

Reproduce with `defect_evidence u34`, and `defect_evidence u34 --unsafe` under
the asan preset for the trace. Both live calls are also `DISABLED_` tests in
the differential suite.

### In the port

**Cannot occur.** Both functions take `const ZeroCurve &`, so the input does
not exist. This is the third defect in this register retired by a reference
rather than a check — the others being the pointer-nullness assumptions in
`cash_flow_bootstrap.hpp` and `curve_discount.hpp`.

The useful lesson is not "return early", which would throw away a good
pattern. `stream_cash_flows.hpp`'s `is_well_formed` accumulates in exactly the
same style and is safe, because every check it performs is independent of the
others. The rule is:

> An accumulating validator is safe only when no check guards another.

Where one check *is* a precondition for a later one, it has to return — and
then it stops being an accumulating validator for that input, which is the
honest trade. Upstream wanted both properties and got neither.

## UPSTREAM-35 — `toupper` on a plain `char`, which C does not permit

**Location:** `lib/src/zerocurve.c`, `JpmcdsBuildIRZeroCurve`.
**Severity:** Low. Undefined behaviour with no consequence demonstrated, and
invisible to the sanitizers — but one of the best-known traps in C, with a
one-token fix.

### Mechanism

The top-level curve builder sorts its benchmark list by a one-letter code:

```c
#define JpmcdsSWAPNAME   'S'
#define JpmcdsMONEYNAME  'M'
...
    char    instr;
...
    instr = toupper(instrNames[i]);
    if (instr != JpmcdsMONEYNAME && instr != JpmcdsSWAPNAME)
    {
        JpmcdsErrMsg("%s: unknown instrument type (%c)."
                  " Only (M)oney market or (S)wap is allowed.\n",
                  routine, instrNames[i]);
        goto done;
    }
```

`instrNames` is a `char *`. C §7.4 requires the argument to `toupper` to be
"representable as an `unsigned char`" or to equal `EOF`. On every platform this
port builds on, plain `char` is **signed**, so a byte with the high bit set
arrives as a negative number that is neither.

Measured:

| byte | as `char` | permitted by C? |
|---|---|---|
| 0x41 | 65 | yes |
| 0x4D (`'M'`) | 77 | yes |
| 0x53 (`'S'`) | 83 | yes |
| 0x7F | 127 | yes |
| 0x80 | −128 | **no — undefined behaviour** |
| 0xE9 | −23 | **no — undefined behaviour** |
| 0xFF | −1 | yes |

Note the last row. `0xFF` as a signed `char` is `-1`, which **equals `EOF`** —
so of the 128 high-bit bytes, exactly one is legal, by collision. That is the
kind of accident that lets this class of bug survive review: the obvious test
case is the last byte, and the last byte is fine.

### Reachability and consequence

The call happens *before* the instrument check rejects the character, so the
undefined behaviour is already done by the time the function reports a problem.
Measured: `JpmcdsBuildIRZeroCurve` with an instrument code of `0xE9` returns
NULL — correctly — having passed −23 to `toupper` on the way.

No consequence is demonstrated, and the reasons are worth separating:

- the instrument check rejects the character immediately afterwards, so nothing
  downstream sees a wrong value;
- the C library this links against pads its character tables for exactly this
  mistake, by long-standing convention rather than by requirement;
- **UBSan has no check for it.** Neither does ASan. So this is the second entry
  in this register that is undefined behaviour the harness cannot see, after
  UPSTREAM-31 — and unlike UPSTREAM-31, which needed cross-translation-unit
  reasoning to even identify, this one is a textbook item.

There is a second, independent reason not to write it this way, which has
nothing to do with signedness: `toupper` consults the locale. Under a locale
where `'s'` does not uppercase to `'S'`, the parse changes. A function deciding
whether a benchmark is a swap should not depend on `LC_CTYPE`.

Reproduce with `defect_evidence u35`. There is no `--unsafe` mode: the
behaviour is undefined but not dangerous on this platform, and running it is
how the table above was produced.

### In the port

**Cannot occur**, twice over.

`instrument_kind_from_code` compares the four characters directly:

```cpp
switch (code) {
case 'M': case 'm': return InstrumentKind::MoneyMarket;
case 'S': case 's': return InstrumentKind::Swap;
default:            return std::nullopt;
}
```

No `toupper`, so no argument contract to violate and no locale to consult. And
`InstrumentKind` is an enum, so an `InstrumentQuote` cannot carry an unparsed
letter at all — the parse happens once, at the boundary, and what crosses it is
a value.

A tier-1 test walks all 256 byte values and requires exactly four to be
accepted. That is the test upstream's version could not have: with `toupper` in
the path, 128 of those 256 inputs are undefined behaviour and the test would be
asserting things about a program with no defined meaning.

## UPSTREAM-36 — The fourth unguarded `fArray[fNumItems-1]`

**Location:** `lib/src/linterpc.c:65`, `JpmcdsLinInterpCurve`.
**Severity:** Medium. A null-derived read at a public entry point with no
in-library caller.

### Mechanism

The function clamps each requested date into the old curve's range, to avoid
extrapolating — the intent is right and the comment says so:

```c
/* Make sure that we don't extrapolate. Before the first date
 * in oldCurve, we just use the first rate in oldCurve. After
 * the last date in oldCurve, we just use the last rate in oldCurve.
 */
for (idx = 0; idx < desiredDates->fNumItems; idx++)
{
    modDates[idx] = MIN(desiredDates->fArray[idx],
                        oldCurve->fArray[oldCurve->fNumItems-1].fDate);
    modDates[idx] = MAX(modDates[idx],
                        oldCurve->fArray[0].fDate);
```

Neither end is guarded, and `JpmcdsNewTCurve` accepts a count of **zero** —
only a negative one is refused:

```c
if (numPts < 0)
{
    JpmcdsErrMsg ("%s: Attempt to create curve with %d points.\n", ...);
    goto done;
}
...
if (numPts > 0)
{
    tc->fArray = NEW_ARRAY(TRatePt, numPts);
    ...
}
else
{
    tc->fArray = NULL;
}
```

So with a zero-point curve the two lines are `NULL[-1]` and `NULL[0]`.
Measured:

```
linterpc.c:65:25: runtime error: applying non-zero offset
                  18446744073709551600 to null pointer
ERROR: AddressSanitizer: SEGV on unknown address 0xfffffffffffffff0
SUMMARY: AddressSanitizer: SEGV linterpc.c:65 in JpmcdsLinInterpCurve
```

`18446744073709551600` is `-16`, one `TRatePt` backwards.

A zero-point curve is not a hypothetical shape. `zerocurve.c` builds one as its
**first act**:

```c
zcurveIni = JpmcdsNewTCurve(valueDate, 0, (double) 1L, JPMCDS_ACT_365F);
```

**A second path, found later while porting `convert.c`.** `JpmcdsMakeTCurve`
delegates to `JpmcdsMakeTCurveNoRateCheck`, which stops copying at the first
point whose date is TDate 0 — so the length argument is an upper bound, not a
count. Measured: a *first* point with date 0 yields a **zero-point curve**.
TDate 0 is 1601-01-01, a real date, so any caller building a curve from
externally-supplied dates can produce the empty curve without asking for one
and without an error. That matters for severity here: the first path needs a
caller to pass a literal `0`, which is visible at the call site. This one needs
only a data value that looks ordinary.

### The pattern, which is the point of this entry

At four occurrences, `fArray[fNumItems-1]` without a guard stops being four
accidents:

| entry | file | shape |
|---|---|---|
| UPSTREAM-21 | `zcall.c` | read, `NULL[-1]`, measured SIGSEGV |
| UPSTREAM-29 | `schedule.c` | the zero-period list that enables one |
| UPSTREAM-32 | `zcswutil.c` | read-modify-**write**, `NULL[-1]` |
| UPSTREAM-36 | `linterpc.c` | read, `NULL[-1]` *and* `NULL[0]` |

Four different sites in four different files reached for the same idiom. In
each one the author knew their own count was positive — and in each one that
knowledge lived nowhere a compiler or a later reader could check it. That is
what the absence of a non-empty container type costs, billed four times.

It is worth being precise about what the port did and did not do here. It did
not add four guards. It made one type non-empty, in the `zero_curve` slice,
long before three of these four were found — and the cost of that decision
showed up exactly once, as two overloads in the `zerocurve` slice. Four
defects retired for two overloads is the best trade in this register.

### Reachability

`JpmcdsLinInterpCurve` has no caller inside the library. It appears exactly
once outside its own file:

```c
TCurveInterpFunc JpmcdsLinInterpCurve;      /* interp.h:65 */
```

which is a declaration against a function-pointer typedef, not a call. So this
is an exported function whose only possible callers are external ones — the
same situation as `JpmcdsGenerateUnadjDates` in UPSTREAM-30, and the same
consequence: the arguments nobody inside the library passes are the arguments
nothing inside the library validates.

Reproduce with `defect_evidence u36`, and `defect_evidence u36 --unsafe` under
the asan preset for the trace. The live call is also a `DISABLED_` test.

### In the port

**Cannot occur.** `ZeroCurve` is non-empty by construction, so
`points().front()` and `points().back()` are total functions and the clamp has
nothing to check.

## UPSTREAM-37 — A NULL guard on one path out of four

**Location:** `lib/src/cxbsearch.c:84`, `JpmcdsBinarySearchLong`.
**Severity:** Medium. A store through NULL on three of the function's four
exit paths, where the fourth guards against exactly that.

### Mechanism

The function returns three values through pointers, and its main path treats
two of them as **optional**:

```c
if (loBound != NULL)
{
    while (lo >= 0 && ELEMENT(lo) >= xDesired)
        --lo;

    if (lo >= 0)
        *loBound = lo;
    else
        *loBound = -1;
}

if (hiBound != NULL)
{
    ...
}
```

That guard *is* the documentation. Nothing in the header says NULL is
acceptable; the code saying `if (loBound != NULL)` is the only statement on the
subject, and it says yes. Measured: an in-range search with NULL for both
optional outputs returns SUCCESS and fills `exact` correctly.

The function then has three early returns that write through the same pointers
with no guard at all:

```c
if (xDesired < ELEMENT(0))
{
    *exact   = -1;
    *loBound = -1;
    *hiBound = 0;
    return SUCCESS;
}
else if (xDesired > ELEMENT(arraySize-1))
{
    *exact   = -1;
    *loBound = arraySize-1;
    *hiBound = arraySize;
    return SUCCESS;
}

/* arraySize of 1 we are done */
if (arraySize == 1)
{
    assert (xDesired == ELEMENT(0));
    *exact   = 0;
    *loBound = -1;
    *hiBound = arraySize;
    return SUCCESS;
}
```

So a sought value outside the array's range, or an array of one element, stores
through NULL:

```
cxbsearch.c:84:9: runtime error: store to null pointer of type 'long'
ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
SUMMARY: AddressSanitizer: SEGV cxbsearch.c:84 in JpmcdsBinarySearchLong
```

### Why this one is worse than a missing check

Most of this register is missing checks. This is an **inconsistent** one, forty
lines apart in a single function, and the distinction matters.

A missing check is a precondition. It can be documented, and a caller who
violates it has made a mistake. A check on one path and not another is an
*invitation the code does not honour*: the guarded path teaches the caller that
NULL is fine, and three other paths then punish them for believing it. The
caller did nothing wrong, and there is no document they could have read that
would have saved them — the only statement about NULL in the whole function is
the one that says it is allowed.

### Reachability

Both in-library callers pass non-NULL for all three outputs:

- `cxzerocurve.c:103`, searching a curve's dates for today;
- `feeleg.c:569`, searching a fee leg's accrual start dates for today.

So this is reachable only through the public header — which is also the only
place anyone would read the guard and conclude NULL was allowed. That is now
the fourth time in this register that the externally-reachable surface is the
unvalidated one (after UPSTREAM-25, 30 and 36).

Reproduce with `defect_evidence u37`, and `defect_evidence u37 --unsafe` under
the asan preset. Three `DISABLED_` tests cover the three unguarded paths.

### In the port

**Cannot occur.** `match_range` returns a value:

```cpp
struct MatchRange {
  std::size_t first = 0;
  std::size_t count = 0;
};
```

There are no output pointers, so there is nothing to guard on any path, and the
inconsistency has nowhere to live.

The collapse from three outputs to one range is not cosmetic. Upstream
documents the relationship between its three values:

> The idea behind these return values is that `hiBound-loBound-1` should give
> the number of exact matches in all cases, and that everything in the range
> `[loBound+1, hiBound-1]` inclusive should be an exact match.

That invariant *is* the answer, so returning it directly makes
`hiBound - loBound - 1 == count` a tautology instead of a promise each caller
re-derives. And the sentinels go with it: upstream's `loBound == -1` and
`hiBound == arraySize` are not "absent" values, they are one position past each
end and load-bearing for the subtraction, which a half-open range expresses
without signalling anything.

### Two more things measured here, neither filed

**The parameter comment is wrong.** It says of `hiBound`:

```c
long   *hiBound)   /* (O) Index of first value strictly greater than xDesired,
                      -1 if no such value exists. */
```

while the function comment twenty lines above says `arraySize for hiBound`, and
the code says `arraySize`. Measured: seeking 99 in `{10,20,20,30,40}` gives
`hiBound == 5`. Two comments in one file contradicting each other, with the
code agreeing with one of them. Reproduced from the code, as the port's policy
requires when documentation and a self-consistent implementation disagree.

**The order check never fires.** There is a branch reporting *"x array not in
increasing order"*, reached when the `arraySize + 1` iteration cap expires. I
wrote a test asserting at least one unsorted array would be refused by it. The
test **failed**: across an exhaustive sweep of every unsorted array of length 2
to 4 over the alphabet 0..3, not one was refused. The cap is `arraySize + 1`
iterations on a halving search, which is roughly exponentially generous — a
runaway backstop, not a detector.

Disorder therefore produces a confident wrong answer, which is exactly what the
function's own documentation promises:

> If the input array is not sorted in ascending order, then the output is
> undefined. No checks are performed on the order of xArray — this would in
> fact defeat the purpose of having a fast binary search if you are going to
> slow it down by doing a linear check of the inputs.

So the branch is dead code rather than a broken check, and the documentation is
honest. Not filed. Recorded because the branch reads like a safety net and is
not one.

---

## UPSTREAM-38 — Three bytes lowercased however short the token is

**Location:** `lib/src/convert.c:395`, `NStringToMonth`, reached from
`JpmcdsStringToDate` at `convert.c:119`.
**Severity:** High. Stack buffer overflow — a read *and* a write — at a public
entry point that takes untrusted text.

### Mechanism

`NStringToMonth` turns a token into a month number by lowercasing its first
three characters and comparing against three-letter names:

```c
cp[0] = (char) tolower(cp[0]);
cp[1] = (char) tolower(cp[1]);
cp[2] = (char) tolower(cp[2]);   /* convert.c:395 */
```

There is no length check before those lines, and they are read-modify-**writes**,
not reads. A one-character token has its NUL at `cp[1]` — still inside the
buffer — and nothing at all at `cp[2]`.

Three facts have to line up for that to leave the buffer, and all three are
reachable through the public `JpmcdsStringToDate`:

1. the input is copied into a 32-byte stack buffer, so a token can end *at* the
   buffer's end:
   ```c
   char dateCopy[32];
   (void)strncpy(dateCopy, mmddyyyy, sizeof(dateCopy)-1);
   ```
2. the second token reaches `NStringToMonth` only when it fails to parse as an
   integer — so it must be non-numeric;
3. the *first* token must therefore be long enough to push the second to offset
   30, which means roughly 29 characters `JpmcdsSscanf` accepts as a number.

A 29-digit run and a one-letter month — `"11111111111111111111111111111-a"` —
puts the token at `dateCopy[30]`, the NUL at `[31]`, and `cp[2]` at `[32]`.
Measured under the `asan` preset:

```
lscanf.c:486:28: runtime error: signed integer overflow:
  1111111111111111111 * 10 cannot be represented in type 'long'
==57811==ERROR: AddressSanitizer: stack-buffer-overflow on address 0x00016b1cdc20
    #0 NStringToMonth convert.c:395
    #1 JpmcdsStringToDate convert.c:119
  [32, 64) 'dateCopy' (line 45) <== Memory access at offset 64 overflows this variable
SUMMARY: AddressSanitizer: stack-buffer-overflow convert.c:395 in NStringToMonth
```

ASan reports a READ because the read inside `tolower(cp[2])` happens first; the
store that follows is to the same byte.

The precondition is observable without touching the overflow. Short tokens
placed near the *start* of the buffer, where the third byte is still inside it,
are all refused — but refused *after* the three writes have happened:

```
j-15-2024      refused
ja-15-2024     refused
jan-15-2024    accepted
janu-15-2024   refused
```

Only the three-character token can match, which means every call with a token
of any other length writes three bytes for nothing.

### Why the pair matters

This is the **second** stack overflow in `convert.c`, 225 lines from
[UPSTREAM-4](#upstream-4), and the two together say something neither says
alone. UPSTREAM-4 is an unbounded copy *into* a buffer. This one is a **bounded**
copy — the `strncpy` above is correct, and truncation is precisely why over-long
input is accepted rather than refused — followed by an unchecked read past the
result.

The bound on the copy does not help, because the overflow is downstream of it.
Getting the `memcpy` right is not the same as being safe.

### In the port

Cannot occur. The token is a `string_view` and `to_month_name` returns
`nullopt` for any size but 3 **before** reading a character. The lowercasing
writes into a local three-byte array rather than back into the caller's text —
so the port also does not mutate the string it was asked to parse, which
upstream does, unannounced, to a `char *` parameter.

---

## UPSTREAM-39 — An unparseable year is invented, not refused

**Location:** `lib/src/convert.c:124`, `JpmcdsStringToDate`.
**Severity:** High. Silent wrong answer from untrusted input, at a public entry
point, with no diagnostic.

### Mechanism

The function reads three delimited tokens. The month and the day are parsed
with a function whose return value is **checked**:

```c
if (JpmcdsSscanf(cp,"%ld",&mdy.month) == 0)  /* If not integer try string */
{
    if (NStringToMonth(cp,&mdy.month) == FAILURE)
        goto done;                           /* refused */
}
```

Eleven lines later, the year is parsed with a function that **cannot** report
failure:

```c
mdy.year = atol(cp);
if (mdy.year > 99 && mdy.year < JPMCDS_TDATE_BASE_YEAR)
    ... out of range ...
if (mdy.year < CROSSOVER_YEAR)
    mdy.year += 2000;
```

`atol` returns 0 both for `"0"` and for text it cannot parse, and stops silently
at the first non-digit. Neither case is detectable. So a year of `"aaaa"`
becomes 0, 0 is below `CROSSOVER_YEAR`, and the date comes back as the year
2000 — a year nobody wrote down.

Measured:

| input | result | |
|---|---|---|
| `03/15/2024` | 2024-03-15 | a real year, for contrast |
| `03/15/aaaa` | **2000**-03-15 | no digits at all → `atol` 0 → +2000 |
| `15-Mar-twenty` | **2000**-03-15 | a spelled-out year |
| `1-1-x2024` | **2000**-01-01 | digits present, but not first |
| `03/15/2024XX` | 2024-03-15 | `atol` stops at the junk |
| `03/15/99bogus` | **1999**-03-15 | 99 is *above* the pivot, so +1900 |
| `aa/15/2024` | refused | an unparseable **month**, for contrast |
| `03/aa/2024` | refused | an unparseable **day**, for contrast |

The last two rows are what make this a defect rather than a convention. An
unparseable month is refused. An unparseable day is refused. An unparseable year
is accepted and replaced with 2000. One function, one input, three fields, two
different policies.

I predicted `99bogus` would give 2099 and it gives 1999 — 99 is above the
crossover, so it takes the `+1900` branch. The table holds measurements, not
predictions, for that reason.

### Severity

Higher than the mechanism suggests. A crash gets investigated; a date does not.
This takes untrusted text — a holiday file, a regression input, a spreadsheet
cell — and returns a date twenty-four years off with no error channel touched.
Every downstream discount factor, accrual and par spread is then computed
correctly from a wrong date.

### In the port

**Corrected, not reproduced** — the one place in this port where that call went
against the default. The project rule is to reproduce when the code is
self-consistent and only the documentation disagrees. Here the code disagrees
with *itself*, eleven lines apart, so there is no single upstream behaviour to
preserve. `date_from_string` refuses any year token containing a non-digit.

The divergence is pinned from **both** sides in
`tests/date_from_string_differential_test.cpp`: upstream accepts and the port
refuses, both asserted, so a change on either side fails the test. The recorded
vectors deliberately **exclude** these inputs — freezing a known divergence into
the file the port is checked against would turn a documented difference into a
silent one.

---

## UPSTREAM-40 — Digits accumulated into a signed long, unchecked

**Location:** `lib/src/lscanf.c:486`, `JpmcdsSscanf`.
**Severity:** Low-Medium. Undefined behaviour on untrusted input; out of the
port's scope, but reachable from an in-scope public function.

### Mechanism

`JpmcdsSscanf` is a hand-written `scanf`. Its integer conversion accumulates
digits with no range check:

```c
value = value * 10 + (*cp - '0');     /* lscanf.c:486 */
```

Signed overflow is undefined behaviour in C, so there is no defined result for a
19-or-more-digit number. Under the `asan` preset, which enables UBSan:

```
lscanf.c:486:28: runtime error: signed integer overflow:
  1111111111111111111 * 10 cannot be represented in type 'long'
```

Reached here through `JpmcdsStringToDate`, which hands it the first token of a
date string — so the input is untrusted text, and a long digit run is a
plausible thing for a spreadsheet cell to hold.

### Why it is filed at all

`lscanf.c` is **out of scope** for this port: it exists to read holiday files,
which the port does with the standard library instead. It is filed because the
register's subject is upstream's defects, not the port's surface, and because
this one is reachable from an in-scope public function.

It was found while measuring UPSTREAM-38 — the same input triggers both, and
UBSan reported this one first. The two share an input and nothing else: a
different file, a different mechanism, a different fix. Three entries from one
probe, which is what the "count defects, not entry points" rule produces when
one entry point really does have three.

### In the port

Not applicable — no `scanf` reimplementation exists. The equivalent arithmetic
in `date_from_string` range-checks before each multiply and returns `nullopt`
instead of wrapping.

---

## UPSTREAM-41 — The upper-cased copy nobody reads

**Location:** `lib/src/buscache.c:519` and `:574`, `JpmcdsHolidayListRead`.
**Severity:** High. A silently wrong holiday calendar, from input a user wrote
in good faith, with no diagnostic.

### Mechanism

The function declares a buffer for an upper-cased copy of each line, fills it,
and then compares the **original**:

```c
char       buffer[MAX_BUFFER];    /* line of text from file      */
char       ucBuffer[MAX_BUFFER];  /* upper case line of text     <- line 519 */
...
if (buffer[0] == '#')
{
    static char satString[] = "# SATURDAY_NOT_ALWAYS_HOLIDAY";
    ...
    COPY_CAPITALIZE(ucBuffer, buffer, MAX_BUFFER-1);              /* line 574 */
    if (strncmp (buffer, monString, sizeof(monString)-1) == 0)    /* buffer   */
        weekends |= JPMCDS_WEEKEND_MONDAY;
    ...
```

`ucBuffer` is **written once and never read** — the only two mentions in the
file are its declaration and that one assignment. All seven directive
comparisons use `buffer`.

So the weekend directives are case-**sensitive**, while the dead code exists
for no purpose other than to make them case-insensitive. The intent is
unmistakable and the effect is absent.

### Measured

A two-line holiday file, differing only in the case of the directive:

| file contents | Saturday 2024-03-16 |
|---|---|
| `# SATURDAY_NOT_ALWAYS_HOLIDAY`<br>`19960101` | **a business day** |
| `# saturday_not_always_holiday`<br>`19960101` | **a holiday** |

Both files load. Neither produces a diagnostic. A user who wrote the directive
in lower case gets standard weekends, and every date the model adjusts — every
coupon date, every accrual boundary, every settlement — is silently wrong.

### Severity

This is the shape of defect this register treats most seriously: not a crash,
but a *confident wrong answer* from input nobody would suspect. The file
loaded. No error was reported. The calendar is simply not the one that was
asked for, and nothing downstream can tell.

### Two more things in the same macro, neither filed

```c
#define COPY_CAPITALIZE(pTarget,pSrc,len) \
...
   while ( (*PSRC) != 0  && (PTARGET - pTarget) <= (len) )\
   {\
          (*PTARGET) = (char)toupper((int)(*PSRC));\
```

**`toupper` on a plain `char`** is undefined for the high-bit bytes — the same
mechanism as [UPSTREAM-35](#upstream-35), already filed. Here it happens in
code whose result is discarded, which makes it undefined behaviour executed for
nothing.

**A predicted off-by-one, refuted.** The loop continues while the offset is
`<= len`, so the terminator appears to land at `len + 1` — one past the end
with `len = MAX_BUFFER-1`. **Tested and it does not occur:** `JpmcdsFgets`
reads at most `MAX_BUFFER-1` characters, so the source string is never long
enough to push the terminator past the last valid index. A 300-character
comment line under ASan produces nothing. The macro is unsafe if ever called
with a longer source; at this one call site the caller's bound protects it.

Recorded because the prediction was wrong and the reason it was wrong — the
bound lives in the caller, not the macro — is the useful part.

### In the port

**Corrected, and this is the second correction in the project** (the first was
UPSTREAM-39). The reasoning differs from that one and is worth stating.

Upstream's *behaviour* here is unambiguous: `buffer` is always what gets
compared, so there is one upstream behaviour and the standing rule says
reproduce it. The dead buffer is evidence of *intent*, not of a second
behaviour.

But reproducing it means reproducing a silently wrong calendar. So
`parse_holiday_file` takes the third option: a comment that case-insensitively
matches a directive **but is not exactly it** is refused, quoting the line.

```
line 1: "# saturday_not_always_holiday" looks like the directive
# SATURDAY_NOT_ALWAYS_HOLIDAY but the spelling must match exactly,
including case. Upstream would ignore this line without saying so.
```

That reproduces nothing silently and accepts nothing upstream rejects. The
differential test asserts **both** sides — that upstream honours the upper-case
form and drops the lower-case one — so an upstream release that fixes this
fails the test and tells us to revisit the refusal, rather than letting the
divergence become invisible.
