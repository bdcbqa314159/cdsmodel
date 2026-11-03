// TIER 2 -- live differential tests for the zero curve.
//
// THE QUESTION THIS FILE EXISTS TO ANSWER: can a C++20 port reproduce the
// original's floating point bit-for-bit, or only to a tolerance?
//
// Everything ported before this was integer and calendar logic, where
// agreement is exact or it is a bug. Here the answers come from multiplies, a
// divide, a log and an exp. If EXPECT_DOUBLE_EQ holds across a wide sweep,
// the project's verification standard survives into the numerical layers. If
// it does not, the standard has to weaken to a tolerance, and that is a
// methodological change worth knowing about before five thousand more lines
// of curve code depend on it.
//
// EXPECT_DOUBLE_EQ allows 4 ULPs, so the strict comparisons below use
// EXPECT_EQ on the raw doubles instead -- bit equality, nothing less.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <span>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cxzerocurve.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class ZeroCurveDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Build upstream's TCurve from the same inputs the port receives.
TCurve *reference_curve(cdsmodel::Date base, std::span<const cdsmodel::RatePoint> points,
                        double basis, long day_count) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(points.size()), basis, day_count);
}

struct Scenario {
  const char *name;
  double basis;
  cdsmodel::DayCount day_count;
  std::vector<cdsmodel::RatePoint> points;
};

std::vector<Scenario> scenarios() {
  const auto p = [](int y, int m, int day, double r) {
    return cdsmodel::RatePoint{d(y, m, day), r};
  };
  return {
      {"annual, ACT/365F, upward",
       1.0,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 7, 1, 0.0310), p(2025, 1, 1, 0.0345), p(2026, 1, 1, 0.0390), p(2029, 1, 1, 0.0425),
        p(2034, 1, 1, 0.0450)}},
      {"annual, ACT/365F, inverted",
       1.0,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 7, 1, 0.0520), p(2025, 1, 1, 0.0480), p(2027, 1, 1, 0.0410),
        p(2031, 1, 1, 0.0375)}},
      {"continuous basis, ACT/365F",
       cdsmodel::kContinuousBasis,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 7, 1, 0.0295), p(2026, 1, 1, 0.0330), p(2030, 1, 1, 0.0365)}},
      {"continuous basis, ACT/360",
       cdsmodel::kContinuousBasis,
       cdsmodel::DayCount::Act360,
       {p(2024, 7, 1, 0.0295), p(2026, 1, 1, 0.0330), p(2030, 1, 1, 0.0365)}},
      {"semi-annual basis, ACT/360",
       2.0,
       cdsmodel::DayCount::Act360,
       {p(2024, 7, 1, 0.0300), p(2025, 7, 1, 0.0340), p(2028, 1, 1, 0.0380)}},
      {"quarterly basis, ACT/365F",
       4.0,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 4, 1, 0.0250), p(2025, 4, 1, 0.0290), p(2027, 4, 1, 0.0330)}},
      {"near-zero rates",
       1.0,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 7, 1, 1e-9), p(2026, 1, 1, 1e-9), p(2030, 1, 1, 1e-9)}},
      {"single point", 1.0, cdsmodel::DayCount::Act365Fixed, {p(2027, 1, 1, 0.0400)}},
      {"negative rates",
       1.0,
       cdsmodel::DayCount::Act365Fixed,
       {p(2024, 7, 1, -0.0050), p(2026, 1, 1, -0.0020), p(2030, 1, 1, 0.0100)}},
  };
}

TEST_F(ZeroCurveDifferential, ZeroRateIsBitIdenticalAcrossEveryScenario) {
  const cdsmodel::Date base = d(2024, 1, 1);
  std::size_t compared = 0;
  for (const Scenario &s : scenarios()) {
    TCurve *ref = reference_curve(base, s.points, s.basis, cdsmodel::to_upstream_code(s.day_count));
    ASSERT_NE(ref, nullptr) << s.name;
    const auto curve = cdsmodel::ZeroCurve::create(base, s.points, s.basis, s.day_count);
    ASSERT_TRUE(curve.has_value()) << s.name;

    // Before the curve, on every knot, between knots, and well past the end.
    for (int offset = -400; offset <= 4200; offset += 7) {
      const cdsmodel::Date date = base + std::chrono::days{offset};
      const double expected = JpmcdsZeroRate(ref, cdsmodel::to_tdate(date));
      const double actual = curve->zero_rate(date);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << s.name << " at offset " << offset;
        continue;
      }
      // Bit equality, not EXPECT_DOUBLE_EQ's 4 ULPs.
      EXPECT_EQ(actual, expected) << s.name << " at offset " << offset << " (delta "
                                  << (actual - expected) << ")";
      ++compared;
    }
    JpmcdsFreeTCurve(ref);
  }
  EXPECT_GT(compared, 3000u) << "the sweep barely ran";
}

