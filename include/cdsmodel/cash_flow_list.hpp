#pragma once

// Port of upstream cashflow.c -- the container half.
//
//     TCashFlow     { TDate fDate; double fAmount; }
//     TCashFlowList { int fNumItems; TCashFlow *fArray; }
//
// Four of the file's six functions are container operations with no
// dependencies outside it, and they are what this slice covers:
// `JpmcdsMergeCFL`, `JpmcdsNewEmptyCFL`, `JpmcdsNewCFL` and `JpmcdsFreeCFL`.
//
// The other two -- `JpmcdsMakeCFL` and `JpmcdsNewCFLFromDL3`, ~400 lines
// between them -- build a list from a payment schedule, and need
// `JpmcdsNewDateListExtendedRoll`, `JpmcdsNewDateListBusDayAdj`,
// `JpmcdsBusinessDay` and `JpmcdsStubPayment`, none of which are ported. They
// wait for datelist.c.
//
// The container is the half that matters right now, because the remaining
// third of zcall.c -- `ZCAddCashFlowList`, `ZCAddCFLPoints`,
// `JpmcdsJpmcdsZCPresentValueCFL` -- only ever CONSUMES a list. Checked
// rather than assumed: those three call nothing from cashflow.c beyond the
// type itself.
//
// ## Two functions that do not need a port, and one reason that is not obvious
//
// `JpmcdsNewEmptyCFL(n)` allocates and `JpmcdsFreeCFL` releases;
// `std::vector<CashFlow>` does both. What is worth recording is that upstream's
// "empty" really is empty. The allocation goes through `NEW_ARRAY`, hence
// `JpmcdsMallocSafe`, whose first line reads:
//
//     ptr = malloc(theSize);        /* Don't use calloc due to RS6000. */
//
// On that line alone the function returns `fNumItems = n` over uninitialised
// memory, which would be a defect worth filing: a public function named
// "NewEmpty" reporting n cash flows of heap garbage. It is not, because the
// next branch does this:
//
//     /* Clear out the memory for safety. */
//     (void)memset((char *)ptr, (size_t)0, theSize);
//
// The comment about calloc means "we reproduce calloc by hand", not "we skip
// the zeroing".
//
// So the contents are defined -- but note what they are. `fDate = 0` is not a
// null in upstream's encoding; TDate 0 is **1601-01-01**, a date like any
// other. A list from `JpmcdsNewEmptyCFL(n)` that a caller under-fills does not
// look uninitialised, it looks like n deliberate payments of zero in 1601.
// Garbage might get caught downstream; this will not.
//
// `std::vector<CashFlow>(n)` is NOT the equivalent, and the first draft of
// this header claimed it was. A default-constructed `Date` is 1970-01-01, the
// `sys_days` epoch, not 1601-01-01 -- the port's `Date` has no zero, which is
// the point of it. The port therefore has no "empty list of n items" at all:
// length is the number of payments present, and capacity is `reserve`. That
// conflation is upstream's, and `ZCurve` does not make it -- it carries
// `numItems` and `numAlloc` separately, in the same library.
//
// That generalises, and it rules out a whole class of search: every `NEW` and
// `NEW_ARRAY` in the library returns zeroed memory, so upstream cannot have an
// uninitialised-HEAP read. The one uninitialised read in the register,
// UPSTREAM-14, is a stack local -- where no allocator intervenes. Worth
// stating positively, because the alternative is re-deriving it at every
// allocation site in the remaining 11,000 lines.

#include <cdsmodel/date.hpp>
#include <cdsmodel/expected.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// One payment. Replaces `TCashFlow`.
struct CashFlow {
  Date date;
  double amount = 0.0;

  friend constexpr bool operator==(const CashFlow &, const CashFlow &) = default;
};

/// True when no payment precedes its predecessor.
///
/// Non-decreasing rather than strictly increasing: a list may legitimately
/// carry two payments on one date, and upstream's merge passes such a pair
/// through without coalescing it. Two entries of 1 on the same date and one
/// entry of 2 are the same cash flow, so neither form is wrong.
[[nodiscard]] constexpr bool is_ordered_by_date(std::span<const CashFlow> flows) noexcept {
  return std::ranges::is_sorted(flows, {}, &CashFlow::date);
}

