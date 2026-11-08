# Migration journal

A record of how this port was made, kept because the decisions are the part
git cannot reconstruct. A diff shows what changed; it does not show what was
rejected, what turned out to be wrong, or how we found out.

Organised by **decision**, not by date, because that is the useful axis. A
chronology is at the end.

Written as the work happened, including the mistakes. The mistakes are the
load-bearing content: a migration account with no wrong turns in it is either
dishonest or describes a trivial migration.

---

## The subject

The ISDA CDS Standard Model, version 1.8.3, released 2009. The reference
implementation of credit default swap pricing that market participants settle
against. Also: 21,775 lines of C, twelve hand-written makefile fragments
across four platforms, no shipped test suite, a process-wide mutable holiday
cache with external linkage, and a distribution whose only user interface is
an Excel add-in that cannot run on macOS or Linux.

The goal is a C++20 port with Python bindings and a cross-platform
spreadsheet layer, keeping the numbers and modernising everything around
them.

---

## Decision 1 — The oracle comes before the port

**Chosen:** compile the untouched upstream C into a static library, and link
it into the *same test binary* as the port. Tests call both implementations
side by side and compare in-process.

**Rejected:** generating a CSV of reference outputs and diffing against it.
That was the first plan, and it is worse: a recorded file is a snapshot that
silently goes stale, and when it disagrees you cannot ask it why.

```cpp
const auto ported    = cdsmodel::bracket(view, desired);
const auto reference = reference_bracket(view, desired);   // the 2009 C, live
ASSERT_EQ(ported->lo, reference->lo);
```

**Why it matters more than it sounds.** There is no written specification for
this model. The C *is* the specification. So "correct" has no meaning except
"agrees with the original", and the only way to say that with force is to run
both.

**What would change it:** if the original could not be built at all, or if a
module had no upstream counterpart to compare against. Both happened later,
and both needed different answers — see decisions 3 and 6.

**Cost paid up front:** nothing was ported until the 2009 C compiled and
emitted golden values on a 2026 toolchain. That took three portability fixes
(`stricmp`, implicit function declarations, clang-16 error promotions) and
they were all solved with CMake flags, never by editing a source file.

---

## Decision 2 — The oracle is never edited

A rule, written into `CLAUDE.local.md` on day one: `isda_cds_model_c_v1.8.3/`
is never modified. Compiler problems are solved with build flags.

**Why:** the moment you edit the thing you are verifying against, you have
destroyed the only independent check you have. It is extremely tempting —
upstream has defects, and some are trivially fixable — and it is always the
wrong move.

**Enforced mechanically**, not by discipline: `verify.py upstream-unmodified`
checks 132 files against a committed SHA-256 manifest. Verified by tampering
with `bsearch.c` and watching the gate fire.

---

## Decision 3 — The oracle cannot be redistributed, so verification is two-tier

Partway in, the obvious was pointed out: upstream is only available from
cdsmodel.com after accepting the ISDA licence and supplying an email address.
Shipping it in a public repo routes around that gate.

So it was removed — from the working tree *and* from git history, which at
that point meant rebuilding a two-commit history rather than just
untracking 152 blobs.

**The consequence nobody wants:** CI has no oracle. Every differential test
becomes local-only, and a CI run that checks nothing is worse than no CI.

**The resolution** is in §4(b) of the upstream licence, which exempts *"data
derived from calculations that have been attained by using the ISDA CDS
Standard Model"*. So:

| tier | runs | what it checks |
|---|---|---|
| 2 | locally, with an oracle | the port against the original, live, in-process |
| 1 | everywhere, including CI | the port against reference answers recorded from the oracle |

Committing the **generators** alongside the vectors is what keeps tier 1
honest. Numbers with no generator are magic; numbers with one are
reproducible by anyone who has accepted the terms.

**What would change it:** putting a licensed copy behind a CI secret would
restore tier 2 in CI. Worth doing when 14,000 recorded cases stop feeling
like enough.

---

## Decision 4 — Reimplement, or transcribe?

The sharpest recurring judgement call, and the same upstream file forced it
both ways.

**ACT/365 was reimplemented.** Upstream walks years by repeatedly adding 365
or 366 to a running date and re-deriving the calendar year. The port
intersects the period with each calendar year directly. The *intent* was
recoverable — "days in leap years over 366, the rest over 365" — so a cleaner
implementation of that intent is a legitimate port. Equivalence was then
proven, not assumed: bit-identical across 3,456 pair/convention combinations
and every start day of a full leap cycle.

**`BusinessDaysDiff` was transcribed.** Upstream's own comment says why:

> The values of the table were chosen so that the current ALIB behaviour was
> left unchanged. Note that because fwdDiffTable is not identical to
> (-1 x bwdDiffTable), swapping fromDate and toDate in the call to this
> function does not simply reverse the sign of the result.

There is no intent to recover. The answer is *defined by* two 7×7 lookup
tables, and they disagree with each other — forward Monday→Saturday is 4,
backward is −5. A cleaner implementation would be a different function. So
both tables are carried verbatim, and a test asserts the asymmetry still
exists upstream, so a future release that fixes it surfaces rather than
hides.

**The rule:** reimplement when the intent is recoverable and equivalence can
be proven; transcribe when the behaviour *is* the specification. Guessing
which case you are in is how a port silently changes results.

**A worked example of nearly guessing wrong.** `AddBusinessDays` looked like
a transcription case: upstream's `offsetTable` gives −1 for Saturday and −2
for Sunday, which reads like a departure from plain counting. Working it
through showed it is not — five business days from a Saturday is
`Sat + 7 − 1 = Friday`, and counting Mon–Fri from that Saturday also lands on
Friday. The table *encodes* the walk. So it was reimplemented, and the sweep
confirmed it across 28 start days, 15 counts and 7 weekend sets.

---

## Decision 5 — Preserve conventions, fix contract violations

Forced by UPSTREAM-7, where "preserve upstream behaviour" was not available:
two upstream implementations of date list truncation contradict their own
header *and* each other.

The project had been deciding these case by case. The cases turned out to
follow one rule:

**Preserve** when the behaviour is a *convention* — observable at a public
API, not wrong so much as particular, and a caller could reasonably depend on
it. `"14D"` never abbreviating to `"2W"`. A convention parser accepting
`"GARBAGE365"`. The asymmetric business-day tables, which *are* the ALIB
definition.

**Fix** when the behaviour is wrong *by its own contract* — undefined
behaviour, non-termination, or contradicting the documented intent.

**With a condition on every fix:** enumerable, and its consequence
**measured, not assumed** —

1. a row in a register naming the site;
2. a test asserting the defect still exists upstream, so an upstream fix makes
   the divergence visible instead of leaving a correction nobody revisits;
3. where the function is on the pricing path, a test that **prices both ways
   and reports whether the number moves**.

Point 3 is the one that is easy to skip and matters most. As of writing, no
fix has been *shown* to change a price — but nothing in the port prices
anything yet, so that is partly true by construction rather than verified.
The debt is recorded against the layer where `timeline.c` lands.

**Why fix at all:** a port that faithfully reproduces defects is a
translation. A port that corrects them, enumerates the corrections and
measures their effect is a contribution.

---

## Decision 6 — Audit the port separately from verifying it

Differential testing proves the port agrees with the oracle on the inputs
swept. It cannot see three things, and all three turned out to matter:

**Agreement on undefined behaviour is not agreement.** Where both
implementations are undefined, the test passes while both produce garbage.
The port accumulated interval digits into an `int` with signed overflow;
upstream does the same through `atoi`; the suite was green because no test
fed a ten-digit interval. UBSan on the port found it in seconds.

**A third of the port has no oracle at all** — the interop seam
(`to_tdate`, `from_tdate`, code conversions, the string preprocessor). There
is nothing to compare against, so those functions are only exercised
indirectly, and a bug in them surfaces somewhere confusing or not at all.

**Coverage is only what was swept by hand.** 13,866 recorded cases sounds
substantial; every one was a judgement call about what mattered. The gaps are
invisible by construction.

The answer was a property-based adversarial harness: totality over hostile
input, round trips across the interop seam, postconditions, fixed seed so
failures are reproducible. It found four defects on its first run.

---

## Decision 7 — Go vertical before going wide

**Chosen:** stop completing layers bottom-up and cut a vertical slice toward
one priced CDS.

**Why:** at the end of layer 4 the project had retired 20% of the lines and
**0% of the 8,871-line pricing path**. Everything done was infrastructure,
where answers are crisp integers. The biggest unknown — whether a C++20 port
can reproduce the original's *floating point* — sat five layers away, and the
answer reshapes the whole verification story. Finding out late is the
expensive way.

**Cost accepted:** a slice means stubbing parts of layers 6 to 8 and
revisiting them, so some rework is certain.

**First step of the slice:** the zero curve, because that is where the
floating point actually lives. `JpmcdsZeroRate` / `JpmcdsZeroPrice` reduce to
a handful of multiplies, a divide, a `log` and an `exp`.

### The result: bit-exactness is attainable

This was genuinely open. Floating point addition and multiplication are not
associative, so an algebraically equivalent rearrangement changes the last
bits. If the port could only match to a tolerance, the verification standard
would have to weaken from "equal" to "close enough", and the project would
need a tolerance policy before the remaining 5,000 lines of curve code
arrived.

It matched exactly. Across ten curve scenarios — annual, continuous,
semi-annual and quarterly compounding, ACT/365F and ACT/360, upward,
inverted, near-zero and negative rates, and a single-point curve — zero
rates, discount factors and forward discount factors are **bit-identical**
over roughly 6,500 queries spanning from 400 days before the base date to
4,200 days after.

Compared with `EXPECT_EQ` on raw doubles, not `EXPECT_DOUBLE_EQ`, which
tolerates 4 ULPs and would have hidden a real difference. And verified on
three toolchains: arm64/clang, x86-64/gcc and x86-64/MSVC.

### But "bit-exact" turned out to mean something narrower than I wrote

The first version of this entry claimed bit-exactness without qualification.
CI disagreed: the tier-1 replay failed on Linux and Windows while passing on
macOS. The numbers were 1 ULP apart —

```
0.60220191189840799   recorded on macOS
0.60220191189840788   computed on Linux
```

— and crucially **only the tier-1 test failed. The differential tests passed
on every platform.**

That splits the claim in two, and the distinction is the real lesson:

| claim | holds? | why |
|---|---|---|
| the port equals upstream, bit for bit | **yes, on all three toolchains** | both run in one process and call the *same* libm, so `exp` and `log` return identical bits |
| a recorded double can be replayed bit-for-bit elsewhere | **no** | `exp` and `log` are not specified to the last bit; macOS libm, glibc and the MSVC CRT disagree by about 1 ULP |

**So "bit-exact" is a property of two implementations sharing one libm, not a
property of a number.** Demanding bit equality from a recorded transcendental
value is really asserting that every libm agrees, which is false.

The fix is not to weaken the differential test — that claim stands and is the
load-bearing one. It is to recognise that tier 1 and tier 2 verify different
things:

- **tier 2**, port against upstream in one process: exact equality;
- **tier 1**, port against doubles recorded elsewhere: 4 ULPs, which absorbs
  a differing libm and is still far tighter than any porting error this
  project has actually produced — those moved results by orders of magnitude,
  or not at all.

Golden vectors containing transcendental results are **platform-specific
artifacts**, and the generator now says so in the file it emits.

This failed on two of three platforms and passed on the one I develop on,
which is precisely how a platform-specific assumption survives review.

**What made it work**, and what to copy:

- **Write the arithmetic in upstream's expression order**, including
  groupings that look redundant. `zt = z1t1 + (z2t2 - z1t1) * (t-t1)/(t2-t1)`
  is not refactored, because any rearrangement moves the last bit.
- **Reproduce the branch structure**, not just the formula. Upstream
  short-circuits the rate conversion when the compounding bases are equal;
  the two paths are algebraically the same and numerically are not.
- **Record golden doubles with `%.17g`**, which round-trips exactly. A lossy
  decimal rendering would have quietly weakened the very test that justifies
  the bit-exact claim.

**What would change this conclusion:** a module whose answer depends on
accumulation over many terms — the contingent leg integral is next — where
the compiler may vectorise or contract differently between the C and C++
front ends. The claim is established for the curve, not for the project.

**A sentinel removed along the way.** Upstream's `JpmcdsZeroRate` returns
`NaN` on failure. The port validates the curve once at construction — at
least one point, strictly increasing dates, a supported basis and day count —
so `zero_rate` and `zero_price` return a plain `double` that cannot be a
failure signal. Upstream checks the basis and day count on *every call*, from
inside a loop it describes as a hotspot.

---

## Decision 8 — Pay a measurement debt with a reason, not a test

The divergence policy (Decision 5) says a correction on the pricing path owes
a measurement: price it both ways and report whether the number moves. The
first such debt came due with the integration timeline, which is the only
pricing-path caller of the truncation the port corrects (UPSTREAM-7).

**The answer: the correction is free**, and the reason is structural.
`risky_timeline` merges the start and end dates into the list *before* either
truncation runs, and the contract requires `end > start`. So at least two
distinct dates always survive — and two or more survivors is exactly the
regime where upstream's defective truncation is correct. The minimal possible
timeline is the two endpoints, which is already safe.

Confirmed empirically too: 96 integration windows, plus 8 constructed to
produce the minimal two-date timeline on purpose, all matching exactly.

**The form of the answer is the point.** A passing test says the defect did
not bite in the cases tried. The structural argument says it cannot bite at
all. When a divergence needs justifying, reach for the second if it is
available — and a sweep alongside it, because an argument about code you have
just written is worth less than you think.

A test now pins the reason as well as the result: if a future refactor
truncated *before* merging the endpoints, the minimal timeline would drop to
one date, the defect would become reachable, and
`Timeline.MinimalTimelineIsExactlyTheTwoEndpoints` fails.

---

## Decision 9 — Bit-exactness survives accumulation, and a proxy guard gets replaced

The vertical slice reached its destination: **the port computes a CDS
protection leg price, bit-identically to the original.**

### The result that mattered

Decision 7 established bit-exactness for the zero curve and scoped the claim
on purpose: a handful of operations inside one expression is the easy case.
The hard case is **accumulation**, because summing over a grid is where a
compiler is most free to reassociate, vectorise, or contract differently
between the C and C++ front ends. The contingent leg integral sums one term
per timeline segment.

It held. Across **720 trades** — six start dates, six tenors from one month
to fifteen years, five recovery rates including the degenerate 0 and 1, both
protection-payment conventions, and both settings of `protectStart` — the
present value is bit-identical. Also with `today`, `valueDate` and
`stepinDate` all distinct, which exercises the start-date clamping and the
value-date restatement that a sweep setting them equal would skip entirely.

What made it work was the same discipline as the curve, applied to a loop:
preserve the expression order, preserve the *branch* structure, and preserve
the order terms are accumulated in. Upstream's Taylor expansion for small
denominators sums five terms in a specific order; summing them any other way
is algebraically identical and numerically is not.

That branch needed its own scenario. It only runs when
`|lambda + fwdRate| <= 1e-4`, which needs both a near-zero hazard and
near-zero rates — a realistic sweep never touches it. Half the function would
have been untested while the suite reported success.

### UPSTREAM-8: replacing a proxy with the thing it stood for

UPSTREAM-8 is a precedence bug — `!price > THRESHOLD` instead of
`!(price > THRESHOLD)` — that leaves a guard firing only on an exactly zero
price. It was left **open on purpose**, because the divergence policy pointed
the wrong way: "fix contract violations" would have made the port refuse to
price cases upstream prices correctly.

With the integral built, the question became testable instead of arguable. The
threshold `1e-100` was a proxy for "the logarithm will misbehave", so the port
checks that directly: compute, and reject a result that is not finite.

The measurement: down to survival around `1e-300` the logarithms stay finite
and the integral converges on `notional * (1 - recovery)`, the correct limit
for a credit certain to default. Only an exactly zero survival gives
`log(0) = -inf` and a NaN. **So the honest guard and the broken guard fire on
identical inputs** — verified from hazard 0.001 to 200, across the boundary
where survival crosses from tiny to underflowed.

The generalisable part: **when a guard tests a proxy, port the thing the proxy
stood for, then measure whether the behaviour changed.** Here it did not,
which is the best possible outcome — upstream's behaviour preserved, with a
condition that is actually true.

### The verification standard now has three tiers, not two

CI corrected me again, and the correction is worth more than the code.

The tier-1 replay failed on Linux and Windows with a 4-ULP tolerance already
in place — the one I had carefully introduced in Decision 7. Accumulation
amplifies: summing one term per timeline segment turned a 1-ULP libm
difference in the curve into roughly **600 ULPs** in the total.

```
7352.2330864798769   computed on Linux
7352.2330864809865   recorded on macOS
```

A relative difference of 1.5e-13 — nothing financially, and far outside any
ULP budget. So **a ULP tolerance does not survive accumulation**, and the
standard needs a third tier:

| what | bar | why |
|---|---|---|
| integers, calendars | exact | no rounding exists |
| non-accumulating floating point | bit-exact in-process; **4 ULPs** cross-platform | `exp`/`log` are libm-specific, but the error does not compound |
| **accumulating floating point** | bit-exact in-process; **relative 1e-12** cross-platform | each segment contributes its own libm difference and they add |

The relative bound is chosen against the size of errors that matter, not to
make the test pass: every port-side defect this project produced changed an
answer by orders of magnitude or not at all, and none would hide under 1e-12.
The test also **reports the worst relative difference it observed**, so the
remaining margin is visible rather than assumed — on the generating platform
it is exactly zero, and elsewhere it shows how much of the budget cross-libm
accumulation actually consumes.

The measured margin on x86-64 is about 1.5e-13 against a budget of 1e-12 —
roughly seven times the headroom.

A footnote on the reporting, because the first attempt at it was wrong. The
intent was to print the observed margin on every CI run, so the remaining
headroom would be visible rather than assumed. That does not work: `ctest`
shows a passing test's output only under `-V`, so the line never reaches the
harness, and a change to `verify.py` to surface it was dead code that was
removed again. The margin is now carried in the assertion's failure message
instead — visible exactly when it starts to matter, which is the next best
thing and does not require machinery that pretends to work.

### A second kind of oracle

With a price to check, a new class of test became available: not
"does it match upstream" but "is it financially possible". Full recovery is
worth exactly zero. Protection is monotone in tenor and in recovery. A PV
never exceeds the undiscounted worst-case loss. Paying at maturity is cheaper
than paying at default, since it is the same loss discounted further.

None of those compare against the original. They would catch a class of error
differential testing cannot: one where the port and upstream agree because the
port reproduced a misreading faithfully.

---

## Decision 10 — Bit-exactness survives nested accumulation too

The premium leg is the larger half of a CDS and the fiddlier one: a sum over
coupon periods, each of which may carry its own integral for the accrual owed
if default interrupts it. So where the protection leg tested one integral,
this tests **a sum of integrals** — the deepest nesting in the model.

It held. **576 configurations** bit-identical: one to sixty coupon periods,
four coupon rates including zero, three day count conventions, both accrual
conventions, both settings of `obsStartOfDay`, and clean and dirty prices.
Plus step-in dates inside a coupon period, which is what exercises the
accrued-interest deduction and the part-period integral — a sweep anchored on
schedule dates reaches neither.

The accrual integral has its **own** Taylor expansion, different from the
protection leg's: five terms where each carries a `(t0 + k*t1)` factor and the
coefficients nest differently. It needed its own near-zero-rate scenario; the
other module's coverage says nothing about it.

So the conclusion from Decision 7, scoped then to a single expression, now
holds for a single integral and for a sum of integrals. The discipline that
carried it all the way was unchanged: **preserve the expression order,
preserve the branch structure, preserve the order of accumulation.** Nothing
cleverer was needed, and anything cleverer would have cost the result.

### Three parallel arrays become one vector

Worth recording as a port improvement with a concrete failure mode behind it.
Upstream carries the schedule as

```c
int    nbDates;
TDate *accStartDates;
TDate *accEndDates;
TDate *payDates;
```

Nothing ties the three arrays to each other or to `nbDates`. A caller can
hand over arrays of different lengths and the library will read past the end
of two of them, with no diagnostic. The port uses one
`std::vector<FeePayment>`, which makes a mismatched schedule unrepresentable
rather than merely discouraged.

That is the same move as the month that cannot be 13 and the period type
normalised at construction: **the bug stops being expressible.** It is now
happened four times in this port, which is enough to call it the single most
productive technique here — more productive than any amount of review.

---

## Decision 11 — An unreliable reproducer is a lead, not a failure

The schedule generator was supposed to be a dull slice: turn a start date, an
end date and a period into the list of coupon dates. It is the last piece
between the two priced legs and an actual CDS price, and the algorithm has no
numerics in it at all — just dates.

Reading `stub.c` to port the stub-position parser turned up **UPSTREAM-9**.
One error message carries two `%s` conversions. The branch for a bad LOCATION
field passes both arguments. The branch for a bad LENGTH field, twelve lines
below it, passes one. So `vsprintf` reaches for an argument nobody pushed,
takes whatever is in that varargs slot, and dereferences it as a string.

A ten-line reproducer caught it immediately:

```
AddressSanitizer: stack-buffer-underflow, READ of size 5
  #0 strlen                      <- consuming the second %s
  #5 JpmcdsErrMsgV
  #7 JpmcdsStringToStubMethod+0x120
```

Then it stopped reproducing.

The same call inside the GoogleTest binary did not fault. Added to the
evidence tool — did not fault. Promoted to its own CMake target, deliberately
minimal, C rather than C++, nothing before the call under test — **did not
fault.** The scratch file faulted; the committed target did not.

There were two ways to handle that. Write up the crash seen once and move on,
which is how a finding becomes folklore. Or treat the inconsistency as the
actual finding and ask *why* a read of an arbitrary pointer sometimes lands on
mapped memory and sometimes does not.

The second question leads straight into `cerror.c`, and there is
**UPSTREAM-10**:

```c
char buffer[JPMCDS_ERR_MSG_BUFFER];   /* 4096 */
vsprintf(buffer, format, parminfo);
```

`vsprintf`, not `vsnprintf`. Every formatted error message in the library goes
through this one function, and 67 of the 452 message sites interpolate a
caller-supplied string. Pass a day count name of 3,900 characters and the
process dies, deterministically, from two independent public entry points —
survives at 3,800, aborts at 3,900, which is exactly where the ~200-character
prefix plus the argument reaches 4,096.

That is the more serious defect, and it is the mechanism that makes U9 more
than a cosmetic slip: **U9 supplies a bad pointer, U10 is the unbounded write
it feeds.** It was only found because U9's reproducer misbehaved.

### What got deleted

The U9 reproducer target. It was written, wired into CMake, and removed in the
same slice, because it did not do the thing its own comment claimed. Shipping
a file called `u9_reproducer.c` that exits 0 would have been worse than
shipping nothing — the next person to run it concludes the defect is not real.

The write-up changed shape too. The first draft of the differential test said
"this test CRASHES under AddressSanitizer". It does not. What replaced it says
the manifestation is caller-dependent, names the three callers where it does
not fault and the one where it does, and states that the evidence is the
source rather than the crash. The test stays disabled — not because it
crashes, but because running known undefined behaviour in CI means a future
libc can fail the build for a reason nobody will connect back to this test.

### The honest severity

Both are **medium**, and the reasoning is the part worth keeping.

U10 only fires once a host has called `JpmcdsErrMsgOn`. Messages are off by
default and nothing in the shipped distribution turns them on. With messages
off, a 50,000-character argument is harmless. So it is latent in the library
and live in whatever host enabled diagnostics — which is precisely the
configuration someone debugging a pricing discrepancy would be running, and
what an Excel add-in surfacing error text would want.

Calling it "remote code execution in the ISDA CDS model" would be a lie.
Calling it benign because the default configuration is safe would also be a
lie. Medium, with the condition stated, is the only version that survives
someone checking.

### Neither defect exists in the port, and neither was fixed

There is no global error log to format into. A failure is an empty
`std::optional` and a caller's string is never copied anywhere. That is the
fifth defect in this port to die from the shape of the code rather than from a
correction — and the only kind of fix that cannot regress, because there is no
longer a line of code for a future edit to get wrong.

### And the dull part was dull

The schedule generator itself ported cleanly: **1,024 of 1,024** configurations
exactly equal to upstream — sixteen date spans built around month ends and leap
days, twelve intervals, both stub positions, both stub lengths. Dates are
integers, so the bar is exact equality and nothing softer was accepted.

Two structural simplifications, both of which delete code. Upstream
accumulates into a fixed `TDate tmpDates[100]`, flushing each full block into a
growing date list through a routine that performs a **sorted merge dropping
duplicates** — so the block scheme only produces the right count because the
schedule happens to be strictly monotone, an invariant guarded by `ASSERT`,
which compiles away under `NDEBUG`. A `std::vector` removes the blocks, the
merge, and the unchecked invariant together. And upstream's two `TBoolean`s,
`stubAtEnd` and `longStub`, became two enums, so no call site can swap them.

One thing was transcribed exactly rather than tidied, and it is the one that
matters numerically: every date is computed **from the anchor** with a
multiplied interval, never by stepping from the previous date. Stepping one
month at a time from 31 January clamps to the 28th and then stays there;
anchored arithmetic returns to the 31st whenever the month is long enough. A
schedule that drifts to month-end and never recovers would move every
subsequent accrual.

### A test that was wrong about the code

One assertion failed, and the code was right. `HugePeriodDoesNotOverflow`
claimed a period near `INT_MAX` would be rejected; it is not — the first step
lands outside the span, the loop exits after one iteration, and the schedule
collapses to its two endpoints. Upstream does exactly the same thing, measured.

Following that through gives a small result worth having: the widened
multiplication in `advance_multiple` is **unreachable**. Reaching a multiplier
of 2 requires the step at multiplier 1 to land *inside* the span, which bounds
the period by the span length — so while the `periods > 0` precondition holds,
the product cannot overflow. The check stays, because `DateInterval` is an
aggregate with no invariant of its own, but it is now documented as defensive
rather than load-bearing. The test now asserts the behaviour that was measured
instead of the behaviour that was assumed.

## Decision 12 — The composition layer is where the pieces are first required to agree

`cds.c` is 505 lines and contains almost no arithmetic. It builds a premium
leg, builds a protection leg, and subtracts one PV from the other. On the
layer map it looked like the lowest-risk slice in the project — glue over four
modules that were already bit-exact against the oracle.

It found a defect in the first run.

### What the earlier suites could not see

The port's protection leg had a passing differential suite with a bit-exact
sweep behind it. It built a `TContingentLeg` by hand on the upstream side, a
`ContingentLeg` by hand on the port side, and compared the PV. That verified
the integral thoroughly and verified the **constructor** not at all.

Upstream's constructor is in `cx.c` — a different file from the PV routine in
`contingentleg.c` — and it does this:

```c
/* p->startDate is defined as giving protection from end of startDate.
   So if we want to protect on the start date, we need to move this
   date forward by one. */
if (protectStart) { p->startDate = startDate-1; }
```

The port skipped it. The cost is exactly one day of protection on a five-year
contract: a relative price error of about **2e-5** — small enough to look like
rounding, and two orders of magnitude too large to be rounding.

Worse, the port's own comment on `protect_start` asserted that upstream
applied this offset to the step-in date *instead of* the leg start. Both
happen, in both files, and neither substitutes for the other. The comment was
not merely incomplete; it was an argument for why the missing line was not
missing.

### The generalisation, which is worth more than the fix

**A differential test that hands the same pre-built intermediate to both sides
has verified nothing about how that intermediate is produced.**

Every such test in this port was written that way deliberately — constructing
the intermediate by hand is how you get a controlled sweep over the input
domain. So the gap is structural, not careless, and it will exist anywhere
this technique is used. The remedy is explicit: wherever a factory exists,
compare the factory, not only what consumes its output. That is now done for
the protection leg and for the premium leg's schedule, date for date.

The broader lesson about layering: **composition is not glue.** It is the first
place the pieces are required to agree about anything. Four modules can each
be bit-exact in isolation and still disagree about whose responsibility a
one-day convention is, and nothing below the composition layer can ask that
question.

### The milestone, stated plainly

The port now prices a CDS. **1,600 recorded prices** replay in CI without an
oracle, and the live differential sweep is bit-exact across the contract
space: five coupons from zero to 500bp, four maturities including an off-cycle
one, three coupon frequencies, three day counts, both accrual conventions,
clean and dirty, four bad day conventions against two calendars, recovery from
0 to 1, step-in both on and off schedule dates, and settlement up to a month
forward.

One check in that set is not a tautology and is the one worth having. The par
spread is computed as `protection / annuity`; the price is computed as
`protection - annuity x coupon`. Feeding the par spread back in as the coupon
should give zero, and it does — to **1e-14** across every hazard level tested.
Two different arrangements of the same quantities agreeing to that tolerance
is evidence about the arithmetic, not about the plumbing.

### Three asymmetries kept, not tidied

Each is observable in a price, so each is transcribed:

- The premium leg accrues from the contract start; protection runs from
  `max(stepin, start)`. Accrual can predate step-in, protection cannot.
- The last accrual end is the **unadjusted** maturity date (plus a day when
  `protect_start`), while the last **pay** date keeps its bad-day adjustment.
  So a final period can accrue past the day it settles. That is the standard
  convention, and upstream expresses it by overwriting a value the loop just
  computed — which reads like a bug and is not one.
- `end == start` is legal with `protect_start`, and upstream special-cases it
  with a hand-built two-element date list specifically to bypass the schedule
  generator's `end > start` requirement. One period, accruing one day.

### A guard that is not a fix, and says so

`JpmcdsCdsParSpreads` ends with `parSpread[i] = contingentLegPV / feeLegPV` and
no check on the denominator. The port returns `nullopt` instead.

Before writing that up as a defect, it was worth asking whether a zero annuity
is reachable. Measured: hazard rates up to 1000 leave it positive — survival
damps to 9.8e-16, not zero. A contract that has already matured is rejected
earlier. The shortest legal contract still accrues one day. No reachable input
produced one.