TEST_F(ZeroCurveDifferential, ZeroPriceIsBitIdenticalAcrossEveryScenario) {
  const cdsmodel::Date base = d(2024, 1, 1);
  for (const Scenario &s : scenarios()) {
    TCurve *ref = reference_curve(base, s.points, s.basis, cdsmodel::to_upstream_code(s.day_count));
    ASSERT_NE(ref, nullptr) << s.name;
    const auto curve = cdsmodel::ZeroCurve::create(base, s.points, s.basis, s.day_count);
    ASSERT_TRUE(curve.has_value());

    for (int offset = -400; offset <= 4200; offset += 11) {
      const cdsmodel::Date date = base + std::chrono::days{offset};
      const double expected = JpmcdsZeroPrice(ref, cdsmodel::to_tdate(date));
      const double actual = curve->zero_price(date);
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << s.name << " at offset " << offset;
        continue;
      }
      EXPECT_EQ(actual, expected) << s.name << " at offset " << offset << " (relative "
                                  << (actual - expected) / expected << ")";
    }
    JpmcdsFreeTCurve(ref);
  }
}

TEST_F(ZeroCurveDifferential, ForwardZeroPriceIsBitIdentical) {
  const cdsmodel::Date base = d(2024, 1, 1);
  for (const Scenario &s : scenarios()) {
    TCurve *ref = reference_curve(base, s.points, s.basis, cdsmodel::to_upstream_code(s.day_count));
    ASSERT_NE(ref, nullptr);
    const auto curve = cdsmodel::ZeroCurve::create(base, s.points, s.basis, s.day_count);
    ASSERT_TRUE(curve.has_value());

    for (int start = 0; start <= 2000; start += 181) {
      for (int span : {1, 30, 91, 182, 365, 1095}) {
        const cdsmodel::Date a = base + std::chrono::days{start};
        const cdsmodel::Date b = a + std::chrono::days{span};
        const double expected =
            JpmcdsForwardZeroPrice(ref, cdsmodel::to_tdate(a), cdsmodel::to_tdate(b));
        const double actual = curve->forward_zero_price(a, b);
        if (std::isnan(expected)) {
          continue;
        }
        EXPECT_EQ(actual, expected) << s.name << " start=" << start << " span=" << span;
      }
    }
    JpmcdsFreeTCurve(ref);
  }
}

TEST_F(ZeroCurveDifferential, TheBaseDateItselfUsesTheFollowingDay) {
  // zcInterpRate substitutes t = 1 when the requested date is the base date,
  // because the zero rate there is undefined. Worth pinning separately: it is
  // the one branch a uniform sweep can step straight over.
  //
  // Reaching it needs the base date to sit ON the first knot, so the search
  // interpolates rather than taking the flat before-the-curve branch. The
  // first scenario starts after the base date and does NOT exercise it, which
  // is exactly the kind of near-miss a sweep hides.
  const cdsmodel::Date base = d(2024, 1, 1);
  // Hold the vector: `scenarios().front()` would bind a reference into a
  // temporary that dies at the end of the expression. It did, and the symptom
  // was a NULL curve rather than a crash.
  const std::vector<Scenario> all = scenarios();
  const Scenario &s = all.front();
  TCurve *ref = reference_curve(base, s.points, s.basis, cdsmodel::to_upstream_code(s.day_count));
  ASSERT_NE(ref, nullptr);
  const auto curve = cdsmodel::ZeroCurve::create(base, s.points, s.basis, s.day_count);
  ASSERT_TRUE(curve.has_value());

  EXPECT_EQ(curve->zero_rate(base), JpmcdsZeroRate(ref, cdsmodel::to_tdate(base)));
  EXPECT_EQ(curve->zero_price(base), JpmcdsZeroPrice(ref, cdsmodel::to_tdate(base)));
  EXPECT_EQ(curve->zero_price(base), 1.0) << "a discount factor at the base date must be 1";
  JpmcdsFreeTCurve(ref);

  // Now the branch itself: base date equal to the first knot.
  const std::vector<cdsmodel::RatePoint> on_base{
      {base, 0.0310}, {d(2026, 1, 1), 0.0390}, {d(2030, 1, 1), 0.0430}};
  TCurve *ref2 = reference_curve(base, on_base, 1.0,
                                 cdsmodel::to_upstream_code(cdsmodel::DayCount::Act365Fixed));
  ASSERT_NE(ref2, nullptr);
  const auto curve2 =
      cdsmodel::ZeroCurve::create(base, on_base, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve2.has_value());
  EXPECT_EQ(curve2->zero_rate(base), JpmcdsZeroRate(ref2, cdsmodel::to_tdate(base)));
  EXPECT_EQ(curve2->zero_price(base), JpmcdsZeroPrice(ref2, cdsmodel::to_tdate(base)));
  JpmcdsFreeTCurve(ref2);
}

} // namespace
