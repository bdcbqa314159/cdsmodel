// TIER 2 -- live differential tests for curve interpolation.
//
// The sweep is over the three things that pick a branch: the interpolation
// type, where the desired date sits relative to the curve and its base date,
// and whether the date is fractional. The base-date case matters more than it
// looks -- it is the branch that keeps the discount factor at the base date
// exactly 1.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "bsearch.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class InterpCurveDifferential : public ::testing::Test {
protected:
  void SetUp() override {
    JpmcdsErrMsgOff();
    // Every test starts from a known global state, because the function under
    // test leaks it -- see UPSTREAM-28. Without this, test ORDER would decide
    // what the checked searches do.
    JpmcdsBSearchCheckOrder(TRUE);
  }
  void TearDown() override { JpmcdsBSearchCheckOrder(TRUE); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct Ref {
  TCurve *curve = nullptr;
  explicit Ref(TCurve *owned) : curve(owned) {}
  ~Ref() { JpmcdsFreeTCurve(curve); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return curve != nullptr; }
};

long upstream_interp(cdsmodel::InterpType t) {
  return t == cdsmodel::InterpType::Linear ? JPMCDS_LINEAR_INTERP : JPMCDS_FLAT_FORWARDS;
}

/// A curve on both sides, built from the same points.
struct Pair {
  Ref ref;
  std::optional<cdsmodel::ZeroCurve> port;

  Pair(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points, double basis,
       cdsmodel::DayCount dcc)
      : ref(nullptr) {
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (const cdsmodel::RatePoint &p : points) {
      dates.push_back(cdsmodel::to_tdate(p.date));
      rates.push_back(p.rate);
    }
    ref.curve =
        JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                         static_cast<int>(points.size()), basis, cdsmodel::to_upstream_code(dcc));
    port = cdsmodel::ZeroCurve::create(base, points, basis, dcc);
  }
};

std::vector<cdsmodel::RatePoint> points_from(cdsmodel::Date base, int count) {
  std::vector<cdsmodel::RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(cdsmodel::RatePoint{*cdsmodel::make_date(2024 + i, 1, 1),
                                         0.02 + 0.004 * static_cast<double>(i)});
  }
  (void)base;
  return points;
}

TEST_F(InterpCurveDifferential, MatchesAcrossBasesDayCountsAndBothInterpTypes) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 6);
  int compared = 0;
  for (const double basis : {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(base, points, basis, dcc);
      ASSERT_TRUE(pair.ref) << basis;
      ASSERT_TRUE(pair.port.has_value()) << basis;
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        // Before the curve, on the base date, on a point, between points,
        // and past the end.
        for (const cdsmodel::Date at : {d(2020, 1, 1), d(2024, 1, 1), d(2025, 1, 1), d(2025, 7, 4),
                                        d(2027, 3, 15), d(2030, 1, 1), d(2040, 6, 30)}) {
          double want = 0.0;
          const int rc = JpmcdsInterpRate(cdsmodel::to_tdate(at), pair.ref.curve,
                                          upstream_interp(interp), &want);
          const std::optional<double> got = cdsmodel::interp_rate(*pair.port, at, interp);
          const std::string where = "basis " + std::to_string(basis) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(dcc)) + " interp " +
                                    std::to_string(upstream_interp(interp)) + " at " +
                                    std::to_string(cdsmodel::to_tdate(at));
          ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
          if (got) {
            // Flat forwards goes through pow and two conversions, so the
            // non-accumulating tier; linear is arithmetic on stored values.
            EXPECT_NEAR(*got, want,
                        4.0 * std::max(1.0, std::fabs(want)) *
                            std::numeric_limits<double>::epsilon())
                << where;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 150) << compared;
}

TEST_F(InterpCurveDifferential, TheBaseDateBranchKeepsTheDiscountFactorAtOne) {
  // The branch that exists so "basedate will be associated with a 'kink' in
  // the forwards, and discount(base) == 1.0". Reached when the base date falls
  // strictly between two curve points, which needs a curve starting before it.
  const cdsmodel::Date base = d(2025, 6, 1);
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 1, 1), 0.03}, {d(2027, 1, 1), 0.05}};
  int compared = 0;
  for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
    Pair pair(base, points, basis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref) << basis;
    ASSERT_TRUE(pair.port.has_value()) << basis;
    for (const cdsmodel::Date at :
         {d(2024, 6, 1), d(2025, 5, 31), d(2025, 6, 1), d(2025, 6, 2), d(2026, 6, 1)}) {
      double want = 0.0;
      const int rc =
          JpmcdsInterpRate(cdsmodel::to_tdate(at), pair.ref.curve, JPMCDS_FLAT_FORWARDS, &want);
      const std::optional<double> got =
          cdsmodel::interp_rate(*pair.port, at, cdsmodel::InterpType::FlatForwards);
      ASSERT_EQ(got.has_value(), rc == SUCCESS) << basis << " at " << cdsmodel::to_tdate(at);
      if (got) {
        EXPECT_NEAR(*got, want,
                    4.0 * std::max(1.0, std::fabs(want)) * std::numeric_limits<double>::epsilon())
            << basis << " at " << cdsmodel::to_tdate(at);
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 12) << compared;
}

