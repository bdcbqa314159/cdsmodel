"""P4, P5 -- `Expected<T>` arrives as a raised exception with its own message.

The taxonomy exists because upstream's 452 diagnostic messages collapse to
four *kinds*; this is the consumer that finally branches on them. Each class
also derives from the builtin a Python caller reaches for first, so both
`except ValueError` and `except CdsModelError` work.
"""

from __future__ import annotations

import datetime

import pytest

import cdsmodel

D = datetime.date
GOOD_DATES = [D(2024, 1, 1), D(2025, 1, 1), D(2026, 1, 1)]
GOOD_RATES = [0.01, 0.02, 0.03]
BASE = D(2024, 1, 1)


def test_a_valid_curve_builds() -> None:
    """The positive control. Without it the refusals below prove nothing."""
    curve = cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=GOOD_DATES, rates=GOOD_RATES)
    assert len(curve) == 3
    assert curve.base_date == D(2024, 1, 1)


# --------------------------------------------------------------- P4: the kinds

def test_bad_argument_is_raised_and_names_the_offending_value() -> None:
    with pytest.raises(cdsmodel.BadArgument) as e:
        cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=[], rates=[])
    assert "at least one point" in str(e.value)


def test_a_non_increasing_curve_says_which_point() -> None:
    """D5 -- the library's own message arrives unchanged, not a wrapper."""
    with pytest.raises(cdsmodel.BadArgument) as e:
        cdsmodel.ZeroCurve.from_points(
            base_date=BASE, dates=[D(2024, 1, 1), D(2024, 1, 1)], rates=[0.01, 0.02])
    msg = str(e.value)
    assert "strictly increase" in msg and "point 1" in msg, msg


def test_unsupported_is_its_own_kind() -> None:
    with pytest.raises(cdsmodel.Unsupported) as e:
        cdsmodel.ZeroCurve.from_points(
            base_date=BASE, dates=GOOD_DATES, rates=GOOD_RATES,
            day_count=cdsmodel.DayCount.THIRTY_360)
    assert "ACT/365F and ACT/360" in str(e.value)


def test_a_length_mismatch_is_refused_at_the_boundary() -> None:
    """The library takes one span of pairs and cannot see this; Python can."""
    with pytest.raises(cdsmodel.BadArgument) as e:
        cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=GOOD_DATES, rates=[0.01])
    assert "3 dates, 1 rates" in str(e.value)


# ------------------------------------------------- P5: both ways of catching

@pytest.mark.parametrize(
    "kind,builtin",
    [
        ("BadArgument", ValueError),
        ("Unsupported", NotImplementedError),
        ("NoRoot", ArithmeticError),
        ("NoConvergence", ArithmeticError),
    ],
)
def test_every_kind_derives_from_both_bases(kind: str, builtin: type) -> None:
    exc = getattr(cdsmodel, kind)
    assert issubclass(exc, cdsmodel.CdsModelError), f"{kind} is not a CdsModelError"
    assert issubclass(exc, builtin), f"{kind} is not a {builtin.__name__}"


def test_a_failure_is_catchable_as_the_builtin() -> None:
    with pytest.raises(ValueError):
        cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=[], rates=[])


def test_a_failure_is_catchable_as_the_library_base() -> None:
    with pytest.raises(cdsmodel.CdsModelError):
        cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=[], rates=[])


def test_the_kinds_are_distinguishable() -> None:
    """A taxonomy that cannot be branched on is not a taxonomy."""
    with pytest.raises(cdsmodel.BadArgument):
        try:
            cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=[], rates=[])
        except cdsmodel.Unsupported:  # must NOT catch it
            pytest.fail("bad_argument was caught as Unsupported")


# ------------------------------------------------------- P18: under the guard

def test_failures_and_successes_interleave_without_corrupting_state() -> None:
    """D10 releases the GIL; D20 keeps the Python API out of the guarded body.

    One call proves nothing here -- GIL misuse corrupts rather than fails.
    """
    curve = cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=GOOD_DATES, rates=GOOD_RATES)
    expected = curve.discount_factor(D(2025, 6, 1))
    for _ in range(300):
        with pytest.raises(cdsmodel.BadArgument):
            cdsmodel.ZeroCurve.from_points(base_date=BASE, dates=[], rates=[])
        assert curve.discount_factor(D(2025, 6, 1)) == expected
