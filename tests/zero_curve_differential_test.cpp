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

// =====================================================================
// Added with the tcurve.c slice. Every test above builds a VALID curve on
// both sides and compares prices, so the constructor's rejection behaviour
// was never in the comparison -- the same blind spot that hid PORT-9 in the
// protection leg and PORT-11 here.
// =====================================================================

TEST_F(ZeroCurveDifferential, ConstructionAcceptsAndRejectsTheSameRates) {
  // PORT-11: the port had no rate check, so a rate at or below -basis was
  // accepted and every discount factor afterwards was inf or NaN. Upstream's
  // checkTCurve calls JpmcdsRateValid on every point.
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  const cdsmodel::Date mat = *cdsmodel::make_date(2029, 1, 1);
  int compared = 0;
  for (double basis : {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (double rate : {0.05, 0.0, -0.5, -0.999, -1.0, -1.5, -2.0, -2.5, -3.0, -13.0, -400.0}) {
      TDate dates[1] = {cdsmodel::to_tdate(mat)};
      double rates[1] = {rate};
      TCurve *ref =
          JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates, rates, 1, basis, JPMCDS_ACT_365F);
      const std::vector<cdsmodel::RatePoint> pts{{mat, rate}};
      const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
          cdsmodel::ZeroCurve::create(base, pts, basis, cdsmodel::DayCount::Act365Fixed);
      EXPECT_EQ(got.has_value(), ref != nullptr) << "basis " << basis << " rate " << rate;
      if (ref != nullptr) {
        JpmcdsFreeTCurve(ref);
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 60) << compared;
}

TEST_F(ZeroCurveDifferential, Upstream15_AValidatingConstructorAdmitsUnpriceableCurves) {
  // checkTCurve's stated purpose is "to guarantee we can generate positive
  // discount factors". It accepts 30/360 -- a standard convention -- and
  // empty curves. Neither can be priced, and JpmcdsZeroPrice returns a bare
  // double, so the failure surfaces as NaN.
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  const cdsmodel::Date mat = *cdsmodel::make_date(2029, 1, 1);
  TDate dates[1] = {cdsmodel::to_tdate(mat)};
  double rates[1] = {0.05};

  for (long dcc : {JPMCDS_B30_360, JPMCDS_B30E_360}) {
    TCurve *ref = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates, rates, 1, 1.0, dcc);
    ASSERT_NE(ref, nullptr) << "upstream now rejects day count " << dcc
                            << " at construction -- UPSTREAM-15 may be fixed";
    EXPECT_TRUE(std::isnan(JpmcdsZeroPrice(ref, cdsmodel::to_tdate(mat))))
        << "day count " << dcc << " no longer prices to NaN -- revisit UPSTREAM-15";
    JpmcdsFreeTCurve(ref);

    // The port refuses the curve instead, so there is nothing to misprice.
    const cdsmodel::Expected<cdsmodel::DayCount> ported = cdsmodel::from_upstream_code(dcc);
    ASSERT_TRUE(ported.has_value());
    const std::vector<cdsmodel::RatePoint> pts{{mat, 0.05}};
    EXPECT_FALSE(cdsmodel::ZeroCurve::create(base, pts, 1.0, *ported).has_value())
        << "day count " << dcc;
  }

  // An empty curve: accepted upstream, prices to NaN, refused by the port.
  TCurve *empty =
      JpmcdsMakeTCurve(cdsmodel::to_tdate(base), nullptr, nullptr, 0, 1.0, JPMCDS_ACT_365F);
  ASSERT_NE(empty, nullptr) << "upstream now rejects an empty curve";
  EXPECT_EQ(empty->fNumItems, 0);
  EXPECT_TRUE(std::isnan(JpmcdsZeroPrice(empty, cdsmodel::to_tdate(mat))));
  JpmcdsFreeTCurve(empty);
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, {}, 1.0, cdsmodel::DayCount::Act365Fixed).has_value());
}

TEST_F(ZeroCurveDifferential, TheZeroDateTerminatorIsNotReproduced) {
  // JpmcdsMakeTCurveNoRateCheck treats a TDate of 0 as an array terminator:
  // "This allows spreadsheet users to select a big range of dates, but use
  // less than the whole range by terminating with a 1 or 0."
  //
  // Measured: five dates with a zero at index 2 give a curve of TWO points.
  // The port takes a span, so the length is explicit and a terminator has
  // nothing to mean -- and date 0 is a legitimate date (1601-01-01) that a
  // span-based caller could reasonably include.
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  TDate dates[5] = {cdsmodel::to_tdate(*cdsmodel::make_date(2025, 1, 1)),
                    cdsmodel::to_tdate(*cdsmodel::make_date(2026, 1, 1)), 0,
                    cdsmodel::to_tdate(*cdsmodel::make_date(2027, 1, 1)),
                    cdsmodel::to_tdate(*cdsmodel::make_date(2028, 1, 1))};
  double rates[5] = {0.01, 0.02, 0.03, 0.04, 0.05};
  TCurve *ref = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates, rates, 5, 1.0, JPMCDS_ACT_365F);
  ASSERT_NE(ref, nullptr);
  EXPECT_EQ(ref->fNumItems, 2) << "the zero-date terminator no longer truncates";
  JpmcdsFreeTCurve(ref);

  // The port: the same five points are not increasing, so the curve is
  // refused outright rather than silently truncated.
  std::vector<cdsmodel::RatePoint> pts;
  for (int i = 0; i < 5; ++i) {
    pts.push_back({cdsmodel::from_tdate(dates[i]), rates[static_cast<std::size_t>(i)]});
  }
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(base, pts, 1.0, cdsmodel::DayCount::Act365Fixed).has_value())
      << "a span containing an out-of-order date should be rejected, not truncated";
}

} // namespace
