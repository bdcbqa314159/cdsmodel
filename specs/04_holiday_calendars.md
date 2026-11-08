# Artifact #4 — holiday calendars (ratified)

**Status:** ratified 2026-11-08 (Bernardo Cohen)
**Governing purpose:** Read a holiday file, so the add-in can price against a
real calendar instead of refusing everything but `NONE`.
**Inherits:** THE BAR — `CLAUDE.md`

## Reframe

The library is complete and the add-in dispatches. **This is the last hole in
the library itself**, and it is load-bearing: `Holidays` appears in 8 of
upstream's 8 priced XLL functions, so until a calendar can be loaded, the
add-in cannot price a trade a desk would recognise.

It is a hole by decision, not oversight. `JpmcdsHolidayLoadFromDisk` is a
declared non-port whose recorded reason is *"reads a holiday file into a
process-wide cache"* — and the cache was the objection, not the reading. This
slice does the reading without the cache, so `docs/port-notes.md`'s non-port
table gains a row saying what changed rather than quietly losing one.

## The format, from `buscache.c`

    #                                comment; blank lines skipped too
    19631026                         1963-10-26 is a holiday
    # SATURDAY_NOT_ALWAYS_HOLIDAY    Saturday becomes a business day
    # SUNDAY_NOT_ALWAYS_HOLIDAY      Sunday becomes a business day
    # MONDAY_ALWAYS_HOLIDAY          ... and five more, Monday to Friday

Dates must increase. The shipped `NYC.dat` (547 lines) and `TRG.dat` (284) use
only the plain date form.

## UPSTREAM-41, measured

`JpmcdsHolidayListRead` builds an upper-cased copy of every comment line and
then compares the ORIGINAL:

```c
char ucBuffer[MAX_BUFFER];              /* declared at line 519 */
...
COPY_CAPITALIZE(ucBuffer, buffer, MAX_BUFFER-1);   /* written at 574 */
if (strncmp (buffer, satString, sizeof(satString)-1) == 0)   /* reads buffer */
```

`ucBuffer` is written once and **never read**. The seven directive comparisons
all use `buffer`, so the directives are case-SENSITIVE while the dead code
exists to make them case-insensitive. Measured on a two-line file:

| file | result |
|---|---|
| `# SATURDAY_NOT_ALWAYS_HOLIDAY` | Saturday 2024-03-16 is a **business day** |
| `# saturday_not_always_holiday` | Saturday 2024-03-16 is a **holiday** |

A user who writes the directive in lower case gets standard weekends, no
diagnostic, and every adjusted date silently wrong.

**Two further things in the same macro, neither filed.** `toupper((int)*PSRC)`
on a plain `char` is the same undefined behaviour as UPSTREAM-35, already
filed, and it happens in code whose result is discarded. And I predicted an
off-by-one: the loop runs while the offset is `<= len`, so the terminator lands
at `len+1`. **Refuted** — `JpmcdsFgets` caps the line at `MAX_BUFFER-1`, so the
terminator lands on the last valid index. Unsafe in isolation; safe at this one
call site. Recorded because the prediction was wrong and the reason is useful.

## How a calendar reaches a formula

Three surfaces, and the one upstream had is deliberately not the primary.

| | visible in the sheet | travels with the workbook | needs a path | add-in does IO |
|---|---|---|---|---|
| a range of dates | **yes** | **yes** | no | no |
| a file on disk | no | no | yes | yes |

A range of cells is the better primary interface for the same reason the
conventions block is: a reader can see what the formula means, and a `#REF!`
beats a file-not-found at recalculation time. It also needs **no new library
code** -- `Calendar::create(dates, weekend)` already exists.

The file path stays, because desks share `NYC.dat` centrally and an add-in
that cannot read one is not a replacement. It is interop, not the main road.

**Nothing writes a file from a worksheet function.** Excel recalculates
whenever it chooses, so a formula with a side effect on disk is a bug
generator. Creating a file is a `tools/` script, outside both the library and
the add-in.

## MUST — in scope, full discipline

1. `parse_holiday_file(std::string_view contents) -> Expected<Calendar>` --
   **pure, and no filesystem**. The caller reads the bytes. That keeps `errno`,
   paths and encodings out of a numerical library, lets the differential tests
   run with no temp files, and lets one parser serve a file, a pasted block,
   or anything else that produces text.
2. The full format: comments, blank lines, the seven directives, dates,
   increasing order enforced.
3. In the XLL, over a calendar registry mirroring `CurveRegistry` including
   its generation check:
   - `LoadHolidays(path, name)` -- upstream's, unchanged; the XLL does the IO
   - `CDS.Calendar(dates_range, weekend)` -- from cells, no path, no IO
   - `CDS.CalendarFromFile(path)` -- the grouped-form equivalent
   - `CDS.CalendarDates(handle)` -- spills a calendar back into cells, which
     is both the migration path off files and the only way a loaded calendar
     becomes inspectable. Upstream's cached handle never was.
4. Differential tests against `JpmcdsHolidayListRead` over `NYC.dat`,
   `TRG.dat`, and generated files covering every directive and every
   malformed shape.
