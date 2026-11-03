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

## Metrics, with the caveat that matters

As of the end of layer 4 and part of layer 5:

| | |
|---|---|
| upstream lines retired | 5,168 of 21,775 (24%) |
| **pricing-path lines ported** | **815 of 8,871 (9%)** — curve, timeline, protection leg |
| port size | 1,755 lines (≈2.5:1 compression) |
| tests + generators | 5,407 lines (3:1 over the port) |
| recorded reference cases | 16,702 |
| tests | 230 with an oracle, 153 without |
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
