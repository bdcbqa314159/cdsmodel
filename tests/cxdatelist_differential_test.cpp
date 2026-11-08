// TIER 2 -- live differential tests earning the claims added for
// cxdatelist.c and datelist.c.
//
// Nothing was implemented for this slice. Five upstream functions were
// CLAIMED against code that already existed, and a claim without a test is an
// assertion. These tests are what the claims rest on.
//
// The central claim is that upstream ships the same operations twice:
//
//     dtlist.h:92      JpmcdsJpmcdsDateListAddDates(dl, n, dates, allowDuplicates)
//     cxdatelist.h:45  JpmcdsDateListAddDates      (dl, n, dates)
//
// -- the doubled prefix being upstream's own, from a renaming pass that ran
// twice over one file and once over the other. If the three-parameter version
// really is the four-parameter one with the flag FALSE, then the port's single
// `add_dates` covers both, and these tests prove it over a swept domain rather
// than on the one example that made me believe it.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_list.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cmemory.h"
#include "cxdatelist.h"
#include "datelist.h"
#include "dtlist.h"
#include "ldate.h"
#include "macros.h"
#include "tcurve.h"
}

namespace {

class CxDateListDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct RefDL {
  TDateList *dl = nullptr;
  ~RefDL() { JpmcdsFreeDateList(dl); }
  RefDL() = default;
  explicit RefDL(TDateList *owned) : dl(owned) {}
  RefDL(const RefDL &) = delete;
  RefDL &operator=(const RefDL &) = delete;
  explicit operator bool() const { return dl != nullptr; }
};

std::vector<TDate> raw(const std::vector<cdsmodel::Date> &dates) {
  std::vector<TDate> out;
  for (const cdsmodel::Date date : dates) {
    out.push_back(cdsmodel::to_tdate(date));
  }
  return out;
}

void expect_same(const TDateList *ref, const std::vector<cdsmodel::Date> &got, const char *what) {
  ASSERT_EQ(ref->fNumItems, static_cast<int>(got.size())) << what;
  for (int i = 0; i < ref->fNumItems; ++i) {
    EXPECT_EQ(ref->fArray[i], cdsmodel::to_tdate(got[static_cast<std::size_t>(i)]))
        << what << " at " << i;
  }
}

/// Strictly increasing lists only: both upstream functions document that
/// requirement and the port enforces it, so unsorted input is a separate
/// question and is not what these claims are about.
const std::vector<std::vector<cdsmodel::Date>> &lists() {
  static const std::vector<std::vector<cdsmodel::Date>> v{
      {},
      {d(2024, 1, 15)},
      {d(2024, 1, 15), d(2024, 7, 15)},
      {d(2024, 1, 15), d(2024, 4, 15), d(2024, 7, 15), d(2025, 1, 15)},
      {d(2024, 2, 29), d(2024, 3, 1)},
      {d(2020, 1, 1), d(2024, 1, 15), d(2030, 1, 1)},
  };
  return v;
}

// The claim: the three-parameter cxdatelist entry point IS the
// four-parameter dtlist one with allowDuplicates FALSE, which is the port's
// `add_dates(..., false)`.
TEST_F(CxDateListDifferential, TheThreeParameterAddIsTheFourParameterOneWithTheFlagOff) {
  int compared = 0;
  int overlapping = 0;
  for (const auto &a : lists()) {
    for (const auto &b : lists()) {
      if (a.empty()) {
        // cxdatelist accepts dl == NULL and copies the array verbatim without
        // deduping; the port has no null state. Covered by its own test below.
        continue;
      }
      std::vector<TDate> ra = raw(a);
      std::vector<TDate> rb = raw(b);
      RefDL base(JpmcdsNewDateListFromDates(ra.data(), static_cast<int>(ra.size())));
      ASSERT_TRUE(static_cast<bool>(base));

      RefDL cx(JpmcdsDateListAddDates(base.dl, static_cast<int>(rb.size()),
                                      rb.empty() ? nullptr : rb.data()));
      RefDL dt(JpmcdsJpmcdsDateListAddDates(base.dl, static_cast<int>(rb.size()),
                                            rb.empty() ? nullptr : rb.data(), FALSE));
      ASSERT_EQ(static_cast<bool>(cx), static_cast<bool>(dt))
          << "the two upstream functions disagree about acceptance";
      if (!cx) {
        continue;
      }
      // First the claim about upstream: the two really are the same function.
      ASSERT_EQ(cx.dl->fNumItems, dt.dl->fNumItems)
          << "the three-parameter version is NOT the four-parameter one with "
             "the flag off";
      for (int i = 0; i < cx.dl->fNumItems; ++i) {
        EXPECT_EQ(cx.dl->fArray[i], dt.dl->fArray[i]) << i;
      }
      // Then the port against both.
      const cdsmodel::Expected<std::vector<cdsmodel::Date>> got =
          cdsmodel::add_dates(a, b, /*allow_duplicates=*/false);
      ASSERT_TRUE(got.has_value());
      expect_same(cx.dl, *got, "add_dates(false)");

      bool shares = false;
      for (const cdsmodel::Date x : a) {
        for (const cdsmodel::Date y : b) {
          if (x == y) {
            shares = true;
          }
        }
      }
      if (shares) {
        ++overlapping;
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 20);
  EXPECT_GT(overlapping, 3) << "no overlapping pair was tried, so dedupe is untested";
}

// And with the flag ON the two upstream functions must DIFFER, otherwise the
// flag would be doing nothing and the claim above would be vacuous.
TEST_F(CxDateListDifferential, WithTheFlagOnTheTwoUpstreamFunctionsDiffer) {
  const std::vector<cdsmodel::Date> a{d(2024, 1, 15), d(2024, 4, 15), d(2024, 7, 15)};
  const std::vector<cdsmodel::Date> b{d(2024, 4, 15), d(2025, 1, 15)};
  std::vector<TDate> ra = raw(a);
  std::vector<TDate> rb = raw(b);
  RefDL base(JpmcdsNewDateListFromDates(ra.data(), 3));
  ASSERT_TRUE(static_cast<bool>(base));

  RefDL keep(JpmcdsJpmcdsDateListAddDates(base.dl, 2, rb.data(), TRUE));
  RefDL drop(JpmcdsJpmcdsDateListAddDates(base.dl, 2, rb.data(), FALSE));
  RefDL cx(JpmcdsDateListAddDates(base.dl, 2, rb.data()));
  ASSERT_TRUE(static_cast<bool>(keep));
  ASSERT_TRUE(static_cast<bool>(drop));
  ASSERT_TRUE(static_cast<bool>(cx));

  EXPECT_EQ(keep.dl->fNumItems, 5) << "allowDuplicates TRUE should keep the repeat";
  EXPECT_EQ(drop.dl->fNumItems, 4);
  EXPECT_EQ(cx.dl->fNumItems, 4) << "the three-parameter version has no flag and dedupes";
  EXPECT_NE(keep.dl->fNumItems, drop.dl->fNumItems)
      << "the flag changed nothing, so the equivalence claim is vacuous";

  const cdsmodel::Expected<std::vector<cdsmodel::Date>> port_keep = cdsmodel::add_dates(a, b, true);
  ASSERT_TRUE(port_keep.has_value());
  expect_same(keep.dl, *port_keep, "add_dates(true)");
}

// cxdatelist's NULL-list branch copies the array VERBATIM -- it does not
// dedupe, despite the function's own name. A deliberate divergence: a span has
// no null state, so the port runs the merge and dedupes.
TEST_F(CxDateListDifferential, TheNullListBranchSkipsTheDedupeAndThePortDoesNot) {
  // Duplicates inside the added array, which the merge path would collapse.
  std::vector<TDate> dup{cdsmodel::to_tdate(d(2024, 1, 15)), cdsmodel::to_tdate(d(2024, 1, 15)),
                         cdsmodel::to_tdate(d(2024, 7, 15))};
  RefDL ref(JpmcdsDateListAddDates(nullptr, 3, dup.data()));
  ASSERT_TRUE(static_cast<bool>(ref)) << "upstream started refusing a NULL list";
  EXPECT_EQ(ref.dl->fNumItems, 3)
      << "the NULL branch deduped; it is documented not to and measured not to";

  // The port refuses the input outright, because the added list is not
  // strictly increasing -- which is the stricter contract date_list.hpp
  // already declares for this module.
  const std::vector<cdsmodel::Date> extra{d(2024, 1, 15), d(2024, 1, 15), d(2024, 7, 15)};
  EXPECT_FALSE(cdsmodel::add_dates({}, extra, false).has_value());
}

// The curve-date claim: upstream has two functions for this in two files, and
// `ZeroCurve::dates()` is claimed against both.
TEST_F(CxDateListDifferential, BothCurveDateExtractorsAgreeWithDates) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const std::vector<cdsmodel::RatePoint> points{
      {d(2024, 7, 15), 0.030}, {d(2026, 1, 15), 0.032}, {d(2031, 1, 15), 0.035}};
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  TCurve *tc = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(), 3, 1.0,
                                JPMCDS_ACT_365F);
  ASSERT_NE(tc, nullptr);

  RefDL from_datelist(JpmcdsNewDateListFromTCurve(tc));
  ASSERT_TRUE(static_cast<bool>(from_datelist));

  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve =
      cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  expect_same(from_datelist.dl, curve->dates(), "NewDateListFromTCurve");

  // And the tcurve.c sibling, which returns a bare array rather than a list.
  TDate *bare = JpmcdsDatesFromCurve(tc);
  ASSERT_NE(bare, nullptr);
  const std::vector<cdsmodel::Date> got = curve->dates();
  for (int i = 0; i < tc->fNumItems; ++i) {
    EXPECT_EQ(bare[i], cdsmodel::to_tdate(got[static_cast<std::size_t>(i)])) << i;
  }
  FREE(bare);
  JpmcdsFreeTCurve(tc);
}

// JpmcdsNewDateListFromDates and JpmcdsExtendDateList have no port because a
// vector is one. The claims are earned by comparing against the vector
// operations that replace them.
TEST_F(CxDateListDifferential, TheVectorOperationsMatchTheListOnesTheyReplace) {
  const std::vector<cdsmodel::Date> dates{d(2024, 1, 15), d(2024, 4, 15), d(2024, 7, 15)};
  std::vector<TDate> rd = raw(dates);

  RefDL made(JpmcdsNewDateListFromDates(rd.data(), 3));
  ASSERT_TRUE(static_cast<bool>(made));
  expect_same(made.dl, dates, "NewDateListFromDates == vector construction");

  // Extend: upstream grows in place and leaves the new slots at TDate 0,
  // which is 1601-01-01 rather than an obviously-unset value.
  ASSERT_EQ(JpmcdsExtendDateList(made.dl, 5), SUCCESS);
  EXPECT_EQ(made.dl->fNumItems, 5);
  EXPECT_EQ(made.dl->fArray[3], 0) << "the new slots are no longer zeroed";
  EXPECT_EQ(made.dl->fArray[4], 0);
  EXPECT_EQ(cdsmodel::from_tdate(0), cdsmodel::kDateEpoch) << "TDate 0 is 1601-01-01, a real date";

  // And it refuses to shrink.
  EXPECT_NE(JpmcdsExtendDateList(made.dl, 2), SUCCESS);
  EXPECT_EQ(made.dl->fNumItems, 5) << "a refused shrink changed the list anyway";

  // The port's equivalent is resize, which grows to default-constructed
  // Dates. I asserted here that those would also be TDate 0 and that the port
  // therefore inherited upstream's property. THEY ARE NOT.
  //
  // `Date` is `std::chrono::sys_days`, so a default-constructed one is the
  // UNIX epoch -- 1970-01-01, which is 134774 days after the ISDA epoch. So
  // the two implementations disagree about what an unset date is, and neither
  // value is invalid: upstream's reads as 1601-01-01 and the port's as
  // 1970-01-01, and both are dates a contract could plausibly carry.
  //
  // This matters beyond `resize`: the port's aggregates (`CouponDates`,
  // `FeePayment`, `SwapDate`) all have default-initialised `Date` members, so
  // 1970-01-01 is the port's silent "unset" everywhere. Every constructor
  // sets every field, which is why nothing has depended on it -- but the
  // value is recorded here rather than assumed to be harmless.
  std::vector<cdsmodel::Date> v = dates;
  v.resize(5);
  EXPECT_EQ(v.size(), 5u);
  EXPECT_EQ(v[3], cdsmodel::Date{}) << "a default-constructed Date";
  EXPECT_EQ(cdsmodel::to_tdate(v[3]), 134774)
      << "a default-constructed Date is the UNIX epoch, not the ISDA one";
  EXPECT_EQ(v[3], *cdsmodel::make_date(1970, 1, 1));
  EXPECT_NE(cdsmodel::to_tdate(v[3]), 0)
      << "the two implementations agree about the unset value after all, "
         "which would make the comment above wrong";
}

} // namespace
