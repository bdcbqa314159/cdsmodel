// TIER 2 -- live differential tests for rate <-> discount factor conversion.
//
// `basis` is one `double` carrying five meanings, and the switch dispatches on
// `(long)basis` while the FORMULAS use the full double. So a fractional basis
// truncates for dispatch and then appears undivided in the arithmetic -- the
// sweep below includes those deliberately.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cfinanci.h"
#include "dateconv.h"
#include "ldate.h"
}

namespace {

class RateConversionDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Every basis sentinel, plus fractional values that truncate for dispatch.
const std::vector<double> &bases() {
  static const std::vector<double> b{cdsmodel::kSimpleBasis,
                                     cdsmodel::kDiscountFactorBasis,
                                     cdsmodel::kDiscountRateBasis,
                                     cdsmodel::kContinuousBasis,
                                     1.0,
                                     2.0,
                                     4.0,
                                     12.0,
                                     365.0,
                                     1.5,
                                     4.7,
                                     -1.0,
                                     -3.0};
  return b;
}

TEST_F(RateConversionDifferential, RateToDiscountYearFracMatchesEverywhere) {
  int compared = 0;
  int rejected = 0;
  for (double basis : bases()) {
    for (double rate : {0.05, 0.0, -0.05, 0.5, -0.5, -0.999, -1.0, -1.5, 2.0, 100.0}) {
      for (double yf : {0.0, 1.0e-17, 0.25, 1.0, 5.0, 30.0, -1.0}) {
        double expected = 0.0;
        const bool ref_ok = JpmcdsRateToDiscountYearFrac(rate, yf, basis, &expected) == SUCCESS;
        const std::optional<double> got = cdsmodel::rate_to_discount_year_frac(rate, yf, basis);
        const std::string where = "basis " + std::to_string(basis) + " rate " +
                                  std::to_string(rate) + " yf " + std::to_string(yf);
        ASSERT_EQ(got.has_value(), ref_ok) << where;
        if (!ref_ok) {
          ++rejected;
          continue;
        }
        // One formula, same libm in-process: bit equality or the port
        // reordered something.
        if (std::isnan(expected)) {
          EXPECT_TRUE(std::isnan(*got)) << where;
        } else {
          EXPECT_EQ(*got, expected) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 500) << "sweep thinner than expected: " << compared;
  EXPECT_GT(rejected, 50) << "the rejection paths were barely exercised: " << rejected;
}

TEST_F(RateConversionDifferential, DiscountToRateYearFracMatchesEverywhere) {
  int compared = 0;
  int rejected = 0;
  for (double basis : bases()) {
    for (double discount : {1.0, 0.5, 0.9999, 1.0e-8, 2.0, 100.0, 0.0, -0.5}) {
      for (double yf : {0.0, 1.0e-17, 0.25, 1.0, 5.0, 30.0, -1.0}) {
        double expected = 0.0;
        const bool ref_ok = JpmcdsDiscountToRateYearFrac(discount, yf, basis, &expected) == SUCCESS;
        const std::optional<double> got = cdsmodel::discount_to_rate_year_frac(discount, yf, basis);
        const std::string where = "basis " + std::to_string(basis) + " df " +
                                  std::to_string(discount) + " yf " + std::to_string(yf);
        ASSERT_EQ(got.has_value(), ref_ok) << where;
        if (!ref_ok) {
          ++rejected;
          continue;
        }
        if (std::isnan(expected)) {
          EXPECT_TRUE(std::isnan(*got)) << where;
        } else {
          EXPECT_EQ(*got, expected) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 300) << "sweep thinner than expected: " << compared;
  EXPECT_GT(rejected, 50) << rejected;
}

TEST_F(RateConversionDifferential, TheDateBasedFormsMatchIncludingTheirAsymmetries) {
  // The first draft of the port assumed discount_to_rate mirrors
  // rate_to_discount. It does not, and these are the cases that prove it.
  int compared = 0;
  for (double basis : bases()) {
    for (cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
          cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::EffectiveRate}) {
      struct Span {
        cdsmodel::Date start;
        cdsmodel::Date end;
      };
      for (const Span &s :
           {Span{d(2024, 1, 1), d(2029, 1, 1)}, Span{d(2024, 1, 1), d(2024, 1, 1)}, // zero length
            Span{d(2024, 1, 1), d(2024, 1, 2)},                                     // one day
            Span{d(2029, 1, 1), d(2024, 1, 1)}}) {                                  // backwards
        for (double value : {0.05, 1.0, 0.5, -0.05, 0.0}) {
          const std::string where = "basis " + std::to_string(basis) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(dcc)) + " " +
                                    std::to_string(cdsmodel::to_tdate(s.start)) + ".." +
                                    std::to_string(cdsmodel::to_tdate(s.end)) + " v " +
                                    std::to_string(value);

          double ref_disc = 0.0;
          const bool ref_fwd =
              JpmcdsRateToDiscount(value, cdsmodel::to_tdate(s.start), cdsmodel::to_tdate(s.end),
                                   cdsmodel::to_upstream_code(dcc), static_cast<long>(basis),
                                   &ref_disc) == SUCCESS;
          const std::optional<double> fwd =
              cdsmodel::rate_to_discount(value, s.start, s.end, dcc, static_cast<long>(basis));
          ASSERT_EQ(fwd.has_value(), ref_fwd) << "forward: " << where;
          if (ref_fwd) {
            EXPECT_EQ(*fwd, ref_disc) << "forward: " << where;
            ++compared;
          }

          double ref_rate = 0.0;
          const bool ref_back =
              JpmcdsDiscountToRate(value, cdsmodel::to_tdate(s.start), cdsmodel::to_tdate(s.end),
                                   cdsmodel::to_upstream_code(dcc), static_cast<long>(basis),
                                   &ref_rate) == SUCCESS;
          const std::optional<double> back =
              cdsmodel::discount_to_rate(value, s.start, s.end, dcc, static_cast<long>(basis));
          ASSERT_EQ(back.has_value(), ref_back) << "backward: " << where;
          if (ref_back) {
            EXPECT_EQ(*back, ref_rate) << "backward: " << where;
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 300) << "sweep thinner than expected: " << compared;
}

TEST_F(RateConversionDifferential, TheZeroLengthAsymmetryIsReal) {
  // Isolated, because it is the thing I got wrong by assuming symmetry.
  const cdsmodel::Date same = d(2024, 6, 20);
  const long dcc = JPMCDS_ACT_365F;

  // Forward: no zero-length check at all. It asks for the year fraction of
  // nothing, gets 0, and the formula decides.
  double fwd = 0.0;
  EXPECT_EQ(
      JpmcdsRateToDiscount(0.05, cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), dcc, 1, &fwd),
      SUCCESS);
  EXPECT_DOUBLE_EQ(fwd, 1.0) << "a rate over no time should discount by nothing";

  // Backward, non-discount-factor basis: rejected outright.
  double back = 0.0;
  EXPECT_NE(
      JpmcdsDiscountToRate(1.0, cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), dcc, 1, &back),
      SUCCESS)
      << "upstream now accepts a zero-length period going backwards";
  EXPECT_FALSE(
      cdsmodel::discount_to_rate(1.0, same, same, cdsmodel::DayCount::Act365Fixed, 1).has_value());

  // Backward, discount-factor basis over zero length: a factor of 1.0 gives a
  // RATE of 1.0 (not the factor), and anything else is an error.
  double unit = 0.0;
  EXPECT_EQ(JpmcdsDiscountToRate(1.0, cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), dcc,
                                 static_cast<long>(cdsmodel::kDiscountFactorBasis), &unit),
            SUCCESS);
  EXPECT_DOUBLE_EQ(unit, 1.0);
  EXPECT_DOUBLE_EQ(*cdsmodel::discount_to_rate(1.0, same, same, cdsmodel::DayCount::Act365Fixed,
                                               static_cast<long>(cdsmodel::kDiscountFactorBasis)),
                   1.0);

  double off = 0.0;
  EXPECT_NE(JpmcdsDiscountToRate(0.98, cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), dcc,
                                 static_cast<long>(cdsmodel::kDiscountFactorBasis), &off),
            SUCCESS)
      << "a discount factor other than 1 over no time should be contradictory";
  EXPECT_FALSE(cdsmodel::discount_to_rate(0.98, same, same, cdsmodel::DayCount::Act365Fixed,
                                          static_cast<long>(cdsmodel::kDiscountFactorBasis))
                   .has_value());
}

TEST_F(RateConversionDifferential, RateValidityMatches) {
  int compared = 0;
  for (double basis : bases()) {
    for (double rate : {0.05, 0.0, -0.5, -1.0, -1.5, -2.0, -3.0, -13.0, 2.0, 1000.0}) {
      for (double yf : {0.0, 0.25, 1.0, 5.0}) {
        const bool ref_ok =
            JpmcdsRateValidYearFrac(const_cast<char *>("t"), rate, yf, basis) == SUCCESS;
        EXPECT_EQ(cdsmodel::rate_valid_year_frac(rate, yf, basis), ref_ok)
            << "basis " << basis << " rate " << rate << " yf " << yf;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 400) << compared;
}

TEST_F(RateConversionDifferential, AFractionalBasisTruncatesForDispatchButNotInTheFormula) {
  // The switch is on (long)basis; the formulas use the full double. So basis
  // 1.5 dispatches to the compounding branch as if it were 1, and then
  // computes pow(1 + rate/1.5, -1.5 * t). Worth pinning because it looks like
  // a bug in both directions and is consistent in both.
  for (double basis : {1.5, 4.7, 11.9}) {
    double expected = 0.0;
    ASSERT_EQ(JpmcdsRateToDiscountYearFrac(0.05, 2.0, basis, &expected), SUCCESS) << basis;
    const std::optional<double> got = cdsmodel::rate_to_discount_year_frac(0.05, 2.0, basis);
    ASSERT_TRUE(got.has_value()) << basis;
    EXPECT_EQ(*got, expected) << basis;
    // And it really is the fractional basis in the exponent, not the integer.
    EXPECT_NE(*got, *cdsmodel::rate_to_discount_year_frac(0.05, 2.0, std::floor(basis)))
        << basis << " behaved as if truncated in the formula too";
  }
}

} // namespace