/// Build a list from parallel date and amount arrays. Replaces `JpmcdsNewCFL`.
///
/// Upstream takes `dates`, `amounts` and a single `numItems` described as
/// "Length of both arrays", and indexes both to that length without checking
/// either. A caller whose arrays differ in length reads off the end of the
/// shorter one -- the parallel-array hazard, with the length asserted by the
/// caller instead of carried by the data.
///
/// Two spans here, so each length travels with its own array. A mismatch is
/// refused rather than read past; that is a check upstream has no way to make.
/// Upstream also dereferences both pointers without a NULL test, where
/// `JpmcdsMergeCFL` in the same file does test its inputs -- an inconsistency
/// that spans cannot express.
[[nodiscard]] inline Expected<std::vector<CashFlow>>
make_cash_flows(std::span<const Date> dates, std::span<const double> amounts) {
  if (dates.size() != amounts.size()) {
    return fail(ErrorKind::bad_argument, "dates and amounts must be the same length, got " +
                                             detail::num(dates.size()) + " and " +
                                             detail::num(amounts.size()));
  }
  std::vector<CashFlow> flows;
  flows.reserve(dates.size());
  for (std::size_t i = 0; i < dates.size(); ++i) {
    flows.push_back(CashFlow{dates[i], amounts[i]});
  }
  return flows;
}

/// Merge two date-ordered lists, summing payments that fall on one date.
/// Replaces `JpmcdsMergeCFL`.
///
/// ## UPSTREAM-20 -- the precondition the documentation does not mention
///
/// The header states the contract in one sentence: *"Merges two cash flow
/// lists. The input cash flow lists cannot be NULL."* That precondition is
/// real and it is checked.
///
/// The other one is not stated and not checked. The algorithm is a single
/// forward pass that advances whichever side has the earlier date, so it is
/// only a merge if both inputs are already ordered. Given unordered input it
/// produces neither a sorted list nor a stable interleaving -- measured, with
/// a = [40000, 20000, 30000] and b = [25000, 15000]:
///
/// ```
/// MergeCFL -> fNumItems=5
///     [0] date=25000  amount=10
///     [1] date=15000  amount=20
///     [2] date=40000  amount=1
///     [3] date=20000  amount=2
///     [4] date=30000  amount=3
/// ```
///
/// Returned as success. A caller who reads the header, satisfies the one
/// documented precondition and passes lists in whatever order they arrived has
/// no way to learn that the result is meaningless.
///
/// Severity is low, and the reason is reachability: exactly one call site
/// exists in the library -- `zcswap.c:804`, merging a fixed and a floating
/// schedule, both generated in order. So this is unreachable from inside
/// upstream and reachable only through the public header, which is the same
/// tier as UPSTREAM-12.
///
/// The port returns nullopt for unordered input. Checking a precondition the
/// documentation forgot is the same move `ZeroCurve::create` makes over
/// `checkTCurve` (UPSTREAM-15): refuse at the boundary what cannot produce a
/// meaningful answer, so the result type does not have to carry a "this is
/// nonsense" value.
[[nodiscard]] inline Expected<std::vector<CashFlow>> merge_cash_flows(std::span<const CashFlow> a,
                                                                      std::span<const CashFlow> b) {
  if (!is_ordered_by_date(a) || !is_ordered_by_date(b)) {
    return fail(ErrorKind::bad_argument, "both cash flow lists must be ordered by date");
  }

  std::vector<CashFlow> merged;
  merged.reserve(a.size() + b.size());

  std::size_t ia = 0;
  std::size_t ib = 0;
  while (ia < a.size() && ib < b.size()) {
    if (a[ia].date < b[ib].date) {
      merged.push_back(a[ia++]);
    } else if (a[ia].date > b[ib].date) {
      merged.push_back(b[ib++]);
    } else {
      // Upstream's summation order, a + b, kept as written: addition is not
      // associative in floating point and the merged amount is observable.
      merged.push_back(CashFlow{a[ia].date, a[ia].amount + b[ib].amount});
      ++ia;
      ++ib;
    }
  }
  // Whichever side is left over is already ordered and already later.
  merged.insert(merged.end(), a.begin() + static_cast<std::ptrdiff_t>(ia), a.end());
  merged.insert(merged.end(), b.begin() + static_cast<std::ptrdiff_t>(ib), b.end());
  return merged;
}

} // namespace cdsmodel
