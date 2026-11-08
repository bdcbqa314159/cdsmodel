// TIER 2 -- live differential tests for discounting off a finished curve.
//
// Three small functions, so the interest is in what they agree with rather
// than in their own arithmetic: `discount_date` and `ZeroCurve::zero_price`
// are two routes to one quantity, and `cash_flow_pv` is the sum of the first.

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/curve_discount.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "bsearch.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cxzerocurve.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class CurveDiscountDifferential : public ::testing::Test {
protected:
  // JpmcdsInterpRateExact leaks the order-check global on its extrapolation
  // paths (UPSTREAM-28), and discount_date's upstream counterpart calls it.
  void SetUp() override {
    JpmcdsErrMsgOff();
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

std::vector<cdsmodel::RatePoint> rising(int count = 5) {
  std::vector<cdsmodel::RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(cdsmodel::RatePoint{*cdsmodel::make_date(2024 + i, 1, 1),
                                         0.02 + 0.004 * static_cast<double>(i)});
  }
  return points;
}

struct Pair {
  Ref ref;
  cdsmodel::Expected<cdsmodel::ZeroCurve> port;

  Pair(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points, double basis,
       cdsmodel::DayCount dcc)
      : ref(nullptr), port(cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not built")) {
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

TEST_F(CurveDiscountDifferential, DiscountDateMatchesAcrossBasesDayCountsAndInterpTypes) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  int compared = 0;
  for (const double basis : {1.0, 2.0, 4.0, 12.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(base, points, basis, dcc);
      ASSERT_TRUE(pair.ref) << basis;
      ASSERT_TRUE(pair.port.has_value()) << basis;
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        for (const cdsmodel::Date at : {d(2020, 1, 1), d(2024, 1, 1), d(2025, 1, 1), d(2026, 7, 4),
                                        d(2029, 1, 1), d(2035, 1, 1)}) {
          JpmcdsBSearchCheckOrder(TRUE);
          double want = 0.0;
          const int rc = JpmcdsDiscountDate(cdsmodel::to_tdate(at), pair.ref.curve,
                                            upstream_interp(interp), &want);
          const cdsmodel::Expected<double> got = cdsmodel::discount_date(*pair.port, at, interp);
          JpmcdsBSearchCheckOrder(TRUE);
          const std::string where = "basis " + std::to_string(basis) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(dcc)) + " interp " +
                                    std::to_string(upstream_interp(interp)) + " at " +
                                    std::to_string(cdsmodel::to_tdate(at));
          ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
          if (got) {
            // One interpolation plus one conversion: non-accumulating.
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
  EXPECT_GT(compared, 120) << compared;
}

TEST_F(CurveDiscountDifferential, DiscountDateAndZeroPriceAgreeOnBothSides) {
  // Two routes to one discount factor, which is the UPSTREAM-17 shape without
  // the defect -- so the agreement is asserted rather than assumed, on
  // upstream AND on the port.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  int compared = 0;
  // 52 and 365 are here because the first version of this list went
  // 1, 2, 12, continuous -- and missed that the two routes drift apart as the
  // compounding frequency rises, peaking at 365 and recovering at continuous.
  // A basis list that samples only the ends hides a middle.
  for (const double basis : {1.0, 2.0, 12.0, 52.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(base, points, basis, dcc);
      ASSERT_TRUE(pair.ref);
      ASSERT_TRUE(pair.port.has_value());
      for (const cdsmodel::Date at : {d(2025, 1, 1), d(2026, 7, 1), d(2027, 1, 1), d(2029, 1, 1)}) {
        JpmcdsBSearchCheckOrder(TRUE);
        const double upstream_zero_price = JpmcdsZeroPrice(pair.ref.curve, cdsmodel::to_tdate(at));
        double upstream_discount_date = 0.0;
        ASSERT_EQ(JpmcdsDiscountDate(cdsmodel::to_tdate(at), pair.ref.curve, JPMCDS_FLAT_FORWARDS,
                                     &upstream_discount_date),
                  SUCCESS);
        JpmcdsBSearchCheckOrder(TRUE);
        const std::string where =
            "basis " + std::to_string(basis) + " at " + std::to_string(cdsmodel::to_tdate(at));
        // Upstream's own two routes agree only to the accumulating tier, and
        // how closely depends on the compounding frequency: 2.4e-16 at basis
        // 1, 8.3e-14 at basis 365, back to 1.2e-16 at continuous. The
        // conversion through cc = basis * log(1 + rate/basis) is what degrades.
        EXPECT_NEAR(upstream_discount_date, upstream_zero_price,
                    1.0e-12 * std::max(1.0, std::fabs(upstream_zero_price)))
            << where << ": upstream's two routes diverged";

        const double port_zero_price = pair.port->zero_price(at);
        const cdsmodel::Expected<double> port_discount_date =
            cdsmodel::discount_date(*pair.port, at, cdsmodel::InterpType::FlatForwards);
        ASSERT_TRUE(port_discount_date.has_value()) << where;
        EXPECT_NEAR(*port_discount_date, port_zero_price,
                    1.0e-12 * std::max(1.0, std::fabs(port_zero_price)))
            << where << ": the port's two routes diverged";
        // And the port must drift by the same amount upstream does -- that is
        // the claim worth making, since both inherit it from the same algebra.
        EXPECT_NEAR(*port_discount_date - port_zero_price,
                    upstream_discount_date - upstream_zero_price,
                    1.0e-15 * std::max(1.0, std::fabs(port_zero_price)))
            << where << ": the port's drift differs from upstream's";
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 25) << compared;
}

TEST_F(CurveDiscountDifferential, CashFlowPvMatchesIncludingTheSummationOrder) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  // Amounts that differ hugely in magnitude, so the accumulation order is
  // observable rather than incidental.
  const std::vector<cdsmodel::CashFlow> flows{{d(2025, 1, 1), 1.0e12},
                                              {d(2025, 7, 1), 0.05},
                                              {d(2026, 1, 1), 1.0e-6},
                                              {d(2027, 1, 1), 1.0e12},
                                              {d(2029, 1, 1), 1.05}};
  std::vector<TDate> raw_dates;
  std::vector<double> raw_amounts;
  for (const cdsmodel::CashFlow &f : flows) {
    raw_dates.push_back(cdsmodel::to_tdate(f.date));
    raw_amounts.push_back(f.amount);
  }
  TCashFlowList *cfl =
      JpmcdsNewCFL(raw_dates.data(), raw_amounts.data(), static_cast<int>(flows.size()));
  ASSERT_NE(cfl, nullptr);

  int compared = 0;
  for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(base, points, basis, dcc);
      ASSERT_TRUE(pair.ref);
      ASSERT_TRUE(pair.port.has_value());
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        JpmcdsBSearchCheckOrder(TRUE);
        double want = 0.0;
        const int rc = JpmcdsCashFlowPV(cfl, pair.ref.curve, upstream_interp(interp), &want);
        const cdsmodel::Expected<double> got = cdsmodel::cash_flow_pv(*pair.port, flows, interp);
        JpmcdsBSearchCheckOrder(TRUE);
        const std::string where =
            "basis " + std::to_string(basis) + " interp " + std::to_string(upstream_interp(interp));
        ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
        if (got) {
          // A left-to-right sum of five products with a 1e18 magnitude spread:
          // bit-identical only if the order matches, which is the point.
          EXPECT_EQ(*got, want) << where;
        }
        ++compared;
      }
    }
  }
  JpmcdsFreeCFL(cfl);
  EXPECT_GT(compared, 10) << compared;
}

TEST_F(CurveDiscountDifferential, AnEmptyRunOfFlowsIsWorthNothingOnBothSides) {
  Pair pair(d(2024, 1, 1), rising(), 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());
  TCashFlowList *empty = JpmcdsNewEmptyCFL(0);
  ASSERT_NE(empty, nullptr);
  double want = -1.0;
  EXPECT_EQ(JpmcdsCashFlowPV(empty, pair.ref.curve, JPMCDS_LINEAR_INTERP, &want), SUCCESS);
  EXPECT_EQ(want, 0.0);
  JpmcdsFreeCFL(empty);

  const cdsmodel::Expected<double> got =
      cdsmodel::cash_flow_pv(*pair.port, {}, cdsmodel::InterpType::Linear);
  ASSERT_TRUE(got.has_value()) << "no flows is an answer, not an error";
  EXPECT_EQ(*got, 0.0);
}

TEST_F(CurveDiscountDifferential, TheFirstDateIsTheEarlierOfTheBaseDateAndTheFirstPoint) {
  // Upstream takes MIN(baseDate, fArray[0].fDate), so a curve whose first
  // point precedes its base date reports the point.
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 1, 1), 0.03}, {d(2027, 1, 1), 0.05}};
  for (const cdsmodel::Date base : {d(2023, 1, 1), d(2024, 1, 1), d(2025, 6, 1)}) {
    Pair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref) << cdsmodel::to_tdate(base);
    ASSERT_TRUE(pair.port.has_value()) << cdsmodel::to_tdate(base);
    TDate want = 0;
    ASSERT_EQ(JpmcdsCurveFirstDate(pair.ref.curve, &want), SUCCESS);
    EXPECT_EQ(cdsmodel::to_tdate(pair.port->first_date()), want)
        << "base " << cdsmodel::to_tdate(base);
    // And the meaning: it is the minimum, not the base date.
    EXPECT_EQ(pair.port->first_date(), std::min(base, points.front().date));
  }
  // Upstream rejects NULL inputs; the port takes a reference, so there is
  // nothing to reject.
  TDate out = 0;
  EXPECT_EQ(JpmcdsCurveFirstDate(nullptr, &out), FAILURE);
  Pair pair(d(2024, 1, 1), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  EXPECT_EQ(JpmcdsCurveFirstDate(pair.ref.curve, nullptr), FAILURE);
}

