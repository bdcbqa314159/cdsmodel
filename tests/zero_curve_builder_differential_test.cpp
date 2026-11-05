// TIER 2 -- live differential tests for the curve builder.
//
// The builder caches a discount factor beside every rate, so the thing under
// test is an INVARIANT rather than a formula: whatever route a point arrives
// by, the stored rate and discount must agree. Both defects in this module are
// failures of that invariant's surroundings rather than of its arithmetic.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cfinanci.h"
#include "dateconv.h"
#include "ldate.h"
#include "tcurve.h"
#include "zcprvt.h"
}

namespace {

class BuilderDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// RAII wrapper so a test that fails mid-way does not leak a ZCurve.
struct Ref {
  ZCurve *zc = nullptr;
  explicit Ref(TDate vd, int n, long basis, long dcc) : zc(JpmcdsZCMake(vd, n, basis, dcc)) {}
  ~Ref() { JpmcdsZCFree(zc); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return zc != nullptr; }
};

TEST_F(BuilderDifferential, ConstructionAcceptsAndRejectsTheSameInputs) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long basis : {-2L, -1L, 0L, 1L, 2L, 4L, 12L, 365L, 5000L, 5001L, 5002L, 10000L}) {
    for (int n : {-1, 0, 1, 8, 64}) {
      Ref ref(cdsmodel::to_tdate(vd), n, basis, JPMCDS_ACT_365F);
      const std::optional<cdsmodel::ZeroCurveBuilder> got =
          cdsmodel::ZeroCurveBuilder::create(vd, n, basis, cdsmodel::DayCount::Act365Fixed);
      EXPECT_EQ(got.has_value(), static_cast<bool>(ref)) << "basis " << basis << " n " << n;
      ++compared;
    }
  }
  EXPECT_GT(compared, 50) << compared;
  // The two oddities of JpmcdsBasisValid, pinned.
  EXPECT_TRUE(cdsmodel::builder_basis_valid(5001)) << "the bound really is 5001, not 5000";
  EXPECT_FALSE(cdsmodel::builder_basis_valid(0)) << "simple interest is excluded";
  EXPECT_FALSE(cdsmodel::builder_basis_valid(-2)) << "the discount-factor sentinel is excluded";
}

TEST_F(BuilderDifferential, AddRateMatchesPointForPoint) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long basis : {1L, 2L, 4L, 12L, 5000L}) {
    for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Ref ref(cdsmodel::to_tdate(vd), 8, basis, cdsmodel::to_upstream_code(dcc));
      ASSERT_TRUE(ref);
      std::optional<cdsmodel::ZeroCurveBuilder> got =
          cdsmodel::ZeroCurveBuilder::create(vd, 8, basis, dcc);
      ASSERT_TRUE(got.has_value());

      // Deliberately out of order, so the sorted insert is exercised.
      struct Entry {
        cdsmodel::Date date;
        double rate;
      };
      for (const Entry &e :
           {Entry{d(2029, 1, 1), 0.05}, Entry{d(2025, 1, 1), 0.03}, Entry{d(2034, 1, 1), 0.055},
            Entry{d(2026, 7, 1), 0.04}, Entry{d(2025, 1, 1), 0.03}, // exact duplicate: a no-op
            Entry{d(2027, 1, 1), -0.01}}) {
        const bool ref_ok = JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(e.date), e.rate) == SUCCESS;
        const bool got_ok = got->add_rate(e.date, e.rate);
        ASSERT_EQ(got_ok, ref_ok) << "basis " << basis << " at " << cdsmodel::to_tdate(e.date);
      }

      ASSERT_EQ(got->points().size(), static_cast<std::size_t>(ref.zc->numItems))
          << "basis " << basis;
      for (int i = 0; i < ref.zc->numItems; ++i) {
        const cdsmodel::ZeroCurveBuilder::Point &p = got->points()[static_cast<std::size_t>(i)];
        EXPECT_EQ(cdsmodel::to_tdate(p.date), ref.zc->date[i]) << "basis " << basis << " i=" << i;
        EXPECT_EQ(p.rate, ref.zc->rate[i]) << "basis " << basis << " i=" << i;
        // The cached discount factor, which is the whole point of the type.
        EXPECT_EQ(p.discount, ref.zc->discount[i]) << "basis " << basis << " i=" << i;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 40) << compared;
}

