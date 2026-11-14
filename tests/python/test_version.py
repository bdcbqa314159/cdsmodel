"""P12 -- one version, in one place.

This test did not exist when P12 was first recorded as passing. It was checked
by hand, the three numbers agreed, and the criterion was ticked. They agreed
because `src/version.cpp` had `"0.1.0"` hardcoded and `CMakeLists.txt` said
`0.1.0` -- two sources of truth that happened to match. The first version bump
made them disagree and nothing noticed, which is the whole argument for a
committed oracle over a careful look.
"""

from __future__ import annotations

import pathlib
import re

import pytest

import cdsmodel

REPO = pathlib.Path(__file__).resolve().parents[2]
CMAKELISTS = REPO / "CMakeLists.txt"


def _cmake_version() -> str:
    if not CMAKELISTS.exists():  # pragma: no cover -- installed without the repo
        pytest.skip("CMakeLists.txt not available; run from a checkout")
    match = re.search(
        r"project\(cdsmodel VERSION ([0-9]+\.[0-9]+\.[0-9]+)", CMAKELISTS.read_text())
    assert match, "could not find the project version in CMakeLists.txt"
    return match.group(1)


def test_the_package_version_is_the_cmake_project_version() -> None:
    """`pyproject.toml` reads this line with a regex; the extension reports it
    through the C++ `version()`. Both must land on the same number."""
    assert cdsmodel.__version__ == _cmake_version()


def test_the_version_is_not_a_placeholder() -> None:
    """A `#error` guards the C++ side if CMake does not pass the version in,
    but a build that silently produced an empty or default string would pass
    the comparison above if both sides were equally wrong."""
    assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", cdsmodel.__version__), cdsmodel.__version__
    assert cdsmodel.__version__ != "0.0.0"


def test_the_upstream_version_is_the_library_being_ported() -> None:
    """Separate from our own version on purpose: one says which release of
    this project you have, the other which ISDA release it reproduces."""
    assert cdsmodel.__upstream_version__ == "1.8.3"
    assert cdsmodel.__upstream_version__ != cdsmodel.__version__
