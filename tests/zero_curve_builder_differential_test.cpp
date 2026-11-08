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
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cfinanci.h"
#include "dateconv.h"
#include "interp.h"
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
      const cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
      cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
  cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
  cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
    const cdsmodel::Expected<double> ported = got->compute_discount(at, rate);
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
  cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
        cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
  cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> got =
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
    const cdsmodel::Expected<std::size_t> closest = got->find_closest(at);
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
    cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> t =
        cdsmodel::ZeroCurveBuilder::create(vd, 16, 1, cdsmodel::DayCount::Act365Fixed);
    cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> s =
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

  const cdsmodel::Expected<cdsmodel::ZeroCurve> curve =
      cdsmodel::ZeroCurve::create(vd, pts, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  const cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> builder =
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
  const cdsmodel::Expected<cdsmodel::ZeroCurve> ported = builder->to_zero_curve();
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

// =====================================================================
// EVALUATION -- reading the curve.
// =====================================================================

/// A curve seeded identically on both sides, so the evaluation comparisons
/// start from the same points rather than from the same inputs.
struct Pair {
  Ref ref;
  cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> port;

  Pair(cdsmodel::Date vd, long basis, cdsmodel::DayCount dcc)
      : ref(cdsmodel::to_tdate(vd), 16, basis, cdsmodel::to_upstream_code(dcc)),
        port(cdsmodel::ZeroCurveBuilder::create(vd, 16, basis, dcc)) {}

  bool seed(cdsmodel::Date at, double rate) {
    const int rc = JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(at), rate);
    const bool ok = port->add_rate(at, rate);
    return rc == SUCCESS && ok;
  }
};

long upstream_interp(cdsmodel::InterpType t) {
  return t == cdsmodel::InterpType::Linear ? JPMCDS_LINEAR_INTERP : JPMCDS_FLAT_FORWARDS;
}

