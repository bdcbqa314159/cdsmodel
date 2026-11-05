// TIER 2 -- live differential tests for the bad day substitution table.
//
// Three functions with no arithmetic in them, so the only thing to compare is
// which entry a lookup picks. The sweep is therefore over map SHAPES: empty,
// single, duplicated in either column, chained, cyclic, and self-mapping --
// including the malformed ones, which upstream handles deterministically and
// the port must therefore handle identically.

#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "dtlist.h"
#include "ldate.h"
}

namespace {

class BadDayMapDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// An upstream TBadDayList viewing a port map. Owns the two parallel arrays
/// that upstream needs and the port does not.
struct Ref {
  std::vector<TDate> bad;
  std::vector<TDate> good;
  TBadDayList list{};

  explicit Ref(std::span<const cdsmodel::BadDayPair> map) {
    for (const cdsmodel::BadDayPair &pair : map) {
      bad.push_back(cdsmodel::to_tdate(pair.bad));
      good.push_back(cdsmodel::to_tdate(pair.good));
    }
    list.count = static_cast<int>(map.size());
    list.maxAllowed = static_cast<int>(map.size());
    list.badDay = bad.empty() ? nullptr : bad.data();
    list.goodDay = good.empty() ? nullptr : good.data();
  }
};

/// Every shape a two- or three-entry map can take that changes a lookup.
std::vector<std::pair<std::string, std::vector<cdsmodel::BadDayPair>>> maps() {
  const cdsmodel::Date a = d(2024, 1, 6);
  const cdsmodel::Date b = d(2024, 1, 7);
  const cdsmodel::Date c = d(2024, 1, 8);
  const cdsmodel::Date e = d(2024, 1, 9);
  return {
      {"empty", {}},
      {"single", {{a, c}}},
      {"two disjoint", {{a, c}, {b, e}}},
      {"duplicate bad", {{a, c}, {a, e}}},
      {"duplicate good", {{a, c}, {b, c}}},
      {"chained", {{a, b}, {b, c}}},
      {"chained backwards", {{b, c}, {a, b}}},
      {"two-cycle", {{a, b}, {b, a}}},
      {"self mapping", {{a, a}}},
      {"three chained", {{a, b}, {b, c}, {c, e}}},
  };
}

TEST_F(BadDayMapDifferential, BothLookupsMatchForEveryMapShapeAndEveryDate) {
  const cdsmodel::Date probes[] = {d(2024, 1, 5), d(2024, 1, 6), d(2024, 1, 7),
                                   d(2024, 1, 8), d(2024, 1, 9), d(2024, 1, 10)};
  int compared = 0;
  for (const auto &[name, map] : maps()) {
    Ref ref(map);
    for (const cdsmodel::Date probe : probes) {
      const TDate raw = cdsmodel::to_tdate(probe);
      EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::bad_to_good(map, probe)),
                JpmcdsBad2GoodBadDayList(&ref.list, raw))
          << name << " bad_to_good " << raw;
      EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::good_to_bad(map, probe)),
                JpmcdsGood2BadBadDayList(&ref.list, raw))
          << name << " good_to_bad " << raw;
      ++compared;
    }
  }
  EXPECT_GT(compared, 50) << compared;
}

TEST_F(BadDayMapDifferential, AnEmptyMapIsUpstreamsNullPointer) {
  // Upstream's "allow NULL to ignore list" is the port's empty span. The two
  // encodings have to be shown equivalent, since the port has no null to pass.
  const std::vector<cdsmodel::BadDayPair> none{};
  for (const cdsmodel::Date probe : {d(2024, 1, 6), d(2024, 1, 7), d(2099, 12, 31)}) {
    const TDate raw = cdsmodel::to_tdate(probe);
    EXPECT_EQ(JpmcdsBad2GoodBadDayList(nullptr, raw), raw);
    EXPECT_EQ(JpmcdsGood2BadBadDayList(nullptr, raw), raw);
    EXPECT_EQ(cdsmodel::bad_to_good(none, probe), probe);
    EXPECT_EQ(cdsmodel::good_to_bad(none, probe), probe);
  }
  // And a date absent from a non-empty map gives the same answer, which is why
  // the two cases cannot be told apart from the result.
  const std::vector<cdsmodel::BadDayPair> one{{d(2024, 1, 6), d(2024, 1, 8)}};
  Ref ref(one);
  const TDate absent = cdsmodel::to_tdate(d(2024, 1, 7));
  EXPECT_EQ(JpmcdsBad2GoodBadDayList(&ref.list, absent), absent);
  EXPECT_EQ(cdsmodel::bad_to_good(one, d(2024, 1, 7)), d(2024, 1, 7));
}

TEST_F(BadDayMapDifferential, FixingAListMatchesIncludingWhereItCollapsesDates) {
  for (const auto &[name, map] : maps()) {
    Ref ref(map);
    const std::vector<cdsmodel::Date> dates{d(2024, 1, 5), d(2024, 1, 6), d(2024, 1, 7),
                                            d(2024, 1, 8), d(2024, 1, 9)};
    TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(dates.size()));
    ASSERT_NE(dl, nullptr);
    for (std::size_t i = 0; i < dates.size(); ++i) {
      dl->fArray[i] = cdsmodel::to_tdate(dates[i]);
    }
    JpmcdsFixBadDLBadDayList(&ref.list, dl);

    const std::vector<cdsmodel::Date> fixed = cdsmodel::fix_bad_dates(map, dates);
    ASSERT_EQ(fixed.size(), static_cast<std::size_t>(dl->fNumItems)) << name;
    for (std::size_t i = 0; i < fixed.size(); ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(fixed[i]), dl->fArray[i]) << name << " i=" << i;
    }
    JpmcdsFreeDateList(dl);
  }
}

