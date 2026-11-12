"""The argument blocks and their enums.

Five blocks mirroring `xl/core`'s grouping (D23), so one mental model serves
Excel and Python. Everything is keyword-only: the blocks exist precisely
because the flat forms had up to seventeen positional arguments.
"""

from __future__ import annotations

import datetime

import pytest

import cdsmodel as cds

D = datetime.date
CURVE = cds.ZeroCurve.from_points(
    base_date=D(2024, 1, 1), dates=[D(2025, 1, 1), D(2029, 1, 1)], rates=[0.03, 0.035])


def test_conventions_needs_nothing_at_all() -> None:
    """D11a -- six fields, zero required. That is what makes the exemption
    defensible: the cap counts what a caller must pass, and here it is none."""
    c = cds.Conventions()
    assert c.day_count == cds.DayCount.ACT_360
    assert c.bad_day == cds.BadDayConv.NONE
    assert c.pay_accrual_on_default is True
    assert c.coupon_interval == cds.DateInterval(periods=3, unit=cds.PeriodUnit.MONTHS)
    assert len(c.holidays) == 1


def test_overriding_one_convention_leaves_the_rest_alone() -> None:
    c = cds.Conventions(day_count=cds.DayCount.ACT_365F)
    assert c.day_count == cds.DayCount.ACT_365F
    assert c.bad_day == cds.BadDayConv.NONE  # untouched


def test_bad_day_none_is_reachable() -> None:
    """The second `None` collision in the library, after `Weekend::None`."""
    assert cds.BadDayConv.NONE is not None
    assert repr(cds.BadDayConv.NONE) == "BadDayConv.NONE"


def test_valuation_requires_all_three_dates_by_keyword() -> None:
    v = cds.Valuation(today=D(2024, 3, 1), cash_settle=D(2024, 3, 4), step_in=D(2024, 3, 2))
    assert v.today == D(2024, 3, 1) and v.step_in == D(2024, 3, 2)
    # Three adjacent same-typed dates: positionally, a swap would be silent.
    with pytest.raises(TypeError):
        cds.Valuation(D(2024, 3, 1), D(2024, 3, 4), D(2024, 3, 2))


def test_stub_method_keeps_two_fields_rather_than_two_booleans() -> None:
    """Upstream's `TStubMethod` was `stubAtEnd` and `longStub` -- two bools a
    call site could swap silently. The port split them; Python keeps that."""
    s = cds.StubMethod(position=cds.StubPosition.BACK, length=cds.StubLength.LONG)
    assert s.position == cds.StubPosition.BACK
    assert s != cds.StubMethod()
    assert cds.StubMethod() == cds.StubMethod(
        position=cds.StubPosition.FRONT, length=cds.StubLength.SHORT)


def test_date_interval_compares_by_value() -> None:
    assert cds.DateInterval(periods=6, unit=cds.PeriodUnit.MONTHS) == \
           cds.DateInterval(periods=6, unit=cds.PeriodUnit.MONTHS)
    assert cds.DateInterval(periods=6, unit=cds.PeriodUnit.MONTHS) != \
           cds.DateInterval(periods=6, unit=cds.PeriodUnit.DAYS)


def test_market_data_holds_two_curves_and_a_recovery() -> None:
    m = cds.MarketData(discount=CURVE, spread=CURVE, recovery=0.4)
    assert m.recovery == 0.4
    assert m.discount.base_date == D(2024, 1, 1)


def test_flat_market_names_its_quote_by_role() -> None:
    """D11b -- `quote` is a par spread for `upfront` and an upfront charge for
    `par_spread_flat`. The two are inverses, so one block serves both."""
    f = cds.FlatMarket(discount=CURVE, quote=0.01, recovery=0.4)
    assert f.quote == 0.01


@pytest.mark.parametrize("name", ["Valuation", "Conventions", "MarketData",
                                  "FlatMarket", "DateInterval", "StubMethod"])
def test_every_block_reprs_its_contents(name: str) -> None:
    """P17 -- never `<cdsmodel._core.X object at 0x...>`."""
    cls = getattr(cds, name)
    assert cls.__repr__ is not object.__repr__, name


def test_a_convention_block_carries_real_calendars() -> None:
    cal = cds.Calendar.from_dates(holidays=[D(2024, 12, 25)])
    c = cds.Conventions(holidays=[cal, cds.Calendar.none()])
    assert len(c.holidays) == 2
    assert c.holidays[0].is_holiday(D(2024, 12, 25))
