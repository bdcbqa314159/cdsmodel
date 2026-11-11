"""P2 -- a date that goes in comes back as the same date, in every time zone.

The stock `nanobind/stl/chrono.h` caster converts through `localtime_r` and
`mktime`, and a `sys_days` is midnight UTC -- so west of UTC it returns the
*previous day*. Measured before this was written: 2024-03-15 came back as
2024-03-14 in America/New_York, and on a round trip in Pacific/Auckland.

A test pinned to one zone would pass against the broken caster, because UTC
and Europe/London happen to be correct. The western zones are the positive
control: without them this measures the fixture, not the code.
"""

from __future__ import annotations

import datetime
import os
import sys

import pytest

import cdsmodel

# tzset() is POSIX-only; Python exposes no portable equivalent on Windows.
# The regression this guards against is a time-zone conversion in the caster,
# which is platform-independent, so catching it on Linux and macOS is enough --
# but the Windows leg genuinely does not sweep, and says so rather than
# pretending otherwise.
_CAN_SWEEP_TZ = hasattr(__import__("time"), "tzset")

ZONES = ["UTC", "America/New_York", "Europe/London", "Pacific/Auckland"]

# Chosen for the ways day arithmetic breaks: a leap day, both DST transitions
# in each hemisphere, a year boundary, and the epochs the port juggles.
DATES = [
    datetime.date(2024, 2, 29),   # leap day
    datetime.date(2024, 3, 10),   # US DST starts
    datetime.date(2024, 3, 15),   # the date that failed in New York
    datetime.date(2024, 3, 31),   # EU DST starts
    datetime.date(2024, 4, 7),    # AU DST ends
    datetime.date(2024, 11, 3),   # US DST ends
    datetime.date(2024, 12, 31),  # year boundary
    datetime.date(1970, 1, 1),    # Unix epoch
    datetime.date(1900, 1, 1),    # Excel epoch
    datetime.date(2099, 12, 31),  # far future
]


def _set_zone(zone: str) -> None:
    import time

    os.environ["TZ"] = zone
    time.tzset()


@pytest.fixture(autouse=True)
def _restore_tz():
    before = os.environ.get("TZ")
    yield
    if _CAN_SWEEP_TZ:
        import time

        if before is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = before
        time.tzset()


@pytest.mark.parametrize("zone", ZONES if _CAN_SWEEP_TZ else ["<native>"])
@pytest.mark.parametrize("day", DATES)
def test_a_date_round_trips_unchanged(zone: str, day: datetime.date) -> None:
    if _CAN_SWEEP_TZ:
        _set_zone(zone)
    terms = cdsmodel.TradeTerms(start=day, end=day, coupon_rate=0.01)
    assert terms.start == day, f"{terms.start} != {day} under TZ={zone}"
    assert terms.end == day


@pytest.mark.parametrize("zone", ZONES if _CAN_SWEEP_TZ else ["<native>"])
def test_the_result_is_a_date_not_a_datetime(zone: str) -> None:
    if _CAN_SWEEP_TZ:
        _set_zone(zone)
    got = cdsmodel.TradeTerms(start=datetime.date(2024, 3, 15),
                              end=datetime.date(2029, 6, 20)).start
    # Order matters: datetime is a *subclass* of date, so `isinstance(x, date)`
    # passes for a datetime and would not catch the stock caster at all.
    assert type(got) is datetime.date, f"got {type(got).__name__} under TZ={zone}"
    assert not isinstance(got, datetime.datetime)


@pytest.mark.skipif(not _CAN_SWEEP_TZ, reason="tzset() is POSIX-only")
def test_every_zone_agrees_with_every_other() -> None:
    """The property that fails loudly if a time zone ever enters the caster."""
    day = datetime.date(2024, 3, 15)
    seen = {}
    for zone in ZONES:
        _set_zone(zone)
        seen[zone] = cdsmodel.TradeTerms(start=day, end=day).start
    assert len(set(seen.values())) == 1, f"zones disagreed: {seen}"


def test_a_datetime_is_accepted_but_narrowed_to_its_date() -> None:
    noon = datetime.datetime(2024, 3, 15, 12, 30)
    assert cdsmodel.TradeTerms(start=noon, end=noon).start == datetime.date(2024, 3, 15)


def test_repr_names_its_fields() -> None:
    """P17 -- a Jupyter user must see values, not an address."""
    r = repr(cdsmodel.TradeTerms(start=datetime.date(2024, 3, 20),
                                 end=datetime.date(2029, 6, 20), coupon_rate=0.01))
    assert "TradeTerms(" in r and "2024-03-20" in r and "2029-06-20" in r, r


def test_dates_must_be_passed_by_keyword() -> None:
    """D11 -- `start` and `end` are adjacent same-typed dates."""
    with pytest.raises(TypeError):
        cdsmodel.TradeTerms(datetime.date(2024, 3, 20), datetime.date(2029, 6, 20))
