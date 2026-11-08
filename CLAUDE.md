# cdsmodel — the law for this repo

Guardrails, not suggestions. When a change conflicts with this file, this file
wins. Narrow by design: everything here is enforceable, and every line is
checked by a gate named beside it. Rationale lives in `specs/` and
`docs/port-notes.md`; how I work generally lives outside this repo.

## The oracle — two inviolable rules

`isda_cds_model_c_v1.8.3/` is a local copy of the upstream ISDA CDS Standard
Model. It is the project's only independent check on 163 ported functions.

1. **It is never edited.** Compiler problems are solved with CMake flags, never
   by touching a source file. Editing it destroys the only independent check
   the project has.
   → enforced by `python verify.py upstream-unmodified` (132 files byte-compared
   against a recorded manifest).

2. **It is never committed.** Upstream is gated behind accepting the ISDA
   licence and giving an email address on cdsmodel.com; redistributing it here
   routes around that. It is gitignored and stays that way.
   → enforced by `python verify.py upstream-absent`.

Computed output under `tests/golden/` **is** committable — licence section 4(b)
exempts derived data. Each generator says so in its header.

## The verification contract

CI has no oracle, because of rule 2. A module tested only against upstream is
therefore verified **on one laptop and nowhere else.** So every module needs
both:

- a **tier-2** differential test that compiles upstream into the same binary and
  sweeps the input domain against it (local only); and
- that sweep **recorded into tier-1 vectors** under `tests/golden/`, replayed
  without the oracle.

→ enforced by `python verify.py golden` (every vector set paired with its
generator) and `python verify.py tests` (a floor on the count collected with no
oracle present — raise it when a module lands, never lower it to go green).

The build must be green **both** ways before a commit:
`ctest --preset debug` and the same with `-DCDSMODEL_UPSTREAM_DIR=/nonexistent`.

## Divergence from upstream

The port reproduces upstream's behaviour, including its defects. Correcting is
the exception and needs a reason: reproduce when the implementation is
self-consistent and only its documentation disagrees; correcting is available
only when the code contradicts *itself*, so there is no single behaviour to
preserve.

Any deliberate difference gets all three of: a comment at the site, a row in
the "Deliberate differences" table in `docs/port-notes.md`, and a test
asserting it from **both** sides — so an upstream release that changes the
behaviour fails a test instead of drifting silently.

A known divergence is never frozen into a recorded vector set. Golden files are
what the port is checked against; putting a divergence there converts a
documented difference into a silent one.

→ enforced by `python verify.py reports` (every catalogued defect present on all
three surfaces) and `python verify.py claims` (no upstream function claimed by
two headers).

## Formatting

`clang-format` covers `include/`, `src/` and `tests/` only. Upstream C is never
reformatted — it is not ours, and a reformatted oracle is a modified oracle.
