// TIER 2 -- live differential tests for the rates a curve implies.
//
// `forward_from_curve` is three operations and its interest is entirely at the
// extremes, where the two discount factors underflow or overflow together.
// `par_coupon_rate` builds a schedule, so the sweep is over the things that
// change one: interval, stub position, bad day convention.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/curve_rates.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/stub.hpp>
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
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
#include "zr2coup.h"
#include "zr2fwd.h"
}

namespace {

class CurveRatesDifferential : public ::testing::Test {
protected:
  // UPSTREAM-28: JpmcdsInterpRateExact leaks the order-check global on its
  // extrapolation paths, and everything here routes through it.
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

std::vector<cdsmodel::RatePoint> rising(int count = 8) {
  std::vector<cdsmodel::RatePoint> points;
  for (int i = 1; i <= count; ++i) {
    points.push_back(cdsmodel::RatePoint{*cdsmodel::make_date(2024 + i, 1, 1),
                                         0.02 + 0.003 * static_cast<double>(i)});
  }
  return points;
}

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

TEST_F(CurveRatesDifferential, ForwardRatesMatchAcrossBasesQuoteTermsAndDirections) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  int compared = 0;
  for (const double basis : {1.0, 2.0, 12.0, 365.0, cdsmodel::kContinuousBasis}) {
    for (const cdsmodel::DayCount curve_dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      Pair pair(base, points, basis, curve_dcc);
      ASSERT_TRUE(pair.ref) << basis;
      ASSERT_TRUE(pair.port.has_value()) << basis;
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        for (const long quote_basis : {0L, 1L, 2L, 4L}) {
          for (const cdsmodel::DayCount quote_dcc :
               {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
            // Forwards both ways round: upstream's comment says a backward
            // forward is a legitimate request during curve construction.
            const std::pair<cdsmodel::Date, cdsmodel::Date> spans[] = {
                {d(2025, 1, 1), d(2026, 1, 1)},
                {d(2026, 7, 1), d(2027, 3, 15)},
                {d(2027, 1, 1), d(2025, 1, 1)},
                {d(2028, 1, 1), d(2032, 1, 1)}};
            for (const auto &[from, to] : spans) {
              JpmcdsBSearchCheckOrder(TRUE);
              double want = 0.0;
              const int rc = JpmcdsForwardFromZCurve(
                  pair.ref.curve, upstream_interp(interp), cdsmodel::to_tdate(from),
                  cdsmodel::to_tdate(to), cdsmodel::to_upstream_code(quote_dcc), quote_basis,
                  &want);
              const std::optional<double> got = cdsmodel::forward_from_curve(
                  *pair.port, interp, from, to, quote_dcc, quote_basis);
              JpmcdsBSearchCheckOrder(TRUE);
              const std::string where =
                  "basis " + std::to_string(basis) + " quote " + std::to_string(quote_basis) + " " +
                  std::to_string(from <= to) + " " + std::to_string(cdsmodel::to_tdate(from));
              ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
              if (got) {
                // Two discount factors, a division and a conversion: the
                // accumulating tier, because the conversion's conditioning
                // depends on the basis -- see curve_discount's note.
                EXPECT_NEAR(*got, want, 1.0e-12 * std::max(1.0, std::fabs(want))) << where;
              }
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 500) << compared;
}

TEST_F(CurveRatesDifferential, Upstream19_SecondSite_AnUnguardedDivisionYieldsNaNAsSuccess) {
  // JpmcdsForwardFromZCurve divides the two discount factors with no guard --
  // strictly less protection than JpmcdsZCFwdDiscountFactor, which at least
  // tested `if (discount1 <= 0.0)`. Both ends of the range reach
  // JpmcdsDiscountToRate, whose guard cannot see NaN (UPSTREAM-19).
  const cdsmodel::Date base = d(2024, 1, 1);
  const cdsmodel::Date start = d(2054, 1, 1);
  const cdsmodel::Date end = d(2055, 1, 1);
  int nan_as_success = 0;
  for (const double rate : {25.0, 30.0, -24.0, -30.0}) {
    const std::vector<cdsmodel::RatePoint> points{{start, rate}, {end, rate}};
    Pair pair(base, points, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref) << rate;
    ASSERT_TRUE(pair.port.has_value()) << rate;

    JpmcdsBSearchCheckOrder(TRUE);
    double want = 0.0;
    const int rc = JpmcdsForwardFromZCurve(pair.ref.curve, JPMCDS_FLAT_FORWARDS,
                                           cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
                                           JPMCDS_ACT_365F, JPMCDS_SIMPLE_BASIS, &want);
    JpmcdsBSearchCheckOrder(TRUE);
    EXPECT_EQ(rc, SUCCESS) << rate << ": upstream reports success";
    EXPECT_TRUE(std::isnan(want)) << rate << ": with a NaN rate";
    if (rc == SUCCESS && std::isnan(want)) {
      ++nan_as_success;
    }

    // The port refuses, because discount_to_rate rejects a non-finite
    // discount factor -- UPSTREAM-19's fix, reused here with no new guard.
    EXPECT_FALSE(cdsmodel::forward_from_curve(*pair.port, cdsmodel::InterpType::FlatForwards, start,
                                              end, cdsmodel::DayCount::Act365Fixed, 0)
                     .has_value())
        << rate;
  }
  EXPECT_EQ(nan_as_success, 4) << "all four extremes must reach the conversion";

  // And the ordinary case still works on both sides, so the refusal is
  // confined to the inputs that have no answer.
  const std::vector<cdsmodel::RatePoint> sane{{start, 0.03}, {end, 0.03}};
  Pair ok(base, sane, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(ok.ref);
  ASSERT_TRUE(ok.port.has_value());
  JpmcdsBSearchCheckOrder(TRUE);
  double want = 0.0;
  ASSERT_EQ(JpmcdsForwardFromZCurve(ok.ref.curve, JPMCDS_FLAT_FORWARDS, cdsmodel::to_tdate(start),
                                    cdsmodel::to_tdate(end), JPMCDS_ACT_365F, JPMCDS_SIMPLE_BASIS,
                                    &want),
            SUCCESS);
  JpmcdsBSearchCheckOrder(TRUE);
  const std::optional<double> got = cdsmodel::forward_from_curve(
      *ok.port, cdsmodel::InterpType::FlatForwards, start, end, cdsmodel::DayCount::Act365Fixed, 0);
  ASSERT_TRUE(got.has_value());
  EXPECT_NEAR(*got, want, 1.0e-12 * std::max(1.0, std::fabs(want)));
}

TEST_F(CurveRatesDifferential, ParCouponRatesMatchAcrossIntervalsStubsAndConventions) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::no_weekends()};
  int compared = 0;
  for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
    Pair pair(base, points, basis, cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(pair.ref) << basis;
    ASSERT_TRUE(pair.port.has_value()) << basis;
    for (const cdsmodel::InterpType interp :
         {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
      for (const int periods : {3, 6, 12}) {
        TDateInterval raw{};
        ASSERT_EQ(JpmcdsMakeDateInterval(periods, 'M', &raw), SUCCESS);
        const cdsmodel::DateInterval iv{periods, cdsmodel::PeriodUnit::Months};
        for (const cdsmodel::StubPosition stub :
             {cdsmodel::StubPosition::Front, cdsmodel::StubPosition::Back}) {
          for (const cdsmodel::DayCount dcc :
               {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
            for (const auto &[start, maturity] : {std::pair{d(2024, 1, 1), d(2029, 1, 1)},
                                                  std::pair{d(2024, 2, 20), d(2030, 5, 10)},
                                                  std::pair{d(2025, 1, 1), d(2028, 1, 1)}}) {
              JpmcdsBSearchCheckOrder(TRUE);
              double want = 0.0;
              const int rc = JpmcdsZerosToCouponsPoint(
                  pair.ref.curve, upstream_interp(interp), cdsmodel::to_tdate(start), &raw,
                  cdsmodel::to_tdate(maturity), cdsmodel::to_upstream_code(dcc),
                  stub == cdsmodel::StubPosition::Back ? TRUE : FALSE, &want);
              const std::optional<double> got = cdsmodel::par_coupon_rate(
                  *pair.port, interp, start, iv, maturity, dcc, stub, cdsmodel::BadDayConv::None,
                  cdsmodel::BadDayConv::None, calendars);
              JpmcdsBSearchCheckOrder(TRUE);
              const std::string where = "basis " + std::to_string(basis) + " iv " +
                                        std::to_string(periods) + " stub " +
                                        std::to_string(static_cast<int>(stub)) + " dcc " +
                                        std::to_string(cdsmodel::to_upstream_code(dcc)) +
                                        " start " + std::to_string(cdsmodel::to_tdate(start));
              ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
              if (got) {
                // A quotient of two accumulated present values, so the
                // accumulating tier.
                EXPECT_NEAR(*got, want, 1.0e-12 * std::max(1.0, std::fabs(want))) << where;
              }
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 200) << compared;
}

TEST_F(CurveRatesDifferential, ParCouponRatesMatchUnderBadDayAdjustment) {
  // JpmcdsZerosToCouponsPointAdj, the form that takes conventions. "NONE" is
  // the holiday file both sides agree on without one on disk.
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  Pair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());
  TDateInterval raw{};
  ASSERT_EQ(JpmcdsMakeDateInterval(6, 'M', &raw), SUCCESS);
  const cdsmodel::DateInterval iv{6, cdsmodel::PeriodUnit::Months};

  int compared = 0;
  for (const cdsmodel::BadDayConv accrual :
       {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified}) {
    for (const cdsmodel::BadDayConv pay :
         {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
      JpmcdsBSearchCheckOrder(TRUE);
      double want = 0.0;
      const int rc = JpmcdsZerosToCouponsPointAdj(
          pair.ref.curve, JPMCDS_FLAT_FORWARDS, cdsmodel::to_tdate(d(2024, 1, 6)), &raw,
          cdsmodel::to_tdate(d(2029, 1, 6)), JPMCDS_ACT_365F, FALSE,
          cdsmodel::to_upstream_code(accrual), cdsmodel::to_upstream_code(pay),
          const_cast<char *>("NONE"), &want);
      const std::optional<double> got = cdsmodel::par_coupon_rate(
          *pair.port, cdsmodel::InterpType::FlatForwards, d(2024, 1, 6), iv, d(2029, 1, 6),
          cdsmodel::DayCount::Act365Fixed, cdsmodel::StubPosition::Front, accrual, pay, calendars);
      JpmcdsBSearchCheckOrder(TRUE);
      const std::string where =
          std::string{static_cast<char>(accrual)} + "/" + std::string{static_cast<char>(pay)};
      ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
      if (got) {
        EXPECT_NEAR(*got, want, 1.0e-12 * std::max(1.0, std::fabs(want))) << where;
      }
      ++compared;
    }
  }
  EXPECT_EQ(compared, 8);
}

TEST_F(CurveRatesDifferential, AStartDateBeforeTheCurveIsRefusedByBoth) {
  // Upstream compares against JpmcdsCurveFirstDate, which is
  // min(baseDate, points[0]) -- so a curve whose first point precedes its base
  // date admits a start date before the base date too. Both halves checked.
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 1, 1), 0.03}, {d(2030, 1, 1), 0.05}};
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::no_weekends()};
  TDateInterval raw{};
  ASSERT_EQ(JpmcdsMakeDateInterval(6, 'M', &raw), SUCCESS);
  const cdsmodel::DateInterval iv{6, cdsmodel::PeriodUnit::Months};

  // base AFTER the first point: first_date is the point, so 2024-06-01 is fine.
  Pair straddling(d(2025, 6, 1), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(straddling.ref);
  ASSERT_TRUE(straddling.port.has_value());
  TDate first = 0;
  ASSERT_EQ(JpmcdsCurveFirstDate(straddling.ref.curve, &first), SUCCESS);
  EXPECT_EQ(first, cdsmodel::to_tdate(d(2024, 1, 1))) << "the point, not the base date";

  for (const cdsmodel::Date start : {d(2023, 1, 1), d(2024, 6, 1), d(2026, 1, 1)}) {
    JpmcdsBSearchCheckOrder(TRUE);
    double want = 0.0;
    const int rc = JpmcdsZerosToCouponsPoint(
        straddling.ref.curve, JPMCDS_FLAT_FORWARDS, cdsmodel::to_tdate(start), &raw,
        cdsmodel::to_tdate(d(2029, 1, 1)), JPMCDS_ACT_365F, FALSE, &want);
    const std::optional<double> got = cdsmodel::par_coupon_rate(
        *straddling.port, cdsmodel::InterpType::FlatForwards, start, iv, d(2029, 1, 1),
        cdsmodel::DayCount::Act365Fixed, cdsmodel::StubPosition::Front, cdsmodel::BadDayConv::None,
        cdsmodel::BadDayConv::None, calendars);
    JpmcdsBSearchCheckOrder(TRUE);
    ASSERT_EQ(got.has_value(), rc == SUCCESS) << "start " << cdsmodel::to_tdate(start);
    if (got) {
      EXPECT_NEAR(*got, want, 1.0e-12 * std::max(1.0, std::fabs(want)))
          << "start " << cdsmodel::to_tdate(start);
    }
  }
}

TEST_F(CurveRatesDifferential, AnInvertedSpanIsRefusedByBoth) {
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> points = rising();
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::no_weekends()};
  Pair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());
  TDateInterval raw{};
  ASSERT_EQ(JpmcdsMakeDateInterval(6, 'M', &raw), SUCCESS);
  const cdsmodel::DateInterval iv{6, cdsmodel::PeriodUnit::Months};

  for (const auto &[start, maturity] :
       {std::pair{d(2029, 1, 1), d(2025, 1, 1)}, std::pair{d(2026, 1, 1), d(2026, 1, 1)}}) {
    JpmcdsBSearchCheckOrder(TRUE);
    double want = 0.0;
    const int rc = JpmcdsZerosToCouponsPoint(
        pair.ref.curve, JPMCDS_FLAT_FORWARDS, cdsmodel::to_tdate(start), &raw,
        cdsmodel::to_tdate(maturity), JPMCDS_ACT_365F, FALSE, &want);
    const std::optional<double> got = cdsmodel::par_coupon_rate(
        *pair.port, cdsmodel::InterpType::FlatForwards, start, iv, maturity,
        cdsmodel::DayCount::Act365Fixed, cdsmodel::StubPosition::Front, cdsmodel::BadDayConv::None,
        cdsmodel::BadDayConv::None, calendars);
    JpmcdsBSearchCheckOrder(TRUE);
    EXPECT_EQ(rc, FAILURE) << cdsmodel::to_tdate(start);
    EXPECT_FALSE(got.has_value()) << cdsmodel::to_tdate(start);
  }
}

} // namespace
