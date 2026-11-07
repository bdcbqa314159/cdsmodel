// TIER 2 -- live differential tests for the swap bootstrap.
//
// The solved rate is a ROOT, so it is bounded by the solver's stopping
// criterion and not by floating-point rounding: upstream stops at
// |f| <= 1e-11 with no x criterion (X_TOLERANCE is DBL_MAX), after at most 50
// iterations. Two implementations taking different step sequences can land on
// different points of the same flat region. So rates compare at the SOLVER
// tier, and the honest check is on f -- both answers must price the benchmark
// to par -- which the par-reprice test below asserts directly.
//
// Three tests assert divergences: UPSTREAM-33 (the misdirected solve) and the
// unreachability of UPSTREAM-29 and UPSTREAM-32 from this entry point.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
#include "zcprvt.h"
}

namespace {

class SwapBootstrapDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// The rate a solve lands on is bounded by the stopping criterion, not by
/// rounding. 1e-7 on a rate is far inside anything a curve consumer can see
/// and far outside what a ULP comparison would allow.
constexpr double kSolvedRateTolerance = 1.0e-7;

struct RefZC {
  ZCurve *zc = nullptr;
  explicit RefZC(TDate vd, int n, long basis, long dcc) : zc(JpmcdsZCMake(vd, n, basis, dcc)) {}
  ~RefZC() { JpmcdsZCFree(zc); }
  RefZC(const RefZC &) = delete;
  RefZC &operator=(const RefZC &) = delete;
  explicit operator bool() const { return zc != nullptr; }
};

struct RefTC {
  TCurve *tc = nullptr;
  ~RefTC() { JpmcdsFreeTCurve(tc); }
  RefTC() = default;
  RefTC(const RefTC &) = delete;
  RefTC &operator=(const RefTC &) = delete;
};

long upstream_interp(cdsmodel::InterpType t) {
  return t == cdsmodel::InterpType::Linear ? JPMCDS_LINEAR_INTERP : JPMCDS_FLAT_FORWARDS;
}

/// Upstream packs a bad-day convention and a stub position into one long.
TBadDayAndStubPos packed(cdsmodel::BadDayConv conv) {
  return static_cast<TBadDayAndStubPos>(cdsmodel::to_upstream_code(conv));
}

void expect_curves_match(const ZCurve *ref, const cdsmodel::ZeroCurveBuilder &got,
                         const char *what) {
  const std::span<const cdsmodel::ZeroCurveBuilder::Point> points = got.points();
  ASSERT_EQ(ref->numItems, static_cast<int>(points.size())) << what;
  for (int i = 0; i < ref->numItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->date[i], cdsmodel::to_tdate(points[k].date)) << what << " date " << i;
    EXPECT_NEAR(ref->rate[i], points[k].rate, kSolvedRateTolerance) << what << " rate " << i;
  }
}

/// Seed both sides with the same money-market style stub curve.
void seed(ZCurve *zc, cdsmodel::ZeroCurveBuilder &builder) {
  const std::pair<cdsmodel::Date, double> stub[]{
      {d(2024, 4, 15), 0.0230}, {d(2024, 7, 15), 0.0245}, {d(2025, 1, 15), 0.0260}};
  for (const auto &[date, rate] : stub) {
    ASSERT_EQ(JpmcdsZCAddRate(zc, cdsmodel::to_tdate(date), rate), SUCCESS);
    ASSERT_TRUE(builder.add_rate(date, rate));
  }
}

// ------------------------------------------- the par-float path (no discZC)