TEST_F(InterpCurveDifferential, FractionalDatesMatchForTheConventionsThatAllowThem) {
  // JpmcdsInterpRateExact takes a double so tree models can ask for a time
  // between dates. Only ACT/360 and ACT/365F support it, and those are the
  // only two the port's curve can hold anyway.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 5);
  int compared = 0;
  for (const cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
    for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
      Pair pair(base, points, basis, dcc);
      ASSERT_TRUE(pair.ref);
      ASSERT_TRUE(pair.port.has_value());
      const double anchor = static_cast<double>(cdsmodel::to_tdate(d(2026, 1, 1)));
      for (const double offset : {0.25, 0.5, 0.75, 1.5, 100.125}) {
        const double at = anchor + offset;
        for (const cdsmodel::InterpType interp :
             {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
          double want = 0.0;
          const int rc = JpmcdsInterpRateExact(at, pair.ref.curve, upstream_interp(interp), &want);
          const std::optional<double> got = cdsmodel::interp_rate_exact(*pair.port, at, interp);
          const std::string where = "dcc " + std::to_string(cdsmodel::to_upstream_code(dcc)) +
                                    " basis " + std::to_string(basis) + " at " + std::to_string(at);
          ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
          if (got) {
            EXPECT_NEAR(*got, want,
                        4.0 * std::max(1.0, std::fabs(want)) *
                            std::numeric_limits<double>::epsilon())
                << where;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 50) << compared;
}

TEST_F(InterpCurveDifferential, AnUnknownInterpolationTypeIsRejected) {
  // The port's enum has two values, so this checks the upstream half: that the
  // domain really is those two and everything else is an error.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 4);
  Pair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  double out = 0.0;
  for (const long bogus : {1L, 2L, 5L, 123L, 125L, 999L}) {
    EXPECT_EQ(JpmcdsInterpRate(cdsmodel::to_tdate(d(2026, 1, 1)), pair.ref.curve, bogus, &out),
              FAILURE)
        << bogus;
  }
}

TEST_F(InterpCurveDifferential, TheBranchesThePortCannotReachAreUnreachableByConstruction) {
  // "Missing a branch" and "cannot represent the input that reaches it" look
  // the same in a diff, so this asserts the second.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 4);

  // The three day counts InterpRateFlatFwds handles via JpmcdsDaysDiff. Both
  // sides refuse 30/360 and 30E/360 at construction... except upstream does
  // not -- that is UPSTREAM-15, and it is why the port is stricter.
  for (const cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::Act365}) {
    EXPECT_FALSE(cdsmodel::ZeroCurve::create(base, points, 1.0, dcc).has_value())
        << "day count " << cdsmodel::to_upstream_code(dcc);
  }

  // The discount-factor basis, which InterpRateDiscountFactors exists for.
  EXPECT_FALSE(cdsmodel::ZeroCurve::create(base, points, cdsmodel::kDiscountFactorBasis,
                                           cdsmodel::DayCount::Act365Fixed)
                   .has_value())
      << "basis -2 has no port, so neither does the branch that reads it";
  // And upstream builds it happily, which is the measurement that makes the
  // port's refusal a decision rather than an omission.
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(0.9 - 0.05 * static_cast<double>(rates.size()));
  }
  const Ref upstream_df(JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                                         static_cast<int>(dates.size()),
                                         cdsmodel::kDiscountFactorBasis, JPMCDS_ACT_365F));
  EXPECT_TRUE(static_cast<bool>(upstream_df))
      << "upstream accepts a discount-factor curve; the port does not";
}

// =====================================================================
// UPSTREAM-28.
// =====================================================================

TEST_F(InterpCurveDifferential, Upstream28_ExtrapolationLeavesOrderCheckingDisabled) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 3);
  Pair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  // JpmcdsBSearchCheckOrder(x) sets the flag and returns the PREVIOUS value,
  // so calling it twice reads the state without changing it.
  const auto read_flag = []() {
    const TBoolean was = JpmcdsBSearchCheckOrder(TRUE);
    JpmcdsBSearchCheckOrder(was);
    return was == TRUE;
  };

  double rate = 0.0;
  // A date inside the curve reaches `done:` and restores the flag.
  JpmcdsBSearchCheckOrder(TRUE);
  ASSERT_EQ(JpmcdsInterpRate(cdsmodel::to_tdate(d(2025, 7, 1)), pair.ref.curve,
                             JPMCDS_LINEAR_INTERP, &rate),
            SUCCESS);
  EXPECT_TRUE(read_flag()) << "an interior date restores the flag";

  // Both extrapolation paths `return SUCCESS` before reaching `done:`.
  JpmcdsBSearchCheckOrder(TRUE);
  ASSERT_EQ(JpmcdsInterpRate(cdsmodel::to_tdate(d(2040, 1, 1)), pair.ref.curve,
                             JPMCDS_LINEAR_INTERP, &rate),
            SUCCESS);
  EXPECT_FALSE(read_flag()) << "extrapolating past the end leaked the flag";

  JpmcdsBSearchCheckOrder(TRUE);
  ASSERT_EQ(JpmcdsInterpRate(cdsmodel::to_tdate(d(2000, 1, 1)), pair.ref.curve,
                             JPMCDS_LINEAR_INTERP, &rate),
            SUCCESS);
  EXPECT_FALSE(read_flag()) << "extrapolating before the start leaked it too";

  // The opposite of UPSTREAM-13: here the genuine error path restores it.
  JpmcdsBSearchCheckOrder(TRUE);
  (void)JpmcdsInterpRate(cdsmodel::to_tdate(d(2025, 7, 1)), pair.ref.curve, 999L, &rate);
  EXPECT_TRUE(read_flag()) << "an error path reaches done: and restores";

  // And the consequence, in two calls: a precondition check silently stops
  // checking, process-wide.
  double unsorted[4] = {1.0, 5.0, 3.0, 9.0};
  int lo = 0;
  int hi = 0;
  JpmcdsBSearchCheckOrder(TRUE);
  EXPECT_EQ(JpmcdsBSearchDouble(4.0, unsorted, sizeof(double), 4, &lo, &hi), FAILURE)
      << "with checking on, an unsorted array is rejected";
  ASSERT_EQ(JpmcdsInterpRate(cdsmodel::to_tdate(d(2040, 1, 1)), pair.ref.curve,
                             JPMCDS_LINEAR_INTERP, &rate),
            SUCCESS);
  EXPECT_EQ(JpmcdsBSearchDouble(4.0, unsorted, sizeof(double), 4, &lo, &hi), SUCCESS)
      << "after one extrapolation the same array passes -- no longer validated";

  // The port has no such flag to leak. Extrapolating repeatedly changes
  // nothing about any other call, because there is no shared state to change.
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(
        cdsmodel::interp_rate(*pair.port, d(2040, 1, 1), cdsmodel::InterpType::Linear).has_value());
  }
  EXPECT_FALSE(cdsmodel::bracket_checked(std::span<const double>{unsorted}, 4.0).has_value())
      << "the port's checked search still checks, however many times it ran";
}

