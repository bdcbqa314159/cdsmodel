// TIER 2 -- live differential tests for linear interpolation.
//
// This suite has to defend itself against the defect it is testing.
//
// UPSTREAM-13: `JpmcdsLinInterpLongPoint1Discontinuous` clears the
// process-wide order-checking global and fails to restore it on either of its
// error paths. Any test that trips one of those paths corrupts every
// subsequent test in the same binary -- so TearDown restores the global
// unconditionally. That a differential test needs to clean up after the code
// under test is itself the clearest statement of why the global is a defect.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interpolation.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bsearch.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
#include "lintrp.h"
#include "yearfrac.h"

/// Exported from the library but declared in no public header, so a caller
/// must declare it. The XLL does exactly this.
int JpmcdsLinInterpLongPoint1Discontinuous(long *x, int xskip, int N, double *f, int fskip,
                                           double xDesired, TMetricDoubleFunc mfunc,
                                           TBoolean rightLimit, double *fInterp);
}

namespace {

class InterpolationDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  /// Undo UPSTREAM-13's leak so one test cannot poison the next.
  void TearDown() override { JpmcdsBSearchCheckOrder(TRUE); }
};

std::vector<long> xs_of(std::initializer_list<long> v) { return {v.begin(), v.end()}; }
std::vector<double> fs_of(std::initializer_list<double> v) { return {v.begin(), v.end()}; }

struct Curve {
  std::vector<long> xs;
  std::vector<double> fs;
};

const std::vector<Curve> &curves() {
  static const std::vector<Curve> c{
      {xs_of({100}), fs_of({0.05})},            // single point
      {xs_of({100, 200}), fs_of({0.01, 0.02})}, // two points
      {xs_of({100, 200, 400, 800, 1600}), fs_of({0.01, 0.02, 0.025, 0.03, 0.028})},
      {xs_of({0, 1, 2, 3}), fs_of({-1.0, 0.0, 1.0, 2.0})}, // spans zero
      {xs_of({154497, 154862, 155227, 155592}),            // real TDates
       fs_of({0.028, 0.031, 0.034, 0.037})},
  };
  return c;
}

TEST_F(InterpolationDifferential, PointInterpolationMatchesWithTheDefaultMetric) {
  int compared = 0;
  for (const Curve &c : curves()) {
    std::vector<long> x = c.xs;
    std::vector<double> f = c.fs;
    for (double desired :
         {-50.0, 0.0, 99.5, 100.0, 150.0, 199.9, 200.0, 333.3, 1000.0, 2000.0, 154700.0}) {
      double expected = 0.0;
      const bool ref_ok =
          JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), static_cast<int>(x.size()), f.data(),
                                    sizeof(double), desired, nullptr, &expected) == SUCCESS;
      const cdsmodel::Expected<double> got = cdsmodel::linear_interpolate(x, f, desired);
      const std::string where = "n=" + std::to_string(x.size()) + " at " + std::to_string(desired);
      ASSERT_EQ(got.has_value(), ref_ok) << where;
      if (ref_ok) {
        EXPECT_EQ(*got, expected) << where;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 40) << "sweep thinner than expected: " << compared;
}

TEST_F(InterpolationDifferential, NullAndActualDaysAgreeOnlyWhileTheBracketHasWidth) {
  // The claim I made in the first draft of interpolation.hpp -- that upstream's
  // NULL branch and the ACT metric are interchangeable -- with the caveat the
  // differential suite forced. They agree bit-for-bit on any curve with a
  // non-degenerate bracket, because numerator and denominator are both negated
  // and negation is exact. They diverge on a zero-width bracket, because -0.0
  // is not 0.0; that case is pinned in
  // NullAndActualDaysDisagreeOnAZeroWidthBracket below.
  int compared = 0;
  for (const Curve &c : curves()) {
    std::vector<long> x = c.xs;
    std::vector<double> f = c.fs;
    for (double desired : {-50.0, 0.0, 150.0, 333.3, 1000.0, 154700.0}) {
      double with_null = 0.0;
      double with_metric = 0.0;
      const bool a =
          JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), static_cast<int>(x.size()), f.data(),
                                    sizeof(double), desired, nullptr, &with_null) == SUCCESS;
      const bool b = JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), static_cast<int>(x.size()),
                                               f.data(), sizeof(double), desired,
                                               JpmcdsDayCountToMetricFunc(JPMCDS_ACT_365F),
                                               &with_metric) == SUCCESS;
      ASSERT_EQ(a, b) << desired;
      if (!a) {
        continue;
      }
      EXPECT_EQ(with_null, with_metric) << "NULL and ACT/365F metric differ at " << desired << ": "
                                        << with_null << " vs " << with_metric;
      // And the port agrees with both, through either metric.
      for (cdsmodel::Metric m : {cdsmodel::Metric::Direct, cdsmodel::Metric::ActualDays}) {
        const cdsmodel::Expected<double> port = cdsmodel::linear_interpolate(x, f, desired, m);
        ASSERT_TRUE(port.has_value());
        EXPECT_EQ(*port, with_null) << desired << " metric " << static_cast<int>(m);
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 20) << compared;
}

