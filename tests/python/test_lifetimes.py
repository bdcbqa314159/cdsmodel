"""P6 and P13 -- objects outlive the names that built them, and survive threads.

The XLL needs a handle registry because a cell cannot hold a curve, and that
registry needed a mutex and `shared_ptr<const T>` before it was safe. Python
holds the object directly, so the stale-handle class of bug does not exist
here -- but the lifetime guarantee that replaces it has to be checked rather
than assumed, because the C++ side takes `const ZeroCurve &`.
"""

from __future__ import annotations

import datetime
import gc
from concurrent.futures import ThreadPoolExecutor

import pytest

import cdsmodel as cds

D = datetime.date
TODAY = D(2024, 1, 2)


def _curve(rate: float = 0.03) -> cds.ZeroCurve:
    return cds.ZeroCurve.from_points(
        base_date=TODAY, dates=[D(2025, 1, 2), D(2031, 1, 2)], rates=[rate, rate + 0.005])


def _market(rate: float = 0.03) -> cds.MarketData:
    return cds.MarketData(discount=_curve(rate), spread=_curve(0.015), recovery=0.4)


TRADE = cds.TradeTerms(start=TODAY, end=D(2029, 1, 2), coupon_rate=0.01)
VALUATION = cds.Valuation(today=TODAY, cash_settle=D(2024, 1, 4), step_in=D(2024, 1, 3))


# --------------------------------------------------------------- P6 lifetimes

def test_a_curve_outlives_the_lists_that_built_it() -> None:
    dates = [D(2025, 1, 2), D(2031, 1, 2)]
    rates = [0.03, 0.035]
    curve = cds.ZeroCurve.from_points(base_date=TODAY, dates=dates, rates=rates)
    before = curve.discount_factor(D(2027, 1, 2))

    dates.clear()
    rates.clear()
    del dates, rates
    gc.collect()

    assert curve.discount_factor(D(2027, 1, 2)) == before


def test_a_curve_outlives_the_name_that_built_it() -> None:
    market = _market()
    expected = cds.price(TRADE, VALUATION, cds.Conventions(), market)

    # Rebind the only name holding the curves; the MarketData must still own
    # them. If the binding kept a reference rather than a value, this is
    # where it would crash or return garbage.
    market2 = cds.MarketData(discount=_curve(0.09), spread=_curve(0.05), recovery=0.1)
    gc.collect()

    assert cds.price(TRADE, VALUATION, cds.Conventions(), market) == expected
    assert cds.price(TRADE, VALUATION, cds.Conventions(), market2) != expected


def test_a_calendar_outlives_the_conventions_that_held_it() -> None:
    conv = cds.Conventions(holidays=[cds.Calendar.from_dates(holidays=[D(2024, 12, 25)])])
    held = conv.holidays[0]
    del conv
    gc.collect()
    assert held.is_holiday(D(2024, 12, 25))


def test_a_curve_cannot_be_mutated_through_its_properties() -> None:
    """Immutable in C++, so immutable here. `shared_ptr<const T>` is what
    makes sharing one across threads safe, and a setter would undo it."""
    curve = _curve()
    for attr, value in (("base_date", D(2030, 1, 1)), ("basis", 2.0),
                        ("day_count", cds.DayCount.ACT_360)):
        with pytest.raises(AttributeError):
            setattr(curve, attr, value)


def test_flows_returned_to_python_are_independent_copies() -> None:
    flows_a = cds.fee_leg_flows(TRADE, cds.Conventions(), notional=1e7)
    flows_b = cds.fee_leg_flows(TRADE, cds.Conventions(), notional=1e7)
    assert flows_a[0].amount == flows_b[0].amount
    assert flows_a[0] is not flows_b[0]


# ----------------------------------------------------------------- P13 threads

def test_four_threads_price_identically_to_one() -> None:
    """D10 releases the GIL around the call, so this really is concurrent.

    A single call proves nothing: the failure mode of a mishandled GIL or a
    shared mutable is corruption under contention, not an immediate error.
    """
    market = _market()
    conventions = cds.Conventions()
    serial = cds.price(TRADE, VALUATION, conventions, market)

    def once(_: int) -> float:
        return cds.price(TRADE, VALUATION, conventions, market)

    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(once, range(400)))

    assert len(results) == 400
    assert all(r == serial for r in results), "threads disagreed with the serial result"


def test_threads_sharing_one_curve_do_not_corrupt_it() -> None:
    """The curves are `shared_ptr<const T>`; four threads reading one object
    is exactly what that buys."""
    shared = _curve()
    expected = shared.discount_factor(D(2027, 1, 2))

    def read(_: int) -> float:
        return shared.discount_factor(D(2027, 1, 2))

    with ThreadPoolExecutor(max_workers=4) as pool:
        assert all(v == expected for v in pool.map(read, range(400)))


def test_threads_mixing_success_and_failure_stay_consistent() -> None:
    """Failure unwinds past the GIL guard's destructor; doing that from four
    threads at once is where a mistake in D20 would show."""
    market = _market()

    def work(i: int) -> float:
        if i % 3 == 0:
            with pytest.raises(cds.BadArgument):
                cds.ZeroCurve.from_points(base_date=TODAY, dates=[], rates=[])
        return cds.price(TRADE, VALUATION, cds.Conventions(), market)

    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(work, range(300)))
    assert len(set(results)) == 1, f"{len(set(results))} distinct prices across threads"