So the guard is **defensive**, and it is recorded as defensive rather than
counted among the upstream defects. This is the second time in two slices that
following a guard through to "is this actually reachable?" turned a would-be
finding into a documented non-finding. Both times the honest answer was less
impressive and more useful.

## Decision 13 — A sensitivity probe is not evidence of reproducibility

The root finder was the slice I expected to break the verification standard.

Every module ported before it evaluates a formula. A one-ULP difference in an
intermediate produces a one-ULP difference in the answer, which is why the
three tiers worked: exact for integers, 4 ULPs for a single `exp`, relative
1e-12 once differences accumulate across segments.

A root finder is categorically different. Its arithmetic feeds **comparisons
that choose the next iterate**. A difference far below any tolerance can flip
a branch, change the iteration path, and move the answer by orders of
magnitude more than the difference that caused it. If that happened, recorded
roots could not be replayed cross-platform at 1e-12, and the standard would
need a fourth and much looser tier for iterative results.

### So it was measured rather than assumed

Perturb the objective's return value by exactly one ULP — `std::nextafter` in
each direction — and see what the returned root does. That is the right proxy
for a cross-platform libm difference, because that is precisely how one shows
up: the search is identical, the function values differ in the last bit.

| objective | root | evaluations | effect of one ULP |
|---|---|---|---|
| Linear, Cubic, Quadratic, Exponential, SteepTanh, CdsMonotone | — | 5–25 | **identical root, identical evaluation count** |
| Quantised (output truncated to 6 digits) | 1.4142132 | 21 | 3.1e-16 relative, ±2 evaluations |

No amplification, and the reasoning looked sound: the convergence tolerance
**is** a quantiser. The search stops when `|f| <= 1e-8` and the iterates are
within `1e-8`, so a perturbation of 1e-17 cannot change which iterate first
satisfies that test unless it sits exactly on the boundary.

I wrote that up, committed it, pushed it, and CI disagreed within the hour.

### What CI found that the probe could not

Linux, Windows and the sanitizer job all failed one test. macOS passed. 156
failing assertions out of 10,296 recorded cases, and the split matters:

- **146 were evaluation COUNTS** -- a different number of iterations on a
  different platform. Including for `Quadratic` (97 cases) and `Cubic` (36),
  which are **pure polynomials with no libm anywhere in them.** Worst
  observed ratio 1.93x: 108 evaluations against 208.
- **9 were roots**, all of them the same objective, discussed below.

The cause of the count differences is almost certainly floating-point
contraction. A compiler may fuse `a*b + c` into a single FMA with one rounding
instead of two, and `-ffp-contract=fast` is the default for both GCC and
Clang. That changes values **mid-expression**, and by different amounts at
different points.

Which is exactly what my probe could not model. Shifting the objective's
return value by one ULP, uniformly, is a strictly weaker disturbance than
re-rounding the middle of an expression at every evaluation. The probe passed
on all three platforms; the property it measures is true. It simply is not the
property that mattered, and I presented it as though it were.

**A single-platform sensitivity probe is not evidence of cross-platform
reproducibility. The three-OS matrix is.** That is the actual decision here,
and it was made for me.

### The nine root differences, which are not a defect

All nine are `Quantised`, whose output is truncated to six digits. The
disagreements are 1.1e-10 and 6.3e-9 on a root of 1.414.

That function is **flat in steps**. Near its root the derivative is about
2.83, so with a 1e-6 output quantum every x within roughly 3.5e-7 produces an
identical value. Every point on a step satisfies `|f| <= f_tolerance` equally
well. Which point a search lands on is a property of the path, not of the
function -- and both observed differences sit comfortably inside one step.

So neither platform is wrong, and the fix is not a tolerance fudge. It is to
say what is true: **the root is determined only to the resolution of the
objective.** Objectives now declare that resolution, and the replay test
widens to it where it is non-zero.

### Three properties, three different answers

The correction took two CI rounds, because I fixed the root tolerance and the
count assertion and still had one assertion left. The second round found the
property I had not separated: **whether the search converges at all.**

One case out of 10,296. `TwoRoots` has a root on the lower bound and another
near 3.75; with one bracket-and-guess combination the search exhausts its 100
iterations on macOS and converges on Linux. Same code, same inputs, different
contraction. Neither verdict is wrong -- "did not converge in 100 steps" is a
statement about the path, and the path is not portable.

So an iterative result has three properties and they do not travel together:

| property | portable across platforms? |
|---|---|
| the root, for a well-posed objective | **yes** -- relative 1e-12, verified on three platforms |
| the root, for an objective with plateaus | only to the plateau width (6.3e-9 observed, 3.5e-7 bound) |
| the number of evaluations | **no** -- 146 of 10,296 cases differ, up to 1.93x |
| whether it converged at all | **yes** for a well-posed objective; **no** for a pathological one |

Working that out took publishing a wrong conclusion and then being corrected
twice by the same CI job. Each round removed one assertion that was true on
one platform and presented as though it were true everywhere.

### What the standard actually says now

The three tiers survive **for the root**. Every smooth objective agreed to
relative 1e-12 on all three platforms. No fourth tier.

What does not survive is my idea of recording the iteration path as a
portable check. The path is in-process evidence -- the tier-2 differential
suite asserts count equality against the oracle and passes -- and it is NOT a
cross-platform claim. The recorded count is now checked as a 4x band, which
catches a port iterating an order of magnitude more than upstream while
tolerating the 1.93x the platforms actually differ by.

And the replay test gained the check that should have been there first:
**every returned root is verified to satisfy the solver's own contract**,
`|f(root)| <= f_tolerance`, with the root inside the bracket. That needs
neither an oracle nor a recorded number, holds identically everywhere, and is
the property a caller actually depends on. A recorded root says "macOS got
this number". This says "the returned point solves the equation to the
accuracy requested".

### The case that does amplify, and is not a defect

One objective moves by 3.75 under a one-ULP perturbation. It took a second
look to describe correctly.

`0.045(1 - e^{-4x}) - 0.012x` is zero at `x = 0` and again near `x = 3.75`.
The cdsone call site puts a bound at exactly 0, so unperturbed the search
accepts `x = 0` through the exact `y == 0.0` test, in three evaluations. One
ULP disqualifies it, and the search goes and finds the other root.

My first reading of that table said "AMPLIFIED" and I nearly wrote it up as
the fourth tier being necessary after all. It is not amplification: it is a
function with two roots, and the finder promises *a* root, not a particular
one. The objective was mis-designed — I had intended it to be monotone — and
the fix was to split it into a genuinely monotone case and a deliberately
two-rooted one.

Both are now executable facts, because "the root finder is robust to one ULP"
is true **only for single-rooted objectives** and the unqualified claim would
be false.

### Recording the iteration count, not just the answer

For every other module a recorded answer is sufficient evidence, because the
computation is a formula. Here the answer is the end of a path, so the
differential suite and the recorded vectors both compare the **number of
objective evaluations** as well as the root.

That matters: two different paths can land on the same root whenever the
tolerance absorbs the difference, and a root-only check would pass. **10,296
recorded cases** carry their evaluation count, and the live differential sweep
matched root and count on the first run across 2,000+ option combinations.

### A convergence test that depends on where the bounds are

Preserved, documented, and not counted as a defect:

```c
if (y0 == 0.0 || (ABS(y0) <= facc && (ABS(boundLo-x0) <= xacc ||
                                      ABS(boundHi-x0) <= xacc)))
```

The initial guess is accepted when its function value is inside `facc` **and
the guess happens to lie within `xacc` of either bound**. Proximity to a bound
is not evidence about a root. It is standing in for the "did the last two
iterates converge" test that the later checks use and that cannot run when
there is no previous iterate.

The consequence is real: the same guess, with the same function value, is
accepted in a narrow bracket and rejected in a wide one. It only ever accepts
a point already inside `facc`, so it cannot return something the caller
declared unacceptable — which is why it is a quirk rather than a defect. It is
pinned by a test that demonstrates both outcomes from the same guess.

## What went wrong, and how we found out

The honest section. Eight defects in the port against seven in a
seventeen-year-old C library — more in my own new code than in theirs.

The lesson is **not** "modern C++ is safer". It is "instrumented code is
safer". Without ASan, UBSan-made-fatal, a three-OS matrix and a live oracle,
every one of these would have shipped.

### A sanitizer that cannot fail the build is documentation, not a gate

The worst process failure. UndefinedBehaviorSanitizer **prints and continues,
exiting 0.** The sanitizer job had only ever failed on AddressSanitizer
errors — which is exactly why two upstream defects surfaced (both are *also*
ASan errors) while pure undefined behaviour was invisible.

Noticed only because a new test "passed" while printing five overflow
reports. Fixing it (`-fno-sanitize-recover=undefined`, applied to the port
and never to the oracle) immediately failed three tests that had been green
for days.

**Check that a new gate can actually fail before trusting it.**

### The code you write to handle the edge case is itself edge-case code

Three times:

- PORT-2 and PORT-3 live in the code written to fix PORT-1. I guarded the
  multiply and missed the scale; then guarded the months path and missed the
  days path.
- PORT-6 lives in the test written to prove PORT-4 fixed — it asserted one
  platform's answer as a literal.
- PORT-7 lives in a bounds check: negating `LONG_MIN` to get a magnitude is
  undefined, and silently returned the input unchanged.

The version that holds up stopped guarding individual operations and instead
**widened the arithmetic once and range-checked at the boundary.**

### Cross-platform CI is not ceremony

Three defects came from one root: `std::chrono::days::rep` is 32-bit on
libc++ and 64-bit on libstdc++, and `long` — hence upstream's `TDate` — is
32-bit on Windows and 64-bit elsewhere.

The sharpest was PORT-8: a signed overflow computing "how much room is left"
meant **ordinary backward business-day offsets were rejected**. It passed
locally and passed on Windows. Only Linux caught it.

A missing `<functional>` was caught only by Windows; libc++ supplies it
transitively. The upstream model's own date type is not a stable width across
platforms, which matters at every ABI boundary — and both the planned C shim
and the Python bindings cross one.

### Measure before naming a mechanism

Investigating the date list defects I hypothesised a use-after-free and said
so. ASan showed nothing of the kind; it was deterministic logic all along. I
had a plausible story and reached for it instead of a sanitizer that would
have answered in thirty seconds.

### My own gate hid the diagnostic

When CI failed, the log showed only ctest's summary — no assertion detail, no
numbers. `verify.py` was printing the *last 4,000 characters* of ctest output
on failure, which reliably cut off everything explaining the failure and kept
the part that merely announced it.

Fixed to print everything on failure. A gate exists to produce a diagnostic;
truncating that diagnostic defeats the purpose, and it cost a push-and-wait
cycle to notice.

### A dangling reference in test code

`const Scenario &s = scenarios().front();` — `scenarios()` returns by value,
so the reference bound into a temporary that died at the end of the
expression. The symptom in the debug build was a null curve and a confusing
assertion failure. ASan named it immediately as a `heap-use-after-free`.

Worth recording because it was in *test* code, which is easy to treat as
lower-stakes. A broken test is worse than no test: it fails for a reason that
has nothing to do with the thing under test, and the natural next move is to
doubt the code instead of the test.

### A patch can silently not apply

Twice, an edit targeted text that had since been reformatted. The replace was
a no-op and reported success. Caught both times only because a test still
failed. Now: assert the target text exists before replacing it.

### I understated a finding, then the data corrected me

UPSTREAM-7 was first reported as "truncation never examines index 0".
Generating the vectors showed upstream disagreeing on **234 of 600 cases —
39%**, which that framing cannot explain. The real scope is broader: the scan
finds nothing whenever *fewer than two dates survive*, and one implementation
then **ignores the truncation entirely**.

The published write-up had to be corrected. Generating data you did not need
is a good way to discover your description was wrong.

---

## What the upstream defects say about 2009 C

Seven, all with reproducers. The pattern is more interesting than the list:

- **Two are ordering bugs in validation** — a month indexed into a table
  before the month was range-checked; a digit copied into a buffer before the
  length was checked. Validation written as an afterthought rather than a
  precondition.
- **Two are sentinel collisions** — `JPMCDS_DEFAULT` and `FAILURE` are both
  `-1`; `JpmcdsDate` returns `FAILURE` as a date. The result space and the
  error space share values.
- **Two are loop-boundary errors** — `i > 0` where `i >= 0` was meant, and a
  `do/while` with no iteration bound at all.
- **One is a missing `break`** that makes a branch unreachable.

Not one of them is a hard algorithm being got wrong. They are all the
interface between "the computation" and "everything around the computation" —
which is precisely the part a type system can carry for you. Three defects
became *unrepresentable* in the port without anyone writing a patch: a month
that cannot be 13, a result that cannot carry a sentinel, a period type
normalised at construction.

That is the strongest argument for the port existing, and it is not
"C++ is better". It is: **put the invariant in the type and the bug stops
being expressible.**

---

## Decision 14 — Write the contract check first

The bootstrap turns quoted par spreads into a clean spread curve by solving
for one node at a time. It is the last module before the upfront charge, and
the one where the root finder's lessons had somewhere to land.

So this time the first test in the file was not a comparison against upstream.
It was the bootstrap's defining property: **a curve bootstrapped from a set of
quoted benchmarks must reprice those benchmarks to zero.**

That ordering is the entire lesson of the previous slice. Three CI rounds went
into discovering that the things I had recorded -- the iteration count, the
convergence verdict -- were not portable, while the property a caller actually
depends on had never been asserted at all. A recorded rate says "macOS got
this number". "The curve reprices its own inputs" says the bootstrap works,
needs no oracle, and holds everywhere.

1,488 benchmark repricings, all within 1e-9 of zero. And then the differential
comparison, which is still bit-exact: 588 solved rates equal to the last bit
against the 2009 C.

### The tolerance was derived, not tuned

The recorded rates come out of a solver, so the right tolerance is set by the
solver's stopping rule rather than by how the arithmetic rounds:

- the solve stops at `|pv(s)| <= 1e-10`;
- two platforms may return different spreads both satisfying that, bounded by
  `2 x 1e-10 / |d pv/d s|`;
- that derivative is the premium leg's risky duration, at least about 1 for
  the shortest benchmark, giving `|ds| <= 2e-10` continuously compounded;
- the published rate is `exp(s) - 1`, derivative up to about 1.7 for the
  distressed ladders, so `|d(published)| <= 3.4e-10`.

**1e-9 absolute**, with a factor of three in hand. Explicitly not the relative
1e-12 used for accumulating values: a solver's output is determined by where
it stopped, not by the rounding of the arithmetic that got it there.

### One mutable curve, shared with the solver

Worth recording because it is the only shared mutable state in the model and
it looks like a bug until you see why it is not.

The objective function handed to the root finder **writes the trial spread
into the curve** and then prices the benchmark against that same curve. Node
`i` is being solved; nodes before it hold solved spreads; nodes after it still
hold their initial values, which are the raw quoted coupons. Those later nodes
are never read, because a CDS maturing at `end[i]` is priced only from dates
at or before `end[i]`.

The port keeps the structure. The one change is forced: `ZeroCurve` validates
on construction and has no setter, so the vector of rate points is the mutable
state and a curve is rebuilt from it per evaluation. Same arithmetic, same
order, same results -- with the validation upstream does once now done every
time.

### The feasible region, which I did not expect

The first run of the contract test failed, and the premise was mine: I had
asserted that every quote ladder bootstraps. It does not.

Growing a distressed ladder one benchmark at a time at recovery 0.8 -- loss
given default 0.2 -- the first four nodes solve to hazard rates of 0.287,
0.326, 0.375 and 0.542. The fifth, a seven-year quoted at 800bp, does not.
Survival to five years is already about 6.6% by then, and reaching the
seven-year quote would need a forward hazard the curve cannot deliver while
staying arbitrage-free.

About a tenth of the realistic quote space is infeasible at high recovery.
Upstream refuses exactly the same combinations, and that agreement is now
asserted: **agreeing to reject is as much a part of the contract as agreeing
on a number.** The recorded vectors include the 32 infeasible cases with
`n == 0` rather than omitting them.

Unlike the root finder's convergence verdict, this verdict IS portable, and
the reason is worth separating. There the question was "did this iteration
converge in 100 steps", a property of the path. Here it is "does a root exist
in `[0, 1e10]`", a property of the market data. The first travels badly across
platforms; the second does not travel at all because it never depended on the
path.

### Two more assumptions I had to measure

Both were wrong in the same direction -- I expected rejection and found
permissiveness.

`guess = coupon / (1 - recovery)`, unguarded. I wrote it up as rejecting
degenerate recovery rates and then checked:

- `recovery == 1` with a non-zero coupon gives an infinite guess, outside the
  bounds. Rejected.
- `recovery > 1` gives a negative guess. Rejected.
- `recovery == 1` with a ZERO coupon gives `0.0/0.0`, and **a NaN guess passes
  the bounds check** -- neither `NaN < 0` nor `NaN > 1e10` is true. The search
  proceeds on NaN, fails to bracket, retries at the bounds, and fails. No
  wrong answer, so a non-finding rather than a defect.
- `recovery < 0` is **accepted**. A recovery of -0.5 is a loss given default
  of 1.5; the arithmetic is coherent and upstream permits it. Measured at
  -0.5 and -2.0: both bootstrap, and the implied hazard falls as LGD rises,
  which is the right direction. I had asserted rejection and the test failed.

That is three slices in a row where following "is this actually reachable?" or
"is this actually rejected?" turned an expected finding into a documented
non-finding. The pattern is consistent enough to be worth stating as a habit:
**an unguarded division is a question, not a defect.**

## Decision 15 — The model produces its number, and the contract is what the solver promises

`cdsone.c` is 234 lines and does almost nothing: bootstrap one quoted spread
into a flat curve, price the contract against it. Every dependency was already
verified. All six differential tests passed on the first run, including the one
that matters most.

### ISDA's own numbers

The distribution ships one example program, and it prints three numbers. They
are the only published output in the whole package, and the only external
reference this project has -- everything else is either upstream's behaviour
observed, or a property argued from first principles.

```
Upfront charge @ cpn = 0bps    =  38651.944609
Upfront charge @ cpn = 3600bps =  -10682.519172
Upfront charge @ cpn = 7200bps = -60016.982954
```

The port reproduces all three, and in CI with no oracle present.

That last clause took one decision. The example builds its discount curve with
`JpmcdsBuildIRZeroCurve`, the single public entry point of the 5,126-line
interest-rate stack this port does not implement. Rather than port 5,126 lines
to check three numbers, the curve is **recorded as input data** -- it is
derived from running the model, so licence section 4(b) covers it exactly like
every other recorded vector. 22 nodes generated from 14 quoted instruments,
every rate 1e-9.

That is a defensible split rather than a shortcut: the CDS model takes a
discount curve as an input, and a caller with a curve already in hand -- which
is every realistic caller -- never touches that stack.

### The contract I asserted wrongly, and what it taught

The obvious contract for an invertible pair is a round trip: feed
`cdsone_spread` the charge that `cdsone_upfront_charge` produced, get the
spread back. I asserted it and it failed at 7200bps, by 7.9e-4 -- six orders
of magnitude outside the tolerance.

Not a defect. The wrong contract. Measured sensitivity, five-year contract,
recovery 0.4:

| quoted spread | upfront charge | d(charge)/d(spread) |
|---|---|---|
| 25bps | 0.0126 | 4.98 |
| 500bps | 0.207 | 3.32 |
| 2000bps | 0.487 | 0.91 |
| 3600bps | 0.565 | 0.22 |
| 5000bps | 0.582 | 0.053 |
| 7200bps | 0.585 | **-0.008** |

The charge **saturates**. At wide spreads survival has already collapsed, so
nearly all protection is paid regardless and the charge stops responding to
the quote. Past about 5000bps the derivative changes sign: the map is not
injective, and **no implementation can invert it uniquely there.**

So the honest contract is the one the solver actually promises --
`|charge(returned_spread) - target| <= f_tolerance` -- which is well posed
everywhere it converges. That is now the assertion, with the spread round trip
kept as a separate test restricted to spreads below 1000bps, where the
derivative exceeds 2 and a 1e-8 charge tolerance bounds the spread error near
5e-9.

This is worth more than a passing test. **A user quoting a distressed name and
asking for the implied flat spread will get an unreliable answer**, and
nothing in upstream says so. It is not a bug in either implementation; it is a
property of inverting a saturating function, and it is now written down.

### Four dates of the same type, finally given a struct

`cds.hpp` flagged adjacent same-typed dates as the hazard argument order
cannot protect against, and then did nothing about it, because it mirrors
upstream's internal composition layer. `cdsone` is the published entry point
-- the one Python and spreadsheet bindings will wrap -- so here it was worth
paying for:

```cpp
struct UpfrontDates { Date today, value, benchmark_start, stepin; };
```

Two of those four do jobs that are easy to conflate and genuinely independent:
`benchmark_start` is the start of the synthetic CDS used to imply the flat
curve, while `Cds::start` is the traded contract's, carrying upstream's own
comment "cds can start from past". A test asserts that moving either one
changes the answer, so neither can be silently ignored by a future edit.

A third mapping is worth naming because the names lie: upstream's
`payAccruedAtStart` is passed straight through as `JpmcdsCdsPrice`'s
`isPriceClean`. The two terms suggest opposite polarity and the code treats
them as identical. The code is the specification.

### Where this leaves the project

The CDS pricing chain is complete: curve, timeline, both legs, composition,
schedule, stub, root finder, bootstrap, upfront charge. **325 tests with the
oracle, 214 without.** The port prices a CDS and computes an upfront charge
bit-identically to the 2009 C, and reproduces ISDA's published example on
three platforms with no oracle anywhere.

What remains is not capability. `defaulted.c` (95 lines) handles accrual on a
name that has already defaulted; `JpmcdsCdsFeeLegFlows` needs `cashflow.c` to
report cash flows rather than price them; and the 5,126-line interest-rate
stack sits behind one function signature for callers who want upstream to
build their discount curve too. All three are API completeness, not pricing.

## Decision 16 — The last module held the worst defect

`defaulted.c` is 95 lines, one function, and it was the smallest thing left on
the list. It computes the coupon accrued up to a credit event -- the cash owed
when a defaulted CDS settles.

It contains the only defect in this project that silently returns the wrong
**money**.

### What it does

```c
while (i < fl->nbDates)
{
    if (fl->accStartDates[i] <= tradeDate && tradeDate < fl->accEndDates[i])
    {
        *accrualDays = edd + 1.0 - fl->accStartDates[i];
```

It locates the coupon period by the **trade** date and then accrues to the
**event** date. It guards `edd >= startDate` but never
`edd >= accStartDates[i]`. So once the trade date crosses a coupon boundary
the event date did not, the subtraction goes negative -- and `SUCCESS` is set
on the next line.

Quarterly from 20 March 2024, credit event 1 August, notional 1e7, coupon
100bp. Varying only **when the question is asked**:

| trade date | upstream | accrual days | amount | port |
|---|---|---|---|---|
| event+0 | SUCCESS | 43 | 11,944.44 | 43 |
| event+45 | SUCCESS | 43 | 11,944.44 | 43 |
| event+50 | SUCCESS | **-49** | **-13,611.11** | 43 |
| event+365 | SUCCESS | **-322** | **-89,444.44** | 43 |

The flip lands exactly where the trade date passes 20 September.

### The argument is the dependence, not the sign

A negative accrual is obviously wrong and a careful caller might notice. The
part that makes this High is simpler and worse: **the coupon accrued at a
credit event cannot depend on when someone asks.** Upstream's answer does.

In the window before the sign flips, it is wrong without being obviously
wrong. And the affected regime is the ordinary one -- settlement after a CDS
auction routinely lands weeks after the determination date, and a quarterly
coupon boundary sits every 13 weeks. Measured over event dates across April
to December with lags of 0 to 120 days: **62% of 2,250 cases differ from
asking on the event date, 60% go negative.**

### Finding it required asking the right question

Not reading the code. I read those 95 lines and the `tradeDate`/`edd` mix did
not announce itself -- the guard on `edd >= startDate` looks like someone who
thought about date ordering.

What found it was a probe written for a different reason. The plan was to map
the function's edge cases before porting, the way the last three slices had
established as a habit: list the degenerate inputs, measure what upstream
actually does. A row of that table was "edd one period earlier than trade",
and it came back `-49`.

Then the second probe made it undeniable: fix the event date, walk the trade
date forward, watch the answer change. **Invariance is the cheapest test there
is and it needs no oracle** -- just a reason the answer should not move.

### The correction is one date

Locate the period containing the event date. The period start then precedes
the event by construction, the result is always positive, and it stops varying
with the trade date. The arithmetic, the conventions and the
`trade < event` gate are all upstream's, untouched.

A second behavioural change falls out of the same root cause and is recorded
rather than absorbed: upstream **fails** when the trade date lies past the
final accrual end, because no period contains it -- even though the event sits
inside the schedule and the debt is well defined. Locating by the event date
means a late settlement still returns what is owed. Intended, but a divergence
rather than a sign fix, and it gets its own test.

This is the second module to correct upstream rather than reproduce it, after
the date list. Both follow the same pattern: record the **documented intent**
rather than upstream's answers, with a flag for whether upstream agreed when
the vectors were generated -- 859 of 1,302 here -- and assert in the
differential suite that the disagreement **still exists**, so an upstream fix
surfaces instead of leaving a correction nobody revisits.

### PORT-10, caught in the same run

My first implementation wrote the amount as `fraction * coupon_rate * notional`.
Upstream computes the year fraction into its output and then does
`*defaultAccrual *= couponRate * notional`, which is
`fraction * (coupon_rate * notional)`. Different association, one ULP apart:
3333.333333333333 against 3333.3333333333335 on a 30/360 stub.

The differential suite caught it immediately, on the only day count where the
difference was representable. Ten slices of "preserve the expression order"
and I still wrote it left to right -- which is the argument for the harness
rather than for being careful.

## Decision 17 — The harness caught the test, not the code

Core-first from here: finish the library, then interface. `date_sup.c` plus
`dateadj.c` is the first slice of the remaining 8,711 lines, chosen because
`date_sup` is a leaf with six modules above it -- the highest unlock per line
left in the project.

The slice went cleanly and then taught something about my own testing.

### Three failures, and the code was right

The differential suite reported three failures, all on the lunar-month
interval `'U'`, all of the same shape: the port succeeded where upstream
failed. My first reading was that I had found a second defect in
`JpmcdsDtFwdAny`, which refuses `'U'` outright.

Then I built the comparison table properly:

| letter | `JpmcdsMakeDateInterval` gives | `DtFwdAny(raw)` | `DtFwdAny(constructed)` | `IntervalToYears(raw)` |
|---|---|---|---|---|
| W | 7D | ok | ok | ok |
| U | **28D** | **FAILURE** | **ok** | **FAILURE** |

`JpmcdsMakeDateInterval('U')` normalises to 28 days -- **exactly what the
port's `make_date_interval` does.** So through the official constructor, `'U'`
works everywhere in upstream and the port agrees. My test had been comparing
the port's normalised interval against a hand-built `TDateInterval{n,'U',0}`,
which is a different animal.

The port was right. The comparison was not like for like. Fixed by building
the upstream interval through its own constructor, as callers do; hand-built
structs now appear only in the test that is *about* them.

### What that does to the finding

UPSTREAM-12 is real -- `case 'U'` has no `break`, so it computes 28/365 and
then returns FAILURE -- but it is reachable only through a hand-constructed
interval, and the first draft of my write-up did not say so.

The reachability is not a technicality I invented to deflate my own finding.
Upstream contemplates hand-built intervals explicitly: `JpmcdsDtFwdAny` keeps
code for `'A'`, `'S'` and `'Q'` with the comment *"left here for people who
have set up their own TDateIntervals without calling
JpmcdsMakeDateInterval"*. It extends that courtesy to `'W'` and not to `'U'`.
So a hand-built lunar interval is refused by one function and mishandled by
the other -- a genuine inconsistency, at low severity, with the condition
stated.

The evidence tool now prints the self-proving version: the same function, on
the same semantic interval, returns FAILURE for the hand-built form and
SUCCESS for the constructed one, with identical values in both.

### An argument that had to become a measurement

`interval_to_years` has two branches where upstream has eleven, because a
`DateInterval` carries only days or months. The claim that this is safe is
arithmetic: normalisation divides numerator and denominator by the same
factor, so `'Q'` with 5 periods becomes 15 months and 15/12 is exactly 5/4.

That is a correct argument and it is still an argument. It is now 120 recorded
cases comparing the normalised quotient bit-for-bit against upstream's, across
every letter the parser accepts and period counts from 1 to 1000. The habit
that keeps earning its place: **if the reasoning is sound, the measurement is
cheap; if the measurement is expensive, the reasoning was probably wrong.**

### One design change forced by a decision six modules old

`JpmcdsDtFwdAdj` branches on `isBusDays` and then branches AGAIN on whether
the unit letter is literally `'D'`. So its BUSINESS case means two different
things: step this many business days, or advance by a period with
end-of-month carry.

A `DateInterval` cannot express that, because `'W'` and `'U'` were normalised
to days at construction. Under upstream's test a weekly interval is *not*
`'D'` and takes the period path; a naive port would send it down the other
one.

Rather than smuggle the unit letter back in, the choice became explicit:
`OffsetBasis` has four values where upstream had three, splitting BUSINESS
into `BusinessDays` and `BusinessPeriod`. The caller states which it means
instead of encoding it in a character the type no longer carries. The one
caller in the library -- `streamcf.c`, for a floating rate's spot offset --
knows perfectly well which it wants.

### And a small piece of tidying that paid immediately

`detail::advance_multiple` was living in `schedule.hpp`, where it was first
needed. Multiplying an interval is interval arithmetic, not scheduling, and
the moment a second module wanted it the misplacement showed. Moved to
`date_interval.hpp`; 338 tests confirmed nothing shifted.

## Decision 18 — The generator found what the targeted sweeps missed

`interpc.c`'s interpolation layer: `lintrp1.inc` -- the second
preprocessor-as-template in the distribution after `bsearch.inc` -- plus the
three x-axis metric functions from `yearfrac.c`. Two defects, and both were
found by something other than reading the code or writing the obvious test.

