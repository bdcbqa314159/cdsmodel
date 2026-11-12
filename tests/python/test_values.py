"""P3 -- every number the package returns equals the C++ value, bit for bit.

This is the only place the Python suite checks arithmetic, and it does not
*recompute* anything. The fixtures are emitted by `tools/gen_python_fixtures.cpp`,
which links the same library the extension does, and every value is printed
with `%.17g` -- enough to round-trip an IEEE double exactly. So the comparison
is `==`, not `approx`.

A tolerance would defeat the purpose. What this looks for is corruption in the
binding layer: a narrowing cast, a float in a caster, a long double spilling
to memory at a different precision. All of those move the last bits and none
of them would survive an exact comparison -- and all of them would hide under
`rel=1e-12`.

The exact comparison holds only WITHIN a platform, which the fixture records.
`exp`, `log` and `pow` are not correctly rounded in IEEE 754, the library
calls them 24 times, and glibc and macOS libm differ in the last bit.
Measured, on this project's own CI:

    0.08687005698251757  (Linux)   vs  0.08687005698251758  (macOS)
    delta = -1.39e-17, one ULP

Neither is wrong, and no amount of care in the bindings would reconcile them.
So CI regenerates the fixtures on each runner and compares exactly; a fixture
carried across platforms falls back to a tolerance, which still catches a
wrong formula or a unit error while tolerating libm. The fallback is stated
in the failure message so a tolerant pass is never mistaken for an exact one.

Whether the port matches *upstream* is a different question, answered by 49
differential suites that compile upstream into the same binary. Conflating
the two would make this file unrunnable anywhere the oracle cannot exist,
which is everywhere except one laptop.
"""

from __future__ import annotations

import datetime
from typing import Any
import json
import pathlib
import sys

import pytest

import cdsmodel as cds

FIXTURES = pathlib.Path(__file__).parent / "fixtures" / "pricing.json"


def _load() -> dict[str, Any]:
    if not FIXTURES.exists():  # pragma: no cover
        pytest.fail(f"missing {FIXTURES}; run tools/gen_python_fixtures.cpp")
    loaded: dict[str, Any] = json.loads(FIXTURES.read_text(encoding="utf-8"))
    return loaded


DATA = _load()

# Exact when the fixture was produced here; tolerant when it was carried from
# another platform. 1e-12 relative is the project's recorded-vector tier -- a
# few ULPs of libm fit inside it, a wrong formula does not.
_HERE = ("win32" if sys.platform.startswith("win")
         else "darwin" if sys.platform == "darwin" else "linux")
SAME_PLATFORM = DATA.get("platform") == _HERE
CROSS_PLATFORM_TOLERANCE = 1e-12


def _assert_matches(got: float, want: float, what: str) -> None:
    if SAME_PLATFORM:
        assert got == want, (
            f"{what}: {got!r} != {want!r}  delta={got - want!r}\n"
            f"Exact comparison: fixture was generated on this platform ({_HERE}).")
    else:
        assert got == pytest.approx(want, rel=CROSS_PLATFORM_TOLERANCE), (
            f"{what}: {got!r} != {want!r}\n"
            f"TOLERANT comparison (rel={CROSS_PLATFORM_TOLERANCE}): the fixture was "
            f"generated on {DATA.get('platform')!r}, this is {_HERE!r}. "
            f"Regenerate with tools/gen_python_fixtures.cpp for an exact check.")


def _date(s: str) -> datetime.date:
    return datetime.date.fromisoformat(s)


def _curve(spec: dict[str, Any]) -> cds.ZeroCurve:
    return cds.ZeroCurve.from_points(
        base_date=_date(DATA["today"]),
        dates=[_date(d) for d in spec["dates"]],
        rates=spec["rates"],
        basis=1.0,
        day_count=cds.DayCount.ACT_365F,
    )