TEST_F(InterpCurveDifferential, TheTwoFlatForwardsImplementationsAgree) {
  // interpc.c's InterpRateFlatFwds and zcall.c's ZCInterpolate are
  // independent implementations of flat forwards on two curve types -- noted
  // two slices ago as worth comparing directly. This is the comparison.
  //
  // They can only be compared where both types accept the same inputs: basis
  // 1..365 (the builder refuses continuous? no -- it refuses simple), ACT/365F
  // or ACT/360, and a date inside the curve.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = points_from(base, 5);
  int compared = 0;
  for (const long basis : {1L, 2L, 4L, 12L}) {
    const std::optional<cdsmodel::ZeroCurve> curve = cdsmodel::ZeroCurve::create(
        base, points, static_cast<double>(basis), cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(curve.has_value()) << basis;
    std::optional<cdsmodel::ZeroCurveBuilder> builder =
        cdsmodel::ZeroCurveBuilder::create(base, 16, basis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(builder.has_value()) << basis;
    for (const cdsmodel::RatePoint &p : points) {
      ASSERT_TRUE(builder->add_rate(p.date, p.rate)) << basis;
    }
    for (const cdsmodel::Date at : {d(2025, 4, 1), d(2026, 7, 15), d(2028, 2, 29)}) {
      const std::optional<double> from_curve =
          cdsmodel::interp_rate(*curve, at, cdsmodel::InterpType::FlatForwards);
      const std::optional<double> from_builder =
          builder->interpolate(at, cdsmodel::InterpType::FlatForwards);
      ASSERT_TRUE(from_curve.has_value()) << basis;
      ASSERT_TRUE(from_builder.has_value()) << basis;
      // Two independent routes to the same quantity. They are not required to
      // be bit-identical -- one brackets with the base date in play and the
      // other does not -- so the accumulating tier applies.
      EXPECT_NEAR(*from_curve, *from_builder, 1.0e-12 * std::max(1.0, std::fabs(*from_curve)))
          << "basis " << basis << " at " << cdsmodel::to_tdate(at);
      ++compared;
    }
  }
  EXPECT_GT(compared, 10) << compared;
}

} // namespace
