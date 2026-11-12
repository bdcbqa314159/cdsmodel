"""Calendars: holidays, weekend flags, and reading a holiday file.

`Calendar.parse` takes contents, not a path (artifact #4, D4). The reading
lives in Python, which is both less code and safer -- see `read_holiday_file`.
"""

from __future__ import annotations

import datetime
import pathlib

import pytest

import cdsmodel

D = datetime.date
XMAS = D(2024, 12, 25)
SATURDAY = D(2024, 12, 28)
SUNDAY = D(2024, 12, 29)
MONDAY = D(2024, 12, 30)


def test_a_calendar_separates_holidays_weekends_and_business_days() -> None:
    cal = cdsmodel.Calendar.from_dates(holidays=[XMAS])
    assert cal.is_holiday(XMAS) and not cal.is_business_day(XMAS)
    assert cal.is_weekend(SATURDAY) and not cal.is_business_day(SATURDAY)
    assert cal.is_business_day(MONDAY)
    # A holiday is not a weekend and vice versa -- upstream conflates the two
    # in places, and keeping them separate is what makes the predicates useful.
    assert not cal.is_weekend(XMAS)
    assert not cal.is_holiday(SATURDAY)


def test_the_two_degenerate_calendars() -> None:
    assert not cdsmodel.Calendar.none().is_business_day(SATURDAY)
    assert cdsmodel.Calendar.no_weekends().is_business_day(SATURDAY)
    assert len(cdsmodel.Calendar.none()) == 0


# ------------------------------------------------------ the Weekend flag enum

def test_weekend_none_is_reachable_at_all() -> None:
    """D17 -- `Weekend::None` in C++ would be `Weekend.None`, a syntax error.

    This is the collision the UPPER_CASE rule exists for. It is checked here
    as behaviour as well as in P16 as a name, because the failure mode is an
    import error, not a wrong value.
    """
    assert cdsmodel.Weekend.NONE is not None
    assert not hasattr(cdsmodel.Weekend, "None_")


def test_weekend_flags_combine_and_repr_readably() -> None:
    both = cdsmodel.Weekend.SATURDAY | cdsmodel.Weekend.SUNDAY
    assert both == cdsmodel.Weekend.STANDARD
    assert repr(both) == "Weekend.STANDARD"
    # A Friday/Saturday weekend, as used across the Middle East.
    assert "FRIDAY" in repr(cdsmodel.Weekend.FRIDAY | cdsmodel.Weekend.SATURDAY)


def test_a_friday_saturday_weekend_actually_changes_business_days() -> None:
    cal = cdsmodel.Calendar.from_dates(
        holidays=[], weekends=cdsmodel.Weekend.FRIDAY | cdsmodel.Weekend.SATURDAY)
    assert cal.is_weekend(D(2024, 12, 27))      # Friday
    assert not cal.is_weekend(SUNDAY)           # Sunday is a working day
    assert cal.is_business_day(SUNDAY)


def test_a_calendar_with_every_day_a_weekend_is_refused() -> None:
    """Seven weekend days leaves no business day for date arithmetic to land on."""
    every = (cdsmodel.Weekend.MONDAY | cdsmodel.Weekend.TUESDAY
             | cdsmodel.Weekend.WEDNESDAY | cdsmodel.Weekend.THURSDAY
             | cdsmodel.Weekend.FRIDAY | cdsmodel.Weekend.SATURDAY
             | cdsmodel.Weekend.SUNDAY)
    cal = cdsmodel.Calendar.from_dates(holidays=[], weekends=every)
    assert not cal.allows_business_days


# ----------------------------------------------------------- the holiday file

# 19631026 appears in upstream's own format documentation -- and it was a
# SATURDAY, so `Calendar::create` drops it: a holiday already covered by the
# weekend rule is redundant, and upstream drops it too. Every date here is a
# weekday, so the count is the line count.
GOOD_FILE = """\
# a comment, and the blank line below is skipped too

20241224
20241225
20241226
"""