5. The `Holidays` argument accepts a calendar handle; the current blanket
   refusal goes.

## NOT-NOW — real, but deferred this cycle

- A holiday-file writer in C++. A `tools/` Python helper covers fixtures and
  generated calendars; writing from the library would put IO back in it.
- Multiple calendars per argument (upstream takes a comma-separated list in
  some functions); one calendar first.
- Caching by filename. The cache is what made this a non-port; if a desk
  reloads the same file per cell and it measurably costs, that is its own slice
  with a measurement behind it.

## NEVER — out of scope

- **A process-wide calendar cache.** The stated reason this was a non-port.
- Reproducing UPSTREAM-41's silent ignore — see D1.
- Reading a holiday file from inside the pricing path. Loading is explicit.

## Acceptance

| ID | Criterion | Oracle | Rung |
|----|-----------|--------|------|
| H1 | `NYC.dat` and `TRG.dat` load to exactly the dates and weekend rule upstream produces | differential: every date compared, plus `is_business_day` swept across 20 years against `JpmcdsHolidayListIsBusinessDay` | 3 |
| H2 | Each of the seven directives has the effect upstream gives it, in its documented spelling | one generated file per directive, both implementations compared on a day of that weekday | 3 |
| H3 | IF a comment case-insensitively matches a directive but is not exactly it, THEN the read SHALL fail naming the line | unit test on `# saturday_not_always_holiday`; upstream silently ignores it, which is UPSTREAM-41 | — |
| H4 | Dates out of order are refused, and the message names the offending line number | differential: upstream refuses too, so acceptance matches; the message is ours | 3 |
| H5 | A malformed date line is refused, naming it | upstream treats `abc` as a comment and `19960101x` as 19960101 via `atol`; both diverge deliberately — see D2 | — |
| H6 | THE reader SHALL hold no state between calls | two reads of different files in one process, each correct; no global to inspect because none exists | — |
| H7 | `LoadHolidays` returns a handle that resolves, and a stale one is refused | the `CurveRegistry` tests, repeated for calendars | — |
| H8 | A 10,000-line file reads in under 50 ms | timed test; upstream reads the file TWICE (once to count, once to parse) and this reads once, so the port should win comfortably | — |

## Decisions

| # | Decision | Alternatives | Why this | Revisit when |
|---|----------|--------------|----------|--------------|
| D1 | A near-miss directive is REFUSED, not reproduced-as-ignored and not silently accepted | reproduce the silent ignore; accept any case | Behaviour here is unambiguous — `buffer` is always compared, so there is one upstream behaviour and the rule says reproduce it. But the consequence is a silently wrong calendar, which is the worst failure in this project's catalogue. Refusing a line that *looks* like a directive and is not splits the difference: it reproduces nothing silently, accepts nothing upstream rejects, and turns a wrong answer into a message | a real holiday file in the wild uses mixed case deliberately |
| D2 | A malformed date line is refused | treat it as a comment, as `atol` does | Upstream's test is `atol(line) > 16010101`, so `abc` is a comment and `19960101x` is 1996-01-01. Both are the UPSTREAM-39 family: `atol` cannot report failure. A holiday file is small, hand-maintained, and load-bearing for every date in a trade — a typo there must not become a missing holiday | — |
| D3 | One read, not two | mirror upstream's count-then-parse | Upstream opens, counts, closes, reopens, parses — because it sizes an array first. A `vector` makes that unnecessary, and the second read is where the "More dates on second scan" error comes from, a failure mode that cannot exist here | — |
| D4 | The parser takes CONTENTS, not a path; the caller does the IO | take a path, as upstream does and as this spec first said | A path drags the filesystem into a numerical library -- `errno`, encodings, working directories -- and forces every differential test through a temp file. Contents make the parser pure and let one implementation serve a file, a sheet range and a pasted block. The XLL reads the bytes, which is where IO belongs | a caller needs streaming for a file too large to hold, which no holiday file is |
| D5 | A range of cells is the primary interface; a file path is interop | file only, as upstream | Same argument that settled the conventions block: the sheet should show what the formula means. A range also travels with the workbook and cannot be missing at recalculation time. But desks share calendars centrally, so the path stays | -- |
| D6 | Nothing writes a file from a worksheet function | an `ExportHolidays` function | Excel recalculates on its own schedule, so a formula with a disk side effect fires at times nobody chose. Export is a command, not a function | -- |

## Open questions before ratification

1. Non-blocking — where the header lives. **Assumption:
   `include/cdsmodel/holiday_file.hpp`**, beside `calendar.hpp`, since it is a
   library capability rather than an XL one.
2. Non-blocking — whether `verify.py` gains a gate for the directive table's
   completeness. **Assumption: no.** Seven entries checked by seven tests is
   not a drift risk worth machinery.

## Progress

| ID | Status | Oracle result | Note |
|----|--------|---------------|------|
| H1 | todo | | the binding one |
| H2 | todo | | |
| H3 | todo | | UPSTREAM-41's correction |
| H4 | todo | | |
| H5 | todo | | |
| H6 | todo | | |
| H7 | todo | | |
| H8 | todo | | |
