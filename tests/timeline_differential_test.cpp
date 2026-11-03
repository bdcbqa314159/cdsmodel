// TIER 2 -- live differential tests for the integration timeline.
//
// This file discharges a debt. The divergence policy requires that when the
// port corrects upstream, the consequence is MEASURED rather than assumed --
// and where the function is on the pricing path, measured by pricing both
// ways. `timeline.c` is the only caller of `JpmcdsDateListTruncate`, the
// UPSTREAM-7 function, so this is where that measurement lives.
//
// The result is stronger than a passing test: the defect is unreachable here
// by construction, and the tests below both demonstrate the agreement and
// pin the reason.

#include <cdsmodel/date.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <span>
#include <utility>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cxdatelist.h"
#include "ldate.h"
#include "tcurve.h"
#include "timeline.h"
}

namespace {

class TimelineDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(points.size()), 1.0, JPMCDS_ACT_365F);
}

std::vector<cdsmodel::Date> from_list(TDateList *dl) {
  std::vector<cdsmodel::Date> out;
  if (dl == nullptr) {
    return out;
  }
  for (int i = 0; i < dl->fNumItems; ++i) {
    out.push_back(cdsmodel::from_tdate(dl->fArray[i]));
  }
  return out;
}

struct Fixture {
  cdsmodel::Date base = d(2024, 1, 1);
  std::vector<cdsmodel::RatePoint> disc{{d(2024, 4, 1), 0.031},
                                        {d(2025, 1, 1), 0.035},
                                        {d(2027, 1, 1), 0.040},
                                        {d(2031, 1, 1), 0.044}};
  std::vector<cdsmodel::RatePoint> spread{
      {d(2024, 6, 20), 0.010}, {d(2025, 6, 20), 0.013}, {d(2029, 6, 20), 0.018}};
};