### UPSTREAM-13, and a taste judgement that paid off eighteen slices later

Upstream cannot say "search without checking order" per call, because that
choice lives in a mutable global: `bSearchCheckOrder_g`, consulted by both
binary-search entry points before validating that their domain is sorted.

The discontinuous interpolator legitimately needs the unchecked search --
repeated abscissae *are* the discontinuities -- so it clears the global and
restores it afterwards. On two error paths it returns before the restore:

```
baseline:                      JpmcdsBSearchLong(unordered) -> FAILURE (validated)
one failed call ...            -> FAILURE
after that single failed call: JpmcdsBSearchLong(unordered) -> SUCCESS
current global state:          FALSE (leaked)
```

One failed call disables sorted-input validation for every binary search in
the process, for the rest of its life.

What makes this one satisfying: **the port was already immune, and for the
right reason.** `bsearch.hpp` was written in the very first slice of this
project with `bracket` and `bracket_checked` as two functions, and its comment
read *"process-wide state that silently changed the behaviour of every
caller"*. That was a taste judgement about a design smell. Eighteen slices
later it turns out to be literally true -- not through explicit toggling, but
through a leak.

The clearest statement of the problem is operational rather than rhetorical:
**this module's differential suite has to restore the global in `TearDown`, or
one test corrupts every test after it.** A test suite that must clean up after
the code under test is telling you something.

### UPSTREAM-14, and what a full cross-product buys

```
DaysDiff(-50, 100, 30/360) -> FAILURE  days = -123456789  <- NEVER WRITTEN
metric(-50, 100)           -> 6101050560                  <- uninitialised read
interpolate at -50         -> -667794.1351                (curve is 0.01-0.02)
```

`bondDaysDiff` discards the status of `JpmcdsDaysDiff` over an uninitialised
local. The call fails for a TDate before the 1601 epoch without writing its
out parameter, so the metric returns whatever was on the stack -- and the
interpolation reports SUCCESS.

Two things about this are worth more than the defect.

**The value is not stable.** Two binaries built from the same sources gave
`metric(-50, 100)` as 6101050560 and as 4, interpolating to -667794.1351 and
-619018.2639. My first write-up quoted one figure as though it were the
answer; it is one observation of a stack slot. Correcting that is more honest
than pedantic: a reader who runs the reproducer and sees a different number
should conclude the defect is worse than described, not that the description
is wrong.

**AddressSanitizer is blind to it.** An uninitialised read needs
MemorySanitizer, which this project does not run. The sanitizer job is green.
That is a real gap in the harness and it is now written down.

**The differential suite did not find it; the golden generator did.** My
differential sweeps paired each metric with realistic dates -- bond metrics
got real TDates, the plain metric got a wide numeric range. The generator
swept the full cross-product of abscissae against metrics, hit `-50` with
30/360, and the replay failed by 438406. Targeted sweeps encode an assumption
about which combinations matter. A cross-product does not.

And I had explicitly dismissed this in the header comment I wrote an hour
earlier: *"It cannot fail: the convention is a compile-time constant and both
are calculable."* Wrong, and disproved by my own vectors.

### A claim that was almost right

`interpolation.hpp`'s first draft collapsed upstream's `mfunc == NULL` default
into the actual-days metric, with what looked like a solid argument: the null
branch computes `(desired - x_lo)/(x_hi - x_lo)`, the metric branch computes
`metric(desired, x_lo)/metric(x_hi, x_lo)`, `actualDaysDiff(a,b)` is `b - a`,
so numerator and denominator are both negated, negation is exact, quotient
unchanged.

True -- unless the denominator is zero, because `-0.0` is not `0.0`. A
zero-width bracket, reachable when a repeated abscissa sits at the end of a
discontinuous curve, gives `+inf` down one path and `-inf` down the other:

```
desired    mfunc=NULL     mfunc=ACT/365F agree?
400        inf            -inf           *** NO ***
```

Neither is wrong -- the fraction is undefined there -- so this is a quirk, not
a numbered defect. But they are not interchangeable, and both forms are used
in the library, so `Metric::Direct` and `Metric::ActualDays` stay separate.
The differential suite caught the collapse within minutes of my writing it.

### Smaller things

The generator needed a fix no previous one did: `%.17g` renders an infinity as
`inf`, which is not a C++ literal. This is the first module whose results can
be infinite, because a zero-width bracket divides by zero. It now emits
`std::numeric_limits<double>::infinity()`.

And the affected UPSTREAM-14 combinations are **omitted** from the recorded
vectors rather than recorded with a flag. Recording the output of undefined
behaviour would enshrine it -- the same rule that kept UPSTREAM-4's buffer
overflow out of the vectors.

## Decision 19 — The type was already right; the validation was not

`tcurve.c` looked like the easiest 528 lines left. `TCurve` is
`{base date, array of (date, rate), basis, day count}` -- field for field the
port's `ZeroCurve`. So the slice began with a question rather than a port:
**is any of this a second type, or just a second spelling?**

Just a spelling. `JpmcdsNewTCurve`, `JpmcdsFreeTCurve` and `JpmcdsCopyCurve`
are allocation, deallocation and copying, which a value type does for free.
No new header was written. What the slice turned into was the far more
interesting question of what `checkTCurve` validates that `ZeroCurve::create`
did not.

### PORT-11, and the third time the same blind spot has hidden a defect

`checkTCurve` calls `JpmcdsRateValid` on every point, "to guarantee we can
generate positive discount factors". `ZeroCurve::create` had **no rate check
at all**:

| basis | rate | upstream | port | port `zero_price` |
|---|---|---|---|---|
| 1 | -0.5 | accepted | accepted | 32.12 |
| 1 | -1 | REJECTED | accepted | **inf** |
| 1 | -1.5 | REJECTED | accepted | **nan** |
| 2 | -2 | REJECTED | accepted | **inf** |

Upstream's rule is exactly `rate > -basis` for a compounding frequency, and
anything for continuous compounding. One line, now applied per point, and the
accept/reject behaviour matches across a 66-case sweep.

The way it was found is the part worth recording, because it is **the third
time**: every curve differential test built a *valid* curve on both sides and
compared prices, so the constructor's rejection behaviour was never in the
comparison. PORT-9 was the protection-leg factory. PORT-11 is the curve
factory. The lesson has been written down twice and it keeps costing me
anyway, which suggests writing it down is not the fix -- the fix is a habit:
**when porting a factory, sweep the inputs it must refuse before sweeping the
ones it accepts.**

### UPSTREAM-15, which justifies the port being stricter

Having found what the port was missing, the reverse question: `create` is
*stricter* than `checkTCurve` in two ways -- it requires a non-empty curve,
and it allows only the day counts the rate conversion supports. Were those
divergences improvements or regressions?

Measured:

```
ACT/365F   construct=accepted  JpmcdsZeroPrice=0.7833167239
30/360     construct=accepted  JpmcdsZeroPrice=nan
30E/360    construct=accepted  JpmcdsZeroPrice=nan
(empty)    construct=accepted  JpmcdsZeroPrice=nan
```

Improvements. `checkTCurve` promises to guarantee positive discount factors
and then admits curves that cannot produce any. A caller picks 30/360 -- a
standard convention, accepted by `JpmcdsDayCountValid` because it *is* valid
-- and every discount factor afterwards is NaN, because `JpmcdsZeroPrice`
returns a bare `double` and has nowhere to report a failure.

That is UPSTREAM-2's problem in a new disguise: no error channel, so the error
becomes a number. And it is why the port can return a plain `double` from
`zero_price` with no NaN sentinel -- every curve that exists is priceable.

### A distinction I nearly fumbled

The tier-1 invariant was first written as "every constructible curve prices to
a FINITE non-negative number". It failed immediately: a continuous-basis rate
of -400 over three years is `exp(1200)`, which overflows to `+inf`.

Checking upstream before adjusting the test: it accepts the same curve and
returns the same infinity. So this is not a divergence and not a defect --

**infinity is a value; NaN is an error channel.** `exp(1200)` genuinely is
larger than a double can hold, and saying so with `+inf` is correct. Saying
"I could not compute this" with NaN, from a function whose signature has no
other way to say it, is not.

The invariant is now "never NaN, non-negative, and finite for any rate a
market could quote", over 810 discount factors. Weaker, true, and it makes the
point the stronger version would have obscured.

### One behaviour deliberately dropped

`JpmcdsMakeTCurveNoRateCheck` treats a `TDate` of 0 as an array terminator:
*"This allows spreadsheet users to select a big range of dates, but use less
than the whole range by terminating with a 1 or 0."* Five dates with a zero at
index 2 produce a curve of two points.

Not reproduced. A `std::span` carries its length, so a terminator has nothing
to mean -- and TDate 0 is a legitimate date, 1601-01-01, which a span-based
caller could reasonably include. The same input is now refused as out of order
rather than silently truncated, which is the better failure.

## Decision 20 — A split proposed, checked, and withdrawn

I proposed a three-way split of `zcall.c` and then, before writing any code,
checked it. It did not hold, and finding that out cost twenty minutes instead
of a wasted slice.

**What was wrong.** I had based the split entirely on the `cashflow.c`
dependency and never looked at the others. Two things break it:

- the "builder" half calls into the "add points" half -- `ZCFromTCurve` calls
  `ZCAddRate`, `ZCAddPrefixCurve` calls `ZCAddGenRate` -- so they cannot be
  separated without stubbing;
- both halves need `cfinanci.cpp`, which was not ported. `ZCInterpolate` and
  `ZCComputeDiscount` go through `JpmcdsDiscountToRate` and
  `JpmcdsRateToDiscount`.

I also miscounted the file: 24 functions, not 23. `JpmcdsZCDiscountFactor` (47
lines) has its return type and name on separate lines and my regex skipped it.
Two smaller things fell out of the recount: `JpmcdsBasisValid` is a macro
local to `zcall.c` reading `((x)>=1 && (x)<=5001 ? SUCCESS : FAILURE)` -- note
the bound, 5001, where the continuous sentinel is 5000 -- and the public
function `JpmcdsJpmcdsZCPresentValueCFL` has `Jpmcds` in its name twice,
permanently, because it is an exported symbol.

So the corrected plan put `cfinanci.cpp` first, which is this slice.

### What a "basis" actually is

`cfinanci.cpp` is where the concept lives, and the concept is one `double`
carrying five meanings:

```
    0     simple interest
    1-365 compounding periods per year
   -2     the "rate" IS a discount factor
  512     a discount rate rather than an interest rate
 5000     continuous compounding
```

Five branches, one field, no type. The port keeps the `double` -- it is a
published contract -- but names the sentinels, and `kContinuousBasis` moved
here from `zero_curve.hpp`, where it had been living because that header was
written first. I had annotated that misplacement in a comment rather than
fixing it, which is the wrong instinct: a comment explaining why something is
in the wrong file is a note to fix it later.

### The mistake worth recording: assuming symmetry

I wrote `discount_to_rate` as the mirror of `rate_to_discount`, having read
only the latter. Then I read it. Three behaviours have no counterpart in the
forward direction:

- a zero-length period is **rejected outright** for every basis except the
  discount-factor sentinel, before the year fraction is even computed. The
  forward direction has no such check and asks for the year fraction of
  nothing quite happily -- getting 1.0, which is right: a rate over no time
  discounts by nothing.
- for the discount-factor basis over a zero-length period, a factor that is
  not within `DBL_EPSILON` of 1.0 is an **error** -- over no time nothing can
  be discounted, so any other value is contradictory.
- and in that case the rate returned is **1.0**, not the factor. So the
  discount-factor basis does not simply pass through in both directions.

None of that is deducible from the forward function. Symmetry was an
assumption dressed as a reading, and the only reason it got caught is that I
went back to the source before writing the tests rather than after.

### A signature inconsistency the test found

The year-fraction forms take `double basis`; the **date-based** forms take
`long basis`. So a `TCurve`, whose `fBasis` is a `double`, truncates its basis
whenever it passes through the date-based conversion -- and the formulas use
the basis directly, so 1.5 arriving as 1 is a different number out.

That is the same truncation `ZCurve` performs by storing `basis` as a `long`,
in a second and independent place. My first port widened both to `double`,
which made the port disagree with upstream for every fractional basis. The
differential sweep caught it at basis 1.5 and the signature now matches
upstream, with the inconsistency documented rather than smoothed. No real
convention uses a fractional compounding basis; `checkTCurve` admits them only
because its test is `>= -2.0`.

There is a related curiosity worth knowing: the `switch` dispatches on
`(long)basis` but the formulas use the full `double`. So 1.5 dispatches to the
compounding branch *as if it were 1* and then computes `pow(1 + r/1.5,
-1.5t)`. Consistent in both directions, and pinned by a test.

### Verification

The contract is the round trip, and it needs no oracle: wherever both
directions succeed, the rate must come back. 100+ combinations at relative
1e-12 -- not bit-exact, because the compounding branch is two transcendental
calls deep. Plus 1,183 recorded conversions and 520 recorded validity
verdicts, and the differential sweep over all thirteen bases including the
fractional and negative ones.

## The collaboration record

Started at the schedule slice, which means the earlier rows were reconstructed
from the commit history and the PORT register rather than recorded live. Kept
from here because the interesting question about working this way is not "did
it work" but **what catches the mistakes**, and that is only answerable with
the data.

Nine defects introduced by the port itself, by what found each one:

| caught by | count | which |
|---|---|---|
| a sanitizer, run deliberately | 3 | PORT-1, PORT-2, PORT-3 |
| the differential suite against the oracle | 1 | PORT-10 |
| comparing a FACTORY rather than its output | 2 | PORT-9, PORT-11 |
| the 3-OS CI matrix | 2 | PORT-6, PORT-8 |
| a test written to probe extremes | 2 | PORT-4, PORT-7 |
| noticing a *passing* test print a warning | 1 | PORT-5 |

**Nothing was caught by re-reading the code.** Every one of the eleven was found
by running something — which is the argument for building the harness before
writing the port, and an argument against trusting a careful review of
generated code.

The distribution also says where the money is. Two defects needed three
operating systems to appear, and one of those (PORT-8) rejected *ordinary*
backward date offsets on Linux while macOS and Windows both passed. One needed
a composition layer that did not exist yet. None of those four would have been
found by any amount of looking at the file they were in.

### Claims made and withdrawn

The other half of the record, and the half that would be easy to leave out.
Five times a conclusion was stated and then corrected on measurement:

| claim | what measurement showed |
|---|---|
| a use-after-free in `JpmcdsSubtractDateLists` | ASan found nothing; the logic is deterministic. A mechanism was named before being checked |
| UPSTREAM-7 "never examines index 0" | the real scope is any truncation leaving fewer than two dates — 234 of 600 cases, not an edge |
| UPSTREAM-9 "crashes under ASan" | it faults in one frame and not three others; the reproducer was deleted for not reproducing |
| the par-spread zero-annuity guard "fixes an unchecked division" | no reachable input produces a zero annuity; recorded as defensive, not counted as a defect |
| the root finder's `TwoRoots` case "AMPLIFIED" | a mis-designed objective with two roots, not amplification. Nearly became a fourth tolerance tier |
| the root finder "does not need an iterative tolerance tier", concluded from a one-ULP probe | the probe was too weak to model FMA contraction. CI found 146 of 10,296 cases taking a different iteration count on Linux and Windows, pure polynomials included. The ROOT claim survived; the claim that the iteration path is portable did not |

Three of those five were overstatements in the direction of a more impressive
finding. That is the bias worth knowing about, and the reason every severity
claim in `defect-report.md` now carries the measurement that bounds it.

### What the numbers do not say

Rework per slice is not recorded, and reconstructing it from the commit
history would be guesswork, because the history shows only what was committed.
From here: one line per slice naming what had to be redone and why.

- **schedule** — one test assertion wrong about the code (`HugePeriodDoesNotOverflow`
  asserted a rejection that does not happen); one deleted file (`u9_reproducer.c`);
  one write-up rewritten after the crash claim failed to hold.
- **cds** — one real defect introduced and fixed (PORT-9); one comment corrected
  that had argued the defect was not a defect; two wrong argument orders caught
  by the compiler.
- **cfinanci** — proposed a split of `zcall.c`, checked it before writing, and
  withdrew it (two dependency errors and a miscount). Then wrote
  `discount_to_rate` as a mirror of its inverse by assumption and had to
  correct three behaviours after reading the source; widened a `long` basis to
  `double` and had the differential sweep catch it; shadowed a function with a
  local named `year_fraction`.
- **tcurve** — no new header: `TCurve` is field-for-field the port's
  `ZeroCurve`, so the slice became a validation audit instead. Found PORT-11
  (missing rate check) and UPSTREAM-15. One tier-1 invariant written too
  strong — demanded finiteness where `exp(1200)` legitimately overflows — and
  corrected after checking upstream does the same; one count floor guessed at
  2000 where the measurement is 810.
- **interpolation** — zero port defects; one header claim collapsed two
  non-equivalent code paths and was corrected within minutes; one arithmetic
  slip in a test assertion (hand-computed 0.0085 where the answer is -0.005,
  now derived in code); one dismissal of UPSTREAM-14 as unreachable, disproved
  by my own generated vectors an hour later.
- **date_sup** — zero port defects; **three test failures where the test was
  wrong**, comparing a normalised interval against a hand-built upstream
  struct. Also deflated my own UPSTREAM-12 write-up once reachability was
  measured.
- **defaulted** — one defect introduced and caught by the differential suite
  within seconds (PORT-10: wrote `fraction * coupon * notional` where upstream
  computes `fraction * (coupon * notional)`, one ULP apart on 30/360). Found
  UPSTREAM-11, the worst defect in the project, from an edge-case probe written
  before porting rather than from reading the code.
- **cdsone** — one contract asserted wrongly (the spread round trip, which is
  ill-posed above ~5000bps where the charge saturates and the derivative turns
  negative); replaced with the charge round trip the solver actually
  guarantees. The port itself needed no correction: six differential tests
  green on the first run, ISDA's three published numbers reproduced.
- **bootstrap** — three test premises wrong and corrected against measurement
  (every ladder bootstraps: false, about a tenth are infeasible; negative
  recovery is rejected: false, it is accepted; two case-count floors set above
  the measured counts). The port itself needed no correction: bit-exact and
  contract-satisfying on the first run.
- **root_find** — one mis-designed test objective, corrected after it produced a
  result that would have changed a project-wide standard; one name collision
  with a private member of `testing::Test`; **one conclusion published to a PR
  and then withdrawn when CI contradicted it**, taking the journal entry,
  the test name and two assertions with it; **a second CI round** then found a
  third platform-dependent property (the convergence verdict) that the first
  correction had missed.

## Metrics, with the caveat that matters

As of the end of layer 4 and part of layer 5:

| | |
|---|---|
| upstream lines retired | 5,768 of 21,775 (26%) |
| **pricing-path lines ported** | **1,415 of 8,871 (16%)** — curve, timeline, both legs |
| port size | 1,755 lines (≈2.5:1 compression) |
| tests + generators | 5,407 lines (3:1 over the port) |
| recorded reference cases | 18,142 |
| tests | 242 with an oracle, 161 without |
| defects | 8 upstream, 8 port-side |

**The 22% is the least meaningful number here.** Most of what is done is
infrastructure — dates, day counts, calendars, lists — where answers are
crisp integers. Nothing prices a CDS yet.

The floating-point question that worried this section is now answered for
both the curve (Decision 7) and the accumulating integral (Decision 9):
bit-exact in both. **The port prices a CDS protection leg identically to the
original**, which is the result the vertical slice existed to find.

**Lines retired measures effort. It does not measure capability.**

---

## Chronology

| PR | date | what |
|---|---|---|
| — | 1 Nov | scaffold: CMake, presets, GoogleTest, two-tier harness, `verify.py` gates |
| — | 1 Nov | `bsearch.c` — preprocessor-as-templates collapsed into one constrained template |
| — | 1 Nov | `dateconv.c` — `TDate` epoch pinned at 1601-01-01; 1,500-entry lookup table deleted |
| #1 | 1 Nov | day count conventions → `enum class DayCount` |
| #2 | 1 Nov | `yearfrac.c` — convention string parsing, leniency preserved |
| #3 | 2 Nov | date intervals; UPSTREAM-3 (lowercase `'y'`), UPSTREAM-4 (stack overflow), UPSTREAM-5 |
| #4 | 2 Nov | holiday calendars — **the process-wide `cache` global is gone** |
| #5 | 2 Nov | PORT-1: interval digit overflow |
| #6 | 2 Nov | adversarial harness; PORT-2/3/4 and **PORT-5, the toothless UBSan gate** |
| #7 | 2 Nov | bad day conventions; UPSTREAM-6 (unbounded loop) |
| #8 | 3 Nov | business day counting — transcribed, tables and all |
| #9 | 3 Nov | business day offsets; layer 4 complete; PORT-7, PORT-8 |
| #10 | 3 Nov | UPSTREAM-7 recorded; the divergence question raised rather than defaulted |
| #11 | 3 Nov | date lists, correcting UPSTREAM-7; the divergence **policy** written down |
| #12 | 3 Nov | this journal |
| #13 | 3 Nov | **the vertical slice begins** — zero curve, and floating point proves bit-exact |
| #14 | 3 Nov | integration timeline; UPSTREAM-7's price impact measured as none |
| #15 | 3 Nov | UPSTREAM-8 recorded, decision deliberately deferred |
| #16 | 3 Nov | **protection leg PV — the port prices a CDS, bit-identically**; UPSTREAM-8 resolved |
| #17 | 4 Nov | premium leg — **both legs now price bit-identically**; schedule becomes one vector |

Every PR green on Linux, macOS and Windows plus a sanitizer job, with a
machine-checked floor on tests actually run.

---

## If you are doing this yourself

The short version, in the order it matters:

1. **Build the original first.** Nothing else is verifiable until it runs.
2. **Link old and new into one binary.** Do not diff files.
3. **Never edit the thing you verify against**, and enforce that with a hash
   manifest rather than willpower.
4. **Check your gates can fail.** A sanitizer that only prints is not a gate.
5. **Audit the port separately from comparing it.** Agreement on undefined
   behaviour is not agreement.
6. **Decide reimplement-vs-transcribe explicitly, per function.** Write down
   which, and why.
7. **Enumerate every divergence and measure its consequence.** "It shouldn't
   matter" is not a measurement.
8. **Run the full OS matrix from the first commit.** Platform-dependent
   integer widths are not an exotic problem.
9. **Count capability, not lines.**

## Decision 21 — Two curves disagree about one number, and the fast one wins

The builder half of `zcall.c` is 13 functions that exist to assemble a zero
curve one point at a time. The interesting thing about it is not what it
computes; it is that it stores a cached discount factor beside every rate, and
so holds the same information twice. Both defects in this slice come from that.

### UPSTREAM-17, and the shape of the mistake

`JpmcdsZCComputeDiscount` has a fast path, flagged by a comment as *"a special
optimization"*:

```c
if (zc->basis == 1 && rate >= -1.0 && date >= zc->valueDate && ...)
{
    *discount = pow(1+rate, (zc->valueDate - date) / 365.0);
    return SUCCESS;
}
```

At `rate == -1.0` the base is zero and the exponent negative, so `pow` returns
an infinity. The general path it stands in for **rejects** that input:
`JpmcdsRateToDiscountYearFrac` refuses a base at or below zero. One guard
wants `>`, the other has `>=`, and nobody noticed because the fast path is
tested first and the two were never compared.

This is a recognisable shape, and I want to name it rather than just fix it:
**two implementations of one quantity, where one is a performance shortcut for
the other, and their domains of validity were never checked against each
other.** The optimisation is almost right. It is right everywhere the author
had in mind. The disagreement lives exactly at the edge the author did not
have in mind, which is where the general path's own refusal would have been the
correct answer.

The infinity then gets *stored*, and `JpmcdsZCDiscountFactor` serves it. That
is the part that makes it worth a defect record rather than a footnote: a
number that the function responsible for computing it would have refused now
lives in a data structure, with no trace of where it came from.

### Not every infinity is a defect

I nearly filed this as "upstream returns infinity" and that would have been
wrong, because I filed the opposite judgment eight decisions ago. UPSTREAM-15
also involves an infinity — `exp(1200)` from a continuous-basis rate of −400 —
and there I concluded explicitly that the infinity is *not* part of the
finding, because the answer really is enormous and the port returns the same
thing.

What separates them is not the value. It is whether anything in the system is
on record calling that input an error. For `exp(1200)`, nothing is: every path
accepts the rate and overflows honestly. For `pow(0, negative)`, the general
path refuses the same input in the same source file. The disagreement is the
defect; the infinity is just how it shows up.

That distinction is only available because UPSTREAM-15 had already forced me to
write down *why* an infinity was acceptable. A judgment recorded with its
reasoning can be reused; a judgment recorded as a verdict cannot.

### UPSTREAM-16, and the cost of a missing `return`

`JpmcdsZCAddGenRate` short-circuits when the rate already matches the curve's
terms, and then falls through into the general route, so the matching case adds
the point twice. The duplicate check swallows it — same date, rates within a
hardcoded `1e-7`, return success having done nothing — so for years this has
been pure wasted work.

Except at `date == valueDate`, where the second route fails, because
`JpmcdsDiscountToRate` will not invert a zero-length period. So the function
returns FAILURE with the point already committed.

The fix is the missing `return`. The interesting part is the second-order
effect: with the `return` in place, a rate at the value date now *succeeds* in
the port, because a rate over no time discounts by nothing and that is a
perfectly good point. Upstream's failure was never about the input being bad.
It was the redundant route failing on an input the first route handled fine.
**A missing `return` did not merely waste work; it narrowed the function's
accepted domain, by letting a path that should never have run do the rejecting.**

### What the port asserts instead of what it fixes

Both fixes are one line each, and one-line fixes are the ones that silently
come undone. So neither is pinned as "the line is there". They are pinned as
properties:

- **Nothing partially committed.** Every route into the builder commits only
  after all of its conversions have succeeded, so no failure *anywhere* can
  leave a point behind. That covers UPSTREAM-16 and also the three other
  failure modes I did not find.
- **The two discount paths agree.** For every rate inside the boundary, the
  fast path and the general one return bit-identical factors; at the boundary
  both refuse. That covers UPSTREAM-17 in a form that would fail if someone
  "optimised" the fast path again.
- **The cache never drifts.** For every stored point, the cached discount
  equals `compute_discount(date, rate)` recomputed. This is the invariant the
  type exists to maintain, and the only thing it adds over a finished curve.

The third one is the only property in this slice that upstream cannot be used
to check, because upstream's cache *can* hold a value its own computation would
refuse — that is UPSTREAM-17. A differential test against a buggy oracle
confirms the bug. The invariant is what distinguishes "matches upstream" from
"is correct", and the two came apart here for the first time in the port.

### The one that is not reproduced, deliberately

The golden generator skips any sequence that reaches UPSTREAM-16's failure,
and this is worth stating plainly because it is a hole in tier 1. Upstream
leaves the curve mutated and reports failure; there is no result to record,
because "what the curve contains after a failed call" is not an answer, it is
damage. So the recorded vectors cover the agreeing cases and the differential
suite covers the divergence. CI therefore verifies that the port builds curves
correctly; it does **not** verify that the port diverges from upstream where it
should. Only the local differential run does that, which is the standing
asymmetry of this project and not new here.

## Decision 22 — Four ways to return a wrong answer with a right status

The evaluation half of `zcall.c` is eight functions that read a finished curve.
It yielded two defects, and what makes them worth writing up together is that
they are the same defect at two different levels of the stack.

### The error channel exists and is not used

```c
*discount = 1.0;
if (discount1<=0.0)
{
    JpmcdsErrMsg("JpmcdsZCFwdDiscountFactor: bad discount at start of fwd\n");
}
else
{
    *discount = discount2 / discount1;
}
return SUCCESS;
```

Every previous finding of this shape in this project had an excuse available.
UPSTREAM-2 and UPSTREAM-15 return a bare `double` and have nowhere to report a
failure; the error *becomes* the number because there is no alternative. Here
there is: the function returns `int`, the caller checks it, and the condition is
detected well enough to write a sentence about. The status is simply wrong.

And the fabrication is the maximally plausible one. A forward discount factor
of 1.0 means "no discounting over this period", so the caller is told the
forward rate is exactly **0%** — not a sentinel, not an absurdity, a number a
risk report would print without comment.

### The failure mode gets prettier as the situation gets worse

Sweeping a continuous-basis rate over a 30-year horizon:

```
rate=20   df1=1.70976e-261   fwd rate=4.85165e+08
rate=23   df1=1.31182e-300   fwd rate=9.7448e+09
rate=24   df1=1.20094e-313   fwd rate=2.43073e+10    <- subnormal
rate=25   df1=0              fwd rate=0              <- fabricated
```

This is the part I did not expect. Up to the cliff the answers are visibly
nonsense: 2.4e10 is a forward rate of two billion percent, and any monitor, any
reviewer, any plausibility check catches it. One rate further on, the output
becomes a clean zero. **The observable symptom improves monotonically as the
input deteriorates**, and the single worst-behaved input in the sweep is the one
that produces the most innocuous-looking number.

That inverts the usual assumption behind sanity checks — that worse inputs
produce more obviously wrong outputs, so a bounds check on the output catches
the bad inputs. Here the bounds check catches everything *except* the actual
failure.

Severity also depends on the basis, and measuring that mattered. Annual
compounding never reaches the cliff: `(1+r)^-t` decays polynomially, so even a
rate of 10,000% over 30 years gives `6.7e-61`. Continuous compounding decays
exponentially and crosses at about 24. So this is reachable only on a
continuous-basis curve — which I would not have known without sweeping both.

### `x <= 0.0` is not "x is unusable"

The second defect is one character of intent away from the first:

```c
if (discount<=0.0)                  /* bad discount factor */
{
    JpmcdsErrMsg("JpmcdsZCForwardRate: bad discount for forward\n");
    return FAILURE;
}
```

