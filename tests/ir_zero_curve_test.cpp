// TIER 1 -- replays tests/golden/ir_zero_curve_cases.hpp, so the top-level IR
// curve build is covered in CI, where no oracle is present.
//
// These are the closest thing this project has to end-to-end vectors: one call
// exercises money market, the cash merge, swap schedules, stream cash flows,
// the Brent solve, and the curve container. If a slice two layers down drifts,
// this file is where it shows up without anyone having to guess which test to
// look at.
//
// The tolerance is chosen PER CASE. A money-market-only curve involves no
// solve and compares at the accumulating tier; anything with a swap in it
// compares at the solver tier. One entry point produces both, so the flag
// travels with the case rather than with the array.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/ir_zero_curve.hpp>

#include "golden/ir_zero_curve_cases.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

std::vector<cdsmodel::InstrumentQuote> quotes_of(int cash_count) {
  std::vector<cdsmodel::InstrumentQuote> out;
  for (std::size_t i = 0; i < kIrCurveInstruments; ++i) {
    out.push_back(cdsmodel::InstrumentQuote{
        static_cast<int>(i) < cash_count ? cdsmodel::InstrumentKind::MoneyMarket
                                         : cdsmodel::InstrumentKind::Swap,
        cdsmodel::from_tdate(kIrCurveDates[i]), kIrCurveRates[i]});
  }
  return out;
}

TEST(IrZeroCurveGolden, ReplaysRecordedCurves) {
  ASSERT_GE(kIrCurveCases.size(), 50u);
  int matched = 0;
  int exact_cases = 0;
  for (const IrCurveCase &c : kIrCurveCases) {
    const std::optional<cdsmodel::DayCount> mm_dcc =
        cdsmodel::from_upstream_code(c.money_market_day_count);
    const std::optional<cdsmodel::BadDayConv> conv = cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(mm_dcc.has_value());
    ASSERT_TRUE(conv.has_value());

    const std::vector<cdsmodel::InstrumentQuote> quotes = quotes_of(c.cash_count);
    const std::optional<cdsmodel::ZeroCurve> got =
        cdsmodel::build_ir_zero_curve(cdsmodel::from_tdate(kIrCurveValueDate), quotes, *mm_dcc,
                                      {.fixed_frequency = c.frequency,
                                       .float_frequency = c.frequency,
                                       .interp = cdsmodel::InterpType::FlatForwards,
                                       .bad_day = *conv},
                                      cdsmodel::Calendar::none());

    ASSERT_EQ(got.has_value(), c.accepted)
        << "cash_count " << c.cash_count << " freq " << c.frequency;
    if (!c.accepted) {
      continue;
    }
    const std::span<const cdsmodel::RatePoint> points = got->points();
    ASSERT_EQ(points.size(), c.point_count) << "cash_count " << c.cash_count;

    // A curve with no swap in it was not solved for, so it has no business
    // needing the solver's slack.
    const double tolerance = c.money_market_only ? kIrCurveExactTolerance : kIrCurveSolvedTolerance;
    if (c.money_market_only) {
      ++exact_cases;
    }
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedIrCurvePoint &want = kIrCurvePoints[c.first_point + i];
      EXPECT_EQ(cdsmodel::to_tdate(points[i].date), want.date) << "date " << i;
      EXPECT_NEAR(want.rate, points[i].rate,
                  c.money_market_only ? tolerance * std::max(1.0, std::abs(want.rate)) : tolerance)
          << "rate " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 80);
  EXPECT_GT(exact_cases, 0)
      << "no money-market-only case was replayed, so the tight tolerance is untested";
}

// ---------------------------------------------- properties, no oracle needed

// Every benchmark date appears in the curve. The whole point of the two stages
// is that each instrument pins a point, and a recorded rate would not notice
// if one instrument were silently dropped.
TEST(IrZeroCurveGolden, EveryBenchmarkDateAppearsInTheCurve) {
  int checked = 0;
  for (const IrCurveCase &c : kIrCurveCases) {
    if (!c.accepted) {
      continue;
    }
    for (std::size_t q = 0; q < kIrCurveInstruments; ++q) {
      bool found = false;
      for (std::size_t i = 0; i < c.point_count; ++i) {
        if (kIrCurvePoints[c.first_point + i].date == kIrCurveDates[q]) {
          found = true;
        }
      }
      EXPECT_TRUE(found) << "benchmark " << q << " left no point in the curve"
                         << " (cash_count " << c.cash_count << ")";
      ++checked;
    }
  }
  EXPECT_GT(checked, 500);
}