TEST_F(BadDayMapDifferential, FixingWithNoMapLeavesEveryDateAlone) {
  const std::vector<cdsmodel::Date> dates{d(2024, 1, 6), d(2024, 1, 7), d(2024, 1, 8)};
  TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(dates.size()));
  ASSERT_NE(dl, nullptr);
  for (std::size_t i = 0; i < dates.size(); ++i) {
    dl->fArray[i] = cdsmodel::to_tdate(dates[i]);
  }
  JpmcdsFixBadDLBadDayList(nullptr, dl);
  for (std::size_t i = 0; i < dates.size(); ++i) {
    EXPECT_EQ(dl->fArray[i], cdsmodel::to_tdate(dates[i])) << i;
  }
  JpmcdsFreeDateList(dl);
  EXPECT_EQ(cdsmodel::fix_bad_dates({}, dates), dates);
  // Upstream also accepts a NULL date list and silently does nothing. The port
  // has no null to pass; an empty span is the equivalent and gives an empty
  // result rather than no result.
  JpmcdsFixBadDLBadDayList(nullptr, nullptr);
  EXPECT_TRUE(cdsmodel::fix_bad_dates({}, {}).empty());
}

TEST_F(BadDayMapDifferential, Upstream_ALookupIsOneStepAndNotAClosure) {
  // Not a defect -- recorded as an observation in bad_day_map.hpp. A mapping
  // that sends a bad day to another bad day is malformed, and the library has
  // no constructor that could prevent it. What matters here is that the port
  // reproduces the single step rather than quietly iterating.
  const cdsmodel::Date a = d(2024, 1, 6);
  const cdsmodel::Date b = d(2024, 1, 7);
  const cdsmodel::Date c = d(2024, 1, 8);
  const std::vector<cdsmodel::BadDayPair> chained{{a, b}, {b, c}};
  Ref ref(chained);

  const TDate once = JpmcdsBad2GoodBadDayList(&ref.list, cdsmodel::to_tdate(a));
  EXPECT_EQ(once, cdsmodel::to_tdate(b)) << "one step only";
  EXPECT_EQ(JpmcdsBad2GoodBadDayList(&ref.list, once), cdsmodel::to_tdate(c))
      << "a second call moves again, so the first result was not a fixed point";

  EXPECT_EQ(cdsmodel::bad_to_good(chained, a), b) << "the port takes the same single step";
  EXPECT_EQ(cdsmodel::bad_to_good(chained, cdsmodel::bad_to_good(chained, a)), c);

  // And the port can say that the map is the problem, which upstream cannot.
  EXPECT_FALSE(cdsmodel::is_well_formed(chained));
  EXPECT_TRUE(cdsmodel::is_well_formed(std::vector<cdsmodel::BadDayPair>{{a, c}}));
}

TEST_F(BadDayMapDifferential, Upstream_MaxAllowedIsNeverRead) {
  // The struct carries a capacity field that none of the three functions
  // consults, and the symbol table shows no constructor anywhere in the
  // library to maintain it. A list claiming a capacity of one and holding
  // three is searched to all three.
  const cdsmodel::Date a = d(2024, 1, 6);
  const cdsmodel::Date b = d(2024, 1, 7);
  const cdsmodel::Date c = d(2024, 1, 8);
  const std::vector<cdsmodel::BadDayPair> three{{a, b}, {b, c}, {c, a}};
  Ref ref(three);
  ref.list.maxAllowed = 1; // a lie, and nothing notices

  EXPECT_EQ(JpmcdsBad2GoodBadDayList(&ref.list, cdsmodel::to_tdate(c)), cdsmodel::to_tdate(a))
      << "the third entry was found despite maxAllowed = 1";
  EXPECT_EQ(cdsmodel::bad_to_good(three, c), a) << "and the port agrees";

  // Setting it to zero changes nothing either.
  ref.list.maxAllowed = 0;
  EXPECT_EQ(JpmcdsBad2GoodBadDayList(&ref.list, cdsmodel::to_tdate(a)), cdsmodel::to_tdate(b));
}

TEST_F(BadDayMapDifferential, Upstream_TheReverseLookupIsNotAnInverseUnlessInjective) {
  const cdsmodel::Date a = d(2024, 1, 6);
  const cdsmodel::Date b = d(2024, 1, 7);
  const cdsmodel::Date c = d(2024, 1, 8);
  const std::vector<cdsmodel::BadDayPair> collapsing{{a, c}, {b, c}};
  Ref ref(collapsing);

  // Both bad days map to c; the reverse lookup can only return one of them.
  for (const cdsmodel::Date from : {a, b}) {
    const TDate there = JpmcdsBad2GoodBadDayList(&ref.list, cdsmodel::to_tdate(from));
    const TDate back = JpmcdsGood2BadBadDayList(&ref.list, there);
    EXPECT_EQ(cdsmodel::to_tdate(
                  cdsmodel::good_to_bad(collapsing, cdsmodel::bad_to_good(collapsing, from))),
              back)
        << "the port picks the same one";
  }
  // The first entry wins, so b does not round trip.
  EXPECT_EQ(cdsmodel::good_to_bad(collapsing, cdsmodel::bad_to_good(collapsing, b)), a);
  EXPECT_FALSE(cdsmodel::is_well_formed(collapsing));
}

} // namespace
