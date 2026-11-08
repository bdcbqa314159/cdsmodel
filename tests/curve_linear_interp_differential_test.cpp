// TIER 2 -- live differential tests for re-expressing a curve on new dates.
//
// The sweep is over the three things that change the answer: where the desired
// dates sit relative to the old curve (inside, before, after, exactly on a
// knot), the old curve's day count convention (which selects the interpolation
// metric), and the old curve's shape.
//
// One test asserts a divergence: UPSTREAM-36, a zero-point input curve. Its
// live reproducer is DISABLED, because it dereferences NULL.

#include <cdsmodel/curve_linear_interp.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "dtlist.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class CurveLinearInterpDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct RefTC {
  TCurve *tc = nullptr;
  ~RefTC() { JpmcdsFreeTCurve(tc); }
  RefTC() = default;
  explicit RefTC(TCurve *owned) : tc(owned) {}
  RefTC(const RefTC &) = delete;
  RefTC &operator=(const RefTC &) = delete;
  explicit operator bool() const { return tc != nullptr; }
};

struct RefDL {
  TDateList *dl = nullptr;
  ~RefDL() { JpmcdsFreeDateList(dl); }
  RefDL() = default;
  explicit RefDL(TDateList *owned) : dl(owned) {}
  RefDL(const RefDL &) = delete;
  RefDL &operator=(const RefDL &) = delete;
};

/// One old curve, built on both sides.
struct CurvePair {
  RefTC ref;
  cdsmodel::Expected<cdsmodel::ZeroCurve> port;

  CurvePair(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points, double basis,
            cdsmodel::DayCount dcc)
      : port(cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not built")) {
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (const cdsmodel::RatePoint &p : points) {
      dates.push_back(cdsmodel::to_tdate(p.date));
      rates.push_back(p.rate);
    }
    ref.tc =
        JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                         static_cast<int>(points.size()), basis, cdsmodel::to_upstream_code(dcc));
    port = cdsmodel::ZeroCurve::create(base, points, basis, dcc);
  }
};

const std::vector<std::vector<cdsmodel::RatePoint>> &old_curves() {
  static const std::vector<std::vector<cdsmodel::RatePoint>> v{
      {{d(2024, 7, 15), 0.0300}, {d(2027, 1, 15), 0.0350}},
      {{d(2024, 4, 15), 0.0230},
       {d(2025, 1, 15), 0.0260},
       {d(2027, 1, 15), 0.0310},
       {d(2031, 1, 15), 0.0340}},
      {{d(2025, 1, 15), 0.0400}},                           // one point: every answer is that rate
      {{d(2024, 6, 15), 0.0300}, {d(2024, 6, 16), 0.0301}}, // adjacent dates
  };
  return v;
}

const std::vector<std::vector<cdsmodel::Date>> &desired_sets() {
  static const std::vector<std::vector<cdsmodel::Date>> v{
      {d(2025, 1, 15), d(2026, 1, 15)},              // inside
      {d(2020, 1, 15), d(2023, 1, 15)},              // entirely before
      {d(2035, 1, 15), d(2040, 1, 15)},              // entirely after
      {d(2024, 7, 15), d(2027, 1, 15)},              // exactly on knots
      {d(2020, 1, 1), d(2025, 6, 1), d(2045, 1, 1)}, // straddling both ends
      {d(2026, 1, 15), d(2025, 1, 15)},              // DECREASING order
  };
  return v;
}

