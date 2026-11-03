// TIER 2 -- live differential tests for the date list operations.
//
// This file is shaped differently from the other differential suites, because
// this is the first module that CORRECTS upstream rather than reproducing it
// (UPSTREAM-7). So it does two jobs:
//
//   1. assert agreement wherever upstream is right -- which is whenever two
//      or more elements survive a truncation, and that is the overwhelming
//      majority of cases;
//   2. assert the DISAGREEMENT still exists where upstream is wrong, so a
//      future upstream release that fixes it makes our divergence visible
//      rather than leaving a correction nobody revisits.
//
// Job 2 is the one that is easy to forget. A test that only checks agreement
// would quietly pass forever after upstream changed.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_list.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cxdatelist.h"
#include "dtlist.h"
}

namespace {

class DateListDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

TDateList *make_list(std::span<const cdsmodel::Date> dates) {
  TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(dates.size()));
  for (std::size_t i = 0; i < dates.size(); ++i) {
    dl->fArray[i] = cdsmodel::to_tdate(dates[i]);
  }
  return dl;
}

std::vector<cdsmodel::Date> from_list(TDateList *dl) {
  std::vector<cdsmodel::Date> out;
  if (dl == nullptr) {
    return out;
  }
  for (int i = 0; i < dl->fNumItems; ++i) {
    out.push_back(cdsmodel::from_tdate(dl->fArray[i]));
  }
  return out;
}

std::vector<cdsmodel::Date> dates_from(std::initializer_list<int> offsets) {
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  std::vector<cdsmodel::Date> out;
  for (int o : offsets) {
    out.push_back(base + std::chrono::days{o});
  }
  return out;
}

// --- where upstream is right, we must agree -------------------------------

TEST_F(DateListDifferential, MergeMatchesForSortedDistinctInputs) {
  const std::vector<std::vector<cdsmodel::Date>> lists{{},
                                                       dates_from({1}),
                                                       dates_from({1, 3, 5}),
                                                       dates_from({2, 3, 6}),
                                                       dates_from({1, 2, 3}),
                                                       dates_from({7, 8, 9}),
                                                       dates_from({0, 10, 20, 30})};
  for (const auto &a : lists) {
    for (const auto &b : lists) {
      TDateList *ra = a.empty() ? nullptr : make_list(a);
      TDateList *rb = b.empty() ? nullptr : make_list(b);
      TDateList *merged = JpmcdsMergeDateLists(ra, rb);
      const auto ported = cdsmodel::merge_date_lists(a, b);
      ASSERT_TRUE(ported.has_value()) << "port rejected sorted distinct inputs";
      if (a.empty() && b.empty()) {
        EXPECT_EQ(merged, nullptr) << "upstream returns NULL for two NULL lists";
        EXPECT_TRUE(ported->empty());
      } else {
        ASSERT_NE(merged, nullptr);
        EXPECT_EQ(from_list(merged), *ported) << "merge disagreed on valid input";
      }
    }
  }
}

TEST_F(DateListDifferential, AddDatesMatchesForBothDuplicateModes) {
  const auto base = dates_from({1, 3, 5});
  const std::vector<std::vector<cdsmodel::Date>> additions{{},
                                                           dates_from({4}),
                                                           dates_from({3}),
                                                           dates_from({3, 4}),
                                                           dates_from({0, 6}),
                                                           dates_from({1, 3, 5})};
  for (const auto &extra : additions) {
    for (bool dupes : {false, true}) {
      std::vector<TDate> raw;
      for (cdsmodel::Date d : extra) {
        raw.push_back(cdsmodel::to_tdate(d));
      }
      TDateList *result =
          JpmcdsJpmcdsDateListAddDates(make_list(base), static_cast<int>(raw.size()),
                                       raw.empty() ? nullptr : raw.data(), dupes ? TRUE : FALSE);
      const auto ported = cdsmodel::add_dates(base, extra, dupes);
      ASSERT_TRUE(ported.has_value());
      ASSERT_NE(result, nullptr) << "upstream rejected valid input";
      EXPECT_EQ(from_list(result), *ported) << "add_dates disagreed, allow_duplicates=" << dupes;
    }
  }
}

TEST_F(DateListDifferential, DistinctMatchesOnSortedInput) {
  for (const auto &in : {dates_from({1, 1, 2, 3, 3, 3, 5}), dates_from({1, 2, 3}),
                         dates_from({4, 4, 4}), std::vector<cdsmodel::Date>{}}) {
    TDateList *result = JpmcdsNewDateListDistinct(make_list(in));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(from_list(result), cdsmodel::distinct(in));
  }
}