TEST_F(SwapBootstrapDifferential, AddSwapsMatchesOnTheParFloatPath) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  int compared = 0;
  for (const long freq : {1L, 2L, 4L}) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        for (const cdsmodel::BadDayConv conv :
             {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
          RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
          ASSERT_TRUE(ref);
          std::optional<cdsmodel::ZeroCurveBuilder> got =
              cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
          ASSERT_TRUE(got.has_value());
          seed(ref.zc, *got);

          const std::vector<cdsmodel::Date> maturities{d(2026, 1, 15), d(2027, 1, 15),
                                                       d(2029, 1, 15), d(2031, 1, 15)};
          const std::vector<double> rates{0.0280, 0.0295, 0.0320, 0.0340};
          std::vector<TDate> raw_dates;
          for (const cdsmodel::Date date : maturities) {
            raw_dates.push_back(cdsmodel::to_tdate(date));
          }

          TInterpData idata;
          std::memset(&idata, 0, sizeof idata);
          const int rc = JpmcdsZCAddSwaps(
              ref.zc, nullptr, raw_dates.data(), const_cast<double *>(rates.data()),
              static_cast<int>(rates.size()), static_cast<int>(freq), static_cast<int>(freq),
              cdsmodel::to_upstream_code(dcc), cdsmodel::to_upstream_code(dcc),
              upstream_interp(interp), &idata, nullptr, packed(conv), const_cast<char *>("NONE"));

          const cdsmodel::SwapConventions conventions{.fixed_frequency = freq,
                                                      .float_frequency = freq,
                                                      .fixed_day_count = dcc,
                                                      .float_day_count = dcc,
                                                      .interp = interp,
                                                      .bad_day = conv};
          const bool ok = cdsmodel::add_swaps(*got, std::nullopt, maturities, rates, conventions,
                                              std::nullopt, cdsmodel::Calendar::none());

          ASSERT_EQ(rc == SUCCESS, ok) << "the two sides disagree about acceptance";
          if (rc == SUCCESS) {
            expect_curves_match(ref.zc, *got, "par float");
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 15) << "the par-float sweep compared almost nothing";
}

TEST_F(SwapBootstrapDifferential, AddSwapMatchesForASingleBenchmark) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  int compared = 0;
  for (const auto maturity : {d(2026, 1, 15), d(2027, 4, 15), d(2029, 7, 20)}) {
    for (const double rate : {0.02, 0.03, 0.045}) {
      for (const bool on_cycle : {false, true}) {
        RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
        ASSERT_TRUE(ref);
        std::optional<cdsmodel::ZeroCurveBuilder> got =
            cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
        ASSERT_TRUE(got.has_value());
        seed(ref.zc, *got);

        TInterpData idata;
        std::memset(&idata, 0, sizeof idata);
        const int rc = JpmcdsZCAddSwap(
            ref.zc, nullptr, 1.0, cdsmodel::to_tdate(maturity), on_cycle ? TRUE : FALSE, rate, 2, 2,
            JPMCDS_ACT_360, JPMCDS_ACT_360, JPMCDS_FLAT_FORWARDS, &idata, nullptr,
            packed(cdsmodel::BadDayConv::None), const_cast<char *>("NONE"));
        const bool ok = cdsmodel::add_swap(*got, std::nullopt, 1.0, maturity, on_cycle, rate,
                                           {.interp = cdsmodel::InterpType::FlatForwards},
                                           std::nullopt, cdsmodel::Calendar::none());
        ASSERT_EQ(rc == SUCCESS, ok);
        if (rc == SUCCESS) {
          expect_curves_match(ref.zc, *got, "single");
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 10);
}

// --------------------------------------- the explicit-float path (discZC)

TEST_F(SwapBootstrapDifferential, ValueFixedFloatSwapMatchesWithADiscountCurve) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  int compared = 0;
  int refused_by_both = 0;

  std::vector<cdsmodel::RatePoint> disc_points{{d(2030, 1, 15), 0.030}, {d(2045, 1, 15), 0.035}};
  std::vector<TDate> disc_dates;
  std::vector<double> disc_rates;
  for (const cdsmodel::RatePoint &p : disc_points) {
    disc_dates.push_back(cdsmodel::to_tdate(p.date));
    disc_rates.push_back(p.rate);
  }
  RefTC disc;
  disc.tc = JpmcdsMakeTCurve(cdsmodel::to_tdate(vd), disc_dates.data(), disc_rates.data(), 2, 1.0,
                             JPMCDS_ACT_365F);
  ASSERT_NE(disc.tc, nullptr);
  const std::optional<cdsmodel::ZeroCurve> port_disc =
      cdsmodel::ZeroCurve::create(vd, disc_points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(port_disc.has_value());

  for (const auto maturity : {d(2027, 1, 15), d(2029, 1, 15), d(2032, 1, 15)}) {
    for (const double rate : {0.030, 0.040, 0.050}) {
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
        ASSERT_TRUE(ref);
        std::optional<cdsmodel::ZeroCurveBuilder> got =
            cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
        ASSERT_TRUE(got.has_value());
        ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2025, 1, 15)), 0.024), SUCCESS);
        ASSERT_TRUE(got->add_rate(d(2025, 1, 15), 0.024));

        TInterpData idata;
        std::memset(&idata, 0, sizeof idata);
        const int rc =
            JpmcdsZCValueFixFltSwap(ref.zc, disc.tc, 1.0, cdsmodel::to_tdate(maturity), rate, 2, 2,
                                    JPMCDS_ACT_360, JPMCDS_ACT_360, upstream_interp(interp), &idata,
                                    packed(cdsmodel::BadDayConv::None), const_cast<char *>("NONE"));

        const bool ok = cdsmodel::value_fixed_float_swap(
            *got, *port_disc, 1.0, maturity, rate, {.interp = interp}, cdsmodel::Calendar::none());

        if (rc != SUCCESS) {
          // UPSTREAM-33 cases are covered by their own test; these are all
          // benchmarks beyond the curve end, so upstream should succeed.
          EXPECT_FALSE(ok) << "port accepted what upstream refused";
          ++refused_by_both;
          continue;
        }
        ASSERT_TRUE(ok) << "port refused what upstream accepted";
        expect_curves_match(ref.zc, *got, "fix/flt");
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 10) << "the discount-curve sweep compared almost nothing";
}

