#pragma once

/// Everything public, in one include. B5/B6 of specs/02_consumability.md.
///
/// A consumer that wants the library rather than one corner of it includes
/// this. It is **hand-written and gate-checked** rather than generated (D1):
/// a generated header is not greppable and does not appear in the tree a
/// reader browses, so `python verify.py umbrella` fails the build if this list
/// and `include/cdsmodel/` disagree, in either direction. The includes are
/// kept sorted so that two people adding a header do not collide here.
///
/// Including this costs compile time proportional to the whole library. Code
/// inside this project should include the two or three headers it actually
/// uses; this exists for consumers and for bindings, which need all of it
/// anyway.
///
/// The per-header self-test in `CMakeLists.txt` globs this directory, so this
/// file is also compiled alone, on its own, by that mechanism -- which is what
/// proves it is self-sufficient and not relying on a prior include.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_bootstrap.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/cdsone.hpp>
#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/curve_instruments.hpp>
#include <cdsmodel/curve_linear_interp.hpp>
#include <cdsmodel/curve_rates.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_list.hpp>
#include <cdsmodel/date_schedule.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/defaulted.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/float_rate.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/ir_zero_curve.hpp>
#include <cdsmodel/match_range.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/root_find.hpp>
#include <cdsmodel/schedule.hpp>
#include <cdsmodel/stream_cash_flows.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/swap_bootstrap_flows.hpp>
#include <cdsmodel/swap_dates.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/version.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>
