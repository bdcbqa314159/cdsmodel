"""Bootstrapping a zero curve from money-market and swap quotes."""

from __future__ import annotations

import datetime

import pytest

import cdsmodel as cds

D = datetime.date
VALUE = D(2024, 1, 2)
MM, SW = cds.InstrumentKind.MONEY_MARKET, cds.InstrumentKind.SWAP


def q(kind: cds.InstrumentKind, date: datetime.date, rate: float) -> cds.InstrumentQuote:
    return cds.InstrumentQuote(kind=kind, date=date, rate=rate)


MARKET = [q(MM, D(2024, 4, 2), 0.053), q(MM, D(2024, 7, 2), 0.052),
          q(SW, D(2026, 1, 2), 0.045), q(SW, D(2029, 1, 2), 0.040)]


def test_a_curve_bootstraps_from_quotes() -> None:
    curve = cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET)
    # More points than instruments: a swap contributes one per coupon date,
    # not one per quote. Asserting len == 4 here would be asserting a
    # misunderstanding of what bootstrapping produces.
    assert len(curve) > len(MARKET)
    assert curve.base_date == VALUE


def test_discount_factors_fall_as_maturity_lengthens() -> None:
    """A property, not a pinned number: the values themselves are the C++
    suite's business (D6), but a curve whose discount factors rise with
    maturity would be wrong in a way worth catching cheaply."""
    curve = cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET)
    dates = [D(2024, 7, 1), D(2025, 1, 1), D(2026, 1, 1), D(2028, 1, 1)]
    factors = [curve.discount_factor(d) for d in dates]
    assert factors == sorted(factors, reverse=True), factors
    assert all(0.0 < f <= 1.0 for f in factors), factors


def test_the_value_date_discounts_to_one() -> None:
    curve = cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET)
    assert curve.discount_factor(VALUE) == pytest.approx(1.0)


def test_defaults_mean_a_caller_supplies_only_the_quotes() -> None:
    assert len(cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET)) > 0


def test_conventions_change_the_answer() -> None:
    a = cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET)
    b = cds.ZeroCurve.build(value_date=VALUE, instruments=MARKET,
                            conventions=cds.SwapConventions(fixed_frequency=1))
    assert a.discount_factor(D(2028, 1, 1)) != b.discount_factor(D(2028, 1, 1))


@pytest.mark.parametrize(
    "label,instruments,fragment",
    [
        ("empty", [], "no instruments"),
        ("swap before value date", [q(SW, D(2020, 1, 2), 0.04)], "matures before"),
        ("rate at -200%", [q(MM, D(2024, 4, 2), 0.05), q(SW, D(2029, 1, 2), -2.0)],
         "at or below -100%"),
        ("duplicate maturities", [q(MM, D(2024, 4, 2), 0.05), q(MM, D(2024, 4, 2), 0.06)],
         "money-market"),
    ],
)
def test_bad_input_is_refused_with_a_message_that_says_why(
    label: str, instruments: list[cds.InstrumentQuote], fragment: str
) -> None:
    with pytest.raises(cds.BadArgument) as e:
        cds.ZeroCurve.build(value_date=VALUE, instruments=instruments)
    assert fragment in str(e.value), str(e.value)


def test_a_quote_object_prevents_the_mismatched_array_bug() -> None:
    """Upstream keeps kinds, dates and rates in three parallel arrays with one
    shared count, which is the shape that lets them disagree. One object per
    quote makes that unrepresentable rather than merely checked."""
    one = q(MM, D(2024, 4, 2), 0.053)
    assert one == q(MM, D(2024, 4, 2), 0.053)
    assert one != q(SW, D(2024, 4, 2), 0.053)
    assert "MONEY_MARKET" in repr(one) and "2024-04-02" in repr(one)


def test_swap_conventions_need_nothing_but_remember_what_they_are_given() -> None:
    assert cds.SwapConventions().fixed_frequency == 2
    assert cds.SwapConventions(fixed_frequency=4).fixed_frequency == 4
    assert cds.SwapConventions().interp == cds.InterpType.FLAT_FORWARDS