TEST_F(BuilderDifferential, ADuplicateDateIsIdempotentAndAConflictIsAnError) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  const cdsmodel::Date at = d(2029, 1, 1);
  Ref ref(cdsmodel::to_tdate(vd), 8, 1, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);
  std::optional<cdsmodel::ZeroCurveBuilder> got =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(got.has_value());

  ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), 0.05), SUCCESS);
  ASSERT_TRUE(got->add_rate(at, 0.05));

  // The same rate again: a silent no-op in both.
  EXPECT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), 0.05), SUCCESS);
  EXPECT_TRUE(got->add_rate(at, 0.05));
  EXPECT_EQ(ref.zc->numItems, 1);
  EXPECT_EQ(got->points().size(), 1u);

  // Within upstream's hardcoded 1e-7: still a no-op.
  EXPECT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), 0.05 + 1.0e-9), SUCCESS);
  EXPECT_TRUE(got->add_rate(at, 0.05 + 1.0e-9));
  EXPECT_EQ(ref.zc->numItems, 1);
  EXPECT_EQ(got->points().size(), 1u);

  // Beyond it: an error in both, and neither adds anything.
  EXPECT_NE(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), 0.06), SUCCESS);
  EXPECT_FALSE(got->add_rate(at, 0.06));
  EXPECT_EQ(ref.zc->numItems, 1);
  EXPECT_EQ(got->points().size(), 1u);
}

TEST_F(BuilderDifferential, Upstream17_TheFastDiscountPathDisagreesAtRateMinusOne) {
  // ZCComputeDiscount guards `rate >= -1.0` and computes pow(1+rate, ...),
  // so at exactly -1.0 it returns an infinity where the general path refuses.
  const cdsmodel::Date vd = d(2024, 1, 1);
  const cdsmodel::Date at = d(2029, 1, 1);
  Ref ref(cdsmodel::to_tdate(vd), 8, 1, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);

  double fast = 0.0;
  const bool fast_ok =
      JpmcdsZCComputeDiscount(ref.zc, cdsmodel::to_tdate(at), -1.0, &fast) == SUCCESS;
  double general = 0.0;
  const bool general_ok = JpmcdsRateToDiscount(-1.0, cdsmodel::to_tdate(vd), cdsmodel::to_tdate(at),
                                               JPMCDS_ACT_365F, 1, &general) == SUCCESS;
  ASSERT_TRUE(fast_ok) << "the fast path now rejects rate -1.0 -- UPSTREAM-17 may be fixed";
  EXPECT_TRUE(std::isinf(fast)) << "expected an infinity from pow(0, negative), got " << fast;
  EXPECT_FALSE(general_ok) << "the general path now accepts rate -1.0 -- revisit UPSTREAM-17";

  // And the infinity is stored, not merely returned.
  EXPECT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), -1.0), SUCCESS);
  ASSERT_EQ(ref.zc->numItems, 1);
  EXPECT_TRUE(std::isinf(ref.zc->discount[0]))
      << "upstream no longer stores an infinite discount factor";

  // The port takes the general path's answer, so its two paths agree.
  std::optional<cdsmodel::ZeroCurveBuilder> got =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(got.has_value());
  EXPECT_FALSE(got->compute_discount(at, -1.0).has_value());
  EXPECT_FALSE(got->add_rate(at, -1.0));
  EXPECT_TRUE(got->empty()) << "nothing should have been committed";

  // Just inside the boundary, the two implementations agree exactly.
  for (double rate : {-0.9999999, -0.999, -0.5, 0.0, 0.05}) {
    double expected = 0.0;
    ASSERT_EQ(JpmcdsZCComputeDiscount(ref.zc, cdsmodel::to_tdate(at), rate, &expected), SUCCESS)
        << rate;
    const std::optional<double> ported = got->compute_discount(at, rate);
    ASSERT_TRUE(ported.has_value()) << rate;
    EXPECT_EQ(*ported, expected) << rate;
  }
}