Here the status is used correctly and the *predicate* is wrong. `NaN <= 0.0` is
false, so the one check whose entire job is to stop an unusable discount factor
is written as the one comparison every NaN passes. `JpmcdsDiscountToRate` guards
the same way, so there is no second line of defence.

The comment says what the author meant — "bad discount factor". The code says
"non-positive discount factor". Those are the same set only if NaN is not a
double.

### Where the NaN comes from is the interesting part

It is not produced by a bug. Two discount factors overflow to `+inf`, which
UPSTREAM-15 established — in this journal, deliberately — is a *value* and not a
finding, because the answer really is that large. Their ratio is indeterminate.

So the chain is: a correct value, combined with another correct value, produces
something neither function's guard can represent. This is exactly the
relationship UPSTREAM-9 has with UPSTREAM-10, where one supplies the bad pointer
and the other performs the unbounded write. Neither is the whole defect;
together they are.

It also means I could not have found this by looking for wrong arithmetic.
Every individual computation along the path is correct.

### Fixing it at the conversion, not at the caller

The port's answer is one line in `discount_to_rate`:

```cpp
if (!(discount > 0.0) || !std::isfinite(discount)) {
  return std::nullopt;
}
```

Placed there rather than in the forward-rate function, because a NaN discount
factor is not a rate no matter who asks. Guarding each caller means the next
caller has to remember; guarding the conversion means no caller can get it
wrong. That is the "make bugs unrepresentable" move applied to an error check
rather than to a data type.

The headline property is the general form, and it is oracle-free: across every
basis, day count and rate in the sweep, **no evaluation returns NaN** —
while infinity remains permitted, because UPSTREAM-15 settled that it is a
value. Writing the property that way round, allowing one non-finite result and
forbidding the other, is only possible because the earlier decision recorded
*why* rather than *what*.

### A dead local that documents a lost intention

`JpmcdsZCInterpolate` computes the day-count metric function and never reads it:

```c
TMetricDoubleFunc metricFP;
...
metricFP = JpmcdsDayCountToMetricFunc(zc->dayCountConv);   /* never used */
```

Both interpolation branches measure the gap between dates in raw days, so the
day count has no effect on a linear interpolation at all. Meanwhile
`JpmcdsLinInterpCurve` in `linterpc.c` performs the same linear interpolation on
the same kind of curve and *does* pass the metric through, so for a 30/360 curve
the two disagree.

I did **not** file this as a defect, and the reason is worth recording as much
as the finding would have been. Nothing in the library says those two functions
are meant to agree; they have different signatures, different curve types and
different callers. A raw-day interpolation of zero rates is a defensible
convention. The dead assignment is evidence of an intention, not of a contract,
and I have been wrong before by promoting the first into the second — the port's
own PORT-9 came from assuming a line's absence was deliberate.

So it is recorded as an observation at the site, with the note that the next
reader will find it just as suspicious. That is the honest resolution: the
reader is warned, and nothing is claimed that was not measured.

### One assertion I had to weaken, again

The first draft of the no-NaN property also required every forward discount
factor to be strictly positive. It failed at basis 365, rate 25 — the factor at
the far date underflows to zero while the near one is still subnormal, so the
ratio is exactly `0`. A forward over which everything becomes worthless.

I checked upstream before touching the assertion: it returns the same zero and
then refuses to convert it to a rate, which is precisely what the port does. So
the invariant was wrong, not the code, and the fix was to assert `>= 0.0` plus
the separate claim that a zero factor has no rate.

This is the third time in this port that an invariant demanding finiteness or
positivity has turned out to be stronger than the arithmetic justifies. The
pattern is clear enough to state as a rule: **before asserting that a quantity
is well-behaved, check what the floating-point edge of its domain actually
produces** — and prefer an assertion that names the two outcomes separately to
one that bans the harder case outright.

### Addendum — CI rejected the slice, and it was the tolerance tier

The first push of this slice went red on two of four platforms, and the
diagnosis is worth keeping because it is the second time this project has been
caught by the same physics.

The replay of recorded evaluations failed on Ubuntu **asan** and Windows while
passing on Ubuntu **release** and macOS. Ubuntu release and Ubuntu asan are the
same machine with the same libm; the only difference is the compiler flags. So
this cannot be a library-function discrepancy — it is `-ffp-contract` turning
`a + b*c` into an FMA and changing the value *mid-expression*, so the
interpolation feeding `pow` is already a different double before any library
call happens.

Worst observed drift: `1.6686705540502349` against a recorded
`1.6686705540502449`, a relative difference of 6e-15, roughly 27 ULPs.

I had written the replay with the 4-ULP tier. That tier belongs to the
*differential* suite, where both implementations are compiled into one binary
with one set of flags and contraction applies identically to both. A recorded
vector has crossed a compiler and a machine, so the right tier is the
accumulating one — relative 1e-12 — and the measured 6.8e-15 sits comfortably
inside it.

Two separate mistakes, and only one of them is about the number:

1. **Wrong tier.** The forward rate is the end of a chain: interpolate, `pow`,
   ratio, reciprocal, `pow`. Calling that non-accumulating was wishful.
2. **Wrong *shape* of tolerance.** `4.0 * max(1.0, |want|) * eps` is an
   absolute tolerance anchored at 1.0. On a discount factor of 0.02 it permits
   about 200 ULPs; on a rate of 6.5 it permits about 7. It was loose exactly
   where it claimed to be tight, and tight exactly where the arithmetic could
   not deliver. The discount-factor comparison in the same test passed only
   because of that looseness — it was not evidence of anything.

The second is the one I would not have found without the first failing. The
tolerance was wrong in both directions and the test was green.

That pattern appears in seven other tier-1 files in this project, all currently
passing on all four platforms. I have not changed them in this slice: they are
passing with real margin and a project-wide tolerance audit is its own change,
not a rider on a port. But it is now written down rather than noticed and
forgotten, which is the only reason this paragraph exists.

## Decision 23 — The hypothesis that cost four probes and was worth it

`cashflow.c` is 514 lines in six functions, and the first job was finding where
it splits. Four of the six are container operations with no dependencies
outside the file; the other two build a list from a payment schedule and need
`JpmcdsNewDateListExtendedRoll`, `JpmcdsNewDateListBusDayAdj`,
`JpmcdsBusinessDay` and `JpmcdsStubPayment` — none of them ported.

So the slice is the container. And the reason to do it now rather than wait for
`datelist.c` is that the remaining third of `zcall.c` only ever *consumes* a
list: checked rather than assumed, by listing every `Jpmcds*` symbol those
three functions reference. None of them is a constructor.

I also miscounted the file at first — a regex for function definitions found
three, because `JpmcdsMakeCFL` and `JpmcdsNewCFLFromDL3` put their return type
on its own line. That is the second time the same regex has undercounted the
same way (it missed `JpmcdsZCDiscountFactor` in `zcall.c`). Worth noting that
the fix is not a better regex: it is to stop using a count as evidence of
having read a file.

### The hypothesis

`JpmcdsNewEmptyCFL(n)` sets `fNumItems = n` and allocates through `NEW_ARRAY`,
which is `JpmcdsMallocSafe`, whose allocation line is:

```c
ptr = malloc(theSize);              /* Don't use calloc due to RS6000. */
```

A public function named `NewEmpty` that reports *n* cash flows over
uninitialised memory, with a comment explaining that the zeroing primitive was
deliberately avoided. That looked like a defect, and a good one.

### It is not, and finding that out took four attempts

- **A pre-dirtied heap.** Allocate 64 doubles, fill with a recognisable
  pattern, free, then call `NewEmptyCFL(6)`. Came back zeroed.
- **A size-matched block.** `sizeof(TCashFlow)` is 16, so `NewEmptyCFL(6)` asks
  for exactly 96 bytes; hand the allocator back a 96-byte block pre-filled with
  `0x7E7E7E7E`, which is the block most allocators reuse first. Came back
  zeroed.
- **`MallocPreScribble=1`**, macOS's own diagnostic for exactly this, which
  fills new allocations with `0xAA`. Came back zeroed.
- **An ASan build**, whose allocator pre-fills with `0xBE`. Came back zeroed.

Four independent attempts to produce garbage, all producing zeros. At that
point the hypothesis is not merely unproven, it is being actively contradicted,
and the honest next step is to stop trying to prove it and go read the function
properly. Nine lines further on:

```c
/* Clear out the memory for safety. */
(void)memset((char *)ptr, (size_t)0, theSize);
```

The comment about calloc means *"we reproduce calloc by hand"*, not *"we skip
the zeroing"*. I had read fifteen lines of a twenty-four-line function and
filed the conclusion.

### Why the negative result is worth more than the finding would have been

Every `NEW` and `NEW_ARRAY` in the library goes through that function.
So **upstream cannot have an uninitialised-heap read anywhere** — not in the
11,000 lines still unported, not in the 400-line constructors this slice
deferred. That rules out a whole category of search.

It also explains something about the register that previously looked like luck.
UPSTREAM-14 is an uninitialised read, and it is a *stack* local, where no
allocator intervenes. The split is not arbitrary: heap memory in this library
is zeroed by construction, stack memory is not zeroed at all, and every
uninitialised-read defect must therefore be on the stack. That is a prediction,
not a summary, and it is cheap to check as the port continues.

Four probes to establish that something is *not* broken is a good trade. The
alternative was a defect record that a careful reader would have demolished by
reading nine more lines of the source than I did.

### One detail that does survive

`fDate = 0` is not a null in upstream's encoding. TDate 0 is **1601-01-01**, a
date like any other. So a list from `JpmcdsNewEmptyCFL(n)` that a caller
under-fills does not look uninitialised — it looks like *n* deliberate payments
of zero in 1601. Garbage has a chance of being caught downstream; a plausible
date and a zero amount do not.

The port has no equivalent, and the first draft of the header claimed it did:
*"`std::vector<CashFlow>(n)` value-initialises, so the port matches it."* That
is wrong, and a test caught it within a minute — a default-constructed `Date` is
1970-01-01, the `sys_days` epoch, not 1601-01-01. The port's `Date` has no
zero, which is the entire point of it. So the port has no "empty list of n
items" at all: length is the number of payments present, and capacity is
`reserve`.

Upstream conflates the two, and `ZCurve` in the same library does not — it
carries `numItems` and `numAlloc` separately. The library knows the
distinction; this type just does not use it.

### UPSTREAM-20, and a precondition that was never written down

What the slice did find is smaller and cleaner. `cashflow.h` documents
`JpmcdsMergeCFL`'s contract in one sentence:

> Merges two cash flow lists. The input cash flow lists cannot be NULL.

That precondition is real and it is checked, on both arguments. The other one —
that both inputs are ordered by date — is required by the algorithm, stated
nowhere, and checked nowhere. Unordered input produces neither a sorted list
nor a stable interleaving:

```
a = [40000, 20000, 30000]   b = [25000, 15000]
MergeCFL -> SUCCESS, fNumItems=5
    [0] 25000   [1] 15000   [2] 40000   [3] 20000   [4] 30000
```

Low severity, and the measurement that sets it is a grep: exactly one call site
exists in the library, `zcswap.c:804`, merging two schedules that are generated
in order. So it is unreachable from inside upstream and reachable only through
the public header — the same tier as UPSTREAM-12.

What makes it worth a record rather than a shrug is the gap between the
documented contract and the real one. A caller who reads the header carefully,
satisfies everything it asks for, and passes lists in whatever order they
arrived gets a wrong answer with a success status. The documentation is not
silent; it is *specific*, and specifically incomplete, which is worse than
saying nothing.

### The merge, and an exhaustive sweep instead of examples

A single forward pass over two sequences is a shape that hides off-by-ones at
the ends and in ties, so the differential test does not pick examples. It takes
every pair of subsets of a five-date pool — 1,024 pairs — which covers
disjoint, identical, overlapping, prefix, suffix, empty-on-either-side and
single-element without any of those cases having to be thought of first. Eleven
lines of test code instead of eleven hand-written cases, and it does not depend
on my imagining the right eleven.

Three properties carry the tier-1 side, and they are separable on purpose: the
output is ordered, the total is conserved *exactly* (these are sums of
quarter-integers, so a tolerance would hide a lost element), and no date is
lost or invented. What properties cannot pin is the element order upstream
chooses when dates tie, so that is what the recorded vectors are for.

## Decision 24 — The first module whose correctness is not a formula

The cash-flow third of `zcall.c` turns a curve into a bootstrap: an instrument
price goes in and a zero rate that reproduces it comes out, found by a root
search. Every module before this one could be pinned to a formula. This one has
to be pinned to a **fixed point** — price the instrument off the curve you built
from it, and the price must come back.

That property is the reason this slice is verifiable at all. It needs no
upstream and no recorded vector, it would survive the solver being replaced,
and it is the only check here that does not reduce to "the port agrees with the
thing it was copied from".

### Four hypotheses, two of them wrong

I went in with four suspicions from reading the source. Measuring them first
was the whole value of the slice, because half were wrong.

**The cache would go stale.** Upstream ends its root find with

```c
zc->rate[ objData.zcIndex ] = rate;
```

and no matching write to `zc->discount[zcIndex]`. The objective function
updates both on every evaluation, so the cached discount describes whichever
rate the solver last *tried* — which is the returned root only if the solver's
last act was to evaluate there. I expected the two to diverge.

They do not. Measured over the bootstrap's own instruments, every stored point
still satisfies the invariant afterwards. The write is unpaired and happens to
be safe because of where Brent stops.

So there was no defect to file, but there was still something to do: the port
does not expose the unpaired write at all. `set_rate` recomputes the discount or
fails, and no sequence of public calls can leave the two disagreeing. Upstream
is correct by coincidence of the solver's internals; the port is correct by
construction. That is worth the four lines even when the measurement comes back
clean — **"it holds today" and "it cannot break" are different properties, and
only the second one survives someone changing the solver.**

**The dropped points would break the bootstrap.** `JpmcdsZCAddCFLPoints` filters
its insertions through a bare `if (rate >= 0.0)` with no else, in a function
whose comment says it exists to ensure every cash flow date is in the curve. On
a negative curve it places 1 of 5 dates instead of 2, and the cliff is at
exactly zero — a solved rate of −2e-13 drops four points that +2e-13 would have
kept.

I expected the instrument to stop repricing to its own input. It does not.
Under linear interpolation *and* under flat forwards, with 1 of 5 dates placed,
the fixed point holds to the solver's tolerance.

The reason is worth stating because it is not obvious: inserting a point at its
own interpolated rate is idempotent under both schemes. Linear interpolation
puts the new point exactly on the line. Flat forwards is log-linear in the
discount factor, so it puts the point exactly on that curve. Splitting an
interval at a point that already lies on the path leaves the path alone.

That is also why upstream's own comment — *"non-linear interpolation might
actually change when points are added"* — is about the *order* of insertion
rather than about the result. It is a correct warning about a different thing.

So UPSTREAM-23 is low severity and the record says so, along with the fact that
I looked for a pricing consequence and failed to find one. A defect report that
only lists what went wrong with the search is worth less than one that says
where the search stopped.

### UPSTREAM-21, and an error handler that cannot report its error

This one is as bad as it looks.

```c
if (date==0L)
    date = cfl->fArray[ cfl->fNumItems-1].fDate;
...
done:
    if (status == FAILURE)
        JpmcdsErrMsg("...maturing %s", JpmcdsFormatDate(cfl->fArray[cfl->fNumItems-1].fDate));
```

`JpmcdsNewEmptyCFL(0)` is a public constructor returning `fNumItems = 0` with
`fArray = NULL`, so both reads are `NULL[-1]`. Measured: SIGSEGV, exit 139.

The second read is the interesting one. Pass an explicit `date` and the first
read never happens; the function then reaches its own branch —

> no cash flows in list beyond ZeroCurve. Therefore, nothing to add.

— detects the error correctly, composes the message, and crashes formatting it.
**The diagnostic path for an empty list is the one path that cannot survive an
empty list.** The error exists, is reachable, is written, and cannot be
received.

That is a different failure from UPSTREAM-18 (a status that lies) and from
UPSTREAM-2 (no error channel at all). Here the channel exists and works, and
the act of using it is what kills the process. Three distinct ways for an error
to fail to arrive, all in the same file.

### UPSTREAM-22, and four branches that all decline

When the final payment falls exactly on the curve's last date, every branch
declines in turn and `status = SUCCESS` with nothing changed: the guard admits
equality, the backward search walks off the end leaving
`firstUncovered == fNumItems`, and both remaining branches test false —
including the one that would have added the remaining dates.

```
rc=SUCCESS   numItems 3 -> 3   point at 2030 present? NO
```

No single line is wrong. Each guard is defensible on its own; the gap is
between them, and nothing in the function's structure makes "I did nothing" and
"I succeeded" mutually exclusive. That is the argument for returning a value
that has to be *constructed* rather than a status that defaults to success
halfway down.

The consequence compounds, which is why it is medium and not low: the next
instrument bootstraps against a curve one point shorter than the caller
believes. One silent no-op becomes a wrong curve rather than a missing point.

### An ODR violation I introduced, and what it cost

The tier-1 replay crashed with `std::length_error` from inside `find_closest`
— a function that had been passing for two slices. Thirty minutes later: my new
golden header declared `cdsmodel::golden::BootstrapCase`, and
`tests/golden/bootstrap_cases.hpp` from an earlier module already declared a
*different* struct with that exact name. Both are linked into the same test
binary.

That is a textbook ODR violation. It compiles, it links, nothing warns, and the
program reads one struct's bytes through the other's layout — so `c.basis`
came back as a date, `expected_points` as garbage, and `std::vector::reserve`
threw from a call site with no apparent connection to the mistake.

The fix was a rename. The *useful* outcome is a new gate in `verify.py`: every
golden header declares into one namespace and they are all linked together, so
two headers declaring the same name is now a build-time failure that names
itself. I planted a clash to confirm the gate fires, and it does.

Worth noting what the gate's first version got wrong, too. The regex
`^inline constexpr \S+ (\w+)` captured `std::array<Case, 360>`'s **size** as
the declared name, so it reported every pair of headers that happened to record
360 cases as a clash. A gate that cries wolf is worse than no gate — it trains
you to pass `--force`. Matching the last identifier before the initialiser
fixed it.

### One guard I respected instead of lowering

With the gate fixed, an older check fired: the golden file held 32 cases and the
threshold for "suspiciously few cases — truncated regeneration?" is 50. The
tempting fix is to lower the threshold for this one file. The right one is more
instruments, so I added seven — deep discounts, a flat zero curve, an
instrument starting well past the curve's end, a deeper negative curve — taking
the recorded set to 60 bootstraps, of which 12 diverge from upstream via
UPSTREAM-23.

Both counts are asserted in the replay. If upstream's filter ever changes, the
divergence count moves and the test fails, rather than quietly verifying less.
A gate that is inconvenient is usually a gate that is working.

## Decision 25 — Three probes said inert; the sweep said otherwise

`datelist.c` generates schedules: a start, a maturity, an interval, and a stub
direction become a list of coupon dates. It is the file `cashflow.c`'s two
remaining constructors were waiting on.

### The count that was wrong again

A regex for function definitions found three functions. There are seven. Four
of them put the return type on its own line, which is the same reason the same
regex missed `JpmcdsZCDiscountFactor` in `zcall.c` and two functions in
`cashflow.c`.

That is three files in a row, and I wrote in Decision 23 that the fix is not a
better regex but to stop using a count as evidence of having read a file. This
time I read it end to end first, which is the only reason `JpmcdsNewPayDates`,
`JpmcdsNewDateListFromTCurve` and both business-day adjusters are in this
slice rather than discovered by a linker error in the next one.

### UPSTREAM-24, and a header that describes a different function

The header is specific:

> starting from roll-date we see if we can count forward an integral number of
> intervals to the maturity date. If this is so then we can generate our
> datelist by counting forward from the rolldate.

The code counts from `startDate`, not `rollDate`. And the consequence is not a
small discrepancy, it is an inversion:

 * no stub (`extraDays == 0`) → the roll date is used, in the case where it has
   nothing to contribute;
 * a stub → the roll date is dropped, in the only case where it could supply a
   grid that lands on maturity.

Measured on the documented case — roll-to-maturity integral, start-to-maturity
stubbed — the roll date changes nothing at all. The feature's stated purpose is
unreachable.

### The claim I had to withdraw

Here is the part worth keeping. The library's only call site is
`cashflow.c:230`, and it passes **startDate as the roll date**. That looks
decisively inert: `count_dates(roll, start)` is trivially integral when the two
are equal, so `firstDate` becomes `startDate` and the result should be whatever
`rollDate = 0` gives.

I probed three cases. All three agreed. I wrote "the mechanism is inert for this
caller" into the header, the defect record and the severity argument.

Then the differential sweep — every pair of start and maturity day-of-month,
crossed with intervals — failed on `2024-02-29 → 2027-02-28`.

The roll branch ends in `NewDateListExtended(first, maturity, interval, TRUE)`,
generating **forward** from the start. `rollDate = 0` generates **backward**
from maturity. Those agree only where month arithmetic does not clamp, and
clamping is not invertible: 29 February plus twelve months is 28 February, but
28 February minus twelve months is 28 February, not the 29th.

So passing startDate as the roll date silently converts a back-stub schedule
into a forward-generated one, and the two differ wherever a month end clamps.
Measured: 7 of 378 integral-span cases, every one a 29 February start.

The severity does not change much — a one-day difference at one end of a
schedule, in a window that narrow. What changes is the sentence
"unreachable from inside upstream", which was false. **A sweep is not a
thoroughness ritual; it is the only thing that found this.** Three
deliberately-chosen probes, each one reasonable, all missed it, because the
hypothesis I was testing told me which cases to pick.

### Reproduce or correct?

UPSTREAM-24 is the first finding where the port had a real choice and chose
*not* to fix it.

`date_list.hpp` corrects upstream. The policy that allows it is narrow: there
was no single upstream behaviour to preserve, because UPSTREAM-7's two
truncation implementations contradicted each other *and* the header. With
nothing consistent to be faithful to, faithfulness is not available.

That is not this situation. `JpmcdsNewDateListExtendedRoll` is deterministic
and self-consistent. One source disagrees with it and that source is a comment.

A roll date decides coupon dates, coupon dates decide accrual, accrual decides
money. Changing which end a schedule is generated from, on the strength of a
sentence in a header, is not a porting decision — it is a modelling decision
wearing a porter's hat. PORT-9 happened because I decided a missing line was
deliberate; this is the same hazard with the sources swapped, and the same
answer: reproduce the behaviour, record the disagreement, let someone with
authority over the convention decide.

### Two properties that were too strong, both from the same leap day

The tier-1 suite first asserted that with no stub, `date_list` and
`date_list_extended` are identical — on the reasoning that if nothing is
stubbed there is nothing for "extended" to move.

It failed on 2024-02-29 → 2027-02-28, which `count_dates` reports as an
integral 36 months. Stepping back 36 months from the maturity lands on
2024-02-28. So `date_list`, which overwrites the boundary with the requested
start, gives the 29th; `date_list_extended`, which stays on the grid, gives the
28th. Both are correct.

**`extra_days == 0` does not imply that the start lies on the grid generated
backward from maturity.** Counting and stepping disagree on clamped month ends,
and I had quietly assumed they could not.

The replacement property is narrower and says more: the two lists differ in at
most the single boundary element on the stub side, agree everywhere else, and
`date_list`'s boundaries are always exactly what the caller asked for. It also
asserts that the boundary difference actually occurs somewhere in the sweep —
otherwise the test would be checking a vacuous claim and nobody would notice.

Both of this slice's corrections came from the same date. A leap day with a
clamped counterpart is now the first input I will reach for in any schedule
code.

### The gate from last slice earned its keep immediately

The golden header's first generation declared `cdsmodel::golden::ScheduleCase`
and `kScheduleCases`. Both already exist in `schedule_cases.hpp`, from the CDS
fee-schedule module.

That is exactly the ODR violation I spent half an hour diagnosing last slice —
and this time it arrived as:

```
FAIL: the same name is declared in more than one golden header, which is an
ODR violation across the test binary:
  ScheduleCase: date_schedule_cases.hpp, schedule_cases.hpp
  kScheduleCases: date_schedule_cases.hpp, schedule_cases.hpp
```

A rename, thirty seconds, no debugging. One slice between writing the gate and
it catching a real instance is a good return, and it is worth noting that the
instance was mine both times: the hazard is not that someone else is careless,
it is that two modules in one namespace will eventually pick the same obvious
name.

## Decision 26 — A slice with no defect in it, which is also a result

`stub.c` had four functions left: `JpmcdsStubPayment`, `JpmcdsIsEndStub`,
`JpmcdsStubPosMake` and `JpmcdsBadDayAndStubPosSplit`. They are what
`cashflow.c`'s two schedule constructors and four sites in `zcswap.c` were
waiting on, so finishing the file unblocks both.

**No new defect came out of it.** Two things looked like findings and
measurement said otherwise, and since the register is now at twenty-four
entries it is worth being explicit about what did *not* get added and why.

### The dead locals that compute the right answer twice

`JpmcdsStubPayment`, after its four validation checks:

```c
JpmcdsDayCountFraction(prevCouponDate, nextCouponDate, dcc, &couponYearFrac);
coupon = rate * couponYearFrac;              /* never read */

JpmcdsDayCountFraction(stubStart, stubEnd, dcc, &stubYears);
stubFrac = stubYears / couponYearFrac;       /* never read */

*stubPayment = rate * stubYears;
```

Two dead locals, and the obvious reading is that the intended answer was
`coupon * stubFrac` — the coupon scaled by the stub's share of its period —
which is precisely what the two dead lines compute between them. That looks
like a bug where the final assignment lost its intent.

It is not, because the two are the same number:

```
coupon * stubFrac = (rate * couponYearFrac) * (stubYears / couponYearFrac)
                  = rate * stubYears
```

So this is a simplification that was applied and whose scaffolding was left
standing. And the surviving form is the *better* one: one multiplication rather
than a multiply, a divide and a multiply, so one rounding instead of three.

The reflex here would have been to file "two dead locals suggest a lost
computation" and move on. Doing the algebra took a minute and turned a defect
report into a note.

One thing did survive from it, though, and it is small but real. The port still
calls `year_fraction(prev_coupon, next_coupon, ...)` and discards the result,
because that call can **fail** — a day count the fraction cannot compute makes
upstream return FAILURE before it ever reaches the stub arithmetic. The value is
dead; the failure is not. Deleting the call would have been a behavioural
change disguised as a cleanup.

### Validation that outlives its purpose

The same function has four checks, all about the surrounding coupon period, and
the answer does not use that period at all. Measured, by moving `nextCoupon`
and watching nothing happen:

```
stub 154511..154602, coupon period ends 154693 -> 0.012465753424657534
stub 154511..154602, coupon period ends 154877 -> 0.012465753424657534
```

But the period still decides **acceptance**, and the fourth check is the sharp
one: a stub must touch at least one boundary, so a window strictly inside the
period is refused even though `rate * stubYears` is computable for it.

All four are reproduced. They are the function's contract whatever motivated
them, and a caller relying on the rejection is relying on observable behaviour
— the same reasoning that kept `expected_points` in `ZeroCurveBuilder::create`
after it stopped controlling an allocation.

### The asymmetric validation, and a hypothesis that died in one measurement

`JpmcdsBadDayAndStubPosSplit` unpacks two values from one `long` and validates
exactly one of them:

```
input    rc        badDayConv  valid?
0x0346   FAILURE   -           -        <- stub position rejected
0x0041   SUCCESS   65 ('A')    NO       <- convention is not
0x0000   SUCCESS   0           NO
0x00FF   SUCCESS   255         NO
```

My first reading was that this lets a meaningless convention travel until some
date happens to need adjusting — making detection depend on the *data* rather
than on the input, which would have been a good finding of the UPSTREAM-18
family.

One probe killed it. `JpmcdsBusinessDay` validates the convention **before**
asking whether the date is a business day, so the same bogus value fails on a
Monday and a Saturday alike. Every caller does get an error; it is just reported
from the adjustment site rather than from the split that produced the value.
A worse diagnostic, not a wrong answer.

I had already written the "detection depends on the data" sentence into the
probe's own output before testing it. That is the second time this project has
caught me stating a conclusion in the same breath as the code meant to test it,
and the probe printing a claim it had just disproved is a useful embarrassment:
the text was there, the measurement contradicted it, and the only reason the
claim did not ship is that I read the output.

### A deliberate difference that follows from a type

The port cannot reproduce the pass-through even if it wanted to. `BadDayConv` is
a four-valued enum and an arbitrary byte is not one of its values, so
`split_bad_day_and_stub_pos` must reject what upstream passes along.

That is observable, so it is a row in the Deliberate differences table rather
than a silent improvement: upstream returns SUCCESS and fails later, the port
fails now. A caller that splits a packed value and never adjusts a date would
see success from one and failure from the other. The recorded vectors pin both
halves — eleven of the forty cases are ones upstream accepts and the port
refuses, and that count is asserted, so if upstream ever starts validating, the
test says so.

### Two enums where upstream has one integer

`TStubPos` is three-valued: auto, front, back. The port already had
`StubPosition`, which is two-valued: front, back.

They are not the same type and the port does not pretend otherwise.
`StubPosition` answers "which end is the stub at?", and two values is right
because a stub is at one end or the other. `StubPosDefault` answers "what
should I do if nobody said?", and `Auto` is not a position — it is an
instruction to look at the schedule. Upstream encodes both as plain integers
and relies on the parameter name to tell them apart, which is exactly the
mistake the two-enum split of `TStubMethod` was made to prevent in the first
half of this file.

## Decision 27 — Seven booleans in two integers, and an overflow behind a public door

`cashflow.c` is complete with this slice. The two schedule constructors —
`JpmcdsMakeCFL` and `JpmcdsNewCFLFromDL3`, about 400 lines between them — were
the last things in the file, and they were blocked on `datelist.c` and the rest
of `stub.c`. Both arrived in the previous two slices, so every dependency was
already ported before this one started.

### The signature is the specification, and upstream's hides seven booleans