TEST_F(BuilderDifferential, InterpolationMatchesAcrossBothTypesAndTheWholeDomain) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long basis : {1L, 2L, 4L, 12L, 5000L}) {
    for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(vd, basis, dcc);
      ASSERT_TRUE(pair.ref);
      ASSERT_TRUE(pair.port.has_value());
      for (int years : {1, 2, 3, 5, 7, 10, 15, 20, 30}) {
        ASSERT_TRUE(pair.seed(d(2024 + years, 1, 1), 0.02 + 0.004 * years));
      }
      for (cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        // Before the first point, on it, between points, on the value date,
        // and past the last point -- every branch of the function.
        for (const cdsmodel::Date at :
             {d(2020, 1, 1), d(2024, 1, 1), d(2025, 1, 1), d(2025, 6, 15), d(2029, 3, 2),
              d(2039, 7, 1), d(2054, 1, 1), d(2060, 1, 1)}) {
          double want = 0.0;
          const int rc = JpmcdsZCInterpolate(pair.ref.zc, cdsmodel::to_tdate(at),
                                             upstream_interp(interp), nullptr, &want);
          const cdsmodel::Expected<double> got = pair.port->interpolate(at, interp);
          ASSERT_EQ(got.has_value(), rc == SUCCESS)
              << "basis " << basis << " at " << cdsmodel::to_tdate(at);
          if (got) {
            // Linear is arithmetic on stored values; flat forwards goes
            // through pow and a conversion, so the non-accumulating tier.
            EXPECT_NEAR(*got, want,
                        4.0 * std::max(1.0, std::fabs(want)) *
                            std::numeric_limits<double>::epsilon())
                << "basis " << basis << " at " << cdsmodel::to_tdate(at) << " interp "
                << upstream_interp(interp);
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 150) << compared;
}

TEST_F(BuilderDifferential, AnUnknownInterpolationTypeIsRejectedByBoth) {
  // The port's enum cannot express one, so this checks the upstream half of
  // the claim: that the domain really is just those two values.
  Pair pair(d(2024, 1, 1), 1, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.seed(d(2029, 1, 1), 0.05));
  ASSERT_TRUE(pair.seed(d(2034, 1, 1), 0.06));
  double out = 0.0;
  for (const long bogus : {1L, 2L, 123L, 125L, 999L}) {
    EXPECT_EQ(
        JpmcdsZCInterpolate(pair.ref.zc, cdsmodel::to_tdate(d(2031, 1, 1)), bogus, nullptr, &out),
        FAILURE)
        << "interp type " << bogus;
  }
}

TEST_F(BuilderDifferential, DiscountFactorAndPresentValueMatch) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long basis : {1L, 2L, 12L, 5000L}) {
    Pair pair(vd, basis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref);
    for (int years : {1, 3, 7, 12, 25}) {
      ASSERT_TRUE(pair.seed(d(2024 + years, 1, 1), 0.03 + 0.002 * years));
    }
    for (cdsmodel::InterpType interp :
         {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
      for (const cdsmodel::Date at :
           {d(2024, 1, 1), d(2026, 5, 9), d(2031, 1, 1), d(2040, 11, 30), d(2049, 1, 1)}) {
        double want_df = 0.0;
        double want_pv = 0.0;
        const int rc_df = JpmcdsZCDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(at),
                                                 upstream_interp(interp), nullptr, &want_df);
        const int rc_pv = JpmcdsZCPresentValue(pair.ref.zc, 1'000'000.0, cdsmodel::to_tdate(at),
                                               upstream_interp(interp), nullptr, &want_pv);
        const cdsmodel::Expected<double> df = pair.port->discount_factor(at, interp);
        const cdsmodel::Expected<double> pv = pair.port->present_value(1'000'000.0, at, interp);
        ASSERT_EQ(df.has_value(), rc_df == SUCCESS) << "basis " << basis;
        ASSERT_EQ(pv.has_value(), rc_pv == SUCCESS) << "basis " << basis;
        if (df) {
          EXPECT_NEAR(*df, want_df,
                      4.0 * std::max(1.0, std::fabs(want_df)) *
                          std::numeric_limits<double>::epsilon());
          // The present value is one multiplication on top, so it must agree
          // exactly once the discount factor does.
          EXPECT_EQ(*pv, *df * 1'000'000.0);
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 30) << compared;
}

TEST_F(BuilderDifferential, ForwardRatesAndMoneyMarketRatesMatch) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  int compared = 0;
  for (long quote_basis : {0L, 1L, 2L, 4L}) {
    Pair pair(vd, 1, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref);
    for (int years : {1, 2, 4, 8, 16}) {
      ASSERT_TRUE(pair.seed(d(2024 + years, 1, 1), 0.03 + 0.003 * years));
    }
    for (cdsmodel::InterpType interp :
         {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
      const cdsmodel::Date starts[] = {d(2024, 1, 1), d(2025, 1, 1), d(2028, 6, 1)};
      const cdsmodel::Date ends[] = {d(2026, 1, 1), d(2030, 1, 1), d(2039, 1, 1)};
      for (const cdsmodel::Date start : starts) {
        for (const cdsmodel::Date end : ends) {
          double want_fwd_df = 0.0;
          double want_fwd = 0.0;
          const int rc_df = JpmcdsZCFwdDiscountFactor(
              pair.ref.zc, cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
              upstream_interp(interp), nullptr, &want_fwd_df);
          const int rc_fwd = JpmcdsZCForwardRate(
              pair.ref.zc, cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), quote_basis,
              JPMCDS_ACT_365F, upstream_interp(interp), nullptr, &want_fwd);
          const cdsmodel::Expected<double> fwd_df =
              pair.port->forward_discount_factor(start, end, interp);
          const cdsmodel::Expected<double> fwd = pair.port->forward_rate(
              start, end, quote_basis, cdsmodel::DayCount::Act365Fixed, interp);
          ASSERT_EQ(fwd_df.has_value(), rc_df == SUCCESS) << "basis " << quote_basis;
          ASSERT_EQ(fwd.has_value(), rc_fwd == SUCCESS) << "basis " << quote_basis;
          if (fwd_df) {
            EXPECT_NEAR(*fwd_df, want_fwd_df,
                        4.0 * std::max(1.0, std::fabs(want_fwd_df)) *
                            std::numeric_limits<double>::epsilon());
          }
          if (fwd) {
            EXPECT_NEAR(*fwd, want_fwd,
                        4.0 * std::max(1.0, std::fabs(want_fwd)) *
                            std::numeric_limits<double>::epsilon());
          }
          ++compared;
        }
      }
      // And the money market rate, which is the simple-interest forward from
      // the value date.
      for (const cdsmodel::Date at : {d(2025, 1, 1), d(2029, 7, 1), d(2040, 1, 1)}) {
        double want = 0.0;
        const int rc = JpmcdsZCMoneyMarketRate(pair.ref.zc, cdsmodel::to_tdate(at), JPMCDS_ACT_360,
                                               upstream_interp(interp), nullptr, &want);
        const cdsmodel::Expected<double> got =
            pair.port->money_market_rate(at, cdsmodel::DayCount::Act360, interp);
        ASSERT_EQ(got.has_value(), rc == SUCCESS);
        if (got) {
          EXPECT_NEAR(*got, want,
                      4.0 * std::max(1.0, std::fabs(want)) *
                          std::numeric_limits<double>::epsilon());
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 80) << compared;
}

TEST_F(BuilderDifferential, AddMoneyMarketMatchesPointForPoint) {
  const cdsmodel::Date vd = d(2024, 1, 1);
  for (long basis : {1L, 2L, 12L}) {
    for (cdsmodel::DayCount quote_dcc :
         {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
      Ref ref(cdsmodel::to_tdate(vd), 8, basis, JPMCDS_ACT_365F);
      ASSERT_TRUE(ref);
      cdsmodel::Expected<cdsmodel::ZeroCurveBuilder> port =
          cdsmodel::ZeroCurveBuilder::create(vd, 8, basis, cdsmodel::DayCount::Act365Fixed);
      ASSERT_TRUE(port.has_value());

      TDate dates[4] = {cdsmodel::to_tdate(d(2024, 2, 1)), cdsmodel::to_tdate(d(2024, 4, 1)),
                        cdsmodel::to_tdate(d(2024, 7, 1)), cdsmodel::to_tdate(d(2025, 1, 1))};
      double rates[4] = {0.0525, 0.0531, 0.0540, 0.0552};
      const std::vector<cdsmodel::RatePoint> quotes{{cdsmodel::from_tdate(dates[0]), rates[0]},
                                                    {cdsmodel::from_tdate(dates[1]), rates[1]},
                                                    {cdsmodel::from_tdate(dates[2]), rates[2]},
                                                    {cdsmodel::from_tdate(dates[3]), rates[3]}};

      const int rc =
          JpmcdsZCAddMoneyMarket(ref.zc, dates, rates, 4, cdsmodel::to_upstream_code(quote_dcc));
      const bool ok = port->add_money_market(quotes, quote_dcc);
      ASSERT_EQ(ok, rc == SUCCESS) << "basis " << basis;
      ASSERT_EQ(port->points().size(), static_cast<std::size_t>(ref.zc->numItems));
      for (std::size_t i = 0; i < port->points().size(); ++i) {
        EXPECT_EQ(cdsmodel::to_tdate(port->points()[i].date), ref.zc->date[i]) << i;
        EXPECT_NEAR(port->points()[i].rate, ref.zc->rate[i],
                    4.0 * std::max(1.0, std::fabs(ref.zc->rate[i])) *
                        std::numeric_limits<double>::epsilon())
            << i;
      }
    }
  }
}

// =====================================================================
// The two divergences. These fail loudly if upstream is ever fixed.
// =====================================================================

TEST_F(BuilderDifferential, Upstream18_ABadStartingDiscountIsLoggedAndReportedAsSuccess) {
  // Continuous basis, 30 years, a rate large enough to underflow the discount
  // factor to exactly zero. Upstream detects it, writes the error message,
  // and returns SUCCESS with a fabricated 1.0 -- which becomes a forward rate
  // of exactly zero.
  const cdsmodel::Date vd = d(2024, 1, 1);
  const cdsmodel::Date start = d(2054, 1, 1);
  const cdsmodel::Date end = d(2055, 1, 1);
  Pair pair(vd, 5000, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.seed(start, 25.0));
  ASSERT_TRUE(pair.seed(end, 25.0));

  double at_start = -1.0;
  ASSERT_EQ(JpmcdsZCDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(start), JPMCDS_LINEAR_INTERP,
                                   nullptr, &at_start),
            SUCCESS);
  ASSERT_EQ(at_start, 0.0) << "the premise: the discount factor underflowed";

  double fwd_df = -1.0;
  EXPECT_EQ(JpmcdsZCFwdDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(start),
                                      cdsmodel::to_tdate(end), JPMCDS_LINEAR_INTERP, nullptr,
                                      &fwd_df),
            SUCCESS)
      << "upstream reports success";
  EXPECT_EQ(fwd_df, 1.0) << "with a fabricated 1.0 -- no discounting over the forward";

  double fwd = -1.0;
  EXPECT_EQ(JpmcdsZCForwardRate(pair.ref.zc, cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
                                JPMCDS_SIMPLE_BASIS, JPMCDS_ACT_365F, JPMCDS_LINEAR_INTERP, nullptr,
                                &fwd),
            SUCCESS);
  EXPECT_EQ(fwd, 0.0) << "and the caller is told the forward rate is zero";

  // The port refuses both.
  EXPECT_FALSE(
      pair.port->forward_discount_factor(start, end, cdsmodel::InterpType::Linear).has_value());
  EXPECT_FALSE(pair.port
                   ->forward_rate(start, end, 0, cdsmodel::DayCount::Act365Fixed,
                                  cdsmodel::InterpType::Linear)
                   .has_value());
  // The underflow itself is NOT the divergence -- both sides agree there.
  const cdsmodel::Expected<double> ported_df =
      pair.port->discount_factor(start, cdsmodel::InterpType::Linear);
  ASSERT_TRUE(ported_df.has_value());
  EXPECT_EQ(*ported_df, 0.0) << "the port underflows to the same zero";
}

TEST_F(BuilderDifferential, Upstream19_ANanDiscountFactorPassesTheGuardMeantToStopIt) {
  // Two legitimately infinite discount factors -- the overflow UPSTREAM-15
  // rules is a value, not a defect -- whose ratio is indeterminate. The guard
  // is `if (discount <= 0.0)`, and NaN passes every comparison.
  const cdsmodel::Date vd = d(2024, 1, 1);
  const cdsmodel::Date start = d(2054, 1, 1);
  const cdsmodel::Date end = d(2055, 1, 1);
  Pair pair(vd, 5000, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.seed(start, -24.0));
  ASSERT_TRUE(pair.seed(end, -24.0));

  double at_start = 0.0;
  double at_end = 0.0;
  ASSERT_EQ(JpmcdsZCDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(start), JPMCDS_LINEAR_INTERP,
                                   nullptr, &at_start),
            SUCCESS);
  ASSERT_EQ(JpmcdsZCDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(end), JPMCDS_LINEAR_INTERP,
                                   nullptr, &at_end),
            SUCCESS);
  ASSERT_TRUE(std::isinf(at_start) && std::isinf(at_end)) << "the premise: both overflowed";

  double fwd_df = 0.0;
  EXPECT_EQ(JpmcdsZCFwdDiscountFactor(pair.ref.zc, cdsmodel::to_tdate(start),
                                      cdsmodel::to_tdate(end), JPMCDS_LINEAR_INTERP, nullptr,
                                      &fwd_df),
            SUCCESS);
  EXPECT_TRUE(std::isnan(fwd_df)) << "inf / inf";

  double fwd = 0.0;
  EXPECT_EQ(JpmcdsZCForwardRate(pair.ref.zc, cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
                                JPMCDS_SIMPLE_BASIS, JPMCDS_ACT_365F, JPMCDS_LINEAR_INTERP, nullptr,
                                &fwd),
            SUCCESS)
      << "a NaN discount factor walked through `if (discount <= 0.0)`";
  EXPECT_TRUE(std::isnan(fwd)) << "and came back out as a NaN rate, reported as success";

  // Upstream's own conversion has the same blind guard, independent of the
  // curve: this is why the port fixed it there rather than at each caller.
  double direct = 0.0;
  EXPECT_EQ(JpmcdsDiscountToRate(std::numeric_limits<double>::quiet_NaN(),
                                 cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
                                 JPMCDS_ACT_365F, JPMCDS_SIMPLE_BASIS, &direct),
            SUCCESS);
  EXPECT_TRUE(std::isnan(direct));
  EXPECT_FALSE(cdsmodel::discount_to_rate(std::numeric_limits<double>::quiet_NaN(), start, end,
                                          cdsmodel::DayCount::Act365Fixed, 0)
                   .has_value())
      << "the port refuses it at the conversion, so no caller can leak a NaN";

  // The port refuses the whole chain, while still agreeing on the infinities
  // that produced it.
  const cdsmodel::Expected<double> ported_start =
      pair.port->discount_factor(start, cdsmodel::InterpType::Linear);
  ASSERT_TRUE(ported_start.has_value());
  EXPECT_TRUE(std::isinf(*ported_start)) << "the port overflows to the same infinity";
  EXPECT_FALSE(
      pair.port->forward_discount_factor(start, end, cdsmodel::InterpType::Linear).has_value());
  EXPECT_FALSE(pair.port
                   ->forward_rate(start, end, 0, cdsmodel::DayCount::Act365Fixed,
                                  cdsmodel::InterpType::Linear)
                   .has_value());
}

} // namespace
