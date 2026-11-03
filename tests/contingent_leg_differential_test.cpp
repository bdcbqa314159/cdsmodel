// TIER 2 -- live differential tests for the protection leg PV.
//
// This is the first module that computes a price, and the test that matters
// most in the project so far: it asks whether a C++20 port reproduces the
// original's PRICE bit-for-bit, after accumulation over a timeline.
//
// zero_curve.hpp established bit-exactness for a handful of operations in one
// expression. That claim was deliberately scoped, because accumulation is
// where a compiler is most free to reassociate or contract differently
// between the C and C++ front ends. This file is where it generalises or
// does not.
//
// Comparisons use EXPECT_EQ on raw doubles -- bit equality. Where a tolerance
// appears it is for an identity about the port, never for agreement with
// upstream, and it says so.

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "contingentleg.h"
#include "cx.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class ContingentLegDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points,
              double basis = 1.0) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(points.size()), basis, JPMCDS_ACT_365F);
}

struct Market {
  cdsmodel::Date base = d(2024, 1, 1);
  std::vector<cdsmodel::RatePoint> disc{{d(2024, 4, 1), 0.0310},
                                        {d(2025, 1, 1), 0.0350},
                                        {d(2027, 1, 1), 0.0400},
                                        {d(2031, 1, 1), 0.0440},
                                        {d(2039, 1, 1), 0.0455}};
  std::vector<cdsmodel::RatePoint> spread{{d(2024, 6, 20), 0.0090},
                                          {d(2025, 6, 20), 0.0130},
                                          {d(2029, 6, 20), 0.0185},
                                          {d(2034, 6, 20), 0.0220}};
};

