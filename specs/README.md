# specs/ — the contract index

Rationale and contracts live here; the enforceable subset lives in the repo's
`CLAUDE.md` (which does not exist yet — see artifact #1, D4). *Useful for
**why**, never for **what next**.* Only ratified artifacts say what to do.

Every artifact carries a status. **An artifact with no status is presumed
stale.**

## ► START HERE

Nothing yet. The port itself was built before this directory existed, under the
module loop in `CLAUDE.local.md`; its reasoning record is `docs/journal.md`
(47 decisions) and `docs/port-notes.md`. Neither is a spec — they are the
action and reasoning records of work already done.

## ► THE METHOD

- `~/.claude/spec-driven.md` — the general method this follows.
- `CLAUDE.local.md` — the module porting loop and the two inviolable rules
  about the upstream oracle. Still governing; the port is complete but the
  oracle remains the verification substrate.

## ► REFERENCE WHILE BUILDING

| # | artifact | status | covers |
|---|---|---|---|
| 01 | [`01_error_channel.md`](01_error_channel.md) | **current** | `std::optional` → `Expected<T>` in C++20; all 341 sites; the four-kind taxonomy |
| 02 | [`02_consumability.md`](02_consumability.md) | **current** | install/export, package config, the umbrella header |
| 03 | [`03_xll.md`](03_xll.md) | **current** (10/10; C8 run in Excel twice, 2026-11-11 and 2026-11-14) | the XLL foundation: SDK, x64, XLOPER12, one function end to end |
| 04 | [`04_holiday_calendars.md`](04_holiday_calendars.md) | **ratified** | reading a holiday file; UPSTREAM-41 |
| 05 | [`05_python_bindings.md`](05_python_bindings.md) | **ratified** | the PyPI package: nanobind, cp312-abi3, Trusted Publishing |

## ► ARCHIVED

Empty. *Kept for history, do not act on.*