TEST_F(BuilderDifferential, Upstream16_AddGenRateFailsWithTheCurveAlreadyModified) {
  // ZCAddGenRate short-circuits when the terms match and then forgets to
  // return, so it adds the point and adds it again by a second route. At a
  // date equal to the value date the second route fails -- after the commit.
  const cdsmodel::Date vd = d(2024, 1, 1);
  Ref ref(cdsmodel::to_tdate(vd), 8, 1, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);

  const int rc = JpmcdsZCAddGenRate(ref.zc, cdsmodel::to_tdate(vd), 0.05, 1, JPMCDS_ACT_365F);
  EXPECT_NE(rc, SUCCESS) << "upstream now succeeds here -- UPSTREAM-16 may be fixed";
  EXPECT_EQ(ref.zc->numItems, 1)
      << "upstream no longer leaves the point behind -- revisit UPSTREAM-16";
  if (ref.zc->numItems > 0) {
    EXPECT_EQ(ref.zc->date[0], cdsmodel::to_tdate(vd));
    EXPECT_EQ(ref.zc->rate[0], 0.05);
  }

  // The port returns after the matching case, so a failure commits nothing...
  std::optional<cdsmodel::ZeroCurveBuilder> got =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(got.has_value());
  // ...and in fact this case SUCCEEDS in the port, because the only reason it
  // failed upstream was the redundant second route. A rate at the value date
  // discounts by nothing, which is a perfectly good point.
  EXPECT_TRUE(got->add_general_rate(vd, 0.05, 1, cdsmodel::DayCount::Act365Fixed));
  ASSERT_EQ(got->points().size(), 1u);
  EXPECT_EQ(got->points()[0].date, vd);
  EXPECT_EQ(got->points()[0].rate, 0.05);
  EXPECT_DOUBLE_EQ(got->points()[0].discount, 1.0);
}