TEST_F(CurveLinearInterpDifferential, MatchesAcrossCurvesDatesAndDayCounts) {
  const cdsmodel::Date base = d(2024, 1, 15);
  int compared = 0;
  int refused_by_both = 0;
  for (const auto &points : old_curves()) {
    for (const cdsmodel::DayCount dcc :
         {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360}) {
      for (const double basis : {1.0, 2.0, cdsmodel::kContinuousBasis}) {
        CurvePair pair(base, points, basis, dcc);
        if (!pair.ref || !pair.port.has_value()) {
          continue;
        }
        for (const auto &desired : desired_sets()) {
          std::vector<TDate> raw;
          for (const cdsmodel::Date date : desired) {
            raw.push_back(cdsmodel::to_tdate(date));
          }
          RefDL dl(JpmcdsNewDateListFromDates(raw.data(), static_cast<int>(raw.size())));
          ASSERT_NE(dl.dl, nullptr);

          RefTC out(JpmcdsLinInterpCurve(dl.dl, pair.ref.tc, nullptr));
          const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
              cdsmodel::linear_interp_curve(desired, *pair.port);

          if (!out) {
            EXPECT_FALSE(got.has_value()) << "port accepted what upstream refused";
            ++refused_by_both;
            continue;
          }
          // A decreasing desired list makes upstream produce a curve whose
          // dates decrease; the port's ZeroCurve cannot hold one, so it
          // refuses. Covered by its own test below.
          bool increasing = true;
          for (int i = 1; i < out.tc->fNumItems; ++i) {
            if (out.tc->fArray[i].fDate <= out.tc->fArray[i - 1].fDate) {
              increasing = false;
            }
          }
          if (!increasing) {
            EXPECT_FALSE(got.has_value()) << "the port accepted a non-increasing curve";
            ++refused_by_both;
            continue;
          }

          ASSERT_TRUE(got.has_value()) << "port refused what upstream accepted";
          const std::span<const cdsmodel::RatePoint> result = got->points();
          ASSERT_EQ(out.tc->fNumItems, static_cast<int>(result.size()));
          EXPECT_EQ(out.tc->fBaseDate, cdsmodel::to_tdate(got->base_date()));
          for (int i = 0; i < out.tc->fNumItems; ++i) {
            const auto k = static_cast<std::size_t>(i);
            EXPECT_EQ(out.tc->fArray[i].fDate, cdsmodel::to_tdate(result[k].date)) << "date " << i;
            // One computation against itself in the same binary, so ULPs.
            EXPECT_NEAR(out.tc->fArray[i].fRate, result[k].rate,
                        4.0 * std::max(1.0, std::abs(out.tc->fArray[i].fRate)) *
                            std::numeric_limits<double>::epsilon())
                << "rate " << i;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 80) << "the sweep compared almost nothing";
  EXPECT_GT(refused_by_both, 0) << "no refusal path was exercised";
}

// The clamp is on the date interpolated AT; the date stored is the one asked
// for. So a desired date past the old curve's end carries the final rate at
// its OWN date -- a flat extension, not an extrapolation.
TEST_F(CurveLinearInterpDifferential, ADateBeyondTheCurveKeepsItsOwnDateAndTheFinalRate) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 7, 15), 0.0300}, {d(2027, 1, 15), 0.0350}};
  CurvePair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  const std::vector<cdsmodel::Date> desired{d(2020, 1, 1), d(2045, 1, 1)};
  std::vector<TDate> raw{cdsmodel::to_tdate(desired[0]), cdsmodel::to_tdate(desired[1])};
  RefDL dl(JpmcdsNewDateListFromDates(raw.data(), 2));
  ASSERT_NE(dl.dl, nullptr);
  RefTC out(JpmcdsLinInterpCurve(dl.dl, pair.ref.tc, nullptr));
  ASSERT_TRUE(out);

  // Upstream's own answer, asserted so the port cannot drift from it quietly.
  EXPECT_EQ(out.tc->fArray[0].fDate, cdsmodel::to_tdate(d(2020, 1, 1)));
  EXPECT_DOUBLE_EQ(out.tc->fArray[0].fRate, 0.0300) << "before the curve: the FIRST rate";
  EXPECT_EQ(out.tc->fArray[1].fDate, cdsmodel::to_tdate(d(2045, 1, 1)));
  EXPECT_DOUBLE_EQ(out.tc->fArray[1].fRate, 0.0350) << "after the curve: the LAST rate";

  const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
      cdsmodel::linear_interp_curve(desired, *pair.port);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->points()[0].date, d(2020, 1, 1));
  EXPECT_DOUBLE_EQ(got->points()[0].rate, 0.0300);
  EXPECT_EQ(got->points()[1].date, d(2045, 1, 1));
  EXPECT_DOUBLE_EQ(got->points()[1].rate, 0.0350);
}

