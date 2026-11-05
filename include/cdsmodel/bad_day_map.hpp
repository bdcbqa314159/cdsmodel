#pragma once

// Port of upstream badday.c -- an explicit table of substitute dates, as
// opposed to a rule. Three functions and 97 lines, and the smallest module in
// the port.
//
// Distinct from bad_day.hpp, which ports busday.c: that one computes a
// business day from a CONVENTION and a calendar, this one looks one up in a
// caller-supplied list of exceptions. Upstream's filenames differ by one
// character and the port's by five, which is as much separation as the names
// can carry.
//
//     typedef struct {
//        int     count;        /* number of items currently in list */
//        int     maxAllowed;   /* max no of items allowed in arrays */
//        TDate  *badDay;       /* array of bad business days */
//        TDate  *goodDay;      /* array of corresponding good ays */
//     } TBadDayList;
//
// Two parallel arrays again, and `maxAllowed` is **never read** by any of the
// three functions -- measured. There is also no constructor anywhere in the
// library: the symbol table shows exactly three `*BadDayList` functions and
// all three are lookups. So every instance is built by a caller outside the
// library, with nothing to maintain the invariant `maxAllowed` was meant to
// express. One vector of pairs here, and the field has nothing to be.
//
// ## No defect in this module
//
// Five behaviours looked like candidates and none survived. They are recorded
// as observations at their sites, because each is a thing the next reader will
// also wonder about:
//
//   * a chained mapping is not followed transitively;
//   * the round trip only holds for an injective map;
//   * "not in the list" and "no list at all" return the same thing;
//   * `maxAllowed` is dead;
//   * `JpmcdsFixBadDLBadDayList` returns void.
//
// Each is a consequence of the module being a lookup table rather than an
// algorithm, and in every case upstream's behaviour is well defined for every
// input. That is the line the port's policy draws, and this module sits on the
// faithful side of it.

#include <cdsmodel/date.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace cdsmodel {

/// One substitution: a date the caller considers bad, and what to use instead.
///
/// Upstream holds these as `badDay[i]` and `goodDay[i]` in two allocations
/// kept in step by hand -- the same shape that produced a defect in the
/// premium leg, and the same fix.
struct BadDayPair {
  Date bad;
  Date good;

  friend constexpr bool operator==(const BadDayPair &, const BadDayPair &) = default;
};

/// The substitute for `date`, or `date` itself. Replaces
/// `JpmcdsBad2GoodBadDayList`.
///
/// A linear scan and a single return. Three things follow, all reproduced:
///
/// **It is one step, not a closure.** If a bad day's replacement is itself
/// listed as bad, one call leaves a date the list still calls bad:
///
/// ```
/// list: 154502 -> 154503, 154503 -> 154504
/// bad_to_good(154502)                = 154503   <- still a listed bad day
/// bad_to_good(bad_to_good(154502))   = 154504
/// ```
///
/// Not treated as a defect. The function's stated job is to convert a bad day
/// *"(if it's in list)"*, and a mapping that sends a bad day to another bad day
/// is malformed -- there is no constructor in the library to prevent it, so a
/// caller that builds one is responsible for it. `is_well_formed` below is the
/// port's offer to check, not a change in what lookup does.
///
/// **The first match wins**, so duplicates in the `bad` column make later
/// entries unreachable.
///
/// **An empty map and an absent date are the same answer.** Upstream's comment
/// says so -- *"allow NULL to ignore list"* -- and an empty span says it
/// without a null pointer. Measured: both return the input, and nothing in the
/// signature can distinguish them. That is a deliberate identity rather than a
/// missing error channel, which is why it is not the UPSTREAM-2 shape: there is
/// no failure here to report.
[[nodiscard]] constexpr Date bad_to_good(std::span<const BadDayPair> map, Date date) noexcept {
  for (const BadDayPair &pair : map) {
    if (pair.bad == date) {
      return pair.good;
    }
  }
  return date;
}

/// The reverse lookup. Replaces `JpmcdsGood2BadBadDayList`.
///
/// Searches the `good` column and returns the corresponding `bad` date. Not an
/// inverse unless the map is injective -- measured, on a map sending two bad
/// days to one good day:
///
/// ```
/// list: 154502 -> 154504, 154503 -> 154504
/// good_to_bad(bad_to_good(154502)) = 154502   round trips
/// good_to_bad(bad_to_good(154503)) = 154502   a DIFFERENT date
/// ```
///
/// Which is simply what a non-injective map does, and not a defect: the second
/// entry's good day is already claimed by the first. `is_well_formed` reports
/// it; the lookup reproduces it.
[[nodiscard]] constexpr Date good_to_bad(std::span<const BadDayPair> map, Date date) noexcept {
  for (const BadDayPair &pair : map) {
    if (pair.good == date) {
      return pair.bad;
    }
  }
  return date;
}

/// Every date in `dates`, substituted. Replaces
/// `JpmcdsFixBadDLBadDayList`.
///
/// Upstream modifies the caller's `TDateList` in place and returns `void`, so
/// it cannot report anything -- which is sound here, because the operation
/// cannot fail: every date either matches an entry or is returned unchanged.
/// A new vector rather than mutation in place, so a caller can keep the
/// original.
///
/// Note what this does NOT do: the result is not sorted, and substitution can
/// reorder dates or collapse two onto one. Upstream has the same property and
/// nothing downstream re-sorts, so neither does this.
[[nodiscard]] inline std::vector<Date> fix_bad_dates(std::span<const BadDayPair> map,
                                                     std::span<const Date> dates) {
  std::vector<Date> fixed;
  fixed.reserve(dates.size());
  for (const Date date : dates) {
    fixed.push_back(bad_to_good(map, date));
  }
  return fixed;
}

/// Is this map one the lookups behave sensibly on?
///
/// **Not a port of anything.** Upstream has no such check and no constructor
/// to put one in; this is the port offering a name for the precondition the
/// three lookups quietly assume. Three conditions, each corresponding to one
/// of the measured oddities above:
///
///   * no date appears twice in the `bad` column -- otherwise the first entry
///     shadows the rest;
///   * no date appears twice in the `good` column -- otherwise `good_to_bad`
///     has to pick one;
///   * no `good` date is also a `bad` date -- otherwise `bad_to_good` returns
///     something the map itself calls bad, and the caller would have to
///     iterate to a fixed point.
///
/// Deliberately separate from the lookups rather than enforced by a
/// constructor. Validating on construction would make the malformed cases
/// unrepresentable -- and so untestable against upstream, which handles them
/// deterministically. The differential suite needs to feed both sides a
/// chained map and compare; it can only do that if the port will hold one.
[[nodiscard]] inline bool is_well_formed(std::span<const BadDayPair> map) {
  std::vector<Date> bad;
  std::vector<Date> good;
  bad.reserve(map.size());
  good.reserve(map.size());
  for (const BadDayPair &pair : map) {
    bad.push_back(pair.bad);
    good.push_back(pair.good);
  }
  std::ranges::sort(bad);
  std::ranges::sort(good);
  if (std::ranges::adjacent_find(bad) != bad.end()) {
    return false;
  }
  if (std::ranges::adjacent_find(good) != good.end()) {
    return false;
  }
  // A good day that is also a bad day means lookup does not reach a fixed
  // point in one step.
  return std::ranges::none_of(good, [&](Date g) { return std::ranges::binary_search(bad, g); });
}

} // namespace cdsmodel