TEST_F(BuilderDifferential, AddGenRateMatchesWhereUpstreamIsRight) {
  // Everywhere the fall-through is harmless -- which is everywhere the second
  // route succeeds -- the two must agree point for point.
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long curve_basis : {1L, 2L, 5000L}) {
    for (long rate_basis : {1L, 2L, 4L, 5000L}) {
      for (cdsmodel::DayCount rate_dcc :
           {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
        Ref ref(cdsmodel::to_tdate(vd), 8, curve_basis, JPMCDS_ACT_365F);
        ASSERT_TRUE(ref);
        std::optional<cdsmodel::ZeroCurveBuilder> got =
            cdsmodel::ZeroCurveBuilder::create(vd, 8, curve_basis, cdsmodel::DayCount::Act365Fixed);
        ASSERT_TRUE(got.has_value());

        for (int years : {1, 3, 7}) {
          const cdsmodel::Date at = d(2024 + years, 1, 1);
          const bool ref_ok = JpmcdsZCAddGenRate(ref.zc, cdsmodel::to_tdate(at), 0.04, rate_basis,
                                                 cdsmodel::to_upstream_code(rate_dcc)) == SUCCESS;
          const bool got_ok = got->add_general_rate(at, 0.04, rate_basis, rate_dcc);
          const std::string where = "curve " + std::to_string(curve_basis) + " rate " +
                                    std::to_string(rate_basis) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(rate_dcc));
          ASSERT_EQ(got_ok, ref_ok) << where;
        }
        ASSERT_EQ(got->points().size(), static_cast<std::size_t>(ref.zc->numItems));
        for (int i = 0; i < ref.zc->numItems; ++i) {
          const auto &p = got->points()[static_cast<std::size_t>(i)];
          EXPECT_EQ(cdsmodel::to_tdate(p.date), ref.zc->date[i]) << "i=" << i;
          EXPECT_EQ(p.rate, ref.zc->rate[i]) << "i=" << i;
          EXPECT_EQ(p.discount, ref.zc->discount[i]) << "i=" << i;
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 60) << compared;
}

TEST_F(BuilderDifferential, FindDateMatches) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  Ref ref(cdsmodel::to_tdate(vd), 8, 1, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);
  std::optional<cdsmodel::ZeroCurveBuilder> got =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(got.has_value());

  // An empty builder finds nothing.
  EXPECT_EQ(JpmcdsZCFindDateClosest(ref.zc, cdsmodel::to_tdate(vd)), -1);
  EXPECT_FALSE(got->find_closest(vd).has_value());
  EXPECT_EQ(JpmcdsZCFindDateExact(ref.zc, cdsmodel::to_tdate(vd)), -1);
  EXPECT_FALSE(got->find_exact(vd).has_value());

  for (int years : {1, 2, 5, 10}) {
    const cdsmodel::Date at = d(2024 + years, 1, 1);
    ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), 0.03 + 0.002 * years), SUCCESS);
    ASSERT_TRUE(got->add_rate(at, 0.03 + 0.002 * years));
  }

  int compared = 0;
  for (int offset = -800; offset <= 4400; offset += 37) {
    const cdsmodel::Date at = vd + std::chrono::days{offset};
    const int ref_closest = JpmcdsZCFindDateClosest(ref.zc, cdsmodel::to_tdate(at));
    const std::optional<std::size_t> closest = got->find_closest(at);
    ASSERT_TRUE(closest.has_value()) << offset;
    EXPECT_EQ(static_cast<int>(*closest), ref_closest) << "closest at offset " << offset;

    const int ref_exact = JpmcdsZCFindDateExact(ref.zc, cdsmodel::to_tdate(at));
    const std::optional<std::size_t> exact = got->find_exact(at);
    EXPECT_EQ(exact.has_value(), ref_exact >= 0) << "exact at offset " << offset;
    if (ref_exact >= 0) {
      EXPECT_EQ(static_cast<int>(*exact), ref_exact) << offset;
    }
    ++compared;
  }
  EXPECT_GT(compared, 100) << compared;
}

TEST_F(BuilderDifferential, PrefixAndSuffixMatch) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  const auto seed = [&](ZCurve *zc, cdsmodel::ZeroCurveBuilder &b, std::initializer_list<int> years,
                        double base) {
    for (int y : years) {
      const cdsmodel::Date at = d(2024 + y, 1, 1);
      ASSERT_EQ(JpmcdsZCAddRate(zc, cdsmodel::to_tdate(at), base + 0.001 * y), SUCCESS);
      ASSERT_TRUE(b.add_rate(at, base + 0.001 * y));
    }
  };

  for (bool suffix : {false, true}) {
    Ref target(cdsmodel::to_tdate(vd), 16, 1, JPMCDS_ACT_365F);
    Ref source(cdsmodel::to_tdate(vd), 16, 2, JPMCDS_ACT_360);
    ASSERT_TRUE(target);
    ASSERT_TRUE(source);
    std::optional<cdsmodel::ZeroCurveBuilder> t =
        cdsmodel::ZeroCurveBuilder::create(vd, 16, 1, cdsmodel::DayCount::Act365Fixed);
    std::optional<cdsmodel::ZeroCurveBuilder> s =
        cdsmodel::ZeroCurveBuilder::create(vd, 16, 2, cdsmodel::DayCount::Act360);
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(s.has_value());

    seed(target.zc, *t, {5, 7, 10}, 0.04);
    seed(source.zc, *s, {1, 2, 3, 5, 12, 15}, 0.03);

    const bool ref_ok = suffix ? JpmcdsZCAddSuffixCurve(target.zc, source.zc) == SUCCESS
                               : JpmcdsZCAddPrefixCurve(target.zc, source.zc) == SUCCESS;
    const bool got_ok = suffix ? t->add_suffix(*s) : t->add_prefix(*s);
    ASSERT_EQ(got_ok, ref_ok) << (suffix ? "suffix" : "prefix");
    ASSERT_EQ(t->points().size(), static_cast<std::size_t>(target.zc->numItems))
        << (suffix ? "suffix" : "prefix");
    for (int i = 0; i < target.zc->numItems; ++i) {
      const auto &p = t->points()[static_cast<std::size_t>(i)];
      EXPECT_EQ(cdsmodel::to_tdate(p.date), target.zc->date[i])
          << (suffix ? "suffix" : "prefix") << " i=" << i;
      EXPECT_EQ(p.rate, target.zc->rate[i]) << (suffix ? "suffix" : "prefix") << " i=" << i;
      EXPECT_EQ(p.discount, target.zc->discount[i]) << (suffix ? "suffix" : "prefix") << " i=" << i;
    }
  }
}