// This is the library's only caller that passes a METRIC rather than NULL, so
// the day count convention must actually reach the interpolation. Two
// conventions selecting different metrics must disagree somewhere.
TEST_F(CurveLinearInterpDifferential, TheDayCountConventionReachesTheInterpolation) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 7, 15), 0.0300}, {d(2027, 1, 15), 0.0350}};
  const std::vector<cdsmodel::Date> desired{d(2025, 3, 20), d(2026, 9, 5)};
  std::vector<TDate> raw{cdsmodel::to_tdate(desired[0]), cdsmodel::to_tdate(desired[1])};
  RefDL dl(JpmcdsNewDateListFromDates(raw.data(), 2));
  ASSERT_NE(dl.dl, nullptr);

  // ACT/365F and 30/360 select DIFFERENT metrics -- the three ACT variants all
  // share one, because the ratio is scale-invariant.
  CurvePair act(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  CurvePair thirty(base, points, 1.0, cdsmodel::DayCount::Thirty360);
  ASSERT_TRUE(act.ref);
  ASSERT_TRUE(thirty.ref);

  RefTC a(JpmcdsLinInterpCurve(dl.dl, act.ref.tc, nullptr));
  RefTC b(JpmcdsLinInterpCurve(dl.dl, thirty.ref.tc, nullptr));
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  int differing = 0;
  for (int i = 0; i < a.tc->fNumItems; ++i) {
    if (a.tc->fArray[i].fRate != b.tc->fArray[i].fRate) {
      ++differing;
    }
  }
  EXPECT_GT(differing, 0)
      << "ACT/365F and 30/360 produced identical rates, so the metric is being ignored";

  // And a convention outside the five JpmcdsDayCountToMetricFunc knows is
  // refused by both.
  EXPECT_FALSE(cdsmodel::metric_for_day_count(cdsmodel::DayCount::EffectiveRate).has_value());
}

// ------------------------------------------------------------ UPSTREAM-36

// The precondition, measured: a zero-point curve is constructible and has a
// NULL array, so both ends of the clamping loop read through NULL.
TEST_F(CurveLinearInterpDifferential, Upstream36AZeroPointCurveHasANullArray) {
  RefTC empty(JpmcdsNewTCurve(cdsmodel::to_tdate(d(2024, 1, 15)), 0, 1.0, JPMCDS_ACT_365F));
  ASSERT_TRUE(empty) << "a zero-point curve is refused, so UPSTREAM-36 may be fixed";
  EXPECT_EQ(empty.tc->fNumItems, 0);
  EXPECT_EQ(empty.tc->fArray, nullptr)
      << "fArray is non-null, so fArray[-1] would not be a null dereference";
  // Only a NEGATIVE count is refused.
  EXPECT_EQ(JpmcdsNewTCurve(cdsmodel::to_tdate(d(2024, 1, 15)), -1, 1.0, JPMCDS_ACT_365F), nullptr);

  // The port has no such curve to pass: ZeroCurve::create refuses an empty
  // point list.
  const std::vector<cdsmodel::RatePoint> none;
  EXPECT_FALSE(
      cdsmodel::ZeroCurve::create(d(2024, 1, 15), none, 1.0, cdsmodel::DayCount::Act365Fixed)
          .has_value());
}

TEST_F(CurveLinearInterpDifferential, DISABLED_Upstream36DereferencesANullArray) {
  RefTC empty(JpmcdsNewTCurve(cdsmodel::to_tdate(d(2024, 1, 15)), 0, 1.0, JPMCDS_ACT_365F));
  TDate raw[2] = {cdsmodel::to_tdate(d(2025, 1, 15)), cdsmodel::to_tdate(d(2026, 1, 15))};
  RefDL dl(JpmcdsNewDateListFromDates(raw, 2));
  // The clamping loop reads oldCurve->fArray[fNumItems-1] and fArray[0] with
  // fArray == NULL. UBSan: "applying non-zero offset ... to null pointer" at
  // linterpc.c:65. ASan: SEGV on 0xfffffffffffffff0. Never enabled.
  JpmcdsFreeTCurve(JpmcdsLinInterpCurve(dl.dl, empty.tc, nullptr));
}

// A decreasing desired list produces a curve whose dates decrease. Upstream
// builds it; the port's ZeroCurve cannot hold one.
TEST_F(CurveLinearInterpDifferential, ADecreasingDesiredListDivergesByConstruction) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const std::vector<cdsmodel::RatePoint> points{{d(2024, 7, 15), 0.0300}, {d(2027, 1, 15), 0.0350}};
  CurvePair pair(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  const std::vector<cdsmodel::Date> desired{d(2026, 1, 15), d(2025, 1, 15)};
  std::vector<TDate> raw{cdsmodel::to_tdate(desired[0]), cdsmodel::to_tdate(desired[1])};
  RefDL dl(JpmcdsNewDateListFromDates(raw.data(), 2));
  ASSERT_NE(dl.dl, nullptr);
  RefTC out(JpmcdsLinInterpCurve(dl.dl, pair.ref.tc, nullptr));
  ASSERT_TRUE(out) << "upstream started rejecting a decreasing desired list";
  EXPECT_GT(out.tc->fArray[0].fDate, out.tc->fArray[1].fDate)
      << "upstream's output really is out of order";

  EXPECT_FALSE(cdsmodel::linear_interp_curve(desired, *pair.port).has_value())
      << "the port built a curve whose dates decrease";
}

} // namespace
