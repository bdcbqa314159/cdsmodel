"""The ISDA CDS Standard Model, ported to C++20.

A port of the ISDA CDS Standard Model 1.8.3. Every function is checked against
the upstream C implementation; see the project's README for how that works and
what it does not cover.
"""

from ._core import __upstream_version__, __version__

__all__ = ["__version__", "__upstream_version__"]
