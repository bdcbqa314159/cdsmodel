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
