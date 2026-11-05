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