TEST_F(DateListDifferential, TruncateMatchesWheneverTwoOrMoreElementsSurvive) {
  // Upstream is correct in exactly this regime. Sweeping it is what keeps the
  // correction honest: the port must not have changed the common case.
  const auto list = dates_from({0, 1, 2, 3, 4, 5, 6, 10, 20});
  int compared = 0;
  for (int cut_offset = -2; cut_offset <= 22; ++cut_offset) {
    const cdsmodel::Date cut = *cdsmodel::make_date(2024, 1, 1) + std::chrono::days{cut_offset};
    for (bool inclusive : {false, true}) {
      for (cdsmodel::Keep side : {cdsmodel::Keep::Before, cdsmodel::Keep::After}) {
        const auto ported = cdsmodel::truncate_date_list(list, cut, side, inclusive);
        ASSERT_TRUE(ported.has_value());
        if (ported->size() < 2) {
          continue; // the regime where upstream is broken; covered below
        }
        TDateList *result = JpmcdsDateListTruncate(
            make_list(list), cdsmodel::to_tdate(cut), inclusive ? TRUE : FALSE,
            side == cdsmodel::Keep::After ? TRUE : FALSE, FALSE);
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(from_list(result), *ported)
            << "truncate disagreed where upstream is correct: cut offset " << cut_offset
            << " inclusive=" << inclusive
            << " side=" << (side == cdsmodel::Keep::Before ? "Before" : "After");
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 50) << "the agreement regime was barely exercised";
}

// --- where upstream is wrong, the disagreement must persist ---------------

TEST_F(DateListDifferential, Upstream7_BothImplementationsStillLoseElementZero) {
  // If either of these starts agreeing with the port, upstream fixed the
  // off-by-one and the correction in date_list.hpp should be revisited.
  struct Case {
    std::vector<cdsmodel::Date> list;
    int cut_offset;
    bool inclusive;
  };
  const std::vector<Case> cases{
      {dates_from({1, 2}), 2, false},
      {dates_from({10, 30}), 30, false},
      {dates_from({5}), 9, false},
      {dates_from({9, 10}), 9, true},
  };

  int dtlist_wrong = 0;
  int cx_wrong = 0;
  for (const Case &c : cases) {
    const cdsmodel::Date cut = *cdsmodel::make_date(2024, 1, 1) + std::chrono::days{c.cut_offset};
    const auto intended =
        cdsmodel::truncate_date_list(c.list, cut, cdsmodel::Keep::Before, c.inclusive);
    ASSERT_TRUE(intended.has_value());
    ASSERT_EQ(intended->size(), 1u) << "these cases are chosen to yield one element";

    TDateList *a = JpmcdsJpmcdsDateListTruncate(make_list(c.list), cdsmodel::to_tdate(cut),
                                                c.inclusive ? TRUE : FALSE, FALSE, FALSE);
    TDateList *b = JpmcdsDateListTruncate(make_list(c.list), cdsmodel::to_tdate(cut),
                                          c.inclusive ? TRUE : FALSE, FALSE, FALSE);
    if (from_list(a) != *intended) {
      ++dtlist_wrong;
      EXPECT_TRUE(from_list(a).empty()) << "dtlist.c failed in an unexpected way";
    }
    if (from_list(b) != *intended) {
      ++cx_wrong;
      EXPECT_EQ(from_list(b).size(), c.list.size()) << "cxdatelist.c failed in an unexpected way";
    }
  }
  EXPECT_EQ(dtlist_wrong, 4) << "dtlist.c no longer loses element 0 -- revisit the correction";
  EXPECT_GT(cx_wrong, 0) << "cxdatelist.c no longer over-keeps -- revisit the correction";
}

TEST_F(DateListDifferential, Upstream7_SubtractInheritsTheLossAndAnEmptySubtrahendBug) {
  const auto list = dates_from({1, 2, 3});
  const auto remove_none = std::vector<cdsmodel::Date>{};
  const auto remove_absent = dates_from({9});

  // Documented intent: removing nothing leaves the list alone.
  EXPECT_EQ(cdsmodel::subtract_date_lists(list, remove_none), list);
  EXPECT_EQ(cdsmodel::subtract_date_lists(list, remove_absent), list);

  // Upstream returns an empty list when the subtrahend is empty, because its
  // inner loop never executes, and loses the largest date otherwise.
  TDateList *empty_sub = JpmcdsSubtractDateLists(make_list(list), JpmcdsNewEmptyDateList(0));
  EXPECT_TRUE(from_list(empty_sub).empty())
      << "upstream no longer drops everything for an empty subtrahend";

  TDateList *absent_sub = JpmcdsSubtractDateLists(make_list(list), make_list(remove_absent));
  EXPECT_NE(from_list(absent_sub), list)
      << "upstream subtract agrees with the port now -- revisit the correction";
}

} // namespace