// ------------------------------------------------------------ UPSTREAM-33

// The mechanism: JpmcdsZCAddRate inserts in SORTED position, so the point just
// added is `numItems-1` only when it is the latest. Asserted directly, because
// everything else about this defect follows from it.
TEST_F(SwapBootstrapDifferential, Upstream33AddRateInsertsSortedNotAtTheEnd) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);
  ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2026, 1, 15)), 0.025), SUCCESS);
  ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2035, 1, 15)), 0.040), SUCCESS);

  ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2027, 1, 15)), 0.045), SUCCESS);
  ASSERT_EQ(ref.zc->numItems, 3);
  EXPECT_EQ(ref.zc->date[1], cdsmodel::to_tdate(d(2027, 1, 15)))
      << "the new point went somewhere other than index 1";
  EXPECT_NE(ref.zc->date[ref.zc->numItems - 1], cdsmodel::to_tdate(d(2027, 1, 15)))
      << "numItems-1 IS the new point, so UPSTREAM-33 may be fixed";
  EXPECT_EQ(ref.zc->date[ref.zc->numItems - 1], cdsmodel::to_tdate(d(2035, 1, 15)))
      << "and numItems-1 -- which the solver writes to -- is the pre-existing point";
}

// The consequence, with POSITIVE CONTROLS. Without controls, "it always fails"
// could just mean the fixture is wrong.
TEST_F(SwapBootstrapDifferential, Upstream33TheSolveFailsWhenTheBenchmarkIsInsideTheCurve) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  std::vector<TDate> disc_dates{cdsmodel::to_tdate(d(2030, 1, 15)),
                                cdsmodel::to_tdate(d(2045, 1, 15))};
  std::vector<double> disc_rates{0.030, 0.035};
  RefTC disc;
  disc.tc = JpmcdsMakeTCurve(cdsmodel::to_tdate(vd), disc_dates.data(), disc_rates.data(), 2, 1.0,
                             JPMCDS_ACT_365F);
  ASSERT_NE(disc.tc, nullptr);

  const auto attempt = [&](int far_year, int benchmark_year) {
    RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
    if (!ref) {
      return -1;
    }
    JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2025, 1, 15)), 0.024);
    if (far_year > 0) {
      JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(far_year, 1, 15)), 0.040);
    }
    TInterpData idata;
    std::memset(&idata, 0, sizeof idata);
    return JpmcdsZCValueFixFltSwap(ref.zc, disc.tc, 1.0,
                                   cdsmodel::to_tdate(d(benchmark_year, 1, 15)), 0.045, 2, 2,
                                   JPMCDS_ACT_360, JPMCDS_ACT_360, JPMCDS_FLAT_FORWARDS, &idata,
                                   packed(cdsmodel::BadDayConv::None), const_cast<char *>("NONE"));
  };

  // Positive controls: benchmark BEYOND the curve's last point.
  EXPECT_EQ(attempt(0, 2029), SUCCESS) << "control: no far point";
  EXPECT_EQ(attempt(2026, 2029), SUCCESS) << "control: far point before the benchmark";
  EXPECT_EQ(attempt(2027, 2032), SUCCESS) << "control: far point well before the benchmark";

  // The defect: benchmark INSIDE the curve, so maxDate is not the last index.
  EXPECT_NE(attempt(2035, 2029), SUCCESS) << "UPSTREAM-33 may be fixed";
  EXPECT_NE(attempt(2030, 2029), SUCCESS);
  EXPECT_NE(attempt(2029, 2028), SUCCESS);
}

// The port solves for the point it added, by index, so the same input that
// defeats upstream is accepted here. A deliberate divergence.
TEST_F(SwapBootstrapDifferential, Upstream33ThePortSolvesForThePointItAdded) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  const std::vector<cdsmodel::RatePoint> disc_points{{d(2030, 1, 15), 0.030},
                                                     {d(2045, 1, 15), 0.035}};
  const std::optional<cdsmodel::ZeroCurve> disc =
      cdsmodel::ZeroCurve::create(vd, disc_points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc.has_value());

  std::optional<cdsmodel::ZeroCurveBuilder> curve =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  ASSERT_TRUE(curve->add_rate(d(2025, 1, 15), 0.024));
  ASSERT_TRUE(curve->add_rate(d(2035, 1, 15), 0.040));

  EXPECT_TRUE(cdsmodel::value_fixed_float_swap(*curve, *disc, 1.0, d(2029, 1, 15), 0.045,
                                               {.interp = cdsmodel::InterpType::FlatForwards},
                                               cdsmodel::Calendar::none()))
      << "the port should solve a benchmark inside the curve, where upstream cannot";

  // And the pre-existing far point is untouched, which is the thing upstream
  // would have overwritten had its solve converged.
  bool found_far = false;
  for (const cdsmodel::ZeroCurveBuilder::Point &p : curve->points()) {
    if (p.date == d(2035, 1, 15)) {
      EXPECT_DOUBLE_EQ(p.rate, 0.040) << "the far point was overwritten";
      found_far = true;
    }
  }
  EXPECT_TRUE(found_far) << "the far point vanished";
}