def test_parsing_contents_needs_no_file() -> None:
    cal = cdsmodel.Calendar.parse(GOOD_FILE)
    assert len(cal) == 3
    assert cal.is_holiday(D(2024, 12, 24)) and cal.is_holiday(XMAS)


def test_a_holiday_falling_on_a_weekend_is_dropped() -> None:
    """Upstream drops it, and so does the port: the weekend rule already
    covers the date, so storing it again would be redundant.

    Worth pinning rather than discovering: 1963-10-26 is the example date in
    upstream's own format documentation, and it was a Saturday -- so the
    obvious smoke test for the parser silently counts one fewer than its
    input has lines.
    """
    saturday = D(1963, 10, 26)
    assert saturday.weekday() == 5
    cal = cdsmodel.Calendar.parse("19631026\n")
    assert len(cal) == 0
    # Still not a business day -- the weekend rule covers it.
    assert not cal.is_business_day(saturday)


def test_the_same_holiday_survives_once_saturday_is_a_business_day() -> None:
    """The directive and the drop interact: make Saturday a working day and
    the Saturday holiday is no longer redundant, so it is kept."""
    cal = cdsmodel.Calendar.parse("# SATURDAY_NOT_ALWAYS_HOLIDAY\n19631026\n")
    assert len(cal) == 1
    assert cal.is_holiday(D(1963, 10, 26))
    assert not cal.is_business_day(D(1963, 10, 26))


def test_a_directive_turns_a_weekend_day_into_a_business_day() -> None:
    cal = cdsmodel.Calendar.parse("# SATURDAY_NOT_ALWAYS_HOLIDAY\n20241225\n")
    assert not cal.is_weekend(SATURDAY)
    assert cal.is_weekend(SUNDAY)


def test_out_of_order_dates_are_refused() -> None:
    with pytest.raises(cdsmodel.BadArgument):
        cdsmodel.Calendar.parse("20241226\n20241225\n")


def test_a_malformed_date_line_is_refused() -> None:
    """D2 of artifact #4 -- upstream silently skipped these."""
    with pytest.raises(cdsmodel.BadArgument):
        cdsmodel.Calendar.parse("20241225\nnot-a-date\n")


def test_reading_a_real_file(tmp_path: pathlib.Path) -> None:
    path = tmp_path / "holidays.txt"
    path.write_text(GOOD_FILE, encoding="utf-8")
    cal = cdsmodel.read_holiday_file(path)
    assert len(cal) == 3
    assert cdsmodel.read_holiday_file(str(path)).is_holiday(XMAS)


def test_an_empty_file_is_legitimate(tmp_path: pathlib.Path) -> None:
    """Upstream accepts one, producing weekends only -- which is why the
    directory case below cannot be distinguished by emptiness alone."""
    path = tmp_path / "empty.txt"
    path.write_text("", encoding="utf-8")
    cal = cdsmodel.read_holiday_file(path)
    assert len(cal) == 0
    assert not cal.is_business_day(SATURDAY)


def test_a_missing_file_raises_the_python_error(tmp_path: pathlib.Path) -> None:
    with pytest.raises(FileNotFoundError):
        cdsmodel.read_holiday_file(tmp_path / "nope.txt")


def test_a_directory_is_refused_rather_than_read_as_empty(tmp_path: pathlib.Path) -> None:
    """The trap `xl/core` has to guard against explicitly.

    `std::ifstream` opens a directory on some platforms and reads zero bytes,
    and an empty holiday file is legitimate -- so a C++ reader without an
    is-regular-file check hands back a working calendar with no holidays, the
    same silent-wrong-calendar failure as UPSTREAM-41. Python raises instead.

    `OSError` and not a subclass: POSIX raises `IsADirectoryError`, Windows
    raises `PermissionError` (WinError 5). Both are `OSError`, and the
    property under test is that it raises **at all** rather than handing back
    an empty calendar -- which is what the assertion below states explicitly.
    """
    with pytest.raises(OSError) as excinfo:
        cdsmodel.read_holiday_file(tmp_path)
    assert isinstance(excinfo.value, (IsADirectoryError, PermissionError)), excinfo.value