`JpmcdsMakeCFL` takes ten parameters, two of which are integers carrying
independent flags:

```c
flags & JPMCDS_SUBTRACT_INITIAL            (1)
flags & JPMCDS_ADD_FINAL                   (2)
flags & JPMCDS_PRESTART_ZERO_PAYMENT       (4)
flags & JPMCDS_POST_MATURITY_ZERO_PAYMENT  (8)
flags & JPMCDS_KEEP_START_DATE            (16)
stubPlacement & JPMCDS_STUB_POSN_MASK      (1)
stubPlacement & JPMCDS_STUB_LENGTH_LONG    (2)
```

Seven booleans in two `long`s, decoded by mask at the top of the function.
Nothing stops a caller putting the stub bits in the flags word, where they
would silently read as `SUBTRACT_INITIAL` and `ADD_FINAL` — bit 1 is
`JPMCDS_STUB_POSN_BACK` and also `JPMCDS_SUBTRACT_INITIAL`.

Two structs with named fields in the port. The payoff is not readability, it is
that the two combinations upstream forbids become conditions on names:

```cpp
if ((flags.subtract_initial || flags.keep_start_date) && flags.prestart_zero_payment)
```

reads as what upstream's error message says — *"cannot subtract initial payment
at prestart date"* and *"Also cannot keep both the start and prestart dates"* —
rather than as arithmetic on a word.

### UPSTREAM-25, and why the grid being an argument matters

`JpmcdsNewCFLFromDL3` works out whether there is a stub by comparing the
instrument's dates against the grid it was handed:

```c
frontStub = ( startDate    > dates->fArray[0] );
backStub  = ( maturityDate < dates->fArray[numDates-1] );
```

and then, with no check on the grid's length, indexes `dates[1]` in the front
branch and `dates[numDates-2]` in the back one. On a one-date grid those are
`dates[1]` and `dates[-1]`. The front branch also *writes* `cfl->fArray[1]`,
past a one-element allocation.

ASan confirms both, and the addresses are the tell: `0x…0f8` past the
allocation for the front case, `0x…0e8` before it for the back one. Two
directions, one missing length check.

What I want to record is the reasoning that nearly stopped me looking. I had
already proved, for a different purpose, that `frontStub` and `backStub` are
mutually exclusive for any grid `date_list_extended` produces — it anchors the
grid on whichever end the stub is *not* at, so one comparison is always false.
From that it is a short step to "stubs are well-behaved here" and on to the next
function.

The step that is wrong is the one from *the in-library caller is safe* to *the
function is safe*. `JpmcdsNewCFLFromDL3` is declared in `cashflow.h`. It takes
the grid as an argument. A caller outside the library supplies its own, and
nothing in the function's contract says it must be longer than one date. The
mutual-exclusivity result bounds what `JpmcdsMakeCFL` can trigger; it says
nothing about what the public entry point accepts.

That is the same shape as UPSTREAM-20 and UPSTREAM-24, where severity turned on
"reachable only through the public header". The difference is that there I used
reachability to *lower* a severity, and here I nearly used it to skip a search.
**Reachability is an argument about how bad a defect is, never an argument that
there is not one.**

### The unsanitised build returns NULL, which is worse than crashing

In a plain build the front case returns NULL rather than misbehaving. Tracing
why: the garbage read as `nextCouponDate` happened to fail
`JpmcdsStubPayment`'s `prevCouponDate >= nextCouponDate` check, so the function
reported failure.

That is luck. A garbage value on the other side of that comparison lets
execution continue to the out-of-bounds write — which ASan never reports,
because it aborts on the read first. So the sanitiser shows the read and the
plain build shows an orderly error, and neither shows the write that is sitting
behind both.

### The memcpy that ASan and UBSan both miss

A smaller thing in the same function. The long-stub coalescing block runs two
branches in sequence and the first reassigns the pointer the second reads from:

```c
if (frontStub) { ...; dates = datesCopy; --numDates; }
if (backStub)  { memcpy(&datesCopy->fArray[0], &dates->fArray[0], ...); }
```

With both stubs, `dates == datesCopy`, so the second `memcpy` has source and
destination equal. C11 7.24.1p2 makes copying between overlapping objects
undefined, and identical objects overlap.

Not filed. The result is correct — five grid dates coalesce to three flows, as
intended — and no real implementation mishandles `memcpy(p, p, n)`.

The part worth keeping is what it says about the harness. **Neither
AddressSanitizer nor UndefinedBehaviorSanitizer reports it**, in the same
function where ASan caught the overflow. The project already records that ASan
cannot see uninitialised reads and that no MSan job exists; this is a second
blind spot, and it is in the tool the project leans on hardest. A clean ASan run
is evidence about one class of bug, not about memory correctness.

### The property that would catch a shifted period boundary

The tier-1 suite checks that the amounts sum to `rate * yearFraction(start,
maturity)`. ACT/365F is additive in days, so the per-period fractions must add
to the whole span.

That is worth more than it looks. Every amount is `rate * yearFraction(dates[i-1],
dates[i])`, and a port that computed the boundary from the wrong neighbour —
`dates[i]` to `dates[i+1]`, say — would produce a stream that still had the
right number of flows, the right dates, plausible amounts, and would match
upstream nowhere. But a differential test catches that only if the two
implementations disagree, and this is exactly the kind of mistake a careful
transcription can make on *both* sides of a rewrite. The sum pins it against
arithmetic rather than against upstream.

It is also the slice's one accumulating quantity, so it is the only comparison
here at the relative 1e-12 tier. Everything else — each amount, each date — is
a single multiplication or an integer, and compared exactly.

## Decision 28 — Measuring the order before choosing the next file

`zcswap.c` was the obvious next slice: 940 lines, the largest single thing
left, and the reason four `stub.c` functions were ported two slices ago. I did
not port it, because I measured its dependencies first and it is not reachable.

`zcswap.c` calls into five unported files — `streamcf` (6 call sites),
`zcswdate` (4), `schedule` (2), `zcswutil` (1), `zr2coup` (1) — about 2,650
lines of prerequisite. Starting it would have been the `zcall.c` three-way
split all over again: a plan that collapses the moment the second function
needs the first.

So this slice is a dependency measurement and then the smallest leaf it found.

### The measurement, and why the symbol table rather than a regex

The tool that mattered is `nm` on the oracle archive. For each remaining file:
which `Jpmcds*` symbols does its source reference, which object file defines
each of them, and is that file already claimed by a port header?

Using the symbol table instead of parsing C is the direct lesson of three
slices in a row where a regex for function definitions undercounted — it missed
one function in `zcall.c`, two in `cashflow.c` and four in `datelist.c`, always
because the return type sat on its own line. The archive has no such ambiguity:
a defined symbol is a defined symbol.

The result, for the fifteen in-scope files not yet started:

```
  fltrate         47 lines   *** NO UNPORTED DEPENDENCIES -- a leaf ***
  zr2fwd          64 lines   *** leaf ***
  linterpc        93 lines   *** leaf ***
  badday          97 lines   *** leaf ***
  cxbsearch      180 lines   *** leaf ***
  cx             230 lines   *** leaf ***
  zr2coup        231 lines   *** leaf ***
  interpc        572 lines   *** leaf ***
  schedule       832 lines   *** leaf ***
  zerocurve      135 lines   blocked by 1: gtozc(2)
  zcswdate       318 lines   blocked by 1: badday(2)
  gtozc          494 lines   blocked by 1: zcswap(1)
  zcswutil       169 lines   blocked by 2: badday(1), zcswdate(1)
  streamcf      1100 lines   blocked by 4: schedule(3), fltrate(1), zr2coup(1), zr2fwd(1)
  zcswap         940 lines   blocked by 5: streamcf(6), zcswdate(4), schedule(2), ...
```

Nine leaves, and `zcswap` last in the order rather than next. The path to it
runs `badday → zcswdate → zcswutil` on one side and
`schedule`/`zr2coup`/`fltrate`/`zr2fwd` → `streamcf` on the other.

Writing this down as a script rather than as a conclusion matters, because the
answer changes every slice. It will be re-run rather than remembered.

### `badday.c`, and a module with nothing wrong with it

The smallest leaf on the critical path: 97 lines, three functions, a table of
substitute dates. Not to be confused with `busday.c` — that computes a business
day from a *convention* and a calendar; this one looks one up in a
caller-supplied list of exceptions. The upstream filenames differ by one
character.

Five things looked like findings and **none of them survived**:

- **A lookup is one step, not a closure.** If a bad day's replacement is itself
  listed as bad, one call leaves a date the list still calls bad. Measured, and
  real — but the function's stated job is to convert a bad day *"(if it's in
  list)"*, and a map that sends a bad day to another bad day is malformed.
- **The round trip only holds for an injective map.** Two bad days mapping to
  one good day means the reverse lookup picks the first. That is what a
  non-injective map *is*.
- **"Not in the list" and "no list at all" return the same thing.** Deliberate,
  and upstream says so: *"allow NULL to ignore list"*. Not the UPSTREAM-2 shape,
  because there is no failure here to report — the identity is the answer.
- **`maxAllowed` is never read** by any of the three functions, and the symbol
  table shows no constructor anywhere in the library to maintain it. Dead
  state, not a defect.
- **`JpmcdsFixBadDLBadDayList` returns void.** It also cannot fail.

Every one is a consequence of the module being a lookup table rather than an
algorithm, and in every case upstream's behaviour is well defined for every
input it will accept. That is the line the port's policy draws and this module
sits on the faithful side of it.

Two slices in a row with no defect. Worth saying plainly rather than leaving as
an absence, because the register reaching twenty-five could otherwise make a
clean module look like an unfinished search.

### The one thing the port adds, and why it is not a constructor

`is_well_formed` is not a port of anything. It names the precondition the three
lookups quietly assume: no repeated bad day, no repeated good day, and no good
day that is also a bad day. Each condition corresponds to exactly one of the
measured oddities.

The tempting design is to validate on construction — a `BadDayMap` type that
cannot hold a malformed table. I did not, and the reason is a verification
constraint rather than a taste one: **upstream handles malformed maps
deterministically, so the port has to be able to hold one in order to be
compared against upstream on them.** A validating constructor would make the
duplicated, chained and cyclic shapes unrepresentable, and therefore untestable
— and those are precisely the shapes where which-entry-wins is the only thing
there is to verify.

So the lookups are free functions over a span, faithful on every input, and the
check is available beside them for callers who want it. That is the opposite of
the `ZeroCurve::create` decision, where refusing at construction was right
because upstream's accepted-but-unpriceable curves had no defined answer. The
distinction is whether upstream's behaviour on the bad input is *defined* or
merely *reached*.

## Decision 29 — Four parallel arrays, and a parameter that had to get worse

`zcswdate.c` is the first step on the path to `zcswap.c`, and the first module
whose upstream type carries **four** parallel arrays:

```c
typedef struct _swapDates
{
    TDate    *adjusted;    TDate    *original;
    TDate    *previous;    TBoolean *onCycle;
    int       numDates;    /* Length of all 4 arrays. */
} TSwapDates;
```

The comment is the invariant, and it is maintained by hand across three
functions. One vector of one struct, as with `ZCurve`'s three and
`TCashFlowList`'s two.

### A parameter that had to become less convenient

The interesting decision in this slice is a signature, and it came from a
measurement I nearly skipped.

`JpmcdsZCAdjustDate` takes a substitution table *and* a bad day convention
*and* a holiday file, and branches like this:

```c
if (badDayList != NULL)
{
    *adjDate = JpmcdsBad2GoodBadDayList(badDayList, date);
    return SUCCESS;
}
```

The obvious port takes `std::span<const BadDayPair>` and treats an empty span
as "no table" — that is what every other span-taking function in this port
does, and it reads better than an optional.

It is wrong. The branch is on the **pointer**, not on `count`:

```
no list (NULL),   conv=F -> 154504
EMPTY list,       conv=F -> 154502   <- unchanged, still a Saturday
```

So "no table" and "a table with nothing in it" are different inputs with
different results, and a plain span cannot tell them apart. The port takes
`std::optional<std::span<const BadDayPair>>`.

That is the first time in this project a parameter has had to be made *less*
convenient to stay faithful. Everywhere else the span replaced a
pointer-plus-length and lost nothing — the pointer's nullness was never
load-bearing. Here it is, and the habit of "span replaces pointer+length"
would have silently changed what an empty table means.

Worth naming the general shape: **a span is the right replacement for a
pointer and a length only when the pointer's nullness carries no
information.** Three slices ago `std::optional<Date>` replaced a TDate-0
sentinel for the same underlying reason, and I did not connect the two at the
time.

### UPSTREAM-26, where the code is right and the documentation is wrong

The same branch is the defect. With a table supplied, the convention and
holiday file are never consulted, and the table returns its input unchanged for
any date it does not mention. So a Saturday absent from the table comes back a
Saturday — from a function documented as *"Adjusts date to fall on a business
day, if necesssary"*.

I filed it low and reproduced the behaviour, because the two modes are
evidently alternatives: a caller supplies either a complete substitution table
or a convention. An incomplete table is the caller's business. What is wrong is
the sentence, not the code.

Which makes the port's answer the signature rather than a behaviour change —
`bad_days` being an `optional` says "either this or the convention" where
upstream's three unrelated parameters say "all of them". That is the cheapest
kind of fix: the compiler now tells the caller what the comment failed to.

### UPSTREAM-27, and a guess that is stated in a comment

```c
if ((valueMDY.day <= 28) && (origMDY.day <= 28))
{
    /*
     * We assume we can only be on cycle if date is not on or after
     * the 29th of the month.
     */
```

The assumption is false, and `JpmcdsCountDates` — called three lines later —
contradicts it: it reports an exact whole number of intervals for month-end
dates perfectly well.

Two consequences, and the second needed a sweep to find. The flag is wrong for
600 of 7,296 swept month-end combinations, which is bad enough on its own since
the flag is returned to the caller. But the flag also *chooses how `previous`
is computed* — forward from the value date when on cycle, backward from the
maturity when not — so 14 of those 600 get a previous coupon date one day out.

Every one has a 29 February value date. That is the third finding in this
project to land on the leap day, after UPSTREAM-24 and the two tier-1
properties I had to weaken in Decision 25. Month arithmetic is not invertible,
and 29 February is where that stops being an abstraction.

The precision worth keeping: in the annual case the forward route is
`valueDate + 0 intervals`, the value date itself, which is on the schedule by
definition. The backward route gives the day before it — a date the schedule
does not contain. So on those fourteen the heuristic does not merely lose
precision; it picks the route whose answer is not a coupon date.

Reproduced anyway. Moving a swap's previous coupon date is a convention
decision, not a porting one, and that is now the third time that rule has
decided a case (UPSTREAM-24, UPSTREAM-26, this).

### A test that passed for the wrong reason, caught by its own assertion

The UPSTREAM-27 differential test swept value-date day and maturity day
*together* — `make_date(2024, month, day)` against
`make_date(2024 + years, month, day)` — and reported **zero** differing cases
against the probe's fourteen.

The cause is in the dates. The differing cases are 29 February against a 28
February maturity: two *different* days of month. Holding them equal means
2024-02-29 is only ever paired with 2029-02-29, which is not a date, so every
candidate was skipped.

The test caught this itself, because it asserts `differing > 0` rather than
just comparing whatever it happens to find. Without that line it would have
passed, verified nothing about the finding it was written for, and looked
exactly like the other six green tests beside it. **An assertion that the
sweep found something is not a formality; it is the only thing standing between
a narrowed sweep and a test that silently stops testing.**

### One header that had to come out of lib/src

`TSwapDates` is declared in `zcswdate.h`, which lives in `lib/src` rather than
`lib/include/isda` — the first private upstream header a differential test has
needed. `cds_reference` now exposes both directories, with a comment saying
why. Reaching into a private header is exactly what a differential test is for;
the alternative is testing the type through its users, which tests them instead.

## Decision 30 — The script was wrong, and it nearly cost a slice

`tools/port_order.py` said `zr2coup.c` was a leaf. I started reading it to port
it, and it needs three `tcurve.c` functions — `JpmcdsDiscountDate`,
`JpmcdsCashFlowPV`, `JpmcdsCurveFirstDate` — none of which is ported.

The script was written last slice specifically to stop me choosing an
unreachable file, and it chose one. Worth being precise about why, because the
flaw is one I have now made twice in different forms.

### File-level claiming is not function-level portedness

The script asked: for each `Jpmcds*` symbol this file references, which object
file defines it, and does some port header claim that file? `tcurve.c` is
claimed — `zero_curve.hpp` says *"Port of upstream cxzerocurve.c + tcurve.c"* —
so every function in `tcurve.c` counted as done.

But a header claiming a file does not mean every function in it is ported.
`zero_curve.hpp` covers nine of `tcurve.c`'s functions and not the other
eight, which is exactly what my own progress measurement reported two sessions
ago: *"26 of the 154 functions in claimed files are never named in any port
header"*. I had the number and then wrote a script that assumed it was zero.

The fix is to ask the question at function level: is *this symbol* named in some
header's provenance clause? That is the same data the progress measurement used,
so the script now agrees with it.

### Over-report rather than under-report

Switching to function level immediately over-reported. It called `JpmcdsFreeCFL`
and `JpmcdsFormatDate` blockers, which they cannot be — the port replaces
allocation with `std::vector` and error strings with `std::optional`, so there
is nothing to port. Those are now a named `STRUCTURAL` set with the reason
given, the same shape as the existing out-of-scope file list.

It also missed `JpmcdsBSearchLong`, which `bsearch.hpp` does cover — under the
words *"Equivalent to upstream"* rather than *"Replaces"*. Both phrasings are
in use in this codebase, so the script accepts both rather than my rewording
thirty headers to match a regex.

The remaining over-reports are left deliberately, and the docstring says so: a
reported blocker is a **ceiling**. It can mean the port already covers that
function under another name and the header never said so. The fix for a false
positive is to make the header claim it — which is the provenance mechanism
`verify.py` already enforces — not to special-case it in the script.
Over-reporting costs a minute of checking; under-reporting costs a slice.

### Four claims the port had earned and never made

Resolving the false positives turned up four functions the port genuinely
replaces without saying so:

- `bad_day.hpp`'s `adjust` also replaces `JpmcdsBusinessDay`, upstream's
  file-based wrapper — the only part not ported is the holiday-file lookup.
- `calendar.hpp`'s `is_business_day` replaces `JpmcdsIsBusinessDay` and
  `JpmcdsHolidayListIsBusinessDay`. Upstream returns a status plus a `TBoolean`
  out-param; a predicate cannot fail once the calendar exists, which is the
  point of validating it at construction.
- `date_interval.hpp`'s `make_date_interval` replaces
  `JpmcdsMakeDateInterval`.
- `day_count.hpp`'s `from_upstream_code` replaces `JpmcdsDayCountValid` —
  upstream returns a status, the port returns the value, so validity and
  conversion are one step rather than two.

And one deliberate non-port, now recorded as such rather than left as an
omission: `JpmcdsBSearchCheckOrder` exists only to toggle the mutable global
that caused UPSTREAM-13. A function whose entire purpose is to mutate
process-wide state has nothing to replace it with.

That is the part worth keeping. **An unstated claim and a missing port look
identical from outside**, and the only reason these four were visible at all is
that a tool started reading the claims instead of a person remembering them.

### What the corrected order actually says

```
  fltrate         47 lines   *** leaf ***
  linterpc        93 lines   *** leaf ***
  zcswutil       169 lines   *** leaf ***
  cxbsearch      180 lines   *** leaf ***
  cx             230 lines   *** leaf ***
  interpc        572 lines   *** leaf ***
  zr2fwd          64 lines   blocked by 1: tcurve:JpmcdsDiscountDate
  zr2coup        231 lines   blocked by 3: tcurve:CashFlowPV, CurveFirstDate, DiscountDate
  schedule       832 lines   blocked by 1: busday:JpmcdsDateFromBusDaysOffset
  streamcf      1100 lines   blocked by 4: schedule, zr2coup, fltrate, zr2fwd
  zcswap         940 lines   blocked by 5: streamcf, zr2coup, zcswutil, tcurve:CashFlowPV, schedule
```

`interpc.c` is the real next step, not `zr2coup`. `JpmcdsDiscountDate` needs
`JpmcdsInterpRate`, which lives there — and `interpc.c` is now a leaf precisely
because the `JpmcdsBSearchCheckOrder` non-port got written down.

Also worth noting what `JpmcdsDiscountDate` is not: `ZeroCurve::zero_price`
already replaces `JpmcdsZeroPrice`, and the two are different functions.
`JpmcdsZeroPrice` hardcodes continuous compounding on ACT/365F;
`JpmcdsDiscountDate` honours the curve's own basis and day count and respects
an interpolation type. I checked that before assuming either was redundant,
which is the habit that found the other four.

### And a clean rebuild found two warnings an incremental one had hidden

`stub_differential_test.cpp` passed a string literal where upstream wants
`char *`, twice. The incremental build never recompiled that file after the
flag change that surfaced it, so CI had been green on a tree that warns.
Zero warnings again, and a reminder that `cmake --build` is not a verification
step — a clean configure is.

## Decision 31 — One decision, two defects, two files, two slices apart

`interpc.c` interpolates a rate off a curve in the curve's own terms, honouring
its basis, its day count and a chosen interpolation type. It is the file
`JpmcdsDiscountDate` needs, and `JpmcdsDiscountDate` is what `zr2coup`,
`zr2fwd` and `zcswap` all need.

### UPSTREAM-28, and what a removed global bought for free

The finding is in the first six lines of `JpmcdsInterpRateExact`:

```c
TBoolean checkOrder = JpmcdsBSearchCheckOrder (FALSE);
```

Save the global, disable order checking, restore at `done:`. Between those two
points sit two `return SUCCESS` statements — the clamps for a date past the end
of the curve and a date before its start. Both bypass the restore, so **every
extrapolation leaves order validation off, process-wide and permanently.**

That is UPSTREAM-13's global and UPSTREAM-13's failure mode, with the
reachability inverted. U13 leaks on two *error* paths in `lintrp1.inc`; this
leaks on two *success* paths. Measured, the error path here restores correctly
— the exact opposite. So two functions share one flag and leak it under
complementary conditions, and between them there is no state of the program in
which it can be trusted.

The consequence is two calls long:

```
order checking ON : JpmcdsBSearchDouble(unsorted) -> FAILURE
after one extrapolation: same call               -> SUCCESS
```

A precondition check stops checking. Not for this call — for the process.

**The port was already immune, and that is the part worth recording.** When
UPSTREAM-13 was filed I split the search into `bracket` and `bracket_checked`
and deleted the flag, writing in `bsearch.hpp` that "the caller picks the
function and the global is gone". Two slices later a second defect in a
different file turned out to need no new work at all.

That is the case for removing mutable global state rather than auditing its
users. An audit finds the leaks you look for; removing the state makes the ones
you have not looked for impossible. I could not have predicted this instance —
I had not read `interpc.c` when I made the decision.

A smaller note in the same spirit: the mechanism was introduced as a
performance *speculation*, and the comment still says so with an exclamation
mark — *"This might well produce a dramatic improvement in all the ALIB zero
curve analytics!"* The optimisation is to skip a validation. The defect is that
it skips it for everyone, forever.

### A test whose own fixture had to defend against the defect

The differential test resets the flag in `SetUp` **and** `TearDown`, because
the function under test leaks it. Without that, test *order* decides what the
checked searches do for every other test in the binary — and the golden
generator needs the same reset between recordings, or the recording would
depend on its own iteration order.

That is an unusual shape and worth naming: a defect that leaks process state
makes the test harness part of its blast radius. The reset is not tidiness, it
is the thing that keeps 571 other tests deterministic.

### Two branches the port cannot reach, and why that is not an omission

`interpc.c` has five day-count arms and a whole function
(`InterpRateDiscountFactors`) for the discount-factor basis. The port carries
two arms and none of that function.

Not because I skipped them. `ZeroCurve::create` refuses a basis outside
{continuous} ∪ [1, 365] and any day count other than ACT/365F or ACT/360 — a
decision made three modules ago for UPSTREAM-15, where upstream's own
validating constructor accepted curves that `JpmcdsZeroPrice` then priced as
NaN. The inputs those branches exist to handle cannot be constructed.

"The port is missing a branch" and "the port cannot represent the input that
reaches it" look identical in a diff, so both suites assert the second
directly: the curves are refused at construction, and upstream is shown
accepting one of them so the refusal reads as a decision rather than an
accident.

### The two flat-forwards implementations, finally compared

Two slices ago I noted that `zcall.c`'s `ZCInterpolate` has its own
flat-forwards arm, independent of `interpc.c`'s, and that comparing them
directly was worth doing. This slice makes it a test.

They agree to the accumulating tier — not bit-for-bit, and they should not be
expected to: `interp_flat_forwards` has a branch for the base date falling
between the brackets, which `ZCInterpolate` has no equivalent of because a
`ZCurve`'s value date is not a curve point. So the two take different routes
through the same algebra on different inputs and land in the same place.

The cheaper observation is why that branch exists at all. Upstream's comment:
*"This ensures that basedate will be associated with a 'kink' in the forwards,
and discount(base) == 1.0"*. Without it the interpolated discount factor at the
base date would not be exactly 1, and every price computed off the curve would
inherit that error. The tier-1 suite asserts the consequence rather than the
branch — `rate_to_discount` at the base date must return exactly 1.0 — which is
the form that would survive someone rewriting the interpolator.

### The property I would keep if I could keep one

`EveryCurvePointInterpolatesToItsOwnRate`, bit-exactly, under both schemes and
every basis. An interpolator that merely came close at its own knots would be
wrong in a way no tolerance should absorb: those rates are inputs, not results.
It is three lines and it would catch a swapped bracket, an off-by-one index, or
a conversion applied twice — none of which a value comparison against upstream
necessarily would, since both sides could be made to agree on a wrong answer.

## Decision 32 — Two routes to one number, and a tolerance that was wrong for the fourth time

The three `tcurve.c` functions `interpc.c` was blocking: `JpmcdsDiscountDate`,
`JpmcdsCashFlowPV` and `JpmcdsCurveFirstDate`, plus `JpmcdsDatesFromCurve`
while the file was open. Together they free `zr2coup` and `zr2fwd`, which the
port-order script now confirms are leaves.

No new defect. What the slice produced instead is a cross-check and a lesson
about tolerances that I have now learned four times in different clothes.

### A header split for a dependency reason, not a taste one

`first_date` and `dates` are methods on `ZeroCurve`. `discount_date` and
`cash_flow_pv` are not, and could not be: `discount_date` interpolates, so it
needs `interp_curve.hpp`, which needs `zero_curve.hpp`. Putting it on the curve
would close a cycle.

That is the whole justification for `curve_discount.hpp` existing, and it is
worth writing down because a reader looking for "discount off a curve" will
look at the curve first. Two headers claiming the same upstream source is
already established practice here — `cash_flow_list.hpp` and
`cash_flow_schedule.hpp` both claim `cashflow.c`.

### The cross-check the port now gets for free

`ZeroCurve::zero_price` and `discount_date` are two implementations of one
quantity. That is the UPSTREAM-17 shape exactly — and UPSTREAM-17 was a case
where two such implementations disagreed at a boundary and nobody had compared
them.

So I compared them, on both sides. They agree. Both suites assert it now, which
means the port carries the check UPSTREAM-17 showed upstream lacked.

I also nearly got the reason wrong. The port's `zero_price` is
`exp(-rate * days/365.0)` with a hardcoded 365, which reads like it must
disagree with a curve on ACT/360 or annual compounding. It does not, because
`JpmcdsZeroRate` — and the port's `rate_cc` that mirrors it — converts the
stored rate to continuous compounding *first*, using the curve's own basis and
day count. The hardcoded 365 is correct because by that point the rate is
continuous and the time is measured in the same convention the conversion used.
I checked that before writing either function rather than after.

### The tolerance, and why this one is a different mistake

The tier-1 property "the two routes agree" failed at **basis 365**, by 350
ULPs. My probe had tested bases 1, 2 and 5000 and found exact agreement, so I
wrote a 4-ULP bound.

Upstream diverges identically — 8.26e-14 at basis 365 — so this is not a port
bug. Swept properly:

```
basis  1    relative 2.4e-16
basis  12   relative 9.6e-16
basis  52   relative 3.1e-15
basis  365  relative 8.3e-14    <- about 350 ULPs
basis  5000 relative 1.2e-16    <- continuous: no conversion at all
```

The mechanism is `cc = basis * log(1 + rate/basis)`. At basis 365 and a 3%
rate that is `log(1 + 8.2e-5)` — the log of a number very close to 1, which
loses about four digits unless `log1p` is used. The agreement therefore
degrades with compounding frequency and *recovers* at continuous, where the
conversion is the identity.

This is the fourth tolerance I have had to widen, and it is a different failure
from the previous three. Those were cases where I asserted a quantity was
well-behaved and the floating-point edge of its domain said otherwise. This one
is about **conditioning**: two routes that are equal in the reals can differ by
far more than rounding if one of them passes through an ill-conditioned step.
A ULP bound on such a comparison is not a tight tolerance, it is a claim that
`log(1+x)` is well conditioned near zero.

So the rule, which I should have had after the FP-contraction episode:
**cross-checks between two mathematically-equal routes get the accumulating
tier, not the ULP tier** — the ULP tier is for one computation compared against
itself under different compilation, which is a different thing entirely.

### The sampling mistake underneath it

The reason the probe missed it is worth more than the tolerance. I sampled
bases 1, 2 and 5000 — the two smallest and the largest. The divergence peaks in
the *middle* and vanishes at both ends I had chosen.

A basis list that samples only the extremes hides a middle, and I had no reason
to expect monotonicity in the first place. The differential test's list is now
1, 2, 12, 52, 365, continuous, with a comment saying why 52 and 365 are there.

### A claim stronger than either comparison

The differential test now asserts something better than "both agree with
themselves": that **the port's drift equals upstream's drift**, to 1e-15. Since
both inherit the discrepancy from the same algebra, that is the claim actually
worth making — it would catch a port that was accidentally *better* conditioned
than upstream, which is a divergence even though it looks like an improvement.

