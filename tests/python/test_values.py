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

Whether the port matches *upstream* is a different question, answered by 49
differential suites that compile upstream into the same binary. Conflating
the two would make this file unrunnable anywhere the oracle cannot exist,
which is everywhere except one laptop.
"""

from __future__ import annotations

import datetime
import json
import pathlib

import pytest

import cdsmodel as cds

FIXTURES = pathlib.Path(__file__).parent / "fixtures" / "pricing.json"


def _load():
    if not FIXTURES.exists():  # pragma: no cover
        pytest.fail(f"missing {FIXTURES}; run tools/gen_python_fixtures.cpp")
    return json.loads(FIXTURES.read_text(encoding="utf-8"))


DATA = _load()


def _date(s: str) -> datetime.date:
    return datetime.date.fromisoformat(s)


def _curve(spec) -> cds.ZeroCurve:
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


def _trade(case) -> cds.TradeTerms:
    today = _date(DATA["today"])
    return cds.TradeTerms(
        start=today,
        end=datetime.date(today.year + case["maturity_years"], today.month, today.day),
        coupon_rate=case["coupon"],
    )


def test_the_fixture_is_not_empty() -> None:
    """A comparison over zero cases passes. This is what stops that."""
    assert len(DATA["cases"]) > 100, len(DATA["cases"])
    assert DATA["fee_leg_flows"]["flows"], "no recorded flows"


@pytest.mark.parametrize("case", DATA["cases"], ids=lambda c: (
    f"{c['maturity_years']}y-{c['coupon']}-{c['recovery']}-{c['day_count']}"
    f"-{'clean' if c['clean'] else 'dirty'}"))
def test_price_matches_the_cpp_value_exactly(case) -> None:
    got = cds.price(
        _trade(case),
        VALUATION,
        cds.Conventions(day_count=getattr(cds.DayCount, case["day_count"])),
        cds.MarketData(discount=DISCOUNT, spread=SPREAD, recovery=case["recovery"]),
        clean=case["clean"],
    )
    assert got == case["price"], f"{got!r} != {case['price']!r}  delta={got - case['price']!r}"


@pytest.mark.parametrize("case", DATA["cases"], ids=lambda c: (
    f"{c['maturity_years']}y-{c['coupon']}-{c['recovery']}-{c['day_count']}"))
def test_par_spread_matches_the_cpp_value_exactly(case) -> None:
    got = cds.par_spread(
        _trade(case),
        VALUATION,
        cds.Conventions(day_count=getattr(cds.DayCount, case["day_count"])),
        cds.MarketData(discount=DISCOUNT, spread=SPREAD, recovery=case["recovery"]),
    )
    assert got == case["par_spread"], f"{got!r} != {case['par_spread']!r}"


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
        assert flow.amount == expected["amount"], f"{flow.amount!r} != {expected['amount']!r}"


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
    assert got.amount == spec["amount"], f"{got.amount!r} != {spec['amount']!r}"