TEST_F(InterpolationDifferential, NullAndActualDaysDisagreeOnAZeroWidthBracket) {
  // A repeated abscissa at the END of the curve: extrapolating past it
  // brackets x[2] == x[3], so the denominator is zero. The null branch gets
  // +inf and the metric branch -inf, because negating a zero changes its sign
  // and nothing else.
  //
  // Neither is wrong -- the fraction is undefined there -- which is why this
  // is a quirk and not a numbered defect. But they are not interchangeable,
  // and the port therefore keeps Metric::Direct and Metric::ActualDays apart.
  std::vector<long> x{100, 200, 300, 300};
  std::vector<double> f{1.0, 2.0, 3.0, 9.0};

  for (double desired : {350.0, 400.0, 450.0}) {
    double with_null = 0.0;
    double with_metric = 0.0;
    ASSERT_EQ(JpmcdsLinInterpLongPoint1Discontinuous(x.data(), sizeof(long), 4, f.data(),
                                                     sizeof(double), desired, nullptr, TRUE,
                                                     &with_null),
              SUCCESS);
    JpmcdsBSearchCheckOrder(TRUE);
    ASSERT_EQ(JpmcdsLinInterpLongPoint1Discontinuous(
                  x.data(), sizeof(long), 4, f.data(), sizeof(double), desired,
                  JpmcdsDayCountToMetricFunc(JPMCDS_ACT_365F), TRUE, &with_metric),
              SUCCESS);
    JpmcdsBSearchCheckOrder(TRUE);

    EXPECT_TRUE(std::isinf(with_null)) << desired;
    EXPECT_TRUE(std::isinf(with_metric)) << desired;
    EXPECT_NE(with_null, with_metric)
        << "upstream's two paths now agree on a zero-width bracket at " << desired;
    EXPECT_GT(with_null, 0.0) << desired;
    EXPECT_LT(with_metric, 0.0) << desired;

    // The port reproduces each path under its own name.
    const cdsmodel::Expected<double> direct = cdsmodel::linear_interpolate_discontinuous(
        x, f, desired, cdsmodel::Limit::Right, cdsmodel::Metric::Direct);
    const cdsmodel::Expected<double> actual = cdsmodel::linear_interpolate_discontinuous(
        x, f, desired, cdsmodel::Limit::Right, cdsmodel::Metric::ActualDays);
    ASSERT_TRUE(direct.has_value());
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*direct, with_null) << desired;
    EXPECT_EQ(*actual, with_metric) << desired;
  }
}

TEST_F(InterpolationDifferential, BondMetricsMatch) {
  // 30/360 and 30E/360 metrics need real dates, since they decompose into
  // year, month and day.
  std::vector<long> x{154497, 154862, 155227, 155592};
  std::vector<double> f{0.028, 0.031, 0.034, 0.037};
  struct Case {
    long dcc;
    cdsmodel::Metric metric;
  };
  int compared = 0;
  for (const Case &k : {Case{JPMCDS_B30_360, cdsmodel::Metric::Bond30_360},
                        Case{JPMCDS_B30E_360, cdsmodel::Metric::Bond30E_360},
                        Case{JPMCDS_ACT_360, cdsmodel::Metric::ActualDays},
                        Case{JPMCDS_ACT_365, cdsmodel::Metric::ActualDays}}) {
    // The port's own mapping must agree with upstream's dispatch.
    const cdsmodel::Expected<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(k.dcc);
    ASSERT_TRUE(dcc.has_value()) << k.dcc;
    const cdsmodel::Expected<cdsmodel::Metric> mapped = cdsmodel::metric_for_day_count(*dcc);
    ASSERT_TRUE(mapped.has_value()) << k.dcc;
    EXPECT_EQ(*mapped, k.metric) << k.dcc;

    for (double desired : {154500.0, 154700.0, 155000.0, 155400.0, 155592.0}) {
      double expected = 0.0;
      const bool ref_ok =
          JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), static_cast<int>(x.size()), f.data(),
                                    sizeof(double), desired, JpmcdsDayCountToMetricFunc(k.dcc),
                                    &expected) == SUCCESS;
      const cdsmodel::Expected<double> got = cdsmodel::linear_interpolate(x, f, desired, k.metric);
      ASSERT_EQ(got.has_value(), ref_ok) << k.dcc << " at " << desired;
      if (ref_ok) {
        EXPECT_EQ(*got, expected) << k.dcc << " at " << desired;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 15) << compared;
}

