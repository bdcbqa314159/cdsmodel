#pragma once

// D23: the Python layer mirrors `xl/core`'s argument grouping but defines its
// own types. `cdsmodel::xl`'s blocks include `xl/value.hpp` and
// `xl/handles.hpp`, so binding them would make Python depend on the Excel
// layer -- which the layering in CLAUDE.md forbids. Python sits *beside*
// `xl/core`, not above it: both depend only on `cdsmodel`.
//
// The grouping is deliberately identical, so one mental model serves both
// surfaces. A field added to one must be added to the other by hand.

#include <cdsmodel/date.hpp>

namespace cdsmodel::py {

/// What was traded. Mirrors `cdsmodel::xl::TradeTerms`.
struct TradeTerms {
  Date start;
  Date end;
  double coupon_rate = 0.0;
};

} // namespace cdsmodel::py
