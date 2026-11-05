// TIER 1 -- rate <-> discount factor conversion. Contract first, then replay.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>

#include <gtest/gtest.h>

#include "golden/rate_conversion_cases.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::discount_to_rate_year_frac;
using cdsmodel::rate_to_discount_year_frac;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

// =====================================================================
// The contract: the two directions must invert each other.
// =====================================================================

TEST(RateConversion, TheTwoDirectionsInvertEachOther) {
  // No oracle, no recorded numbers. Wherever both directions succeed, the
  // round trip must return the rate it started from.
  //
  // The tolerance is relative 1e-12 rather than exact: the compounding branch
  // is pow(1 + r/n, -n*t) one way and n*(pow(d, -1/(n*t)) - 1) the other, so
  // the round trip is two transcendental calls deep and will not come back
  // bit-identical.
  int round_trips = 0;
  for (double basis :
       {cdsmodel::kSimpleBasis, cdsmodel::kDiscountRateBasis, cdsmodel::kContinuousBasis,
        cdsmodel::kDiscountFactorBasis, 1.0, 2.0, 4.0, 12.0, 365.0}) {
    for (double rate : {0.05, 0.0, -0.05, 0.3, -0.3, 0.9}) {
      for (double t : {0.25, 1.0, 5.0, 30.0}) {
        const std::optional<double> discount = rate_to_discount_year_frac(rate, t, basis);
        if (!discount) {
          continue;
        }
        const std::optional<double> back = discount_to_rate_year_frac(*discount, t, basis);
        if (!back) {
          continue;
        }
        const std::string where = "basis " + std::to_string(basis) + " rate " +
                                  std::to_string(rate) + " t " + std::to_string(t);
        EXPECT_NEAR(*back, rate, 1.0e-12 * std::max(1.0, std::fabs(rate))) << where;
        ++round_trips;
      }
    }
  }
  EXPECT_GT(round_trips, 100) << "too few round trips: " << round_trips;
}

TEST(RateConversion, AZeroRateDiscountsByNothing) {
  // Every basis except the discount-factor pass-through must leave a zero rate
  // discounting to exactly 1, for any period.
  for (double basis : {cdsmodel::kSimpleBasis, cdsmodel::kDiscountRateBasis,
                       cdsmodel::kContinuousBasis, 1.0, 2.0, 365.0}) {
    for (double t : {0.0, 0.25, 1.0, 30.0}) {
      const std::optional<double> discount = rate_to_discount_year_frac(0.0, t, basis);
      ASSERT_TRUE(discount.has_value()) << "basis " << basis << " t " << t;
      EXPECT_DOUBLE_EQ(*discount, 1.0) << "basis " << basis << " t " << t;
    }
  }
  // The discount-factor basis is a pass-through, so a "rate" of 0 is a
  // discount factor of 0 -- and that is what it returns, rather than 1.
  EXPECT_DOUBLE_EQ(*rate_to_discount_year_frac(0.0, 1.0, cdsmodel::kDiscountFactorBasis), 0.0);
}

TEST(RateConversion, APositiveRateDiscountsBelowOneAndMonotonically) {
  for (double basis : {cdsmodel::kSimpleBasis, cdsmodel::kContinuousBasis, 1.0, 2.0, 12.0}) {
    std::optional<double> previous;
    for (double t : {0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0}) {
      const std::optional<double> discount = rate_to_discount_year_frac(0.05, t, basis);
      ASSERT_TRUE(discount.has_value()) << "basis " << basis << " t " << t;
      EXPECT_LT(*discount, 1.0) << "basis " << basis << " t " << t;
      EXPECT_GT(*discount, 0.0) << "basis " << basis << " t " << t;
      if (previous) {
        EXPECT_LT(*discount, *previous) << "basis " << basis << " t " << t;
      }
      previous = discount;
    }
  }
}

TEST(RateConversion, TheContinuousBranchIsExactlyExpMinusRateTime) {
  for (double rate : {0.05, 0.0, -0.05, 2.0}) {
    for (double t : {0.0, 0.25, 1.0, 30.0}) {
      EXPECT_EQ(*rate_to_discount_year_frac(rate, t, cdsmodel::kContinuousBasis),
                std::exp(-rate * t))
          << rate << " " << t;
    }
  }
}

// =====================================================================
// The asymmetries, pinned.
// =====================================================================

TEST(RateConversion, ZeroTimeHasAnAnswerForwardAndNotBackward) {
  // Upstream's asymmetry, which I first ported away by assuming the two
  // directions mirrored each other.
  const cdsmodel::Date same = d(2024, 6, 20);

  // Forward: no zero-length check. A rate over no time discounts by nothing.
  const std::optional<double> fwd =
      cdsmodel::rate_to_discount(0.05, same, same, cdsmodel::DayCount::Act365Fixed, 1);
  ASSERT_TRUE(fwd.has_value());
  EXPECT_DOUBLE_EQ(*fwd, 1.0);

  // Backward: rejected outright for every basis but the discount factor.
  for (long basis : {0L, 1L, 2L, 512L, 5000L}) {
    EXPECT_FALSE(cdsmodel::discount_to_rate(1.0, same, same, cdsmodel::DayCount::Act365Fixed, basis)
                     .has_value())
        << "basis " << basis;
  }

  // The discount-factor basis over zero length: a factor of 1 yields a RATE of
  // 1, and any other factor is contradictory.
  const long df = static_cast<long>(cdsmodel::kDiscountFactorBasis);
  EXPECT_DOUBLE_EQ(
      *cdsmodel::discount_to_rate(1.0, same, same, cdsmodel::DayCount::Act365Fixed, df), 1.0);
  EXPECT_FALSE(cdsmodel::discount_to_rate(0.98, same, same, cdsmodel::DayCount::Act365Fixed, df)
                   .has_value());
}