TEST_F(InterpolationDifferential, ArrayInterpolationMatches) {
  std::vector<long> x{100, 200, 400, 800};
  std::vector<double> f{0.01, 0.02, 0.025, 0.03};
  std::vector<long> desired{50, 100, 150, 300, 700, 900};
  std::vector<double> expected(desired.size());
  const bool ref_ok =
      JpmcdsLinInterpLongArray1(x.data(), sizeof(long), static_cast<int>(x.size()), f.data(),
                                sizeof(double), desired.data(), sizeof(long),
                                static_cast<int>(desired.size()), nullptr, sizeof(double),
                                expected.data()) == SUCCESS;
  ASSERT_TRUE(ref_ok);
  const cdsmodel::Expected<std::vector<double>> got =
      cdsmodel::linear_interpolate_many(x, f, desired);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((*got)[i], expected[i]) << "i=" << i;
  }
}

TEST_F(InterpolationDifferential, DiscontinuousInterpolationMatchesIncludingRepeatedAbscissae) {
  // Repeated x values with different f values ARE the discontinuities.
  struct Disc {
    std::vector<long> xs;
    std::vector<double> fs;
    const char *what;
  };
  const std::vector<Disc> discs{
      {xs_of({100, 200, 200, 300}), fs_of({1.0, 2.0, 5.0, 6.0}), "one jump"},
      {xs_of({100, 200, 200, 200, 300}), fs_of({1.0, 2.0, 3.0, 5.0, 6.0}), "three share x"},
      {xs_of({100, 100, 200}), fs_of({1.0, 7.0, 2.0}), "jump at the first point"},
      {xs_of({100, 200, 300, 300}), fs_of({1.0, 2.0, 3.0, 9.0}), "jump at the last point"},
      {xs_of({100, 200, 300}), fs_of({1.0, 2.0, 3.0}), "no jump at all"},
  };
  int compared = 0;
  for (const Disc &dsc : discs) {
    std::vector<long> x = dsc.xs;
    std::vector<double> f = dsc.fs;
    for (double desired : {50.0, 100.0, 150.0, 200.0, 250.0, 300.0, 400.0}) {
      for (bool right : {false, true}) {
        double expected = 0.0;
        const bool ref_ok =
            JpmcdsLinInterpLongPoint1Discontinuous(
                x.data(), sizeof(long), static_cast<int>(x.size()), f.data(), sizeof(double),
                desired, nullptr, right ? TRUE : FALSE, &expected) == SUCCESS;
        JpmcdsBSearchCheckOrder(TRUE); // undo any leak before the next call
        const cdsmodel::Expected<double> got = cdsmodel::linear_interpolate_discontinuous(
            x, f, desired, right ? cdsmodel::Limit::Right : cdsmodel::Limit::Left);
        const std::string where =
            std::string(dsc.what) + " at " + std::to_string(desired) + (right ? " right" : " left");
        ASSERT_EQ(got.has_value(), ref_ok) << where;
        if (ref_ok) {
          EXPECT_EQ(*got, expected) << where;
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 50) << "sweep thinner than expected: " << compared;
}

TEST_F(InterpolationDifferential, Upstream14_BondMetricsReturnStackGarbageBeforeTheEpoch) {
  // yearfrac.c's bond metrics discard the status of JpmcdsDaysDiff over an
  // uninitialised local:
  //
  //     static double bondDaysDiff(double s, double e) {
  //         long days;                     /* uninitialised */
  //         (void)JpmcdsDaysDiff((TDate)s, (TDate)e, JPMCDS_B30_360, &days);
  //         return (double)days;           /* garbage if that failed */
  //     }
  //
  // JpmcdsDaysDiff fails for a TDate before the 1601 epoch and does NOT write
  // its out parameter, so the metric returns whatever was on the stack.
  //
  // This is an uninitialised read, which AddressSanitizer cannot see --
  // MemorySanitizer would. It was found by the golden generator's full
  // cross-product of abscissae against metrics, not by this suite, whose
  // sweeps used realistic dates for each metric.
  //
  // First: establish that the out parameter really is left untouched.
  long days = -123456789; // a sentinel no real answer could be
  EXPECT_NE(JpmcdsDaysDiff(static_cast<TDate>(-50), static_cast<TDate>(100), JPMCDS_B30_360, &days),
            SUCCESS);
  EXPECT_EQ(days, -123456789)
      << "upstream now writes the out parameter on failure -- UPSTREAM-14 may be fixed";
  // And that it succeeds from the epoch onward, so the boundary is real.
  days = -123456789;
  EXPECT_EQ(JpmcdsDaysDiff(static_cast<TDate>(0), static_cast<TDate>(100), JPMCDS_B30_360, &days),
            SUCCESS);
  EXPECT_EQ(days, 100);

  // Now the consequence. The curve runs 0.01 to 0.02; a sane extrapolation to
  // -50 is slightly below 0.01.
  std::vector<long> x{100, 200};
  std::vector<double> f{0.01, 0.02};
  double garbage = 0.0;
  ASSERT_EQ(JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), 2, f.data(), sizeof(double), -50.0,
                                      JpmcdsDayCountToMetricFunc(JPMCDS_B30_360), &garbage),
            SUCCESS)
      << "upstream reports SUCCESS, which is half the problem";
  EXPECT_GT(std::fabs(garbage), 1.0)
      << "upstream returned " << garbage << ", which is suspiciously sane for a garbage read";

  // The port returns a defined extrapolation, because metric_distance checks
  // the status it is given. std::chrono has no 1601 epoch, so a pre-epoch
  // abscissa is simply a date in 1600 and the 30/360 difference is real.
  const cdsmodel::Expected<double> ported =
      cdsmodel::linear_interpolate(x, f, -50.0, cdsmodel::Metric::Bond30_360);
  ASSERT_TRUE(ported.has_value());
  EXPECT_LT(*ported, 0.01) << "extrapolating below the first node should fall below its value";
  EXPECT_GT(*ported, -1.0) << "and should stay in the neighbourhood of the curve";
  EXPECT_NE(*ported, garbage);

  // The ACT metric is unaffected: it is a plain subtraction with no day count.
  double actual_metric = 0.0;
  ASSERT_EQ(JpmcdsLinInterpLongPoint1(x.data(), sizeof(long), 2, f.data(), sizeof(double), -50.0,
                                      JpmcdsDayCountToMetricFunc(JPMCDS_ACT_365F), &actual_metric),
            SUCCESS);
  // Derived rather than hardcoded, because I got it wrong by hand the first
  // time: the fraction is (100 - (-50)) / (100 - 200) = -1.5, so the value is
  // 0.01 + (-1.5) * (0.02 - 0.01) = -0.005.
  const double fraction = (100.0 - -50.0) / (100.0 - 200.0);
  EXPECT_NEAR(actual_metric, 0.01 + fraction * (0.02 - 0.01), 1.0e-15);
  EXPECT_EQ(*cdsmodel::linear_interpolate(x, f, -50.0, cdsmodel::Metric::ActualDays),
            actual_metric);
}

TEST_F(InterpolationDifferential, Upstream13_OneFailedCallDisablesOrderCheckingProcessWide) {
  // The defect, asserted so an upstream fix becomes visible.
  //
  // bSearchCheckOrder_g is a static in bsearch.c, default TRUE, consulted by
  // JpmcdsBSearchLong and JpmcdsBSearchDouble before validating that their
  // input array is sorted. The discontinuous routine clears it and returns
  // without restoring it on either error path.
  JpmcdsBSearchCheckOrder(TRUE); // known-good starting state

  std::vector<long> unordered{10, 50, 20, 90};
  const auto bsearch_validates = [&unordered] {
    int lo = -1;
    int hi = -1;
    return JpmcdsBSearchLong(30.0, unordered.data(), sizeof(long),
                             static_cast<int>(unordered.size()), &lo, &hi) != SUCCESS;
  };
  ASSERT_TRUE(bsearch_validates()) << "baseline: an unordered array should be rejected";

  // Trip the routine's own order check, which returns before the restore.
  std::vector<long> bad_x{100, 50, 200};
  std::vector<double> f{1.0, 2.0, 3.0};
  double out = 0.0;
  EXPECT_NE(JpmcdsLinInterpLongPoint1Discontinuous(bad_x.data(), sizeof(long), 3, f.data(),
                                                   sizeof(double), 120.0, nullptr, TRUE, &out),
            SUCCESS);

  EXPECT_FALSE(bsearch_validates())
      << "upstream no longer leaks the order-check global -- UPSTREAM-13 may be fixed, "
         "revisit the note in interpolation.hpp";
  EXPECT_FALSE(JpmcdsBSearchCheckOrder(TRUE) != FALSE)
      << "the global should have been left FALSE by the failed call";

  // The port has no such state: the same inputs are rejected every time, and
  // checked and unchecked search are different functions.
  for (int attempt = 0; attempt < 3; ++attempt) {
    EXPECT_FALSE(cdsmodel::linear_interpolate_discontinuous(bad_x, f, 120.0, cdsmodel::Limit::Right)
                     .has_value())
        << "attempt " << attempt;
    EXPECT_FALSE(cdsmodel::bracket_checked(std::span<const long>{unordered}, 30.0).has_value())
        << "attempt " << attempt << ": the port's checked search stopped validating";
  }
}

} // namespace
