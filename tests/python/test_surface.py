"""P8, P15, P16, P17 -- the shape of the public surface.

`inspect.signature` does not work on nanobind callables: it reports
`(*args, **kwargs)` for functions and raises for methods. The real signature
is the first line of `__doc__`, which is also where nanobind's own stub
generator reads it from. These tests parse that.
"""

from __future__ import annotations

import keyword
import re

import pytest

import cdsmodel

# D11a: the one named exemption. `Conventions` carries six fields, all
# defaulted, so a caller passes none of them. Named individually, never
# granted by pattern -- the same discipline as the error channel's D7
# exemptions. A *new* callable at six fails; widening this one to seven fails.
ARITY_EXEMPTIONS = {"Conventions.__init__"}
ARITY_CAP = 5

# P8: everything the package exposes, listed. A new export must be added here
# deliberately, so nothing leaks onto the public surface by accident.
EXPECTED_EXPORTS = {
    "BadArgument",
    "Calendar",
    "CdsModelError",
    "DayCount",
    "NoConvergence",
    "NoRoot",
    "TradeTerms",
    "Unsupported",
    "Weekend",
    "ZeroCurve",
    "read_holiday_file",
    "__upstream_version__",
    "__version__",
}


def test_the_package_exports_exactly_what_is_listed() -> None:
    assert set(cdsmodel.__all__) == EXPECTED_EXPORTS
    for name in EXPECTED_EXPORTS:
        assert hasattr(cdsmodel, name), f"{name} is in __all__ but not importable"


def _count_params(signature: str) -> int:
    """Top-level commas in a signature, ignoring nested brackets.

    `Sequence[datetime.date]` contains no top-level comma; `dict[str, int]`
    does contain one, nested. Counting naively would overcount both.
    """
    inner = signature[signature.index("(") + 1 : signature.rindex(")")]
    depth = 0
    params = 0 if not inner.strip() else 1
    for ch in inner:
        if ch in "[({":
            depth += 1
        elif ch in "])}":
            depth -= 1
        elif ch == "," and depth == 0:
            params += 1
    # `*` and `/` are markers, not parameters.
    for marker in ("*,", "/,"):
        params -= inner.count(marker)
    if inner.rstrip().endswith("*") or inner.rstrip().endswith("/"):
        params -= 1
    return max(params, 0)


def _is_bound_from_cpp(obj: object) -> bool:
    """Is this a nanobind-bound callable, rather than a Python builtin?

    Without this the walk below picks up `str.count` (because `__version__`
    is a string) and `Exception.add_note` (inherited by the four error
    classes), so the suite would check about forty things that are not ours
    and would pass with our own surface entirely empty.
    """
    return type(obj).__module__ == "nanobind"


def _callables():
    """(label, first docstring line) for every nanobind-bound callable."""
    for name in cdsmodel.__all__:
        obj = getattr(cdsmodel, name)
        if _is_bound_from_cpp(obj):
            doc = (obj.__doc__ or "").strip()
            first = doc.splitlines()[0] if doc else ""
            # A bound class's docstring is prose, not a signature.
            if "(" in first and ")" in first:
                yield name, first
        if not isinstance(obj, type):
            continue
        for attr in dir(obj):
            if attr.startswith("_") and attr != "__init__":
                continue
            member = getattr(obj, attr, None)
            if not _is_bound_from_cpp(member):
                continue
            # Every overload, not just the first line: nanobind lists each
            # on its own line, so checking only line one would let a wide
            # second overload through unseen.
            for line in (member.__doc__ or "").strip().splitlines():
                line = line.strip()
                if line.startswith(f"{attr}(") or line.startswith("__init__("):
                    yield f"{name}.{attr}", line


def test_no_callable_exceeds_the_five_parameter_cap() -> None:
    offenders = []
    checked = 0
    for label, sig in _callables():
        if label in ARITY_EXEMPTIONS:
            continue
        # `self` is not a parameter the caller supplies.
        n = _count_params(sig) - (1 if ".__init__" in label or sig.startswith("(self") else 0)
        checked += 1
        if n > ARITY_CAP:
            offenders.append(f"{label}: {n} params -- {sig}")
    assert checked > 0, "found no callables -- the test would pass vacuously"
    assert not offenders, "over the five-parameter cap:\n  " + "\n  ".join(offenders)


def test_every_enum_value_is_a_usable_python_name() -> None:
    """P16 -- `Weekend::None` would be `Weekend.None`, which does not parse.

    This fails at import rather than at call, so it must be checked on names.
    """
    enums = [o for o in (getattr(cdsmodel, n) for n in cdsmodel.__all__)
             if hasattr(o, "__members__")]
    assert enums, "no enums found -- the test would pass vacuously"
    for enum in enums:
        for member in enum.__members__:
            assert member.isidentifier(), f"{enum.__name__}.{member} is not an identifier"
            assert not keyword.iskeyword(member), f"{enum.__name__}.{member} is a keyword"
            assert member == member.upper(), f"{enum.__name__}.{member} is not UPPER_CASE (D17)"


@pytest.mark.parametrize("name", ["TradeTerms", "ZeroCurve"])
def test_bound_types_repr_their_contents(name: str) -> None:
    """P17 -- never `<cdsmodel._core.X object at 0x...>`."""
    cls = getattr(cdsmodel, name)
    assert "__repr__" in dir(cls)
    assert cls.__repr__ is not object.__repr__, f"{name} has the default repr"