TEST_F(CurveDiscountDifferential, TheCurveDatesMatchPointForPoint) {
  const std::vector<cdsmodel::RatePoint> points = rising(6);
  Pair pair(d(2024, 1, 1), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());
  TDate *upstream_dates = JpmcdsDatesFromCurve(pair.ref.curve);
  ASSERT_NE(upstream_dates, nullptr);
  const std::vector<cdsmodel::Date> got = pair.port->dates();
  ASSERT_EQ(got.size(), static_cast<std::size_t>(pair.ref.curve->fNumItems));
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_EQ(cdsmodel::to_tdate(got[i]), upstream_dates[i]) << i;
  }
  free(upstream_dates);

  // Upstream returns NULL for an empty curve -- "no dates in curve is not an
  // error" -- which is the same value it returns for a failed allocation. The
  // port cannot build an empty curve, so neither case exists.
  TCurve *empty = JpmcdsMakeTCurve(cdsmodel::to_tdate(d(2024, 1, 1)), nullptr, nullptr, 0, 1.0,
                                   JPMCDS_ACT_365F);
  if (empty != nullptr) {
    EXPECT_EQ(JpmcdsDatesFromCurve(empty), nullptr)
        << "an empty curve and a failed allocation are the same answer upstream";
    JpmcdsFreeTCurve(empty);
  }
  EXPECT_FALSE(cdsmodel::ZeroCurve::create(d(2024, 1, 1), {}, 1.0, cdsmodel::DayCount::Act365Fixed)
                   .has_value())
      << "the port refuses the curve, so the ambiguity cannot arise";
}

} // namespace