DISCOUNT = _curve(DATA["discount_curve"])
SPREAD = _curve(DATA["spread_curve"])
VALUATION = cds.Valuation(
    today=_date(DATA["today"]),
    cash_settle=_date(DATA["settle"]),
    step_in=_date(DATA["stepin"]),
)


def _trade(case: dict[str, Any]) -> cds.TradeTerms:
    today = _date(DATA["today"])
    return cds.TradeTerms(
        start=today,
        end=datetime.date(today.year + case["maturity_years"], today.month, today.day),
        coupon_rate=case["coupon"],
    )


def test_the_comparison_is_exact_here_or_says_why_not() -> None:
    """A tolerant pass must never be mistaken for an exact one."""
    assert DATA.get("platform"), "fixture carries no platform stamp"
    if not SAME_PLATFORM:
        pytest.skip(
            f"fixture from {DATA['platform']!r}, running on {_HERE!r}: "
            f"comparisons are tolerant (rel={CROSS_PLATFORM_TOLERANCE}), not exact")


def test_the_fixture_is_not_empty() -> None:
    """A comparison over zero cases passes. This is what stops that."""
    assert len(DATA["cases"]) > 100, len(DATA["cases"])
    assert DATA["fee_leg_flows"]["flows"], "no recorded flows"


@pytest.mark.parametrize("case", DATA["cases"], ids=lambda c: (
    f"{c['maturity_years']}y-{c['coupon']}-{c['recovery']}-{c['day_count']}"
    f"-{'clean' if c['clean'] else 'dirty'}"))
def test_price_matches_the_cpp_value_exactly(case: dict[str, Any]) -> None:
    got = cds.price(
        _trade(case),
        VALUATION,
        cds.Conventions(day_count=getattr(cds.DayCount, case["day_count"])),
        cds.MarketData(discount=DISCOUNT, spread=SPREAD, recovery=case["recovery"]),
        clean=case["clean"],
    )
    _assert_matches(got, case["price"], "price")


@pytest.mark.parametrize("case", DATA["cases"], ids=lambda c: (
    f"{c['maturity_years']}y-{c['coupon']}-{c['recovery']}-{c['day_count']}"))
def test_par_spread_matches_the_cpp_value_exactly(case: dict[str, Any]) -> None:
    got = cds.par_spread(
        _trade(case),
        VALUATION,
        cds.Conventions(day_count=getattr(cds.DayCount, case["day_count"])),
        cds.MarketData(discount=DISCOUNT, spread=SPREAD, recovery=case["recovery"]),
    )
    _assert_matches(got, case["par_spread"], "par_spread")


def test_fee_leg_flows_match_date_and_amount_exactly() -> None:
    """A vector of structs: a different caster path from the scalars above."""
    spec = DATA["fee_leg_flows"]
    today = _date(DATA["today"])
    trade = cds.TradeTerms(
        start=today,
        end=datetime.date(today.year + spec["maturity_years"], today.month, today.day),
        coupon_rate=spec["coupon"],
    )
    got = cds.fee_leg_flows(trade, cds.Conventions(), notional=spec["notional"])
    assert len(got) == len(spec["flows"])
    for flow, expected in zip(got, spec["flows"]):
        assert flow.date == _date(expected["date"])
        _assert_matches(flow.amount, expected["amount"], "flow amount")


def test_default_accrual_matches_exactly() -> None:
    """A returned struct rather than a scalar or a vector."""
    spec = DATA["default_accrual"]
    today = _date(DATA["today"])
    trade = cds.TradeTerms(start=today, end=datetime.date(2029, 1, 2), coupon_rate=0.01)
    got = cds.default_accrual(
        trade, cds.Conventions(),
        trade_date=_date(spec["trade_date"]),
        event_determination_date=_date(spec["event_determination_date"]),
        notional=spec["notional"])
    assert got.days == spec["days"]
    _assert_matches(got.amount, spec["amount"], "accrual amount")