TEST_F(TimelineDifferential, RiskyTimelineMatchesAcrossManyWindows) {
  const Fixture f;
  TCurve *disc = build(f.base, f.disc);
  TCurve *spread = build(f.base, f.spread);
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(spread, nullptr);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(f.base, f.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(f.base, f.spread, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(disc_c.has_value());
  ASSERT_TRUE(spread_c.has_value());

  std::size_t compared = 0;
  // Windows that start and end before, on, between and after every curve
  // point, including one-day windows and windows containing no curve points
  // at all -- the case that would reduce the timeline to its two endpoints.
  for (int start_off : {0, 1, 60, 91, 92, 170, 365, 366, 900, 1096, 2000, 2800}) {
    for (int span : {1, 2, 5, 30, 180, 365, 1000, 3000}) {
      const cdsmodel::Date start = f.base + std::chrono::days{start_off};
      const cdsmodel::Date end = start + std::chrono::days{span};

      TDateList *ref =
          JpmcdsRiskyTimeLine(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), disc, spread);
      const auto ported = cdsmodel::risky_timeline(start, end, *disc_c, *spread_c);

      ASSERT_EQ(ported.has_value(), ref != nullptr)
          << "validity, start_off=" << start_off << " span=" << span;
      if (ref != nullptr) {
        EXPECT_EQ(from_list(ref), *ported)
            << "timeline differs, start_off=" << start_off << " span=" << span;
        ++compared;
        JpmcdsFreeDateList(ref);
      }
    }
  }
  EXPECT_GT(compared, 90u) << "the sweep barely ran";
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(TimelineDifferential, Upstream7CannotAffectATimeline) {
  // The measurement the divergence policy demands. The port's truncation is
  // CORRECTED; upstream's silently ignores a truncation whenever fewer than
  // two dates would survive. Here that regime is unreachable, because both
  // truncations run after start and end are already in the list and the
  // contract requires end > start.
  //
  // The minimal timeline is therefore exactly two dates, which is already in
  // the regime where upstream is correct. These cases construct it directly:
  // windows containing no curve point at all.
  const Fixture f;
  TCurve *disc = build(f.base, f.disc);
  TCurve *spread = build(f.base, f.spread);
  ASSERT_NE(disc, nullptr);
  ASSERT_NE(spread, nullptr);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(f.base, f.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(f.base, f.spread, 1.0, cdsmodel::DayCount::Act365Fixed);

  // A one-day window inside each gap between curve points: no curve date can
  // fall in it, so the timeline is start and end and nothing else.
  for (int start_off : {10, 100, 200, 400, 800, 1500, 2500, 3500}) {
    const cdsmodel::Date start = f.base + std::chrono::days{start_off};
    const cdsmodel::Date end = start + std::chrono::days{1};

    TDateList *ref =
        JpmcdsRiskyTimeLine(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), disc, spread);
    ASSERT_NE(ref, nullptr) << "start_off=" << start_off;
    const auto ported = cdsmodel::risky_timeline(start, end, *disc_c, *spread_c);
    ASSERT_TRUE(ported.has_value());

    EXPECT_EQ(ported->size(), 2u) << "expected the minimal timeline at " << start_off;
    EXPECT_EQ(from_list(ref), *ported)
        << "upstream and the corrected port disagree on a minimal timeline, which "
           "would mean UPSTREAM-7 does reach the pricing path";
    JpmcdsFreeDateList(ref);
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

TEST_F(TimelineDifferential, TruncateTimelineMatchesIncludingAnEmptyCriticalSet) {
  const std::vector<cdsmodel::Date> critical{d(2024, 3, 20), d(2024, 6, 20), d(2025, 3, 20),
                                             d(2026, 9, 20), d(2030, 3, 20)};
  std::vector<TDate> raw;
  for (cdsmodel::Date c : critical) {
    raw.push_back(cdsmodel::to_tdate(c));
  }

  for (int start_off : {0, 100, 365, 800, 2000}) {
    for (int span : {1, 90, 400, 2500}) {
      const cdsmodel::Date start = d(2024, 1, 1) + std::chrono::days{start_off};
      const cdsmodel::Date end = start + std::chrono::days{span};

      for (bool with_critical : {true, false}) {
        TDateList in{static_cast<int>(raw.size()), raw.data()};
        TDateList *ref = JpmcdsTruncateTimeLine(with_critical ? &in : nullptr,
                                                cdsmodel::to_tdate(start), cdsmodel::to_tdate(end));
        const auto ported =
            cdsmodel::truncate_timeline(with_critical ? std::span<const cdsmodel::Date>{critical}
                                                      : std::span<const cdsmodel::Date>{},
                                        start, end);
        ASSERT_EQ(ported.has_value(), ref != nullptr)
            << "validity, start_off=" << start_off << " span=" << span
            << " critical=" << with_critical;
        if (ref != nullptr) {
          EXPECT_EQ(from_list(ref), *ported)
              << "start_off=" << start_off << " span=" << span << " critical=" << with_critical;
          JpmcdsFreeDateList(ref);
        }
      }
    }
  }
}

TEST_F(TimelineDifferential, RejectsAnEmptyOrInvertedWindowAsUpstreamDoes) {
  const Fixture f;
  TCurve *disc = build(f.base, f.disc);
  TCurve *spread = build(f.base, f.spread);
  const auto disc_c =
      cdsmodel::ZeroCurve::create(f.base, f.disc, 1.0, cdsmodel::DayCount::Act365Fixed);
  const auto spread_c =
      cdsmodel::ZeroCurve::create(f.base, f.spread, 1.0, cdsmodel::DayCount::Act365Fixed);
  const cdsmodel::Date day = d(2025, 6, 1);

  for (auto [start, end] : {std::pair{day, day}, std::pair{day, day - std::chrono::days{1}}}) {
    TDateList *ref =
        JpmcdsRiskyTimeLine(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), disc, spread);
    EXPECT_EQ(ref, nullptr) << "upstream accepted a non-positive window";
    EXPECT_FALSE(cdsmodel::risky_timeline(start, end, *disc_c, *spread_c).has_value());
    EXPECT_FALSE(cdsmodel::truncate_timeline({}, start, end).has_value());
  }
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
}

} // namespace