TEST(RateConversion, TheDiscountRateBasisTreatsZeroTimeAsNoDiscount) {
  // In the forward direction only, a zero year fraction short-circuits to 1.0
  // for the discount-rate basis -- before the formula, which would also give
  // 1.0 but only because `1 - r*0` is 1. The branch exists so that an exactly
  // zero period cannot be rejected by the `discount <= 0` test below it.
  EXPECT_DOUBLE_EQ(*rate_to_discount_year_frac(50.0, 0.0, cdsmodel::kDiscountRateBasis), 1.0);
  // With any real period, that same rate overshoots and is refused.
  EXPECT_FALSE(rate_to_discount_year_frac(50.0, 1.0, cdsmodel::kDiscountRateBasis).has_value());
}

TEST(RateConversion, AFractionalBasisTruncatesForDispatchNotInTheFormula) {
  // The switch is on (long)basis; the formula uses the full double. So 1.5
  // dispatches as if it were 1 -- into the compounding branch -- and then
  // computes with 1.5.
  for (double basis : {1.5, 4.7, 11.9}) {
    const std::optional<double> got = rate_to_discount_year_frac(0.05, 2.0, basis);
    ASSERT_TRUE(got.has_value()) << basis;
    const std::optional<double> truncated =
        rate_to_discount_year_frac(0.05, 2.0, std::floor(basis));
    ASSERT_TRUE(truncated.has_value()) << basis;
    EXPECT_NE(*got, *truncated) << basis << " behaved as if truncated in the formula too";
    EXPECT_EQ(*got, std::pow(1.0 + 0.05 / basis, -basis * 2.0)) << basis;
  }
}

TEST(RateConversion, Rejected) {
  // A non-positive discount factor has no rate.
  for (double discount : {0.0, -0.5, -1.0}) {
    EXPECT_FALSE(discount_to_rate_year_frac(discount, 1.0, 1.0).has_value()) << discount;
  }
  // A simple rate that drives the denominator to zero or below.
  EXPECT_FALSE(rate_to_discount_year_frac(-1.0, 1.0, cdsmodel::kSimpleBasis).has_value());
  EXPECT_FALSE(rate_to_discount_year_frac(-2.0, 1.0, cdsmodel::kSimpleBasis).has_value());
  // A compounding rate at or below -basis.
  EXPECT_FALSE(rate_to_discount_year_frac(-1.0, 1.0, 1.0).has_value());
  EXPECT_FALSE(rate_to_discount_year_frac(-2.5, 1.0, 2.0).has_value());
  // A discount-factor "rate" that is not positive, via the date-based form.
  EXPECT_FALSE(cdsmodel::rate_to_discount(0.0, d(2024, 1, 1), d(2029, 1, 1),
                                          cdsmodel::DayCount::Act365Fixed,
                                          static_cast<long>(cdsmodel::kDiscountFactorBasis))
                   .has_value());
  // A basis below simple, other than the discount-factor sentinel.
  EXPECT_FALSE(cdsmodel::rate_to_discount(0.05, d(2024, 1, 1), d(2029, 1, 1),
                                          cdsmodel::DayCount::Act365Fixed, -1)
                   .has_value());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(RateConversion, MatchesRecordedConversions) {
  int forward = 0;
  int backward = 0;
  for (const auto &c : cdsmodel::golden::kConversionCases) {
    const std::string where = "basis " + std::to_string(c.basis) + " v " + std::to_string(c.value) +
                              " yf " + std::to_string(c.year_fraction);

    const std::optional<double> fwd = rate_to_discount_year_frac(c.value, c.year_fraction, c.basis);
    ASSERT_EQ(fwd.has_value(), c.forward_ok) << where;
    if (c.forward_ok) {
      if (std::isnan(c.discount)) {
        EXPECT_TRUE(std::isnan(*fwd)) << where;
      } else if (std::isinf(c.discount)) {
        EXPECT_EQ(*fwd, c.discount) << where;
      } else {
        // A single pow/exp: the non-accumulating tier, 4 ULPs.
        EXPECT_NEAR(*fwd, c.discount,
                    4.0 * std::max(1.0, std::fabs(c.discount)) *
                        std::numeric_limits<double>::epsilon())
            << where;
      }
      ++forward;
    }

    const std::optional<double> back =
        discount_to_rate_year_frac(c.value, c.year_fraction, c.basis);
    ASSERT_EQ(back.has_value(), c.backward_ok) << where;
    if (c.backward_ok) {
      if (std::isnan(c.rate)) {
        EXPECT_TRUE(std::isnan(*back)) << where;
      } else if (std::isinf(c.rate)) {
        EXPECT_EQ(*back, c.rate) << where;
      } else {
        EXPECT_NEAR(*back, c.rate,
                    4.0 * std::max(1.0, std::fabs(c.rate)) * std::numeric_limits<double>::epsilon())
            << where;
      }
      ++backward;
    }
  }
  EXPECT_GT(forward, 500) << forward;
  EXPECT_GT(backward, 300) << backward;
}

TEST(RateConversion, MatchesRecordedValidityVerdicts) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kValidityCases) {
    EXPECT_EQ(cdsmodel::rate_valid_year_frac(c.rate, c.year_fraction, c.basis), c.valid)
        << "basis " << c.basis << " rate " << c.rate << " yf " << c.year_fraction;
    ++checked;
  }
  EXPECT_GT(checked, 500) << checked;
}

} // namespace
