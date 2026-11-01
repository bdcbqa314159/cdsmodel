# Attribution

This application is based on the ISDA CDS Standard Model (version 1.8.3),
developed and supported in collaboration with Markit Group Ltd.

## The upstream sources are not distributed here

The ISDA CDS Standard Model C sources are available only from cdsmodel.com
after accepting the ISDA CDS Standard Model Public License and providing an
email address. This repository therefore does **not** contain them: shipping
them would bypass that gate and undercut the assent the licence asks
distributors to obtain (section 10(b)).

`isda_cds_model_c_v1.8.3/` is gitignored. Developers fetch their own copy --
see README.md, "Obtaining the reference implementation". When present it is
used **unmodified** as the numerical oracle; compiler incompatibilities are
handled with CMake flags, never by editing a source file.

## What is distributed here

Reference answers computed by running the upstream model, under
`tests/golden/`. Section 4(b) exempts these:

> It is understood that a work that includes data derived from calculations
> that have been attained by using the ISDA CDS Standard Model, and is not
> Source Code or an executable version of any Derivative Work that You may
> create for external use to your organization, shall not be subject to the
> provisions of this Section.

The generators under `tools/` require a local upstream copy to run, so the
numbers stay reproducible for anyone who has accepted the terms.

ISDA and Markit are not affiliated with this project and do not endorse it.

## Modifications

Required by §4 of the upstream licence: all additions and modifications must be
clearly identified. This project adds, outside the upstream tree:

| what | where | status |
|---|---|---|
| CMake build replacing the per-platform makefiles | `CMakeLists.txt` | done |
| Oracle regression test against upstream example output | `tests/` | done |
| Recorded reference vectors (licence s4(b) data) | `tests/golden/` | generated |
| C++20 port of the analytics | `src/`, `include/` | layers 1-2 in progress |
| Python bindings (nanobind) | `python/` | not started |

No upstream file has been edited. Build-time suppressions for modern
compilers are applied via CMake flags only.