### The present value, and an order check disguised as a value check

`cash_flow_pv` is a left-to-right sum of discounted amounts, and the recorded
cases use amounts spanning 1e-6 to 1e12 on purpose. At that spread a
reassociated sum misses by far more than any tolerance would absorb, so the
recorded total pins the accumulation *order* and not merely the arithmetic. The
differential test compares it bit-exactly for the same reason.

That is the cheapest way I know to test an order dependency: choose inputs
where the wrong order cannot produce the right answer.

## Decision 33 — A second entry point is not a second defect

`zr2fwd.c` and `zr2coup.c` together: the rates a zero curve implies. Two files,
one idea — the forward between two dates, and the coupon that prices an
instrument at par — so one header.

Upstream's `JpmcdsZerosToCouponsPoint` and `...PointAdj` collapse into one
function here. The first calls the second with no bad-day adjustment and a
"NO_WEEKENDS" holiday file; the port takes a convention and a calendar span, so
the unadjusted case is `BadDayConv::None` with `Calendar::no_weekends()`. That
is the same two arguments, named rather than hidden in a wrapper.

### The decision worth recording: not filing a 29th defect

`JpmcdsForwardFromZCurve` divides two discount factors with no guard at all:

```c
forwardDiscount = endDiscount/startDiscount;
JpmcdsDiscountToRate(forwardDiscount, startDate, endDate, dayCountConv, basis, rate);
```

Measured, both ends of the range produce a NaN rate reported as SUCCESS —
`0/0` where the factors underflow, `inf/inf` where they overflow. And this is
*strictly less* protection than the `zcall.c` site, which at least tests
`if (discount1 <= 0.0)` — badly, which is UPSTREAM-18, but it tests.

I did not give it a number. The mechanism is UPSTREAM-19's blind guard in
`JpmcdsDiscountToRate`, the consequence is the same NaN-as-success, and the fix
was the same one line — the port was already immune here and needed no new
work. So UPSTREAM-19's record now has a "second site" section on all three
surfaces.

The reasoning is worth stating because the temptation runs the other way: the
register is a visible count, and twenty-nine reads as more thorough than
twenty-eight. But **a register that counts entry points rather than defects
stops being a map of the code and becomes a map of my reading order.** Someone
fixing UPSTREAM-19 fixes both sites with one edit; that is the test for whether
it is one defect.

The contrast is UPSTREAM-9 and UPSTREAM-10, which *are* separate: one supplies
a bad pointer, the other performs the unbounded write, and fixing either leaves
the other. Same file, same symptom, two edits — two numbers.

### The property that is worth more than the whole differential suite

`par_coupon_rate` composes four things: a schedule, a present value, a discount
factor and a quotient. The differential suite compares it against upstream
across 216 combinations and that is useful, but both sides could be wrong
together.

The check that cannot be is the **par round trip**: take the coupon the
function returned, rebuild the instrument at that coupon, price it off the same
curve, and the result must equal the present value of the start date. That is
the definition of "par", it needs no reference implementation, and it would
survive someone replacing the whole function.

It passes across basis, interpolation type, payment interval, stub position and
two schedule shapes — 72 combinations, to 1e-10 relative. For something
assembled from four other modules, that is the first check I would want and the
last one I would delete.

### A property I asserted wrongly, and the arithmetic that corrected it

The flat-curve test first claimed the par coupon lands "within a basis point"
of the curve level. It failed at semiannual by **39 basis points**.

Not a bug. 2 × (1.04^0.5 − 1) = 3.9608%, and the function returned 3.96080% —
the semiannual equivalent of a 4% annual rate, to seven figures. My assertion
was simply false: annual-to-semiannual conversion *is* forty basis points at
that level, and calling it noise was an error of finance, not of tolerance.

The replacement asserts the real relationship — `f * ((1+r)^(1/f) - 1)` — to
2e-5, with the residual attributed to day-count drift, since the schedule falls
on actual dates rather than exact fractions of a year. That is a better test
than the loose one would have been even if the loose one had passed: it now
checks the compounding conversion rather than the magnitude.

Fifth tolerance I have had to revisit, and the first where widening would have
been the wrong fix. The previous four were cases where the arithmetic could not
deliver what I asserted. This one was a case where the arithmetic delivered
something I had not thought about.

### `fltrate.c`, read and deferred, with a reason

I read `fltrate.c` while surveying and did not port it. `JpmcdsFloatRateSet` is
its only function and it has one notable property: `TFloatRate` has **seven**
fields and the function sets six. The one it misses is `rateType`, which the
struct's own documentation describes at length —

> payInterval is ONLY used if rateType = JPMCDS_SIMPLE_BASIS. ... In order to
> compute compounding rates, set rateType = JPMCDS_ANNUAL_BASIS (1) or 2 for
> semi-annual compounding

— and it returns SUCCESS. Measured with a pre-dirtied struct, `rateType`
survives as `0x5a5a5a5a5a5a5a5a`.

Then the thing that decides the severity: **nothing in the library reads it.**
Grepping the whole tree, the only other `rateType` is an unrelated parameter in
`cfinanci.h`. So it is a dead field — declared, documented as load-bearing,
never set by its only initialiser, never consulted.

That makes it an observation rather than a defect, and it belongs with
`streamcf.c`, which is both the only caller of `JpmcdsFloatRateSet` and the only
consumer of `TFloatRate`. Porting a struct setter in isolation would mean
guessing at a type whose users I have not read yet, which is the mistake the
dependency script exists to prevent.

## Decision 34 — The same false positive twice, so fix the class

`schedule.c` is the largest remaining leaf and `port_order.py` said it was
blocked by one function: `JpmcdsDateFromBusDaysOffset`. I went to port that
function and found the port already had it.

`JpmcdsDateFromBusDaysOffset` is nine lines: look a holiday file up in a
process-wide cache, then call `JpmcdsHolidayListAddBusinessDays`. The port's
`add_business_days(from, count, calendar)` is that second function with the
calendar passed explicitly, and the cache lookup is the part the port
deliberately does not have.

So the blocker was a naming omission, which is exactly what happened in
Decision 30 with four other functions. **Twice is a class, not an incident.**

### The pattern, named once instead of chased per function

`busday.c` publishes most of its work twice:

```
JpmcdsHolidayListAddBusinessDays(args, THolidayList *hl)
JpmcdsDateFromBusDaysOffset(args, char *holidayFile)   -> cache lookup + the above
```

and the same for `DateToBusinessEOM`, `BusinessDay`, and `IsBusinessDay`. The
port takes a `Calendar` by value, so **one port function replaces a pair** --
and if only the list-taking name gets claimed, the audit reports a gap that is
really a missing sentence.

Auditing the file systematically rather than per-symbol took one command and
found all four states at once:

```
JpmcdsDateFromBusDaysOffset          ABSENT
JpmcdsDateToBusinessEOM              ABSENT
JpmcdsHolidayListIsHoliday           mentioned only
JpmcdsHolidayLoadFromDisk            mentioned only
```

The first two needed claims. The third was documented as *"matching
`JpmcdsHolidayListIsHoliday`"* -- true, informative, and invisible to a tool
looking for "Replaces". The fourth is a genuine non-port.

`business_days.hpp` now carries a note listing the four pairs explicitly, so
the next reader does not have to rediscover why one port function answers to
two upstream names. That is the difference between fixing an instance and
fixing a class: the note costs twelve lines and removes a category of false
positive.

### A non-port recorded as a decision

`JpmcdsHolidayLoadFromDisk` reads a holiday file from a path and populates the
cache. It will never be ported, and `calendar.hpp` now says so with the reason:
the port takes calendars as values, so where they come from is the caller's
choice -- a file, a database, a test fixture. The whole cache family
(`FromCache`, `AddToCache`, `EmptyCache`, `Read`, `NewGeneral`) is now marked
structural in the script for the same reason.

That matters for the progress number as much as for the script. Those six
functions were being counted as outstanding work, and they are not work at all
-- they are a design decision made in the first slice that nobody had written
down in a place a tool could read.

### What it bought

`schedule.c` is a leaf, `streamcf.c` is down from four blockers to two, and the
function count moved 126 to 129 without a line of new behaviour. Three of those
three were already written; only the claims were missing.

Which is the uncomfortable part worth keeping: **an unstated claim and a
missing port are indistinguishable from outside, and I have now been the person
who could not tell them apart twice.** The provenance comments are not
documentation of the port, they are its index, and an index with gaps sends you
to write code that already exists.

## Decision 35 — A constructor and a validator that disagree about the same value

`schedule.c` is 832 lines and `JpmcdsNewCouponDatesSwap` takes sixteen
parameters — the most extreme signature in the project. I expected the slice to
be about that signature. It was not.

### What the reading suggested, and what measuring said

Reading the file turned up four candidate findings. I wrote a probe for each
before filing any, which is the discipline that keeps paying: **two were real,
one was wrong, and one turned out to be intended behaviour that the port still
has to be careful about.**

The one I got wrong is worth recording. `CheckArgsSwapMakeSchedule` rejects
`startDate > matDate` but not equality, and the empty-accrual-period removal
loop then drops the single zero-length period, so `fNumItems` reaches zero. I
predicted the schedule would also come back claiming a `BACK_STUB`, because at
`i == 0` the removal maps `FRONT_STUB` to `NO_STUB` but leaves `BACK_STUB`
alone. The probe said `NO_STUB`. My reasoning had been right about the mapping
and wrong about which stub the generator produced — with `stubAtEnd == FALSE`
the cycle runs backward and the stub comes out `FRONT`, which *is* mapped. Set
`stubAtEnd = TRUE` and the `BACK_STUB` survives, on a schedule with no periods
in it at all.

That is a small thing, but it is the fourth time this project has caught me
predicting the output of code I had just read. The probe cost two minutes.

### The finding: two functions in one library, disagreeing

`JpmcdsNewCouponDatesSwap` returns a non-NULL `TCouponDateList` with
`fNumItems == 0`, reported SUCCESS. `JpmcdsCouponDateListCheck` — in the same
file, for the same type — rejects it: *"schedule must have at least one coupon
period"*.

Every other defect in this register is a function being wrong about the world.
This one is **two functions being wrong about each other**, and that changes
what "reproduce upstream" even means. The port's policy says to correct only
where there is no single upstream behaviour to preserve, and I have always read
that as a narrow clause about undefined behaviour. Here it applies for a
different reason: there are two upstream behaviours, they contradict, and
reproducing either one means contradicting the other. There is no faithful
option.

So the port refuses, and `CouponSchedule::periods` is non-empty by
construction.

### The guard I nearly got wrong

My first instinct was to refuse `start == maturity`. That would have been the
input test, and it would have been wrong — which the exhaustive sweep caught,
not my probes. A Saturday-to-Sunday span under FOLLOW adjustment collapses the
same way: both dates move to Monday, the single period has
`accrueStartDate == accrueEndDate`, and it is removed. `start < maturity`
throughout.

A one-day span over a weekend is not obviously invalid input. **Guard on the
outcome, not on the input that first showed you the outcome** — the same lesson
as UPSTREAM-24, where three hand-picked probes agreed and the sweep found seven
differing cases I had not imagined.

### The second defect, and the reachability argument I have now made twice

`JpmcdsGenerateUnadjDates` sizes its scratch array from
`CountDates(startDate, matDate)` and then steps the generating loop from the
*roll date*. A roll date outside the span makes the stepped span the longer one
and the array overflows — `heap-buffer-overflow` at `schedule.c:761` under
ASan.

`JpmcdsNewCouponDatesSwap` bounds the roll dates before calling, so the
library's own path is safe. I caught myself about to not file it on exactly
that basis, which is the mistake UPSTREAM-25 taught and I apparently needed to
be taught again. What settled it this time was a sharper fact than
"reachability bounds severity": `JpmcdsGenerateUnadjDates` is declared in the
public `schedule.h` and has **no caller inside the library at all**. It is not
an internal helper that happens to be exported. It exists for external callers,
and those are precisely the callers who get no argument checking.

Two edits in two functions with two mechanisms, so two register numbers — the
counting rule from Decision 33 applied in the direction that *splits* rather
than merges.

### Order is load-bearing, and the obvious implementation loses

The intended-behaviour candidate is the most useful thing in this slice for
anyone porting the rest.

`fullFirstCoupDate` overrides `fArray[0].accrueStartDate`, and that override is
the *last* thing the function does — after the reset dates have already been
computed from the accrual starts. So period 0 ends up with a reset six months
*after* its own accrual start:

```
[0] accStart=2023-09-15  accEnd=2024-07-15  reset=2024-03-15  pay=2024-07-15
```

This is defensible finance: the rate was set when the swap actually started,
and the accrual is backdated to the full-coupon date. It is also exactly the
kind of thing a tidy-minded port destroys, because computing the resets *after*
the override is the obvious order and produces a different schedule. The
validator does not object either way.

It gets a comment at the site saying ORDER IS LOAD-BEARING, and a dedicated
tier-2 test — because the big sweep would not have caught it. The override only
fires on front-stub schedules with a full-first-coupon date, and only period
zero differs. One assertion on one period, written because I could see the trap
rather than because a test went red.

### What the type system absorbed

Of the sixteen parameters, the three TDates using `> 0` as "supplied" became
`std::optional`, and the runtime check that both roll dates are not supplied at
once became a `std::variant` — a check with nothing left to check. The
`i`/`E`/`J` guard in the long-stub branch became nothing at all: those period
types are not representable, and it turns out the guard was dead upstream too,
since `JpmcdsDtFwdAny` refuses them before the branch is reached.

The one place the type system actively misled me: I reached for
`detail::scale` to compute `periods * direction * step` and every single case
refused. `scale`'s bounds check assumes a positive factor — `INT_MIN / -1`
overflows — and it had only ever been called with the constants 12, 6, 3, 1, 7
and 28. A helper correct for every existing caller, wrong for the first one
with a sign. Widening to `long long` and checking the range is three lines and
needs no assumption about the factor.

### Where this leaves the bootstrap

`schedule.c` was the principal blocker on the path to `zcswap.c`. The chain is
now `fltrate` (47 lines) → `streamcf` (1,100) → `zcswap` (940) → `gtozc` (494)
→ `zerocurve` (135), and nothing else is in the way.

## Decision 36 — Proving a field is dead before deleting it

`streamcf.c` is 1,100 lines and most of its interface does nothing. The port
drops six things, and the interesting part of this slice is not the dropping —
it is what licensed it.

### The inventory

| dropped | why |
|---|---|
| `TSwapType` | one enumerator |
| `TStreamFloat.firstCoupon` / `.finalCoupon` | neither field ever read |
| `.averSoFar` / `.compSoFar` | read, passed down two levels, land in unused parameters |
| the fixed path's `valueDate` | passed through three functions, read by none |

`streamcf.c` is the remains of a larger library. The parameters for averaging
and compounding swaps survived; the code that used them did not.
`GetFloatStubRate` takes eleven parameters and uses five.

### The rule I had to make explicit

"Upstream never reads this field" is a claim **about upstream**, so it gets
tested against upstream. Each dropped field is swept over values that would
move the answer if it were read — stub rates at 9.5 and −0.5, stub intervals of
one day and ten years, `averSoFar` and `compSoFar` at 0.77 and −0.33, value
dates across a century including dates long after maturity — and every cash
flow is required to stay put.

Without those tests the omissions are an assertion. With them they are a
measurement. It is the same move as the oracle ladder generally: the claim that
licenses a simplification has to be checked at the same standard as the
arithmetic.

This also forced a distinction I had got wrong one slice earlier.
`FloatRate::weight` is *kept* even though nothing reads it, while `TStubRates`
is dropped. The difference: `weight` is one field a caller fills in and a
consumer might legitimately apply, whereas `TStubRates` is a whole struct
passed into a function that ignores every bit of it. Removing a field two
callers set changes their code; removing one nobody sets or reads does not.

### UPSTREAM-31, and the first finding no sanitizer could catch

`GetFloatStubRate` writes `*stubRate = rate;` **after** the `done:` label every
failure path jumps to, and `rate` is assigned only inside the one success
branch. So a failure copies an indeterminate double to the caller.

I did not file that from the reading. The read is only indeterminate if
`JpmcdsForwardFromZCurve` leaves its output alone on failure, which is a fact
about a different file — so I swept 200 forward computations with a sentinel in
the output. One failed, and the sentinel survived. The precondition holds.