TEST(IrZeroCurveGolden, EveryCurveIsStrictlyIncreasingWithFiniteRates) {
  int checked = 0;
  for (const IrCurveCase &c : kIrCurveCases) {
    if (!c.accepted) {
      continue;
    }
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedIrCurvePoint &p = kIrCurvePoints[c.first_point + i];
      EXPECT_TRUE(std::isfinite(p.rate));
      EXPECT_GT(p.rate, -1.0);
      EXPECT_GT(p.date, kIrCurveValueDate) << "a point at or before the value date";
      if (i > 0) {
        EXPECT_GT(p.date, kIrCurvePoints[c.first_point + i - 1].date) << "not increasing";
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 1000);
}

// Order WITHIN each instrument group is load-bearing; order between the groups
// is not. The function sorts the list into two groups before using either, so
// interleaving the kinds is free -- but reversing makes each group's dates
// decrease, and both stages refuse that.
//
// The name says what it asserts. The first version of this test was called
// "InterleavingTheInstrumentKindsChangesNothing" and then asserted a refusal,
// which is the opposite claim.
TEST(IrZeroCurveGolden, ADecreasingBenchmarkListIsRefused) {
  // Find a recorded case with both kinds present.
  const IrCurveCase *chosen = nullptr;
  for (const IrCurveCase &c : kIrCurveCases) {
    if (c.accepted && c.cash_count > 0 && c.cash_count < static_cast<int>(kIrCurveInstruments)) {
      chosen = &c;
      break;
    }
  }
  ASSERT_NE(chosen, nullptr);

  const std::optional<cdsmodel::DayCount> mm_dcc =
      cdsmodel::from_upstream_code(chosen->money_market_day_count);
  const std::optional<cdsmodel::BadDayConv> conv =
      cdsmodel::bad_day_conv_from_upstream(chosen->conv);
  ASSERT_TRUE(mm_dcc.has_value());
  ASSERT_TRUE(conv.has_value());
  const cdsmodel::SwapConventions conventions{.fixed_frequency = chosen->frequency,
                                              .float_frequency = chosen->frequency,
                                              .interp = cdsmodel::InterpType::FlatForwards,
                                              .bad_day = *conv};

  const std::vector<cdsmodel::InstrumentQuote> grouped = quotes_of(chosen->cash_count);
  // Reverse the list. The dates no longer increase, but each GROUP still does
  // once sorted out, which is all either stage requires.
  std::vector<cdsmodel::InstrumentQuote> reversed(grouped.rbegin(), grouped.rend());

  const std::optional<cdsmodel::ZeroCurve> a =
      cdsmodel::build_ir_zero_curve(cdsmodel::from_tdate(kIrCurveValueDate), grouped, *mm_dcc,
                                    conventions, cdsmodel::Calendar::none());
  const std::optional<cdsmodel::ZeroCurve> b =
      cdsmodel::build_ir_zero_curve(cdsmodel::from_tdate(kIrCurveValueDate), reversed, *mm_dcc,
                                    conventions, cdsmodel::Calendar::none());
  ASSERT_TRUE(a.has_value());
  // Reversing makes each group's dates DECREASE, which both stages refuse --
  // so this asserts the refusal, not an equality. Recorded because it is the
  // honest answer: order within a group is load-bearing, order between groups
  // is not.
  EXPECT_FALSE(b.has_value())
      << "a decreasing benchmark list was accepted; the ordering checks are gone";
}

// UPSTREAM-35, pinned without the oracle: the port's parser is defined for
// every byte, including the ones C forbids passing to toupper.
TEST(IrZeroCurveGolden, Upstream35TheParserIsDefinedForEveryByte) {
  int accepted = 0;
  for (int byte = 0; byte < 256; ++byte) {
    const char code = static_cast<char>(byte);
    const std::optional<cdsmodel::InstrumentKind> kind = cdsmodel::instrument_kind_from_code(code);
    const bool expected = code == 'M' || code == 'm' || code == 'S' || code == 's';
    EXPECT_EQ(kind.has_value(), expected) << "byte " << byte;
    if (kind) {
      ++accepted;
    }
  }
  EXPECT_EQ(accepted, 4) << "exactly M, m, S and s are instrument codes";
  // The high-bit bytes are the ones upstream hands to toupper as negatives.
  EXPECT_FALSE(cdsmodel::instrument_kind_from_code(static_cast<char>(0x80)).has_value());
  EXPECT_FALSE(cdsmodel::instrument_kind_from_code(static_cast<char>(0xFF)).has_value());
}

} // namespace
