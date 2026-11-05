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
