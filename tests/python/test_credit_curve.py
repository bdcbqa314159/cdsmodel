"""The credit curve, and the upfront/spread pair.

These three complete the twelve the XLL exposes. `clean_spread_curve` is the
one a desk starts from: without it a caller hand-builds a curve of hazard
rates with `ZeroCurve.from_points`, which is not how anyone works.
"""

from __future__ import annotations

import datetime

import pytest

import cdsmodel as cds

D = datetime.date
TODAY = D(2024, 1, 2)
DISCOUNT = cds.ZeroCurve.from_points(
    base_date=TODAY, dates=[D(2025, 1, 2), D(2031, 1, 2)], rates=[0.05, 0.04])
VALUATION = cds.Valuation(today=TODAY, cash_settle=D(2024, 1, 4), step_in=D(2024, 1, 3))
CONV = cds.Conventions()
MARKET = cds.CurveMarket(discount=DISCOUNT, recovery=0.4)

QUOTES = [
    cds.Benchmark(end=D(2027, 1, 2), coupon_rate=0.010),
    cds.Benchmark(end=D(2029, 1, 2), coupon_rate=0.012),
    cds.Benchmark(end=D(2031, 1, 2), coupon_rate=0.014),
]
DATES = cds.UpfrontDates(today=TODAY, value=D(2024, 1, 4),
                         benchmark_start=TODAY, stepin=D(2024, 1, 3))


def _curve(quotes: list[cds.Benchmark] = QUOTES) -> cds.ZeroCurve:
    return cds.clean_spread_curve(VALUATION, quotes, CONV, MARKET, start=TODAY)


# ------------------------------------------------------ the calibration oracle

@pytest.mark.parametrize("benchmark", QUOTES, ids=lambda b: f"{b.end}@{b.coupon_rate}")
def test_the_curve_reprices_every_benchmark_to_zero(benchmark: cds.Benchmark) -> None:
    """The property that actually tests a bootstrap.

    A curve calibrated to a quote must value that quote at zero. This is not
    a recomputation of the library's arithmetic (D6) -- it is the defining
    property of the fit, and it would catch a curve built from the wrong
    column, the wrong start date, or the wrong recovery.
    """
    curve = _curve()
    trade = cds.TradeTerms(start=TODAY, end=benchmark.end, coupon_rate=benchmark.coupon_rate)
    priced = cds.price(trade, VALUATION, CONV,
                       cds.MarketData(discount=DISCOUNT, spread=curve, recovery=0.4))
    assert abs(priced) < 1e-12, f"benchmark did not reprice to zero: {priced!r}"


def test_one_point_per_included_benchmark() -> None:
    assert len(_curve()) == len(QUOTES)


def test_an_excluded_benchmark_is_left_out_of_the_fit() -> None:
    """`include` is upstream's own flag: present in the input, out of the fit."""
    quotes = [cds.Benchmark(end=b.end, coupon_rate=b.coupon_rate, include=(b is not QUOTES[1]))
              for b in QUOTES]
    assert len(_curve(quotes)) == len(QUOTES) - 1


def test_no_included_benchmark_is_refused() -> None:
    with pytest.raises(cds.BadArgument) as e:
        _curve([cds.Benchmark(end=D(2027, 1, 2), coupon_rate=0.01, include=False)])
    assert "no instrument" in str(e.value)


def test_a_wider_spread_gives_a_lower_survival() -> None:
    tight = _curve([cds.Benchmark(end=D(2029, 1, 2), coupon_rate=0.005)])
    wide = _curve([cds.Benchmark(end=D(2029, 1, 2), coupon_rate=0.05)])
    assert wide.discount_factor(D(2029, 1, 2)) < tight.discount_factor(D(2029, 1, 2))


# ------------------------------------------------- upfront and its inverse

