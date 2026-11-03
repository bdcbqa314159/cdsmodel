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
| upstream lines retired | 4,353 of 21,775 (20%) |
| **pricing-path lines ported** | **0 of 8,871 (0%)** |
| port size | 1,755 lines (≈2.5:1 compression) |
| tests + generators | 5,407 lines (3:1 over the port) |
| recorded reference cases | 13,866 |
| tests | 198 with an oracle, 133 without |
| defects | 7 upstream, 8 port-side |

**The 20% is the least meaningful number here.** Everything done is
infrastructure — dates, day counts, calendars, lists — where answers are
crisp integers. Nothing prices anything yet. The layers remaining are harder
per line, and layer 6 brings the first real floating-point: interpolation and
root-finding, where accumulation order changes the last bits and
`EXPECT_DOUBLE_EQ` may stop being attainable. When that happens the
verification method itself needs a decision, not just the code.

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
