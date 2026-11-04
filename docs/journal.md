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
| the 3-OS CI matrix | 2 | PORT-6, PORT-8 |
| a test written to probe extremes | 2 | PORT-4, PORT-7 |
| noticing a *passing* test print a warning | 1 | PORT-5 |
| the next layer up composing the module | 1 | PORT-9 |

**Nothing was caught by re-reading the code.** Every one of the nine was found
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