def test_upfront_and_par_spread_flat_are_inverses() -> None:
    """The round trip is the oracle: one is a closed form, the other a root
    find over it, so agreeing to the solver's tolerance exercises both."""
    trade = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.012)
    for quoted in (0.005, 0.012, 0.030, 0.080):
        charge = cds.upfront(trade, DATES, CONV,
                             cds.FlatMarket(discount=DISCOUNT, quote=quoted, recovery=0.4))
        back = cds.par_spread_flat(trade, DATES, CONV,
                                   cds.FlatMarket(discount=DISCOUNT, quote=charge, recovery=0.4))
        assert back == pytest.approx(quoted, abs=1e-9), f"{quoted} -> {charge} -> {back}"


def test_a_coupon_at_the_market_costs_nothing_upfront() -> None:
    trade = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.012)
    charge = cds.upfront(trade, DATES, CONV,
                         cds.FlatMarket(discount=DISCOUNT, quote=0.012, recovery=0.4))
    assert abs(charge) < 1e-3, charge


def test_a_coupon_below_the_market_costs_the_buyer() -> None:
    trade = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.005)
    charge = cds.upfront(trade, DATES, CONV,
                         cds.FlatMarket(discount=DISCOUNT, quote=0.030, recovery=0.4))
    assert charge > 0.0, charge


def test_flat_market_quote_means_different_things_to_the_two_functions() -> None:
    """D11b -- `quote` is named for its role. A par spread going in, an
    upfront charge coming back, and the same block carries both."""
    trade = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.012)
    as_spread = cds.FlatMarket(discount=DISCOUNT, quote=0.02, recovery=0.4)
    charge = cds.upfront(trade, DATES, CONV, as_spread)
    as_charge = cds.FlatMarket(discount=DISCOUNT, quote=charge, recovery=0.4)
    assert cds.par_spread_flat(trade, DATES, CONV, as_charge) == pytest.approx(0.02, abs=1e-9)


def test_an_inverted_curve_reports_no_root() -> None:
    """P4 -- the third of the four error kinds, and the one that took three
    wrong guesses to find.

    A steeply inverted spread curve (8%, 2%, 0.5%) implies a forward hazard
    rate below zero, so no rate solves the later segment. That is `no_root`,
    distinct from `bad_argument`: the input is well-formed and the answer
    does not exist, which is exactly the distinction artifact #01's taxonomy
    was created to draw.

    It is a realistic quote, not a contrived one -- a distressed name trades
    with a downward-sloping curve.
    """
    far = cds.ZeroCurve.from_points(base_date=TODAY, dates=[D(2035, 1, 2)], rates=[0.04])
    quotes = [cds.Benchmark(end=D(2027, 1, 2), coupon_rate=0.080),
              cds.Benchmark(end=D(2029, 1, 2), coupon_rate=0.020),
              cds.Benchmark(end=D(2031, 1, 2), coupon_rate=0.005)]
    with pytest.raises(cds.NoRoot) as e:
        cds.clean_spread_curve(VALUATION, quotes, CONV,
                               cds.CurveMarket(discount=far, recovery=0.4), start=TODAY)
    assert "negative forward rate" in str(e.value), str(e.value)


def test_no_root_is_not_caught_as_bad_argument() -> None:
    """A taxonomy nothing branches on is decoration. These two kinds derive
    from different builtins, so catching one must not catch the other."""
    assert not issubclass(cds.NoRoot, cds.BadArgument)
    assert issubclass(cds.NoRoot, ArithmeticError)
    assert issubclass(cds.BadArgument, ValueError)


def test_a_mildly_inverted_curve_still_builds() -> None:
    """The positive control: inversion alone is not an error, so the test
    above is detecting a real condition rather than any downward slope."""
    curve = _curve([cds.Benchmark(end=D(2027, 1, 2), coupon_rate=0.020),
                    cds.Benchmark(end=D(2029, 1, 2), coupon_rate=0.015),
                    cds.Benchmark(end=D(2031, 1, 2), coupon_rate=0.012)])
    assert len(curve) == 3


def test_an_unreachable_upfront_is_refused_not_silently_wrong() -> None:
    trade = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.01)
    with pytest.raises(cds.CdsModelError):
        cds.par_spread_flat(trade, DATES, CONV,
                            cds.FlatMarket(discount=DISCOUNT, quote=1e9, recovery=0.4))