// -------------------------------- UPSTREAM-29 and 32, unreachable from here

// Both are triggered by a benchmark maturing on the curve's own value date,
// and zcswap.c compares those two dates nowhere. The port refuses.
TEST_F(SwapBootstrapDifferential, ABenchmarkMaturingOnTheValueDateIsRefused) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  std::optional<cdsmodel::ZeroCurveBuilder> curve =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  ASSERT_TRUE(curve->add_rate(d(2024, 4, 15), 0.023));

  // UPSTREAM-32's trigger: the par-float path, through JpmcdsZCGetSwapCFL.
  EXPECT_FALSE(cdsmodel::add_swap(*curve, std::nullopt, 1.0, vd, true, 0.04,
                                  {.interp = cdsmodel::InterpType::FlatForwards}, std::nullopt,
                                  cdsmodel::Calendar::none()));

  // UPSTREAM-29's trigger: the explicit-float path, through
  // JpmcdsNewCouponDatesSwap and then fArray[fNumItems-1].
  const std::vector<cdsmodel::RatePoint> disc_points{{d(2030, 1, 15), 0.030}};
  const std::optional<cdsmodel::ZeroCurve> disc =
      cdsmodel::ZeroCurve::create(vd, disc_points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc.has_value());
  EXPECT_FALSE(cdsmodel::value_fixed_float_swap(*curve, *disc, 1.0, vd, 0.04,
                                                {.interp = cdsmodel::InterpType::FlatForwards},
                                                cdsmodel::Calendar::none()));
}

// Upstream checks this exclusivity here even though JpmcdsZCAdjustDate
// downstream silently assumes it (UPSTREAM-26). Both sides must refuse.
TEST_F(SwapBootstrapDifferential, ATableAndAConventionTogetherAreRefusedByBoth) {
  const cdsmodel::Date vd = d(2024, 1, 15);
  RefZC ref(cdsmodel::to_tdate(vd), 8, 1L, JPMCDS_ACT_365F);
  ASSERT_TRUE(ref);
  ASSERT_EQ(JpmcdsZCAddRate(ref.zc, cdsmodel::to_tdate(d(2024, 4, 15)), 0.023), SUCCESS);

  std::vector<TDate> bad{cdsmodel::to_tdate(d(2026, 1, 17))};
  std::vector<TDate> good{cdsmodel::to_tdate(d(2026, 1, 15))};
  TBadDayList table{};
  table.count = 1;
  table.maxAllowed = 1;
  table.badDay = bad.data();
  table.goodDay = good.data();

  std::vector<TDate> maturities{cdsmodel::to_tdate(d(2026, 1, 15))};
  std::vector<double> rates{0.028};
  TInterpData idata;
  std::memset(&idata, 0, sizeof idata);
  EXPECT_NE(JpmcdsZCAddSwaps(ref.zc, nullptr, maturities.data(), rates.data(), 1, 2, 2,
                             JPMCDS_ACT_360, JPMCDS_ACT_360, JPMCDS_FLAT_FORWARDS, &idata, &table,
                             packed(cdsmodel::BadDayConv::Follow), const_cast<char *>("NONE")),
            SUCCESS);

  std::optional<cdsmodel::ZeroCurveBuilder> curve =
      cdsmodel::ZeroCurveBuilder::create(vd, 8, 1L, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  ASSERT_TRUE(curve->add_rate(d(2024, 4, 15), 0.023));
  const std::vector<cdsmodel::BadDayPair> pairs{
      cdsmodel::BadDayPair{d(2026, 1, 17), d(2026, 1, 15)}};
  const std::vector<cdsmodel::Date> port_maturities{d(2026, 1, 15)};
  const std::vector<double> port_rates{0.028};
  EXPECT_FALSE(cdsmodel::add_swaps(
      *curve, std::nullopt, port_maturities, port_rates,
      {.interp = cdsmodel::InterpType::FlatForwards, .bad_day = cdsmodel::BadDayConv::Follow},
      std::span<const cdsmodel::BadDayPair>{pairs}, cdsmodel::Calendar::none()));
}

} // namespace
