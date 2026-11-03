#pragma once

// Port of upstream dtlist.c -- set operations on lists of dates.
//
// Upstream's type is an owning struct with a count:
//
//     typedef struct { int fNumItems; TDate *fArray; } TDateList;
//
// so every operation allocates, and the caller frees. There is no new type
// here: a list of dates is a std::vector<Date>, and most of these functions
// are a standard algorithm with a name.
//
// THIS MODULE CORRECTS UPSTREAM rather than reproducing it, which is a
// departure from the pattern elsewhere. The justification is UPSTREAM-7: both
// of upstream's truncation implementations contradict their own header AND
// each other, so there is no single upstream behaviour available to preserve.
// See the "Policy" section of docs/port-notes.md. The corrections are:
//
//   * `truncate` examines element 0. Upstream's backward scan is
//     `for (i = numItems - 1; i > 0; i--)`, so a result that is exactly the
//     first element comes back as an empty list (dtlist.c) or as the whole
//     untruncated list (cxdatelist.c), depending on which file you call.
//   * `subtract_date_lists` keeps the largest surviving date, and handles an
//     empty subtrahend. Upstream routes through the broken truncation and
//     inherits its losses.
//
// Preconditions that upstream documents but does not check are checked here
// and reported, rather than producing a silently wrong answer. Upstream's
// `JpmcdsMergeDateLists` on unsorted input returns [2,4,5,1,3] -- neither
// sorted nor a union.

#include <cdsmodel/date.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Is this list strictly increasing? The precondition most of the operations
/// below require, and which upstream assumes without checking.
[[nodiscard]] inline bool is_strictly_increasing_dates(std::span<const Date> dates) noexcept {
  return std::ranges::adjacent_find(dates, std::ranges::greater_equal{}) == dates.end();
}

/// Sort and remove duplicates.
///
/// This is upstream's `JpmcdsSortDateList`, whose header says only "Sorts a
/// datelist" -- it also applies `JpmcdsNewDateListDistinct`, so sorting
/// silently removes duplicates. The name here says what it does.
[[nodiscard]] inline std::vector<Date> sorted_distinct(std::span<const Date> dates) {
  std::vector<Date> out{dates.begin(), dates.end()};
  std::ranges::sort(out);
  out.erase(std::ranges::unique(out).begin(), out.end());
  return out;
}

/// Remove runs of equal adjacent dates. Replaces `JpmcdsNewDateListDistinct`.
///
/// Adjacent only, as upstream is: on unsorted input it removes nothing, which
/// is why it is documented as being for cleaning up a list after bad-day
/// adjustment. Use `sorted_distinct` if the input might not be sorted.
[[nodiscard]] inline std::vector<Date> distinct(std::span<const Date> dates) {
  std::vector<Date> out{dates.begin(), dates.end()};
  out.erase(std::ranges::unique(out).begin(), out.end());
  return out;
}

/// Union of two lists. Replaces `JpmcdsMergeDateLists`.
///
/// Returns nullopt when either input is not strictly increasing. Upstream
/// documents the same requirement and does not enforce it.
[[nodiscard]] inline std::optional<std::vector<Date>> merge_date_lists(std::span<const Date> a,
                                                                       std::span<const Date> b) {
  if (!is_strictly_increasing_dates(a) || !is_strictly_increasing_dates(b)) {
    return std::nullopt;
  }
  std::vector<Date> out;
  out.reserve(a.size() + b.size());
  std::ranges::set_union(a, b, std::back_inserter(out));
  return out;
}

/// Add dates to a list. Replaces `JpmcdsJpmcdsDateListAddDates`.
///
/// Both inputs must be strictly increasing, as upstream documents.
/// `allow_duplicates` keeps a date that is already present, matching
/// upstream's flag of the same name.
[[nodiscard]] inline std::optional<std::vector<Date>>
add_dates(std::span<const Date> dates, std::span<const Date> extra, bool allow_duplicates) {
  if (!is_strictly_increasing_dates(dates) || !is_strictly_increasing_dates(extra)) {
    return std::nullopt;
  }
  std::vector<Date> out;
  out.reserve(dates.size() + extra.size());
  if (allow_duplicates) {
    std::ranges::merge(dates, extra, std::back_inserter(out));
  } else {
    std::ranges::set_union(dates, extra, std::back_inserter(out));
  }
  return out;
}

/// Remove every date in `remove` from `dates`. Replaces
/// `JpmcdsSubtractDateLists`. Inputs may be unsorted; the result is sorted
/// and duplicate-free.
///
/// CORRECTS UPSTREAM. Upstream computes the difference, sorts it, then
/// truncates at its own maximum through the broken truncation of UPSTREAM-7,
/// which loses dates. It also returns an empty list when `remove` is empty,
/// because its inner loop never runs. Both are fixed here.
[[nodiscard]] inline std::vector<Date> subtract_date_lists(std::span<const Date> dates,
                                                           std::span<const Date> remove) {
  const std::vector<Date> to_remove = sorted_distinct(remove);
  std::vector<Date> out;
  out.reserve(dates.size());
  for (Date d : sorted_distinct(dates)) {
    if (!std::ranges::binary_search(to_remove, d)) {
      out.push_back(d);
    }
  }
  return out;
}

/// Which side of the cut to keep.
enum class Keep {
  Before, ///< upstream's `excludeBefore = FALSE`
  After,  ///< upstream's `excludeBefore = TRUE`
};

/// Truncate a list at a date. Replaces `JpmcdsJpmcdsDateListTruncate` and
/// `JpmcdsDateListTruncate`.
///
/// `inclusive` decides whether a date equal to the cut survives:
///
///   | side   | inclusive | kept        |
///   |--------|-----------|-------------|
///   | Before | false     | `d <  cut`  |
///   | Before | true      | `d <= cut`  |
///   | After  | false     | `d >  cut`  |
///   | After  | true      | `d >= cut`  |
///
/// CORRECTS UPSTREAM (UPSTREAM-7). Both upstream implementations scan
/// backwards with `i > 0` and never examine element 0, so a result that is
/// exactly the first element is lost -- as an empty list in `dtlist.c`, as
/// the whole untruncated list in `cxdatelist.c`. They agree with each other
/// and with this function whenever two or more elements survive.
///
/// Requires a strictly increasing input, as upstream documents.
[[nodiscard]] inline std::optional<std::vector<Date>>
truncate_date_list(std::span<const Date> dates, Date cut, Keep side, bool inclusive) {
  if (!is_strictly_increasing_dates(dates)) {
    return std::nullopt;
  }
  std::vector<Date> out;
  out.reserve(dates.size());
  for (Date d : dates) {
    const bool keep =
        side == Keep::Before ? (inclusive ? d <= cut : d < cut) : (inclusive ? d >= cut : d > cut);
    if (keep) {
      out.push_back(d);
    }
  }
  return out;
}

} // namespace cdsmodel