(The same sweep found 39 of 200 returning **NaN as SUCCESS**, which is
UPSTREAM-19's second site turning up unbidden and now quantified.)

What makes this entry worth its number is what it says about the harness.
Every previous memory finding here was caught by a sanitizer. This one cannot
be:

* ASan tracks addressability, not initialisedness;
* UBSan has no check for uninitialised reads;
* MSan would catch it, and there is no MSan job — unavailable on this platform,
  a gap the port notes have recorded since the harness was set up;
* and single-file static analysis cannot see it either. From inside
  `streamcf.c` the call takes `&rate` and *might* write it. I ran
  `-Wconditional-uninitialized`, `-Wuninitialized`, and `clang --analyze` with
  `core.uninitialized`. All three report nothing, and **all three are right** —
  the information they need is in `zr2fwd.c`.

So the blind spot that had been a stated limitation with no example now has
one. It was found by reading the code, which is the only instrument that
crosses translation units.

### Three predictions, three corrections

Same ratio as last slice, and worth tallying because the probes keep paying:

1. I thought the `SUBTRACT_INITIAL` branch overflowed, writing `fArray[i+1]`
   for `i` up to `fNumItems-1`. It allocates `fNumItems+1`.
2. I thought `firstCoupon = 1` on a front stub meant `fArray[0]` never got
   written, losing the initial principal flow. The amounts-computing loop and
   the cash-flow-writing loop are *separate* loops; the second always starts at
   zero. I had merged them while reading.
3. I thought the one-sided guard `swapType > JPMCDS_SINGLE_REFIX` (that
   constant being `0`) let negatives through. It does not: an all-non-negative
   enum gets an unsigned compatible type in C, so `(TSwapType)(-1)` is
   4294967295.

The third correction then corrected *itself*. I wrote the test as
`EXPECT_FALSE(static_cast<TSwapType>(-1) < 0)` and it failed — because in
**C++** clang gives that same enum a signed type. The test file and the oracle
it links against disagree about `TSwapType`, through the same header. The
comparison happens on the C side so the C answer governs, but a guard correct
only by implementation-defined enum signedness, differing between the two
languages sharing its header, is not protection worth carrying forward.

I nearly left behind an `EXPECT_TRUE(x || !x)` while restating that as
something true. A tautology dressed as a test is worse than no test: it reports
green forever and documents nothing. The comment carries it instead.

### The two legs disagree, and the port keeps the disagreement

For a one-period `TWO_STUB` schedule — which `JpmcdsCouponDateListCheck`
accepts and `JpmcdsNewEmptyCouponDateList` makes constructible — the front and
back stub both address index 0:

* the fixed path writes `couponRates[n-1] = final` then `couponRates[0] =
  first`, so **first** wins;
* the float path runs `GetFrontStub` then `GetBackStub` into `amounts[0]`, so
  **back** wins.

Opposite precedence, from the same stub location, in the same file. Reproduced
on both sides with a test asserting the disagreement, because picking a winner
is a convention decision rather than a porting one — UPSTREAM-24 and
UPSTREAM-27's reasoning, now applied to an internal inconsistency rather than a
documentation one.

### Where this leaves the bootstrap

`zcswap.c` is now the only thing between the port and `gtozc` → `zerocurve`.
Which is also where UPSTREAM-29 lands: `zcswap.c` reads
`floatDL->fArray[floatDL->fNumItems-1]` at three sites with no emptiness check,
and `streamcf.c` is the reason the float leg arrives unvalidated —
`CheckFixedStreamDetails` calls the schedule validator and there is no
`CheckFloatStreamDetails` at all.

## Decision 37 — The guarded input and the one two statements later

`zcswutil.c` is 169 lines and two functions, the last thing in the way of
`zcswap.c`. It yielded one defect, and the shape of it is the point.

`JpmcdsZCGetSwapCFL` opens by handling a degenerate input and *saying so*:

```c
/* Bizarre rate==0.0 case */
if (rate==0.0)
{
    cfl = JpmcdsNewEmptyCFL(1);    /* simply cash flow of 1.0 at date */
    ...
    return cfl;
}
```

Everything about that is considered. A zero-coupon swap benchmark really is one
redemption of 1.0 at maturity. Even the `return cfl` rather than a jump to
`done:` is right: `status` is still `FAILURE` there, so routing through the
shared exit would free the list and return NULL. Someone sat with this.

Two statements from the end of the same function:

```c
cfl->fArray[cfl->fNumItems-1].fAmount += 1.0;   /* add principal */
```

and with `matDate == valueDate` that is `NULL[-1].fAmount += 1.0`.

### The chain, measured one link at a time

I did not file it from the reading. Each link got a probe:

```
JpmcdsNewDateList(v, v, 6M)  -> 1 date
JpmcdsNewPayDates(v, v, 6M)  -> 0            <-- the link that matters
JpmcdsNewEmptyCFL(0)         -> fArray NULL
```

The middle one is the interesting link, and it is not an allocation accident:

```c
/* Now remove startDate, and move all dates back by one. */
for (idx=0; idx < payDates->fNumItems-1; idx++)
    payDates->fArray[idx] = payDates->fArray[idx+1];
payDates->fNumItems--;
```

Excluding the value date from a list of *payment* dates is correct. The case
where it is the only date is simply not considered — the count goes to zero and
nothing downstream asks.

Then both sanitizers fire, with diagnostics that are the same bug from two
distances:

```
UBSan: applying non-zero offset 18446744073709551600 to null pointer
ASan:  SEGV on unknown address 0xfffffffffffffff8
```

`18446744073709551600` is −16 unsigned, and `sizeof TCashFlow` is 16: one
element backwards. `0xfffffffffffffff8` is −8, the offset of `.fAmount` inside
that element — the address actually touched. One tool reports the pointer
arithmetic, the other the dereference.

### The contrast with last slice, which is the useful part

UPSTREAM-31, one slice ago, was invisible to the entire harness: ASan blind by
design, UBSan has no check, no MSan job, and single-file analysis could not see
it because the fact it needed lived in another translation unit. I had to argue
for it from reading and then measure the precondition by hand.

UPSTREAM-32 is reported independently by two tools on the first run.

Same codebase, one slice apart, and the tooling gradient between them is
enormous. The lesson I want to keep is not "sanitizers are good" — it is that
**a clean sanitizer run tells you about the shapes sanitizers model.** A null
dereference is one of those shapes. An uninitialised stack read crossing a
translation unit is not. Reading the code found both; only one of them would
ever have shown up in CI.

### The guard goes where the problem is, not where the cause is

My first instinct was to refuse `maturity == value_date` in `swap_cash_flows`.
That is the input test again — the mistake UPSTREAM-29 taught me two slices
ago, where three hand-picked probes agreed and a weekend span reached the same
place with `start < maturity`.

Here the same reasoning applies for a fresh reason. Between the date generation
and the flow construction, two things run: the business-day adjustment and the
substitution table. `fix_bad_dates` is documented — by me, three slices back —
as able to collapse two dates onto one. So the empty list is not only
reachable from the degenerate input. Guard the outcome.

The matching decision is what I did *not* change. `swap_coupon_dates` still
returns the empty list, because that is upstream's answer and the date
generation is faithful. Only the function that would write through it refuses.
Putting the guard in the date generator would have been tidier and would have
made the port disagree with upstream about a date list.

### Where this leaves the bootstrap

`zcswap.c` is next, and it is now unblocked. It is also where both of the last
three slices' findings land: UPSTREAM-29's zero-period schedule and
UPSTREAM-32's empty pay-date list are reached by the *same input* — a benchmark
maturing on the curve's value date — through two different call sites in that
one file, which compares `matDate` against `zc->valueDate` nowhere at all.

## Decision 38 — The bootstrap, and an oracle a tolerance cannot express

`zcswap.c` is the module every slice since the start has been clearing the way
for: adding swap benchmarks to a zero curve, with a Brent solve per instrument.
940 lines, two exported functions and three statics.

### Five earlier findings converge here, and porting it retires them

This is the file where most of the register's reachability arguments end up:

| finding | site in zcswap.c |
|---|---|
| UPSTREAM-21/22 | `JpmcdsZCAddCashFlowList` on the par-float path |
| UPSTREAM-23 | `JpmcdsZCAddCFLPoints`, called on both paths |
| UPSTREAM-29 | lines 724, 743, 744 — `floatDL->fArray[floatDL->fNumItems-1]` |
| UPSTREAM-32 | line 475 — `JpmcdsZCGetSwapCFL(zc->valueDate, matDate, ...)` |

The last two share one trigger — a benchmark maturing on the curve's own value
date — and `zcswap.c` compares `matDate` against `zc->valueDate` **nowhere**.
Three slices of work, two separate memory defects, one input.

Both are unreachable in the port, and not because this module guards against
them: the functions they live in refuse to produce the degenerate value at all.
That is the argument for pushing guards down to where the invariant lives
rather than up to where the caller is.

### UPSTREAM-33, and why positive controls were the whole test

The solve writes into `&zc->rate[zc->numItems-1]` — the point just added, *if*
that point is last. `JpmcdsZCAddRateAndDiscount` inserts in sorted position.
So for a benchmark inside an existing curve the solver varies an
already-solved point and leaves the new one at its placeholder.

My first sweep: 1,400 combinations, **all 1,400 failed.** I nearly wrote that
up as "upstream refuses, low severity" and moved on.

That would have been worthless, because *a broken fixture fails 1,400 times
too.* My very first single probe had also failed, for a reason I never
diagnosed. So before believing the sweep I added positive controls: the same
call with the benchmark **beyond** the curve's end. Those succeed. Only then
did the failures mean anything.

This is the same hole as the differential test two dozen decisions ago that
reported zero differing cases because I had swept two inputs together. The fix
then was `EXPECT_GT(differing, 0)`. The fix here is a control arm. **A sweep
without a positive control measures the fixture, not the code.**

And the honest ending: I tried and failed to produce a silently wrong curve.
Varying a distant zero rate barely moves a shorter swap's PV, so Brent never
converges on the wrong point — it just gives up. The consequence is a
misleading *"Root finder failed"* where the real problem is the benchmark's
position. Filed low, and the entry says plainly what it could not demonstrate.

### The oracle that caught what the vectors could not

The recorded rates here are **roots**. They are bounded by the solver's
stopping criterion — `|f| <= 1e-11`, no x criterion, 50 iterations — not by
rounding. Two implementations taking different Brent step sequences can land on
different points of the same flat region, so demanding 1e-12 agreement would be
demanding they iterate identically. The tolerance is 1e-7 on a rate, the
loosest in the tree.

Which makes the recorded numbers a weak check, so the replay also asserts the
property the solve **exists** to establish: every benchmark must re-price to
par off the curve built from it. That is an oracle, not a tolerance. It would
catch a port that reproduced every recorded rate and still built a curve that
prices nothing.

It immediately earned its place by failing — on strip 1 and only strip 1, the
off-cycle benchmarks, all four of them, at every frequency. On-cycle passed
completely. That pattern is a fingerprint: I had hardcoded `stub_at_end = true`
in the test while `add_swaps` derives it from `is_end_stub` for off-cycle
maturities. A test bug, not a port bug — but a test bug the recorded-rate
comparison had no way to notice, because it compared the port against itself
via the vectors and never asked whether the answer was *right*.

With the stub position derived the way the bootstrap derives it, all 96 cases
re-price to par within 1e-10.

### What the port changed, and one thing it deliberately did not

`X_TOLERANCE = DBL_MAX` looked like a bug for about a minute. Brent's
acceptance test is a **conjunction** — `|f| <= fTol && |dx| <= xTol` — so
DBL_MAX is an idiom meaning "judge convergence on f alone". Checking the
direction of that `&&` before filing saved an embarrassment.

Better: it switches off a quirk I recorded in `root_find.hpp` several slices
ago, where the initial guess is accepted if `|y0| <= fTol` **and** the guess
lies within `x_tolerance` of either bound — proximity to a bound standing in
for a convergence test that cannot run on the first iterate. With DBL_MAX that
disjunction is always true and acceptance reduces to `|y0| <= fTol`, which is
the sensible rule. The one caller in the library that sets this parameter sets
it to the value that neutralises the quirk. Documented then, resolved now.

Three dead locals in `JpmcdsZCAddSwaps`: `newDates` ("Swap dates w/
synthetics") never read and never even freed, `newRates` freed at `done:` while
provably still NULL, `useFastZC` set once and never read. A synthetic-swap
feature was removed and its variables stayed — the same shape as `streamcf.c`.

### Where this leaves the project

`gtozc` (494 lines) and `zerocurve` (135) are what remain on the main path,
plus three independent leaves. The bootstrap works and proves itself: 96
benchmark strips, both paths, every instrument back to par.

## Decision 39 — A good pattern with one bad check in it

`gtozc.c` is 494 lines and two exported functions — the layer a caller actually
drives, with the bootstrap underneath. It produced one defect, and the
interesting part is that the defect is *not* the thing that looks wrong.

### The pattern I had already praised

Both static validators in the file accumulate failures instead of returning at
the first one:

```c
int status = SUCCESS;
if (zeroCurve == NULL) { JpmcdsErrMsg(...); status = FAILURE; }
if (numRates < 0)      { JpmcdsErrMsg(...); status = FAILURE; }
...
return status;
```

Three slices ago I wrote approvingly about exactly this shape in
`streamcf.c`'s `CheckFixedStreamDetails` — it reports every problem with the
input rather than only the earliest, which is strictly more useful to whoever
has to fix the input.

Here the same shape dereferences the NULL it just reported, because the loop
that follows reads `zeroCurve->fBaseDate`. `CheckZCSwapsInputs` is worse: its
dereference is `JpmcdsIsBusinessDay(zeroCurve->fBaseDate, ...)`, not inside a
count-guarded loop, so it runs whatever the instrument count is.

So the rule I needed was narrower than "return early", which would have thrown
away a pattern that is right:

> **An accumulating validator is safe only when no check guards another.**

`is_well_formed` in `stream_cash_flows.hpp` accumulates safely because its
checks are independent. These two do not, because the NULL test is a
precondition for the dereference below it. Where a check *is* a precondition,
it must return — and then it stops being an accumulating validator for that
input. Upstream wanted both properties and got neither.

One edit fixes both sites, so one register number. UPSTREAM-19's counting rule,
applied for the third time.

### The port retires it by having no pointer

Both port functions take `const ZeroCurve &`. That is now the third defect in
this register closed by a reference rather than a check, and I think that is
the single highest-leverage change the port has made: a parameter that cannot
be null needs no validation, no documentation of the precondition, and no
discipline from the caller.

### A property test that refuted its author

I wrote a tier-1 property claiming every point of the input curve survives into
the output — both entry points promise it, *"don't want to replace anything in
the existing curve"* — and the recorded data said no.

`JpmcdsZCAddPrefixCurve` copies the points BEFORE the cash curve's first date
and `JpmcdsZCAddSuffixCurve` the ones AFTER its last. **Nothing copies the
middle.** A caller's curve point that happens to fall inside the span of the
cash quotes is silently discarded.

That is upstream's behaviour, reproduced. The rewritten property asserts the
real rule both ways — outside the range the point must survive, inside it must
not — with a counter requiring at least one interior case, because a property
that is only ever exercised on one side of its own boundary is half a test.

This is the second time this session a property test has corrected me rather
than the code. The par-reprice oracle in the previous slice did the same. Both
times the recorded-vector comparison was perfectly happy, because it compares
the port against itself and never asks whether the answer is right.

### Two validators in one file disagreeing

`CheckZCCashInputs` tests `dateArr[i-1] > dateArr[i]` — equal dates permitted.
`CheckZCSwapsInputs` tests `dates[i+1] <= dates[i]` — equal dates rejected.
Same file, same question, opposite answers. Both reproduced.

The cash path's duplicates are refused later by the curve itself
(`JpmcdsZCFindDateExact`), so the two paths agree on acceptance by accident
rather than by rule — which is worth a test precisely because the accident
could stop holding.

### The dead inventory, continued

`JpmcdsZCSwaps` takes `char fwdLength` — *"For fwd smoothing, length of
fwds"* — which appears three times in the file: the static declaration, the
definition, and the call that hands it to the validator. **None of the three
reads it.** Forward smoothing was removed and the parameter stayed.

It also declares `TInterpData *interpData = NULL` and
`TBadDayList *badDayList = NULL`, assigns neither, passes both down, and calls
`FREE(interpData)` on a pointer that is provably still NULL. That is the third
always-NULL free in this port.

I have stopped finding these interesting one at a time, and started finding the
*count* interesting: four files in a row now — `fltrate`, `streamcf`,
`zcswap`, `gtozc` — each carrying the parameters of a feature that was removed.
The model shipped with the signatures of a larger library and the body of a
smaller one.

### Where this leaves the project

`zerocurve.c` is 135 lines and the last file on the main path. After it the
only in-scope work left is three independent leaves.

## Decision 40 — The top of the stack, and the byte that is legal by accident

`zerocurve.c` is 135 lines and one function, `JpmcdsBuildIRZeroCurve` — the
only entry point in the library a caller reaches without knowing anything about
ZCurves, stub curves or bootstraps. It is the last file on the main path.

### UPSTREAM-35, and the detail that makes it survive review

```c
instr = toupper(instrNames[i]);
```

`instrNames` is a `char *`, plain `char` is signed here, and C requires
`toupper`'s argument to be representable as `unsigned char` or to equal `EOF`.
A high-bit byte is neither. `(char)0xE9` reaches `toupper` as −23.

Textbook stuff. What made it worth writing up properly is the table I printed
while measuring it:

| byte | as `char` | permitted? |
|---|---|---|
| 0x7F | 127 | yes |
| 0x80 | −128 | **no** |
| 0xE9 | −23 | **no** |
| 0xFF | −1 | **yes** |

`0xFF` is −1, which *equals `EOF`*. So of the 128 high-bit bytes, **exactly one
is legal, by collision.** And it is the last one — the obvious thing to test.

I did not set out to find that; I printed the whole range because printing a
range is cheap, and the boundary turned out to be interesting. That is the
third or fourth time this project has got something from sweeping a domain
rather than sampling it, and the cost each time has been about a minute.

No consequence is demonstrated: the instrument check rejects the character
immediately afterwards, and glibc pads its tables for this mistake by
convention. UBSan has no check for it. So this is the second register entry
that is UB the harness cannot see, after UPSTREAM-31 — and the contrast between
them is instructive. UPSTREAM-31 needed cross-translation-unit reasoning to
identify at all. This one is in every C style guide. The harness misses both,
for entirely different reasons.

There is a second reason not to write it this way that has nothing to do with
signedness: `toupper` consults the locale. A function deciding whether a
benchmark is a swap should not depend on `LC_CTYPE`.

### An overload I had to add because of a decision made twelve slices ago

`zerocurve.c` starts with `JpmcdsNewTCurve(valueDate, 0, 1.0, ACT/365F)` — a
curve with **zero points** — and feeds it through both stages. The port's
`ZeroCurve` is non-empty by construction, decided back in the zero_curve slice.

So the function I merged one slice ago, `curve_with_cash(const ZeroCurve &,
...)`, cannot be called the way the library's own top-level entry point calls
it. I had noted the empty-curve gap in that header and said "building from
nothing goes through ZeroCurveBuilder" — which was true, and which turned out
not to be enough, because the *next* file up needs exactly that path.

The fix is additive: overloads taking a `Date value_date` where the others take
a curve. Those are what upstream's empty-curve branch actually means, written
as their own entry points rather than as a state the type forbids.

Worth recording as a cost of the invariant. Making `ZeroCurve` non-empty has
paid for itself repeatedly — UPSTREAM-21, 22, 29 and 32 all die on
non-emptiness somewhere — and the bill came due here, one slice late, as two
overloads. That is a good trade, but it is a trade, and I would rather the
journal say so than imply the invariant was free.

### The dead-parameter inventory closes with a confession

```c
char    fwdLength = '3';   /* not used */
```

Four files in a row now — `fltrate`, `streamcf`, `zcswap`, `gtozc` — carry the
parameters of a feature that was removed. This is the only place where somebody
noticed and wrote it down **at the call site** rather than deleting it.

I have stopped finding these interesting individually and started finding the
count interesting. The model shipped with the signatures of a larger library
and the body of a smaller one, and this comment is the moment that becomes
explicit rather than inferred.

### Two tests that needed their names fixed

`InterleavingTheInstrumentKindsChangesNothing` asserted a refusal. The claim in
the name and the claim in the body were opposites, and the test passed, because
it was asserting the body's claim. Renamed to
`ADecreasingBenchmarkListIsRefused`, which is what it checks: order *within*
each instrument group is load-bearing, order *between* the groups is not.

And I deleted a test called `ThreeParallelArraysBecomeOneSpanOfStructs`, which
could only ever have asserted that a struct has the members it is declared
with. That is a tautology dressed as a test — the same thing I caught myself
writing four slices ago as `EXPECT_TRUE(x || !x)`. The header comment carries
the claim; nothing executable needs to.

### Where this leaves the project

The main path is done. `JpmcdsBuildIRZeroCurve` works end to end, and its 96
recorded curves are the closest thing this project has to an end-to-end oracle:
one call exercises money market, the cash merge, swap schedules, stream cash
flows, the Brent solve and the curve container together.

Three independent leaves remain in scope: `linterpc` (93 lines), `cxbsearch`
(180) and `cx` (230).

## Decision 41 — Four occurrences make an idiom, not four accidents

`linterpc.c` is 93 lines and one function: re-express a zero curve on dates the
caller chooses. It is the smallest file left and it produced the most useful
observation in a while, because of where it falls in the sequence rather than
anything about the file itself.

### The fourth one

```c
modDates[idx] = MIN(desiredDates->fArray[idx],
                    oldCurve->fArray[oldCurve->fNumItems-1].fDate);
modDates[idx] = MAX(modDates[idx],
                    oldCurve->fArray[0].fDate);
```

`JpmcdsNewTCurve` accepts a count of zero — only a negative one is refused —
and leaves `fArray` NULL. So both lines read through NULL, and `zerocurve.c`
builds exactly such a curve as its first act.

That is now four:

| entry | file | shape |
|---|---|---|
| UPSTREAM-21 | `zcall.c` | read, `NULL[-1]`, SIGSEGV |
| UPSTREAM-29 | `schedule.c` | the zero-period list that enables one |
| UPSTREAM-32 | `zcswutil.c` | read-modify-**write**, `NULL[-1]` |
| UPSTREAM-36 | `linterpc.c` | read, `NULL[-1]` *and* `NULL[0]` |

I filed the first three as individual findings, which they are. The fourth is
what makes them a *result*: four sites, four files, and in every case the
author knew their own count was positive while that knowledge lived nowhere a
compiler or a later reader could check. **This is what the absence of a
non-empty container type costs, billed four times.**

### The accounting I want to be honest about

The tempting way to write this up is "the port fixed four defects". It did not.
It made one type non-empty, in the `zero_curve` slice, long before three of the
four were found. Nothing was anticipated; a decision made for clarity turned
out to be load-bearing three times afterwards.

And it was not free. One slice ago, in `zerocurve`, the bill arrived: the
library's own top-level entry point starts from a zero-point curve, which the
port's type forbids, so I had to add two overloads taking a value date instead
of a curve.

**Four defects retired for two overloads.** That is the best trade in this
register, and stating it as a trade rather than as a free win is the only way
the number means anything. A reader who takes "make it non-empty" as costless
advice will be surprised at exactly the moment I was.

### A property that catches a plausible wrong port

The clamp here is subtler than it looks. It applies to the date the
interpolation is *performed* at; the date *stored* in the new curve is the one
the caller asked for. So a date past the old curve's end comes back carrying
the final rate **at its own date** — a flat extension, not an extrapolation.

A port that clamped both would produce entirely plausible rates on slightly
wrong dates, and the recorded-rate comparison would pass for every case where
the requested dates happen to lie inside the curve. So the tier-1 property
asserts the stored dates are exactly the requested ones, and the generator
includes two sets that fall wholly outside the curve so the clamp is actually
exercised. I kept the two date vectors separate in the implementation for the
same reason: conflating them has to be a visible edit, not a one-character
slip.

### Two more entries in the dead-parameter ledger

```c
void *interpData                    /* (I) Interpolation data-not used */
```

Third annotated dead parameter, second annotated at the *declaration* rather
than at a call site. Five files now.

And `JpmcdsLinInterpCurve` has **no caller inside the library at all** — it
appears once as `TCurveInterpFunc JpmcdsLinInterpCurve;` in `interp.h`, which
is a declaration against a function-pointer typedef, not a call. That is the
third exported-but-uncalled function in this register, after
`JpmcdsGenerateUnadjDates` and `JpmcdsZCValueFixFltSwap`, and the correlation
is getting hard to ignore: **the functions nothing inside the library calls are
the functions nothing inside the library validates.** An external caller gets
the least-tested surface, which is the opposite of what a published API should
offer.

### Where this leaves the project

Two leaves left: `cxbsearch` (180 lines) and `cx` (230).

## Decision 42 — An invitation the code does not honour, and a test that was wrong

`cxbsearch.c` is 180 lines and one function: find a value in a sorted array and
report the whole run of equal elements. Second-to-last leaf.

### UPSTREAM-37, which is a different shape from the rest

The main path treats two of its three output pointers as optional:

```c
if (loBound != NULL) { ... }
if (hiBound != NULL) { ... }
```

Nothing in the header says NULL is acceptable. **That guard is the only
statement on the subject, and it says yes.** Three early-return paths then
write through the same pointers with no guard, so a value outside the array —
or an array of one element — stores through NULL.

Almost everything else in this register is a *missing* check. This is an
*inconsistent* one, forty lines apart in a single function, and I think that is
worse. A missing check is a precondition: it can be documented, and a caller
who violates it has made a mistake. A check on one path and not another is an
invitation the code does not honour. The guarded path teaches the caller NULL
is fine; three others punish them for believing it. **The caller did nothing
wrong, and there is no document they could have read that would have saved
them.**

Reachable only through the public header, since both in-library callers pass
all three. That is the fourth time the externally-reachable surface is the
unvalidated one, after UPSTREAM-25, 30 and 36.

### Three outputs become one range, and the invariant becomes a tautology

Upstream documents how its three values relate:

> `hiBound-loBound-1` should give the number of exact matches in all cases, and
> everything in the range `[loBound+1, hiBound-1]` inclusive should be an exact
> match.

That invariant *is* the answer. So the port returns `MatchRange{first, count}`
and the arithmetic stops being a promise each caller re-derives. The sentinels
go with it: `loBound == -1` and `hiBound == arraySize` are not "absent" values,
they are one position past each end and load-bearing for the subtraction, which
a half-open range expresses without signalling anything.

And the defect evaporates as a side effect. There are no output pointers, so
there is nothing to guard on any path.

### `exact` is "A value", and the article is load-bearing

Upstream documents `exact` as *"the index of A value which is equal"*. I
suspected the indefinite article was doing real work, traced the code, and then
**measured it rather than trusting the trace**:

```
{10,10,20,30} seek 10  ->  exact == 1, while the first match is 0
```

The code picks whichever of the two bracketing indices matches, which need not
be the first of a run. So the differential test asserts upstream's `exact` lies
*within* the port's range rather than equalling its start — comparing them
directly would be asserting something upstream never promised, and would have
failed on exactly one shape out of the hundreds swept.

### The test I wrote that was simply wrong

There is a branch reporting *"x array not in increasing order"*, reached when
the `arraySize + 1` iteration cap expires. I wrote a test asserting that at
least one unsorted array would be refused by it.

**It failed. Not one was refused** — across an exhaustive sweep of every
unsorted array of length 2 to 4 over the alphabet 0..3. The cap is
`arraySize + 1` iterations on a halving search, which is roughly exponentially
generous: a runaway backstop, not a detector.

So disorder produces a confident wrong answer. Which is exactly what the
function's own documentation says it will — *"the output is undefined. No
checks are performed on the order of xArray"* — so the branch is dead code
rather than a broken check, and the documentation is honest where the code
merely looks careful. Not filed; recorded, because the branch reads like a
safety net and is not one.

That is the third time this session a claim of mine has been corrected by
measuring it instead of reasoning about it, and the second time the correction
came from a *test I wrote to confirm my own belief*. The pattern is clear
enough to state: when I write a test whose purpose is to confirm something I
already believe, I should expect it to fail about a third of the time.

### Exhaustive, because it was affordable

The domain here is small enough to enumerate: every non-decreasing array of
length 1 to 5 over a four-symbol alphabet, against every sought value in and
around it. 125 arrays, 750 recorded cases, and the tier-2 sweep goes further —
461 arrays over five symbols, 3,227 comparisons.

So this is one of the few modules where "the port agrees with upstream" can be
said with no qualifier about which inputs were tried. Where a domain is small
enough to enumerate, enumerating it beats choosing representatives, and the
cost here was nil. The tier-1 replay asserts the exact case count rather than a
floor, because "exhaustive over this domain" is a claim worth pinning — a
threshold like `> 2000` would have silently accepted a sweep that stopped
early, which is precisely the mistake I made on the first attempt (I asserted
`> 2000` against a domain whose true size was 750).

### One cosmetic defect, deliberately not filed

```c
JpmcdsErrMsg("%s: x array not in increasing order.n", routine);
```

`.n` for `.\n`. The log line never ends. Not filed: this register is for
things that change an answer or touch memory, and a missing newline does
neither. Written down anyway, because a reader comparing the two files will
notice it and should find the reason for its absence rather than infer one.

### I ported the same function twice, and every gate said fine

This is the real finding of the slice, and it is about the project rather than
about upstream.

After writing `match_range.hpp`, `port_progress.py` showed the function count
unchanged at 148 while the completed-file count went up. That should be
impossible — a newly ported function has to move the total — so I went looking,
and found `JpmcdsBinarySearchLong` already claimed by `zero_curve.hpp`, as a
`search` member returning upstream's three-value `{exact, lo, hi}` struct.
Written months earlier, for `cxzerocurve.c`.

**Two implementations of one function, and nothing noticed.** Both compiled.
Both passed their own tests. The provenance gate was satisfied twice over,
because it asks "does every header name its source?" and both did.
`port_progress.py` reported 100% either way, because it counts a function as
done when *any* header claims it. The duplicate was invisible to every
instrument the project has.

It was caught by an arithmetic oddity in a progress report — a number that did
not move when it should have. That is luck, not method.

### The fix, and the gate

`ZeroCurve::search` had no external caller and no test of its own; it was used
only inside `zero_rate`. So it was the one to delete, and the general port in
`match_range.hpp` — swept exhaustively — is the one to keep.

Deleting it had a second benefit I did not expect. The shape it returned was
three values tied together by a documented subtraction, with `-1` and `size` as
sentinels: **precisely the shape UPSTREAM-37 lives in.** Keeping it would have
preserved, inside the port's own public interface, the hazard the port exists
to remove.

Rewiring `zero_rate` through `match_range` needed the projection overload
`bsearch.hpp` promised four slices ago and I had declined to write
speculatively — a curve's dates are not contiguous in `points_`. Now there is a
caller, so it is no longer speculative, and it replaces upstream's byte `skip`
stride with a function of the element, which a caller cannot get wrong by
passing the size of the wrong type.

All 707 tests passed after the rewiring. `zero_rate` is the curve's core and
heavily covered, so that is real evidence rather than silence.

Then the systemic part: `verify.py claims`, which fails if any upstream
function is named by two headers. The existing provenance gate catches a
function nobody claims; this catches the opposite. I proved it fires by
planting a duplicate claim in `date.hpp` and watching it exit 1.

And I managed, while adding the gate that detects duplicates, to insert its
dispatch-table entry twice. Which is funny, and also the point: the class of
mistake is not rare or exotic, it is what happens whenever a fact has to be
maintained in two places by hand.

### The score on provenance gaps

Three times now a provenance omission has cost real work:

* Decision 30 — four functions ported but unclaimed, so the audit reported gaps
  that were not gaps;
* Decision 34 — the same again, which is what made it a class rather than an
  incident;
* this slice — a function claimed twice, so the audit reported completeness
  that was not completeness.

The first two were over-reporting of work remaining. This one was
under-reporting, and it is the worse direction: over-reporting costs a minute
of checking, while under-reporting cost a whole module written for nothing. The
tooling now checks both directions.

### Where this leaves the project

One leaf left: `cx.c`, 230 lines.

## Decision 43 — The gate I built last slice found a bug in the slice before it

`cx.c` is 230 lines and four functions: the constructors for the two CDS legs.
It is the last file in the port that had not been started, and it closed out
with two findings — one about upstream, one about a function I had already
merged.

### The gate earned itself on first use

I added `verify.py claims` last slice, after discovering I had ported
`JpmcdsBinarySearchLong` twice. The first time I ran it on new work it failed:

```
FAIL: the same upstream function is claimed by more than one header
  JpmcdsContingentLegMake: cds.hpp, contingent_leg.hpp
```

Part of that was a false positive of my own making. `cds.hpp` said *"Replaces
`JpmcdsCdsContingentLegMake`, which delegates to `JpmcdsContingentLegMake` in
cx.c"*, and the gate cannot tell a claim from a prose mention inside the same
sentence. Fixed by writing the provenance precisely — the mention moved to its
own sentence. A gate that forces precise claim text is doing its job, not
misbehaving.

**But underneath it was a real bug.** `cds_contingent_leg` in `cds.hpp` applied
the one-day `protect_start` shift itself, duplicating what `cx.c`'s constructor
does — and **omitted the `REQUIRE (endDate > startDate)` that comes with it.**
So the port accepted a zero-length leg with `protect_start == false`, where
upstream refuses it. Merged, green, and wrong.

The fix was to delegate to `make_contingent_leg`, which meant
`cds_contingent_leg` had to return `std::optional` and propagate — five call
sites, two of them in headers. All 707 tests passed afterwards.

Three things worth noting about how that was found. It was not found by a test:
the differential suite fed both sides a pre-built leg, so construction was
never compared — the same blind spot the `ContingentLeg` comment already
records from an earlier slice. It was not found by reading. It was found by a
gate about *provenance text*, which has no opinion about correctness at all.
**The gate detected a duplicated claim, and the duplicated claim was a symptom
of duplicated logic, and the duplicated logic had drifted.** That chain is
worth remembering: duplicate provenance is a smell for duplicate behaviour.

### The function that exists, works, and cannot be called

`JpmcdsFeeLegMake` copies three caller-supplied arrays into a fee leg. It is:

* defined in `cx.c` with **external linkage**, so it occupies the symbol table;
* declared in **no header** — `cx.h` declares `JpmcdsFeeLegMakeEmpty` and
  `JpmcdsFeeLegFree` and skips this one;
* called from **nowhere**. It appears exactly twice in the entire source tree:
  its own definition and its own `routine[]` string.

So the only constructor that builds a fee leg safely is unreachable without
hand-writing its prototype. The library's actual method is
`JpmcdsFeeLegMakeEmpty(n)` — which hands back three zeroed parallel arrays —
followed by poking them directly, which `cds.c` does at line 192.

That also settled a defect I had started writing up. `JpmcdsFeeLegMake` copies
each array only `if (nbDates > 0 && array != NULL)` and otherwise stores NULL,
while `nbDates` keeps whatever was passed. So `(3, NULL, ends, pays)` returns
SUCCESS with a count of three and a NULL array, and `JpmcdsFeeLegPV` reads
`accStartDates[0]` whenever `nbDates > 1`. A negative count is stored as given.
All measured.

**Not filed.** The register's rule is that reachability bounds severity, and a
function with no declaration anywhere is the floor of that scale — a caller
would have to guess the signature to reach it. Recording the reasoning matters
more than a 38th number: the next person to read `cx.c` will find the same
inconsistency and should find the argument too. The differential test reaches
it through a hand-written prototype, which is the only way anyone could.

### The validation order that is not an accident

`JpmcdsContingentLegMake` applies the `protectStart` shift and *then* checks
`endDate > startDate`. So the flag decides whether a zero-length leg is legal:
with it the stored start is `start - 1` and `end == start` passes; without it
the same input is refused.

That looks like a bug caused by ordering and is not one. Protecting on the
start date makes a single-day contract meaningful, so the asymmetry is correct.
Reproduced, and pinned by an exact count rather than a threshold: over the same
grid, 60 spans are accepted with `protect_start` and 48 without, and the
difference of 12 is precisely the zero-length cases. A port that validated
*before* shifting would pass every recorded case except those.

### Two thresholds I wrote without computing them

Second time this session. In `match_range` I asserted `compared > 2000` against
a domain whose exhaustive size was 750. Here I asserted `unshifted > 50` where
the true count is 48 — and the true count is not an arbitrary number, it is
`4 anchors x 3 notionals x 4 accepted offsets`, which I could have worked out
in ten seconds.

Both times the test failed, I looked, and the code was fine. That is the
failure mode of a lazy threshold: it does not hide a bug, it manufactures a
false alarm and costs the time to dismiss it. Worse, a threshold set too LOW
hides a sweep that stopped early, which is the mistake that actually matters.
Where a count is computable, assert the count.

### Where this leaves the port

**Every in-scope file is now started, and 31 of 42 are function-complete.**
149 of 171 in-scope functions = 87%, 63% of lines in complete files.

The remaining 22 functions are stragglers across 11 already-claimed files, and
they are not a dependency chain: four year helpers in `ldate`, three list
operations in `dtlist`, two in `cxdatelist`, the three string parsers
(deliberately held together because UPSTREAM-4 lives in that family), the five
holiday-cache functions that are recorded non-ports, and five one-offs. A
cleanup sweep rather than a path.

## Decision 44 — The audit was lying, and it was my fault three times

This slice began as a cleanup sweep over the 22 functions `port_progress.py`
listed as remaining. Two of them turned out to be already ported.

### The pattern, now unambiguous

* `JpmcdsStringToDateInterval` — ported in `date_interval.hpp`, claimed with
  *"Reproduces"*.
* `JpmcdsSortDateList` — ported as `sorted_distinct` in `date_list.hpp`,
  claimed with *"This is upstream's"*.

The claims parser recognises `Replaces` and `Equivalent to`. Neither of those
phrasings matched, so both functions were ported, tested, merged — and
reported as outstanding work. Fixing two sentences moved the count from
149/171 to 151/171.

That is the third time. Decision 30 found four such functions, Decision 34
found four more and concluded *"twice is a class, not an incident"*, and the
`cxbsearch` slice found the inverse — a function claimed twice, which cost a
whole module written for nothing.

I have now fixed the symptom three times. The thing I had not done is make the
distinction visible in the one place I actually read.

### The fix that targets the mistake rather than the instance

`port_progress.py` printed *"these functions are not named"*. I read that,
three times, as *"these functions are not ported"*. Those are different claims
and the tool had the information to tell them apart: a name that already
appears somewhere in a header is almost always a missing sentence; a name that
appears nowhere is almost always genuine work.

So it now splits them:

```
  cxdatelist   2/4    475 lines
    not ported:   JpmcdsDateListAddDates
    *** MENTIONED but not claimed -- check before porting: JpmcdsDateListAddDatesFreeOld
```

On its first run the new flag produced one true positive and one false one,
which is the right ratio for a heuristic whose message is *check*, not *fix*.

The true positive was better than I expected. `JpmcdsCdsFeeLegFlows` carried a
note in `cds.hpp` saying it was not ported because it *"needs cashflow.c, which
is layer 7 and not on the pricing path"*. **That reason had expired.**
`cashflow.c` has since been ported in full, so the dependency is available and
only the work remains. A deferral with a reason attached had quietly become a
deferral with no reason, and nothing would have noticed — a stale justification
reads exactly like a live one.

### An honest denominator, briefly lost

Adding a function-level `NON_PORTS` set had a side effect I nearly shipped.
`buscache.c` is 726 lines and all five of its functions are deliberate
non-ports, so once they were excluded the file had zero countable functions and
the report called it **complete** — and counted its 726 lines toward "lines in
function-complete files", which jumped from 63% to 81%.

That is precisely the flattering denominator the script's own docstring warns
against: *"Stated so the denominator is honest rather than flattering."* Nothing
about those 726 lines was completed; they were declined.

`buscache` is a file-level decision, so it belongs in `OUT_OF_SCOPE` where its
lines leave the denominator entirely rather than counting as done. With that
corrected the figures are 155/164 functions and 80% of lines — and the 80% is
real.

Worth noting how close that came to being a quietly inflated number in a
progress report I write myself and nobody audits.

### Porting four functions nothing needs

`ldate.c`'s four year helpers — `JpmcdsYear`, `JpmcdsIsLeap`,
`JpmcdsYearStart`, `JpmcdsDaysLeftThisYear` — exist to serve
`JpmcdsDayCountFraction`'s ACT/ACT branch, which walks whole years adding 365
or 366. The port's `year_fraction` intersects the period with each calendar
year instead, so **nothing in the port needs any of them.** That is why they
sat unclaimed for most of the project.

Ported anyway, because all four are declared in `ldate.h` and are therefore
public API: an external caller using the model's date layer can reach them even
though the library no longer does. The alternative was to record four
non-ports, which would have been a defensible decision and a worse one — this
is a port of a published interface, not only of a pricing path.

### The verification is better than usual, and the reason is a property of the
### functions rather than of my diligence

Three of the four have a **definition**, not just a reference implementation.
The Gregorian leap rule is "divisible by 4, except centuries, except multiples
of 400". The start of a year is the first of January. The year of a date is its
year.

So the tier-1 tests do not replay recorded upstream answers. They check the
rule, written out longhand, over all 800 years from 1601 to 2400 — and
separately check that a leap year is exactly a year of 366 days, which compares
the predicate against the calendar's own length rather than against another
predicate. That is a genuinely independent oracle, the highest rung the ladder
offers, and it is available here only because functions this small have a
closed form. Most of this project does not get that luxury.

There is no golden file for this module, and the test header says why.

### Two more predictions corrected by measuring

Fourth and fifth time this session, and both were in test *expectations* rather
than in the port:

1. I asserted 50 leap days in 1896–2104. The answer is 51: there are 53
   multiples of four in that range, not 52, minus 1900 and 2100.
2. I asserted that 31 December gives one day left under ACT and **zero** under
   30/360 — the clean illustration of the function's misleading name. It gives
   **one** under both. 30/360 USA adjusts D1 from 31 to 30, so the difference
   is `360 - 330 - 29 = 1`, and the two conventions agree exactly where I
   expected them to diverge. The honest illustration is 1 January: 365 real
   days against 30/360's 360.

Both times the port matched upstream and only my arithmetic was wrong. The
tally for the session is now five, which is high enough to stop being a
curiosity: **when I write down a number I have not computed, it is wrong about
a third of the time.** The tests now carry the computation next to the
assertion.

## Decision 45 — Four functions I thought were missing, and one typo that wasn't

This slice was meant to be a quiet sweep over the date-list stragglers. Two
of the four were already ported, one of my "typo" theories was wrong, and the
most useful thing found was about the port rather than upstream.

### The doubled prefix is upstream's

`date_list.hpp` claimed `JpmcdsJpmcdsDateListAddDates` and
`JpmcdsJpmcdsDateListTruncate`. I saw the doubled `Jpmcds` and assumed my own
typo — which would have explained neatly why `JpmcdsDateListAddDates` was
reported unported.

It is not a typo. **All three doubled-prefix names are real exported symbols:**

```
_JpmcdsJpmcdsDateListAddDates
_JpmcdsJpmcdsDateListTruncate
_JpmcdsJpmcdsZCPresentValueCFL
```

A renaming pass ran twice over `dtlist.c` and once over `cxdatelist.c`, and
the result shipped. `cash_flow_bootstrap.hpp` already said so about the third
one — *"the doubled prefix is in the..."* — which I had written months ago and
forgotten.

So upstream exports and **publicly declares two of each operation**:

```
dtlist.h:92      JpmcdsJpmcdsDateListAddDates(dl, n, dates, allowDuplicates)
cxdatelist.h:45  JpmcdsDateListAddDates      (dl, n, dates)
```

And they are not copies. The four-parameter version takes a flag; the
three-parameter one is that flag set FALSE. Measured on `{100,200,300}` plus
`{200,400}`:

```
doubled, allowDuplicates TRUE : 100 200 200 300 400
doubled, allowDuplicates FALSE: 100 200 300 400
single (no such parameter)    : 100 200 300 400
```

So the port's one `add_dates` covers both, and `allow_duplicates = false` IS
the cxdatelist entry point. The same holds for the two truncates and for
`JpmcdsDatesFromCurve` / `JpmcdsNewDateListFromTCurve`. **`cxdatelist.c` is a
partial re-implementation of `dtlist.c` under slightly different names**, and a
caller choosing by autocomplete gets whichever.

### Nothing was implemented, so the tests are the whole slice

Five functions were claimed against code that already existed. A claim without
a test is an assertion, so the substance here is a differential suite that
earns them: the two upstream adds compared against each other over a swept
domain, a test that the flag ON makes them **differ** (otherwise the
equivalence claim would be vacuous), and both curve-date extractors against
`ZeroCurve::dates()`.

The count went 155 → 160 of 164, and 98% is now a number about claims rather
than about code.

### The audit flag caught my own fresh writing

I documented the three vanished functions in prose — *"`JpmcdsExtendDateList`
grows a list in place..."* — without the word `Replaces`. The
mentioned-but-not-claimed flag I added one slice ago fired on all three
immediately:

```
  dtlist       6/8    685 lines
    *** MENTIONED but not claimed -- check before porting: JpmcdsExtendDateList, JpmcdsNewDateListFromDates
```

That is the tool working on the author of the tool, within a day of its being
written, on text written after the lesson that motivated it. Which is the
argument for gates over discipline in one line.

### The finding that is about the port

`JpmcdsExtendDateList` grows a list and leaves the new slots at TDate 0 —
1601-01-01, a real date rather than an obviously-unset one. The familiar
theme, so I wrote a test asserting that `std::vector<Date>::resize` inherits
the same property and that the port therefore matches.

**It does not.** `Date` is `std::chrono::sys_days`, so a default-constructed
one is the **UNIX** epoch: 1970-01-01, which is 134,774 days after the ISDA
epoch. The two implementations disagree about what an unset date is, and
neither value is invalid — upstream's reads as 1601-01-01 and the port's as
1970-01-01, and a contract could plausibly carry either.

That reaches well beyond `resize`. The port's aggregates — `CouponDates`,
`FeePayment`, `SwapDate` — all default-initialise their `Date` members, so
**1970-01-01 is the port's silent "unset" everywhere.** Every constructor sets
every field, which is why nothing has ever depended on it. But I had carried an
unexamined belief that the port's unset date was the epoch, and it is not, and
that belief could have become load-bearing in any slice where a default
mattered.

Seventh prediction corrected by measurement this session. The first six were
about upstream or about test arithmetic; this one was about my own code, which
is the more alarming direction.

### Where this leaves the port

**160 of 164 in-scope functions = 98%**, 37 of 41 files complete. Four
functions remain:

* `JpmcdsCdsFeeLegFlows` (cds.c) — the deferral whose reason expired;
* `JpmcdsFeeLegFlows` (feeleg.c) — almost certainly the same story;
* `JpmcdsStringToDate` (convert.c) — the last of the string parsers;
* `JpmcdsMakeTCurveNoRateCheck` (tcurve.c).

## Decision 46 — The deferral whose reason expired

Two functions this slice, both of which had been sitting behind the same note
for most of the project:

```c
// `JpmcdsCdsFeeLegFlows` is NOT ported here. It returns a TCashFlowList and
// needs cashflow.c, which is layer 7 and not on the pricing path.
```

That was true when I wrote it. `cashflow.c` was then ported in full — both
halves, `cash_flow_list.hpp` and `cash_flow_schedule.hpp` — and the note
stayed. For several slices afterwards the project carried a deferral whose
stated reason no longer existed, and nothing surfaced it.

What eventually did was the mentioned-but-not-claimed check added two slices
ago, which flagged `JpmcdsCdsFeeLegFlows` as *mentioned in a header but not
claimed*. I expected a missing claim sentence. What I found was a live-looking
justification for not doing work that had become possible.

**A deferral with an expired reason reads exactly like a live one.** That is
the whole argument for machine-checked provenance over a comment: the comment
was honest, specific, and wrong, and no amount of care in writing it would have
made it notice its own obsolescence.

### The functions themselves

`JpmcdsFeeLegFlows` is one multiply per period —
`yearFraction(accrual_start, accrual_end) * couponRate * notional`, dated at
the pay date. `JpmcdsCdsFeeLegFlows` builds the leg and asks it for its flows.

Two things worth stating about them.

First, they are **undiscounted**, which is unusual in this project and buys
properties a priced quantity would not have. The tier-1 tests use them: the
amounts are exactly linear in the notional (double it, every amount doubles and
no date moves), a zero coupon gives zero amounts on an unchanged schedule, and
every amount's sign follows the coupon's. Those last two would catch a sign
slip that a per-amount tolerance would wave through.

Second, `JpmcdsCdsFeeLegFlows` hardcodes `payAccOnDefault = 1` and
`protectStart = TRUE` as locals before calling the leg builder, so a caller
cannot set either. The port takes a `Cds`, whose defaults are those same
values. And `pay_accrual_on_default` **cannot affect the answer here** — these
are the flows that occur when there is *no* default, so the accrual convention
never comes into play. The field is on the leg because the leg carries it for
pricing. That is asserted rather than left to a reader's inference, because the
field is right there and looking relevant.

### A finding parked for the final slice

While checking how much was left I read `JpmcdsMakeTCurveNoRateCheck`, and it
contains the clearest instance of this project's central theme that I have
found:

```
** If it encounters a TDate = 0, it assumes that this is a terminator.
** This allows spreadsheet users to select a big range of dates, but use
** less than the whole range by terminating with a 1 or 0.
```

TDate 0 is 1601-01-01, a real date, and here it is **deliberately** a sentinel
— explicitly for spreadsheet callers, which is the audience the Cellwright
layer is eventually for. `JpmcdsMakeTCurve`, the checked sibling, has no such
behaviour.

And the comment says "terminating with a 1 or 0" while the code tests only
`== 0`, so TDate 1 is not a terminator. That needs measuring before it is
written up, which is the next slice's job along with `JpmcdsStringToDate`.

### Where this leaves the port

**162 of 164 in-scope functions = 99%**, 39 of 41 files complete. Two
functions remain, and they are the last two: `JpmcdsStringToDate` (convert.c)
and `JpmcdsMakeTCurveNoRateCheck` (tcurve.c).

---

## Decision 47 — The last function was the one that fought back

`JpmcdsStringToDate` was supposed to be the easy one. A date parser: read three
tokens, make a date. It is 90 lines. It was the last entry on the list and I
expected to spend an afternoon on it.

It produced **three** defects, and one of them is the only correction in the
entire port.

### The parser measured first, as always

Before writing a line I swept the input domain. Four documented formats, all
working. The delimiter set is `-/. ` — dot and space are undocumented. Trailing
tokens are silently ignored, so `1-1-2024-GARBAGE` is 2024-01-01. The crossover
year is 60, pivoting 59 → 2059 and 60 → 1960. Years 100–1600 are rejected
outright. And a 48-character input truncates to 31 and still parses, because
`strncpy` bounds the copy.

That last fact is what made me look harder. `convert.c` is the file that
contains [UPSTREAM-4](defect-report.md#upstream-4), an **unbounded** copy into a
fixed buffer. Here, 225 lines away, the same author wrote a bounded one. Same
file, same hazard, two different outcomes — so what else in this function is
one-and-not-the-other?

### UPSTREAM-38: the bound was downstream of the problem

`NStringToMonth` lowercases three characters:

```c
cp[0] = (char) tolower(cp[0]);
cp[1] = (char) tolower(cp[1]);
cp[2] = (char) tolower(cp[2]);
```

No length check. And those are read-modify-**writes**. A one-character token has
its NUL at `cp[1]` and nothing at `cp[2]`.

To get that past the buffer, three things have to line up, and all three are
reachable from the public entry point: a 32-byte stack buffer, a non-numeric
second token, and a first token long enough to push the second to offset 30.
`"11111111111111111111111111111-a"` does it. ASan:

```
ERROR: AddressSanitizer: stack-buffer-overflow
    #0 NStringToMonth convert.c:395
    #1 JpmcdsStringToDate convert.c:119
  [32, 64) 'dateCopy' (line 45)
  <== Memory access at offset 64 overflows this variable
```

**The lesson is the pair, not the bug.** The `strncpy` here is *correct*. It is
bounded, the NUL is placed by hand, truncation works. And it bought nothing,
because the overflow is downstream of it. Getting the `memcpy` right is not the
same as being safe — a sentence I would have nodded at before and can now point
at.

### UPSTREAM-39: the one I corrected

The month and day are parsed with a **checked** call:

```c
if (JpmcdsSscanf(cp,"%ld",&mdy.month) == 0)
    if (NStringToMonth(cp,&mdy.month) == FAILURE)
        goto done;
```

Eleven lines later the year is parsed with `atol`, which cannot report failure
at all. Its 0 is indistinguishable from a parsed zero, and 0 is below the
crossover, so it is promoted:

| input | result |
|---|---|
| `03/15/aaaa` | **2000**-03-15 |
| `15-Mar-twenty` | **2000**-03-15 |
| `03/15/99bogus` | **1999**-03-15 |
| `aa/15/2024` | refused |
| `03/aa/2024` | refused |

An unparseable month is refused. An unparseable day is refused. An unparseable
year is *invented*.

This is **the only place in the port where I corrected rather than reproduced**,
and it is worth being precise about why, because the rule I have been following
for 46 decisions says reproduce. The rule is: reproduce when the code is
self-consistent and only the documentation disagrees. Here the code disagrees
with **itself**, eleven lines apart, in one function, about one question. There
is no single upstream behaviour to preserve — only two, and they contradict.
That is the exception the rule was phrased to leave room for, and this is the
first time in 41 files it has fired.

What makes it worth correcting rather than noting: severity exceeds mechanism. A
crash gets investigated. A date does not. Every downstream accrual, discount
factor and par spread is then computed perfectly correctly from a wrong date,
twenty-four years off, with no error channel touched.

The divergence is asserted from **both** sides — upstream accepts, the port
refuses, both in one test — and those inputs are deliberately **excluded** from
the recorded vectors. Freezing a known divergence into the file the port is
checked against would convert a documented difference into a silent one, which
is the single worst thing a golden file can do.

### UPSTREAM-40: the one I was not looking for

UBSan reported this *before* ASan reported the overflow, from the same input:

```
lscanf.c:486:28: runtime error: signed integer overflow:
  1111111111111111111 * 10 cannot be represented in type 'long'
```

`JpmcdsSscanf` accumulates digits with no range check. `lscanf.c` is out of
scope for this port — it reads holiday files, which the port does with the
standard library — and I filed it anyway, because the register's subject is
upstream's defects, not the port's surface, and this one is reachable from an
in-scope public function.

Three entries from one probe. The "count defects, not entry points" rule has
usually collapsed numbers; here it expanded them, because one entry point really
did have three distinct mechanisms in three distinct files needing three
distinct fixes.

### Two measurements I got wrong

Worth recording because the pattern is now familiar: I wrote expected values
into a test before measuring them, twice in one slice.

`03/15/99bogus` I predicted as 2099. It is **1999** — 99 is *above* the
crossover, so it takes the `+1900` branch, not the `+2000` one. The pivot I had
measured myself forty minutes earlier.

The truncation case I predicted as year 2222. It is **1601-01-01**, because
`atol` on 27 digits overflows a signed long, and the answer is produced by
undefined behaviour. I deleted that assertion rather than pin it. An expectation
resting on UB is not a test; it is a recording of what one compiler did once.

Both failures were caught by the differential test on the first run, which is
the argument for writing it before trusting the port. The cost of guessing is
one build.

### The TDate-0 terminator, found on the way out

`JpmcdsMakeTCurve` delegates to `JpmcdsMakeTCurveNoRateCheck`, which stops
copying at the first point whose date is TDate 0 — so the length argument is an
upper bound, not a count. Measured:

- second point with date 0 → a one-point curve, silently
- second point with date **1** → FAILURE, so the header's *"a 1 or 0 in the date
  field"* is wrong; only 0 terminates
- **first** point with date 0 → a **zero-point curve**

That last line matters: it is a second reachability path for
[UPSTREAM-36](defect-report.md#upstream-36), and a better one. The first path
needs a caller to pass a literal `0`, visible at the call site. This one needs
only a data value that looks ordinary, because TDate 0 is 1601-01-01 — a real
date someone can legitimately hold.

The port copies the whole span. Honouring a real date as a terminator is the
sentinel-in-the-value-space mistake this project has now filed four times.

And `JpmcdsMakeTCurveNoRateCheck` itself is a **deliberate non-port**: its only
difference from `JpmcdsMakeTCurve` is that it skips the rate check, which is the
check `ZeroCurve::create` exists to perform. Porting it would reintroduce the
silent-NaN hazard behind a second door. Three functions are now non-ports, and
in all three the thing that would be ported *is* the defect — which is a better
reason than "not needed yet" and the only reason I would accept.

### Where this leaves the port

**163 of 163 in-scope functions = 100%.** 41 of 41 files complete.

The denominator moved, which needs saying plainly: it was 164, and
`JpmcdsMakeTCurveNoRateCheck` left it by being declared a non-port rather than by
being written. A percentage whose denominator I control is not a fact about the
work, so: 163 functions ported, 3 declared non-ports with reasons recorded above,
0 remaining unexplained.

Forty defects in the register. Thirty-nine of them reproduced faithfully, one
corrected, every divergence asserted from both sides.

---

## Decision 48 — The error channel, and the only correction that was mine to make

The port finished with 341 `return std::nullopt` sites. A failed call said
nothing about why, and `root_find.hpp` admitted the cost in its own doc
comment: *"Returns nullopt both when the objective fails and when the iteration
count"* is exhausted — and separately on backwards bounds. Three different
outcomes, one answer. A caller could not tell *"your bounds are reversed"* (fix
the call) from *"it did not converge"* (widen the tolerance).

### The taxonomy came from counting, not from taste

I did not choose four error kinds. I counted:

| | |
|---|---|
| `return std::nullopt` sites | 341 |
| — pure propagation, a callee already failed | 149 (44%) |
| upstream status checks (`== FAILURE` / `!= SUCCESS`) | **447** |
| — of those, branching on a *reason* | **0** |
| upstream `JpmcdsErrMsg` diagnostic sites | 452 |

447 checks, zero reason-branches, 452 messages. Upstream emitted a message per
failure and never once branched on one — so the payload in this library has
always been **text for a human**, not a code for a `switch`. That is what sizes
the taxonomy at four kinds rather than 452, and what rules out the per-site
enum I would otherwise have built.

And the four kinds are checked against upstream's own messages, which is what
makes the oracle rung 3 rather than my judgement. `rtbrent.c` already
distinguishes these cases; it just cannot return the distinction:

| upstream message | kind |
|---|---|
| `Lower bound (…) >= higher bound (…)` | `bad_argument` |
| `Function values at bounds imply no root exists` | `no_root` |
| `Maximum number of iterations exceeded` | `no_convergence` |
| `Supplied function failed at point …` (8 sites) | *forwarded unchanged* |

### Two proposals of mine, both rejected

**C++23.** `std::expected` is C++23-only, and I proposed moving the project to
get it. That was backwards: this is a C++20 port, and a language-version change
is a decision on its own merits, not a side effect of an error-handling slice.
`cdsmodel::Expected<T>` is a C++20 mirror whose API is a **strict subset** of
`std::expected`'s — which is what gives the duplication an end date. A test
compiles the identical call sequence against both types, so the eventual swap
is a type alias and a deletion.

**Narrowing to 46 sites.** Measured that only the solver's path destroys
information today, proposed migrating just that, and was told no. Rightly: the
mixed API would have been a wart every caller saw, and 341 mechanical
signatures beat two passes.

### D5, the one decision the wide scope forced

Narrowed to the solver I would only have had to classify its failures. Across
341 sites every origin has to classify — and doing that surfaced something:
**propagation is not a kind.** 149 of 341 sites forward a callee's failure, and
8 of the solver's 12 diagnostics are the same forwarded message. A wrapper kind
invented at each of 149 layers would bury the one reason the caller needs. So
forwarding preserves kind and message byte-identically, tested on the real
`cdsone` → `bootstrap` → `root_find` chain.

### D7 took three attempts

First I wrote the rule as *"no function returns `std::optional`"*. A lambda is a
function, and `date_from_string`'s `to_month_name` returns `nullopt` to mean
"not a month name — try reading it as a day". That is a parse alternative, not
a failure; converting it would have meant inventing error messages for
non-events.

So I wrote *"function-local lambdas are exempt"*. Then
`ZeroCurveBuilder::find_exact` falsified that immediately — a lookup miss is
not a failure, whatever scope it sits at.

The rule that survives is semantic: **`optional` means legitimately absent,
`Expected` means failed.** Five false positives came out of my mechanical pass,
every one caught by the compiler rather than review, and every one the same
mistake — a syntactic rule standing in for a semantic one:

| what the pass did | why it was wrong |
|---|---|
| converted `to_month_name`, `to_number` | parse alternatives |
| converted `find_exact`, `MatchRange::exact` | lookup misses |
| protected `leg_pv`, `counted` as fields | both are locals of the same name |
| converted `Outcome::root` | holds the UPSTREAM result, which has no message |
| rewrote `if (!bracketed)` as `unexpected(bracketed.error())` | it is a `bool` |

### UPSTREAM-39, and the consequence worth seeing

The slice's one correction. The month and day are parsed with a **checked**
call; eleven lines later the year uses a bare `atol`, which cannot report
failure. `03/15/aaaa` was accepted as **2000**-03-15. An unparseable month is
refused, an unparseable day is refused, an unparseable year is *invented*.

That is the exception the reproduce-rule leaves room for: the code disagrees
with **itself**, so there is no single behaviour to preserve. The payoff is one
line:

    date_from_string("03/15/aaaa")
      before:  nullopt
      after:   bad argument: field 3 of "03/15/aaaa" is not a year

**341 sites, zero behavioural diffs**, proven by the existing suites across
debug, release and asan.

---

## Decision 49 — The library was finished and could not be installed

`cmake --install` already worked. It staged **52 files: every one GoogleTest or
gmock, and not a single cdsmodel header.** GoogleTest's `FetchContent` brings
its own install rules, so anyone installing this library received our test
framework instead of our library.

| | before | after |
|---|---|---|
| `install(...)` rules | 0 | targets + headers + export set |
| package config / version files | 0 | both, `SameMajorVersion` |
| headers a consumer must include | up to 45 | **1** |
| what `cmake --install` stages | 52 gtest files | 46 headers + lib + 3 package files |

The target had been *written* to be exported since the scaffold —
`$<INSTALL_INTERFACE:include>` and a `cdsmodel::cdsmodel` alias were already
there. This finished a sentence the build file started.

### The ladder has no rung for "is this installable"

`verification.md` is written for numerical results. So the oracle is to **be
the consumer**: install into a throwaway prefix, then build a project that
lives outside the tree and knows nothing but `find_package`, and *run* it.
Reading the CMake and nodding is not an oracle.

Its check is self-consistency rather than a pinned constant — a CDS whose
coupon equals its own par spread is worth nothing by definition:

    par spread   0.0059480339
    price at par -6.939e-18
    refusal      bad argument: end must be on or after start when protection
                 starts at start

That last line is Decision 48 paying off across a package boundary: a refused
call reaches an external consumer with its kind and message intact.

### Two false passes, both mine, both the same mistake

**B3 proved nothing as first written.** It re-ran B2's installed-package
consumer with `-DCDSMODEL_UPSTREAM_DIR=/nonexistent` — a cache variable that
project never reads. The same test twice, passing.

**B7 passed over an install tree containing none of our library** — it reported
*"52 files, none upstream-derived"* while staging gtest and zero cdsmodel
headers. I found it by asking why debug staged 51 files and release 52.

Both were checks phrased as **prohibitions** rather than expectations.
"Nothing forbidden is present" is satisfied by an empty tree; "the consumer
builds" is satisfied by building the wrong thing. That is a failure mode worth
naming once and watching for forever: a check that cannot fail on the empty
case is not a check.

---

## Decision 50 — The XLL could not be improved, because it cannot load

The question was whether to upgrade the existing add-in. The answer came from
four measurements:

| measured in `isda_cds_model_c_v1.8.3/excel/` | value |
|---|---|
| build configurations | `win32debug`, `win32release` — **no x64** |
| `XLOPER` uses / `XLOPER12` uses | **57 / 0** |
| registered functions | 18, of which **6 manage a global error log** |
| worst signatures | `UpfrontFlat`, `ParSpreadFlat` — **17 positional arguments** |

**64-bit Excel cannot load a 32-bit XLL**, and `XLOPER` is the pre-2007 API: no
Unicode, 255-character strings, 65,536-row grids. There is no in-place upgrade
path from that; there is only a new add-in that keeps the function names. And
the vendored SDK header is Excel 97-era — `XLOPER12` appears in it **zero**
times and it declares `int far pascal Excel4v(...)` — so even with permission
to copy it there was nothing worth taking.

### 111 arguments, 38 concepts

The signature problem has a number. Across upstream's 8 priced functions:

| argument | appears in |
|---|---|
| `Holidays` | **8 of 8** |
| `Coupon Interval`, `Stub Type`, `Payment DCC`, `Bad Day Convention` | **7 of 8** each |

111 arguments covering ~38 distinct concepts. Two-thirds of the parameter
surface is the same six things retyped per formula — and that block *is* a
conventions struct. Upstream did not fail to notice: a C API with no aggregate
feeding a grid with only positional cells leaves nowhere to put it. **The
17-argument signature is the absence of a type**, which is precisely what a
C++20 replacement is for.

I had posed compatibility *versus* usability as a blocking question. It is a
false dichotomy — an XLL registers a table, so it registers both:

    CdsPrice     16 args   upstream's, unchanged
    CDS.Price     5 args   Valuation · Trade · Conventions · Market · Clean

Both read into the same four structs and then run one code path, so they cannot
disagree. The test compares them **bitwise**.

### The architecture is not mine

`cpp_fdm_world` had already solved the layering: domain library, SDK wrapper,
add-in, each its own library, Excel as one interface among others. Three
techniques I would have got wrong — the whole Excel layer behind
`#ifdef WIN32`, the `.xll` as a `SHARED` library with a renamed suffix, and no
import library at all because the SDK's `xlcall.cpp` resolves `MdCallBack12`
from the running Excel process.

One deviation, and it is the reason this slice has oracles: **the seam sits one
layer lower.** Guarding the whole Excel layer would put the marshalling, the
dispatch table and the labelled-range reader on the only machine that cannot
run CI. With `XlValue` as the boundary, `xl/core` is ordinary C++ tested on
every platform, and only a thin `XLOPER12` adapter needs Windows. **About 95%
of the remaining XLL work is macOS work** because of that one choice.

### A bug found by audit, before the code that would have hit it

`is_failure()` treated any string result as a failure — which works in
`cpp_fdm_world` because every function there returns a number. But
`IRZeroCurveBuild` and `CleanSpreadCurveBuild` return a curve **handle**, which
is text. A successful build would have been reported as an error. Caught by
auditing before those functions existed, which is the only cheap time.

---

## Decision 51 — Coverage was never measured, and the first number was wrong

There was no instrumentation: nothing in the build, the presets, or CI. Test
*counts* had been quoted for 50 decisions, which is what people quote when they
have not measured coverage.

The first figures, filtered to our own code:

| | lines | branches |
|---|---|---|
| ported library | 86.5% | 82.3% |
| `xl/core` | 81.4% | **54.1%** |

### The 54.1% was my measurement error

My filter matched `xl/core/`, which includes `xl/core/tests/`. Test files are
100% line-covered by definition and ~51% *branch*-covered — gtest's
assertion-failure paths never fire when tests pass — so they dragged the
aggregate down. I was measuring the framework. The real figure was 60.0%.

The gate now excludes test files for exactly that reason, and the comment says
so. A measurement that includes its own instrument is not a measurement.

### Against the original

The comparison worth having. **Upstream ships zero tests** — no test directory,
no regression harness, one example program and two holiday files.

| | upstream C, in-scope | our port |
|---|---|---|
| functions | **98.3%** (231/235) | 97.5% |
| lines | **81.0%** (7,870/9,712) | 85.7% |
| branches | **74.1%** | 81.3% |

Our differential suite drives 98.3% of upstream's in-scope functions and 81% of
its lines — a library its authors never tested. That is the tier-2 machinery
paying off: every sweep runs both implementations side by side, so covering
ours covers theirs.

And the port expresses the same functionality in **4,498 executable lines
against upstream's 9,712** — under half, same 163 functions, same numbers to
the ULP where it matters.

One honest consequence: **Decision 48 lowered line coverage.** 341 new error
sites are 341 branches a test must construct bad input to reach. The
alternative — `optional` with no messages — had better coverage and worse
diagnostics.

### The floors

`verify.py coverage`, per area, because they have different honest ceilings.
Raise when coverage rises; never lower one to make a run green. It gated the
very next slice at 88.9% against a 90 floor, which is the gate doing its job on
the first opportunity it had.

---

## Decision 52 — The last hole, and a dead buffer

`Holidays` appears in 8 of upstream's 8 priced functions. Until this slice the
add-in refused every calendar but `NONE` — explicitly, rather than silently
pricing against no holidays, but a desk could not have used it.

It was a hole by decision: `JpmcdsHolidayLoadFromDisk` was a declared non-port
whose recorded reason was *"reads a holiday file into a process-wide cache"*.
**The cache was the objection, not the reading** — a distinction the non-port
table stated and nobody had acted on. `docs/port-notes.md` now says what
changed rather than quietly losing a row.

### UPSTREAM-41

`JpmcdsHolidayListRead` declares a buffer for an upper-cased copy of each
comment line, fills it, and compares the **original**. `ucBuffer` is written
once at line 574 and never read; all seven directive comparisons use `buffer`.

So the weekend directives are case-**sensitive**, while the dead code exists
for no purpose other than to make them case-insensitive:

| file | Saturday 2024-03-16 |
|---|---|
| `# SATURDAY_NOT_ALWAYS_HOLIDAY` | **a business day** |
| `# saturday_not_always_holiday` | **a holiday** |

Both load. Neither warns. Every date the model adjusts is silently wrong. This
is the register's worst shape — not a crash, but a confident wrong answer from
input written in good faith.

**Corrected**, and the reasoning differs from UPSTREAM-39's. There, the code
disagreed with itself. Here the behaviour is unambiguous — `buffer` is always
compared — and the dead buffer is evidence of *intent*, not of a second
behaviour. By the standing rule that means reproduce. But reproducing means
reproducing a silently wrong calendar, so a comment that case-insensitively
matches a directive **and is not exactly it** is refused, quoting the line.
Asserted from both sides, so an upstream fix fails the test instead of hiding.

### A prediction of mine, refuted

I reasoned the same macro had an off-by-one: the loop runs while the offset is
`<= len`, so the terminator lands at `len + 1`. Tested with a 300-character
comment line under ASan: **nothing.** `JpmcdsFgets` caps the line at 255, so
the source is never long enough to push the terminator past the last valid
index. The macro is unsafe in isolation; at this call site the caller's bound
protects it.

Recorded because the prediction was wrong and the reason is the useful part —
the bound lives in the caller, not in the macro.

### Three of my own test bugs, all from the same confusion

This project juggles three epochs — 1601 (ISDA TDate), 1900 (Excel serial),
1970 (Unix, which is both `cdsmodel::Date`'s and Cellwright's). I hardcoded
`45366` as a TDate; it is an *Excel* serial, and as a TDate it is a date in
1725 with a different weekday. The tests were checking the wrong days and only
one of them disagreed. Everything now computes through `tdate_of(y, m, d)`,
with a comment saying why hardcoding broke.

That is the third time the epochs have bitten. The lesson is not "be careful";
it is that a magic number in a date test should be a computation.

### What the slice looks like

The design question was how a calendar should reach a formula, and the answer
is not upstream's:

| | visible in sheet | travels with workbook | needs a path | add-in does IO |
|---|---|---|---|---|
| a range of dates | **yes** | **yes** | no | no |
| a file on disk | no | no | yes | yes |

A range is primary, and it needed **no new library code** —
`Calendar::create(dates, weekend)` already existed. The file path stays,
because desks share `NYC.dat` centrally. And nothing writes a file from a
worksheet function: Excel recalculates on its own schedule, so a formula with a
disk side effect fires at times nobody chose.

H1 swept every day of 20 years against the oracle for both shipped files —
7,305 comparisons each, zero disagreements, with a positive control so a sweep
finding no holidays could not pass trivially. The date accounting is exact, and
I verified it independently after chasing what looked like dropped dates:
`NYC.dat` is 547 dates = 519 kept + 28 falling on a weekend, which
`Calendar::create` discards as already non-business.

### Where this leaves the upgrade

| | |
|---|---|
| C library, in-scope functions | **163/163** + the holiday reader |
| XLL function surface | **6/13** |
| defects catalogued | **41** — 39 reproduced, 2 corrected |
| tests, with the oracle | **827** |
| tests, without it | **504** |
| coverage, library | 85.7% lines, 81.3% branches |
| coverage, `xl/core` | 93.6% lines, 90.1% branches |
| recorded vector sets | 44 |
| verify gates | 11 |
| ratified specs | 4 |

Every remaining XLL function is **marshalling only** — the library function
exists for each one, differential-tested and with golden vectors behind it. The
hard half is done. What is left to call the upgrade complete is wiring, and one
manual run in Excel that only a Windows machine can do.