TEST_F(BuilderDifferential, TheCurveRoundTripMatches) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  std::vector<TDate> dates;
  std::vector<double> rates;
  std::vector<cdsmodel::RatePoint> pts;
  for (int y : {1, 2, 5, 10}) {
    const cdsmodel::Date at = d(2024 + y, 1, 1);
    dates.push_back(cdsmodel::to_tdate(at));
    rates.push_back(0.03 + 0.002 * y);
    pts.push_back({at, 0.03 + 0.002 * y});
  }
  TCurve *tc =
      JpmcdsMakeTCurve(cdsmodel::to_tdate(vd), dates.data(), rates.data(), 4, 1.0, JPMCDS_ACT_365F);
  ASSERT_NE(tc, nullptr);
  ZCurve *zc = JpmcdsZCFromTCurve(tc);
  ASSERT_NE(zc, nullptr);

  const std::optional<cdsmodel::ZeroCurve> curve =
      cdsmodel::ZeroCurve::create(vd, pts, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  const std::optional<cdsmodel::ZeroCurveBuilder> builder =
      cdsmodel::ZeroCurveBuilder::from_zero_curve(*curve);
  ASSERT_TRUE(builder.has_value());

  ASSERT_EQ(builder->points().size(), static_cast<std::size_t>(zc->numItems));
  for (int i = 0; i < zc->numItems; ++i) {
    const auto &p = builder->points()[static_cast<std::size_t>(i)];
    EXPECT_EQ(cdsmodel::to_tdate(p.date), zc->date[i]) << "i=" << i;
    EXPECT_EQ(p.rate, zc->rate[i]) << "i=" << i;
    EXPECT_EQ(p.discount, zc->discount[i]) << "i=" << i;
  }

  // And back out again.
  TCurve *back = JpmcdsZCToTCurve(zc);
  ASSERT_NE(back, nullptr);
  const std::optional<cdsmodel::ZeroCurve> ported = builder->to_zero_curve();
  ASSERT_TRUE(ported.has_value());
  ASSERT_EQ(ported->points().size(), static_cast<std::size_t>(back->fNumItems));
  EXPECT_EQ(ported->basis(), back->fBasis);
  for (int i = 0; i < back->fNumItems; ++i) {
    EXPECT_EQ(cdsmodel::to_tdate(ported->points()[static_cast<std::size_t>(i)].date),
              back->fArray[i].fDate)
        << "i=" << i;
    EXPECT_EQ(ported->points()[static_cast<std::size_t>(i)].rate, back->fArray[i].fRate)
        << "i=" << i;
  }
  JpmcdsFreeTCurve(back);
  JpmcdsZCFree(zc);
  JpmcdsFreeTCurve(tc);
}

} // namespace