TEST_F(ContingentLegDifferential, PvIsBitIdenticalAcrossManyTrades) {
  const Market m;
  TCurve *disc = build(m.base, m.disc);
  TCurve *spread = build(m.base, m.spread);
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(spread, nullptr);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(m.base, m.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(m.base, m.spread, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc_c.has_value());
  ASSERT_TRUE(spread_c.has_value());

  std::size_t compared = 0;
  for (int start_off : {0, 1, 45, 180, 400, 1200}) {
    for (int tenor : {30, 91, 365, 1826, 3653, 5479}) {
      for (double recovery : {0.0, 0.25, 0.40, 0.75, 1.0}) {
        for (bool protect_start : {true, false}) {
          for (auto pay :
               {cdsmodel::ProtectionPayment::AtDefault, cdsmodel::ProtectionPayment::AtMaturity}) {
            const cdsmodel::Date start = m.base + std::chrono::days{start_off};
            const cdsmodel::Date end = start + std::chrono::days{tenor};

            TContingentLeg cl{};
            cl.startDate = cdsmodel::to_tdate(start);
            cl.endDate = cdsmodel::to_tdate(end);
            cl.notional = 1.0e7;
            cl.payType =
                pay == cdsmodel::ProtectionPayment::AtDefault ? PROT_PAY_DEF : PROT_PAY_MAT;
            cl.protectStart = protect_start ? TRUE : FALSE;

            double expected = 0.0;
            const bool ref_ok =
                JpmcdsContingentLegPV(&cl, cdsmodel::to_tdate(m.base), cdsmodel::to_tdate(m.base),
                                      cdsmodel::to_tdate(m.base), disc, spread, recovery,
                                      &expected) == SUCCESS;

            const cdsmodel::ContingentLeg leg{start, end, 1.0e7, pay, protect_start};
            const auto actual = cdsmodel::contingent_leg_pv(leg, m.base, m.base, m.base, *disc_c,
                                                            *spread_c, recovery);

            ASSERT_EQ(actual.has_value(), ref_ok)
                << "validity, start=" << start_off << " tenor=" << tenor << " R=" << recovery;
            if (ref_ok) {
              // Bit equality on a PRICE, after accumulation over a timeline.
              EXPECT_EQ(*actual, expected)
                  << "start=" << start_off << " tenor=" << tenor << " R=" << recovery
                  << " protectStart=" << protect_start
                  << " pay=" << (pay == cdsmodel::ProtectionPayment::AtDefault ? "DEF" : "MAT")
                  << " delta=" << (*actual - expected);
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 300u) << "the sweep barely ran";
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(ContingentLegDifferential, PvIsBitIdenticalForValueAndStepinDatesAwayFromToday) {
  const Market m;
  TCurve *disc = build(m.base, m.disc);
  TCurve *spread = build(m.base, m.spread);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(m.base, m.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(m.base, m.spread, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc_c.has_value());

  // today, valueDate and stepinDate all distinct: the start-date clamping and
  // the value-date restatement are separate code paths and a sweep that sets
  // them equal exercises neither.
  for (int value_off : {0, 1, 3, 30}) {
    for (int stepin_off : {-5, 0, 1, 10}) {
      const cdsmodel::Date today = m.base + std::chrono::days{10};
      const cdsmodel::Date value = today + std::chrono::days{value_off};
      const cdsmodel::Date stepin = today + std::chrono::days{stepin_off};
      const cdsmodel::Date start = m.base + std::chrono::days{5};
      const cdsmodel::Date end = start + std::chrono::days{1826};

      TContingentLeg cl{};
      cl.startDate = cdsmodel::to_tdate(start);
      cl.endDate = cdsmodel::to_tdate(end);
      cl.notional = 1.0e7;
      cl.payType = PROT_PAY_DEF;
      cl.protectStart = TRUE;

      double expected = 0.0;
      const bool ref_ok = JpmcdsContingentLegPV(
                              &cl, cdsmodel::to_tdate(today), cdsmodel::to_tdate(value),
                              cdsmodel::to_tdate(stepin), disc, spread, 0.4, &expected) == SUCCESS;
      const cdsmodel::ContingentLeg leg{start, end, 1.0e7, cdsmodel::ProtectionPayment::AtDefault,
                                        true};
      const auto actual =
          cdsmodel::contingent_leg_pv(leg, today, value, stepin, *disc_c, *spread_c, 0.4);
      ASSERT_EQ(actual.has_value(), ref_ok)
          << "value_off=" << value_off << " stepin_off=" << stepin_off;
      if (ref_ok) {
        EXPECT_EQ(*actual, expected) << "value_off=" << value_off << " stepin_off=" << stepin_off;
      }
    }
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(ContingentLegDifferential, TheTaylorBranchIsReachedAndStillBitIdentical) {
  // The small-denominator branch only runs when |lambda + fwdRate| <= 1e-4,
  // which needs very low hazard AND very low rates. A realistic sweep never
  // touches it, so it gets its own scenario -- otherwise half the function is
  // untested and the suite says nothing about it.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> tiny_disc{{d(2025, 1, 1), 1e-9}, {d(2034, 1, 1), 1e-9}};
  const std::vector<cdsmodel::RatePoint> tiny_spread{{d(2025, 1, 1), 1e-9}, {d(2034, 1, 1), 1e-9}};
  TCurve *disc = build(base, tiny_disc);
  TCurve *spread = build(base, tiny_spread);
  ASSERT_NE(disc, nullptr);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(base, tiny_disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(base, tiny_spread, 1.0, cdsmodel::DayCount::Act365Fixed);

  for (int tenor : {91, 365, 1826, 3653}) {
    const cdsmodel::Date start = base;
    const cdsmodel::Date end = start + std::chrono::days{tenor};
    TContingentLeg cl{};
    cl.startDate = cdsmodel::to_tdate(start);
    cl.endDate = cdsmodel::to_tdate(end);
    cl.notional = 1.0e7;
    cl.payType = PROT_PAY_DEF;
    cl.protectStart = TRUE;

    double expected = 0.0;
    ASSERT_EQ(JpmcdsContingentLegPV(&cl, cdsmodel::to_tdate(base), cdsmodel::to_tdate(base),
                                    cdsmodel::to_tdate(base), disc, spread, 0.4, &expected),
              SUCCESS);
    const cdsmodel::ContingentLeg leg{start, end, 1.0e7, cdsmodel::ProtectionPayment::AtDefault,
                                      true};
    const auto actual = cdsmodel::contingent_leg_pv(leg, base, base, base, *disc_c, *spread_c, 0.4);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(*actual, expected) << "Taylor branch, tenor=" << tenor;
    // Sanity: a near-zero hazard means a near-zero protection value.
    EXPECT_LT(std::fabs(*actual), 1.0) << "tenor=" << tenor;
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(ContingentLegDifferential, Upstream8TheGuardDecisionAgreesWithUpstreamEverywhere) {
  // UPSTREAM-8 settled by measurement. Upstream's guard rejects only an
  // exactly zero survival probability; the port rejects a non-finite result.
  // If those two conditions coincide over the whole hazard range, the port's
  // honest guard is behaviourally identical to upstream's broken one.
  const cdsmodel::Date base = d(2024, 1, 1);
  const cdsmodel::Date mat = d(2034, 1, 1);

  for (double hazard : {0.001, 0.01, 0.1, 0.5, 1.0, 5.0, 20.0, 50.0, 70.0, 74.0, 80.0, 200.0}) {
    const std::vector<cdsmodel::RatePoint> flat_disc{{base + std::chrono::days{1}, 0.03},
                                                     {mat, 0.03}};
    const std::vector<cdsmodel::RatePoint> flat_spread{{base + std::chrono::days{1}, hazard},
                                                       {mat, hazard}};
    TCurve *disc = build(base, flat_disc, cdsmodel::kContinuousBasis);
    TCurve *spread = build(base, flat_spread, cdsmodel::kContinuousBasis);
    ASSERT_NE(disc, nullptr);
    ASSERT_NE(spread, nullptr);
    const auto disc_c = cdsmodel::ZeroCurve::create(base, flat_disc, cdsmodel::kContinuousBasis,
                                                    cdsmodel::DayCount::Act365Fixed);
    const auto spread_c = cdsmodel::ZeroCurve::create(base, flat_spread, cdsmodel::kContinuousBasis,
                                                      cdsmodel::DayCount::Act365Fixed);

    TContingentLeg cl{};
    cl.startDate = cdsmodel::to_tdate(base);
    cl.endDate = cdsmodel::to_tdate(mat);
    cl.notional = 1.0e7;
    cl.payType = PROT_PAY_DEF;
    cl.protectStart = TRUE;

    double expected = 0.0;
    const bool ref_ok =
        JpmcdsContingentLegPV(&cl, cdsmodel::to_tdate(base), cdsmodel::to_tdate(base),
                              cdsmodel::to_tdate(base), disc, spread, 0.4, &expected) == SUCCESS;
    const cdsmodel::ContingentLeg leg{base, mat, 1.0e7, cdsmodel::ProtectionPayment::AtDefault,
                                      true};
    const auto actual = cdsmodel::contingent_leg_pv(leg, base, base, base, *disc_c, *spread_c, 0.4);

    EXPECT_EQ(actual.has_value(), ref_ok)
        << "the port's finite-result guard and upstream's zero-price guard disagree at "
           "hazard "
        << hazard << " -- UPSTREAM-8's resolution needs revisiting";
    if (ref_ok) {
      EXPECT_EQ(*actual, expected) << "hazard=" << hazard;
    }
    JpmcdsFreeTCurve(disc);
    JpmcdsFreeTCurve(spread);
  }
}

} // namespace
