"""The ISDA CDS Standard Model, ported to C++20.

A port of the ISDA CDS Standard Model 1.8.3. Every function is checked against
the upstream C implementation; see the project's README for how that works and
what it does not cover.
"""

from __future__ import annotations

from pathlib import Path

from ._core import (
    BadArgument,
    BadDayConv,
    Calendar,
    CdsModelError,
    Conventions,
    DateInterval,
    DayCount,
    FlatMarket,
    InstrumentKind,
    InstrumentQuote,
    InterpType,
    MarketData,
    NoConvergence,
    NoRoot,
    PeriodUnit,
    StubLength,
    StubMethod,
    StubPosDefault,
    StubPosition,
    SwapConventions,
    TradeTerms,
    Unsupported,
    Valuation,
    Weekend,
    ZeroCurve,
    __upstream_version__,
    __version__,
)



def read_holiday_file(path: str | Path) -> Calendar:
    """Read a holiday file and parse it into a `Calendar`.

    The reading happens here rather than in C++ on purpose. `Calendar.parse`
    is pure by design (artifact #4, D4), and Python's own file errors are what
    a Python caller expects -- `FileNotFoundError` for a missing path,
    `IsADirectoryError` for a directory, both subclasses of `OSError`.

    That second one matters. `std::ifstream` *opens* a directory on some
    platforms and reads zero bytes, and an empty holiday file is legitimate
    (upstream accepts one, yielding weekends only) -- so a C++ reader that
    does not check for a regular file hands back a working calendar with no
    holidays in it. `xl/core` guards against that explicitly. Here the
    language does it.
    """
    return Calendar.parse(Path(path).read_text(encoding="utf-8"))


__all__ = [
    "BadArgument",
    "BadDayConv",
    "Calendar",
    "CdsModelError",
    "Conventions",
    "DateInterval",
    "DayCount",
    "FlatMarket",
    "InstrumentKind",
    "InstrumentQuote",
    "InterpType",
    "MarketData",
    "NoConvergence",
    "NoRoot",
    "PeriodUnit",
    "StubLength",
    "StubMethod",
    "StubPosDefault",
    "StubPosition",
    "SwapConventions",
    "TradeTerms",
    "Unsupported",
    "Valuation",
    "Weekend",
    "ZeroCurve",
    "read_holiday_file",
    "__upstream_version__",
    "__version__",
]
