#pragma once

// Port of upstream timeline.c -- the integration timeline for risky
// (default-adjusted) cashflows.
//
// Small, and important out of proportion to its size for two reasons.
//
// First, it is the grid the contingent and fee legs integrate over, so its
// contents determine what the price is computed from.
//
// Second, it is the ONLY caller of `JpmcdsDateListTruncate` -- the function
// of UPSTREAM-7, which silently ignores a truncation whenever fewer than two
// dates would survive. The divergence policy in docs/port-notes.md requires
// the price impact of a correction to be measured, not assumed, and this is
// where that debt comes due.
//
// The answer is in `risky_timeline` below, and it is better than a test
// result: the defect is UNREACHABLE here, provably. Both truncations happen
// *after* the start and end dates have been merged into the list, and the
// contract requires end > start, so at least two distinct dates always
// survive -- which is exactly the regime where upstream is correct. The
// differential suite confirms it empirically as well.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_list.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Dates of a curve's points, in order.
[[nodiscard]] inline std::vector<Date> curve_dates(const ZeroCurve &curve) {
  std::vector<Date> out;
  out.reserve(curve.points().size());
  for (const RatePoint &p : curve.points()) {
    out.push_back(p.date);
  }
  return out;
}

/// Timeline for a risky integration assuming flat forward curves.
///
/// Replaces `JpmcdsRiskyTimeLine`. Contains the start date, the end date,
/// every discount curve point and every spread curve point that falls between
/// them, and nothing outside them.
///
/// Returns nullopt when `end` is not strictly after `start`, which upstream
/// also requires.
///
/// On UPSTREAM-7: the two truncations below run *after* `start` and `end` are
/// in the list, and `end > start`, so at least two dates survive each one.
/// Upstream's truncation is correct in that regime, so its defect cannot
/// affect a timeline. Asserted in `timeline_differential_test.cpp`.
[[nodiscard]] inline std::optional<std::vector<Date>>
risky_timeline(Date start, Date end, const ZeroCurve &discount, const ZeroCurve &spread) {
  if (end <= start) {
    return std::nullopt;
  }

  // Upstream builds this by repeated duplicate-excluding merges, which is a
  // set union; `sorted_distinct` of the concatenation is the same set.
  std::vector<Date> all = curve_dates(discount);
  const std::vector<Date> spread_dates = curve_dates(spread);
  all.insert(all.end(), spread_dates.begin(), spread_dates.end());
  all.push_back(start);
  all.push_back(end);
  std::vector<Date> timeline = sorted_distinct(all);

  // Drop anything strictly outside [start, end], keeping both endpoints.
  const auto after_start = truncate_date_list(timeline, start, Keep::After, true);
  if (!after_start) {
    return std::nullopt;
  }
  return truncate_date_list(*after_start, end, Keep::Before, true);
}

/// Truncate a set of critical dates to `[start, end]`, ensuring both
/// endpoints are present. Replaces `JpmcdsTruncateTimeLine`.
///
/// `critical` may be empty, as upstream's may be NULL.
[[nodiscard]] inline std::optional<std::vector<Date>>
truncate_timeline(std::span<const Date> critical, Date start, Date end) {
  if (end <= start) {
    return std::nullopt;
  }
  std::vector<Date> all{critical.begin(), critical.end()};
  all.push_back(start);
  all.push_back(end);
  std::vector<Date> timeline = sorted_distinct(all);

  const auto after_start = truncate_date_list(timeline, start, Keep::After, true);
  if (!after_start) {
    return std::nullopt;
  }
  return truncate_date_list(*after_start, end, Keep::Before, true);
}

} // namespace cdsmodel
