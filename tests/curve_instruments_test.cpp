// TIER 1 -- replays tests/golden/curve_instruments_cases.hpp, so adding cash
// and swap instruments to a curve is covered in CI, where no oracle is
// present.
//
// Two case arrays with two tolerances, and that split is the point. Cash rates
// are a direct conversion and compare at the accumulating tier; swap rates are
// roots bounded by the solver's stopping criterion and need a thousand times
// more slack. Recording them together would have forced the loose tolerance
// on both and quietly stopped the cash path from being checked properly.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/curve_instruments.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include "golden/curve_instruments_cases.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

std::vector<cdsmodel::RatePoint> points_of(const cdsmodel::TDate *dates, const double *rates,
                                           std::size_t n) {
  std::vector<cdsmodel::RatePoint> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(cdsmodel::RatePoint{cdsmodel::from_tdate(dates[i]), rates[i]});
  }
  return out;
}

std::vector<cdsmodel::RatePoint> stub_of(int id) {
  switch (id) {
  case 0:
    return points_of(kStub0Dates.data(), kStub0Rates.data(), kStub0Count);
  case 1:
    return points_of(kStub1Dates.data(), kStub1Rates.data(), kStub1Count);
  case 2:
    return points_of(kStub2Dates.data(), kStub2Rates.data(), kStub2Count);
  default:
    return points_of(kStub3Dates.data(), kStub3Rates.data(), kStub3Count);
  }
}

std::vector<cdsmodel::RatePoint> cash_quotes_of(int id) {
  switch (id) {
  case 0:
    return points_of(kCashQuotes0Dates.data(), kCashQuotes0Rates.data(), kCashQuotes0Count);
  case 1:
    return points_of(kCashQuotes1Dates.data(), kCashQuotes1Rates.data(), kCashQuotes1Count);
  case 2:
    return points_of(kCashQuotes2Dates.data(), kCashQuotes2Rates.data(), kCashQuotes2Count);
  case 3:
    return points_of(kCashQuotes3Dates.data(), kCashQuotes3Rates.data(), kCashQuotes3Count);
  default:
    return points_of(kCashQuotes4Dates.data(), kCashQuotes4Rates.data(), kCashQuotes4Count);
  }
}

std::vector<cdsmodel::RatePoint> swap_strip_of(int id) {
  switch (id) {
  case 0:
    return points_of(kSwapStrip0Dates.data(), kSwapStrip0Rates.data(), kSwapStrip0Count);
  case 1:
    return points_of(kSwapStrip1Dates.data(), kSwapStrip1Rates.data(), kSwapStrip1Count);
  default:
    return points_of(kSwapStrip2Dates.data(), kSwapStrip2Rates.data(), kSwapStrip2Count);
  }
}

std::optional<cdsmodel::ZeroCurve> input_curve(int stub) {
  const std::vector<cdsmodel::RatePoint> points = stub_of(stub);
  return cdsmodel::ZeroCurve::create(cdsmodel::from_tdate(kInstrumentsBaseDate), points, 1.0,
                                     cdsmodel::DayCount::Act365Fixed);
}

TEST(CurveInstrumentsGolden, ReplaysRecordedCashCurves) {
  ASSERT_GE(kCashCurveCases.size(), 50u);
  int matched = 0;
  for (const CashCurveCase &c : kCashCurveCases) {
    const std::optional<cdsmodel::ZeroCurve> in = input_curve(c.stub);
    ASSERT_TRUE(in.has_value());
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value());

    const std::optional<cdsmodel::ZeroCurve> got =
        cdsmodel::curve_with_cash(*in, cash_quotes_of(c.quotes), *dcc);
    ASSERT_EQ(got.has_value(), c.accepted) << "stub " << c.stub << " quotes " << c.quotes;
    if (!c.accepted) {
      continue;
    }
    const std::span<const cdsmodel::RatePoint> points = got->points();
    ASSERT_EQ(points.size(), c.point_count);
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedInstrumentPoint &want = kCashCurvePoints[c.first_point + i];
      EXPECT_EQ(cdsmodel::to_tdate(points[i].date), want.date) << "date " << i;
      EXPECT_NEAR(want.rate, points[i].rate,
                  kCashCurveTolerance * std::max(1.0, std::abs(want.rate)))
          << "rate " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 50);
}

TEST(CurveInstrumentsGolden, ReplaysRecordedSwapCurves) {
  ASSERT_GE(kSwapCurveCases.size(), 50u);
  int matched = 0;
  for (const SwapCurveCase &c : kSwapCurveCases) {
    const std::optional<cdsmodel::ZeroCurve> in = input_curve(c.stub);
    ASSERT_TRUE(in.has_value());
    const std::optional<cdsmodel::BadDayConv> conv = cdsmodel::bad_day_conv_from_upstream(c.conv);
    ASSERT_TRUE(conv.has_value());

    const std::vector<cdsmodel::RatePoint> strip = swap_strip_of(c.strip);
    std::vector<cdsmodel::Date> maturities;
    std::vector<double> rates;
    for (const cdsmodel::RatePoint &p : strip) {
      maturities.push_back(p.date);
      rates.push_back(p.rate);
    }

    const std::optional<cdsmodel::ZeroCurve> got =
        cdsmodel::curve_with_swaps(*in, std::nullopt, maturities, rates,
                                   {.fixed_frequency = c.frequency,
                                    .float_frequency = c.frequency,
                                    .interp = cdsmodel::InterpType::FlatForwards,
                                    .bad_day = *conv},
                                   cdsmodel::Calendar::none());
    ASSERT_EQ(got.has_value(), c.accepted)
        << "stub " << c.stub << " strip " << c.strip << " freq " << c.frequency;
    if (!c.accepted) {
      continue;
    }
    const std::span<const cdsmodel::RatePoint> points = got->points();
    ASSERT_EQ(points.size(), c.point_count);
    for (std::size_t i = 0; i < c.point_count; ++i) {
      const RecordedInstrumentPoint &want = kSwapCurvePoints[c.first_point + i];
      EXPECT_EQ(cdsmodel::to_tdate(points[i].date), want.date) << "date " << i;
      EXPECT_NEAR(want.rate, points[i].rate, kSwapCurveTolerance) << "rate " << i;
    }
    ++matched;
  }
  EXPECT_GT(matched, 60);
}

// ---------------------------------------------- properties, no oracle needed

// A stub point survives the cash merge only if it is OUTSIDE the cash range.
//
// I first asserted that every stub point survives, and the recorded data said
// otherwise -- which is the property test doing its job on its author.
// `JpmcdsZCAddPrefixCurve` copies the points BEFORE the cash curve's first
// date and `JpmcdsZCAddSuffixCurve` the ones AFTER its last; nothing copies
// the middle. So a caller's curve point that happens to fall inside the cash
// range is silently discarded, and that is upstream's behaviour, reproduced.
TEST(CurveInstrumentsGolden, AStubPointSurvivesOnlyOutsideTheCashRange) {
  int interior_dropped = 0;
  int outside_kept = 0;
  for (const CashCurveCase &c : kCashCurveCases) {
    if (!c.accepted || c.point_count == 0) {
      continue;
    }
    const std::vector<cdsmodel::RatePoint> quotes = cash_quotes_of(c.quotes);
    const cdsmodel::TDate first_quote = cdsmodel::to_tdate(quotes.front().date);
    const cdsmodel::TDate last_quote = cdsmodel::to_tdate(quotes.back().date);

    for (const cdsmodel::RatePoint &stub_point : stub_of(c.stub)) {
      const cdsmodel::TDate date = cdsmodel::to_tdate(stub_point.date);
      bool found = false;
      for (std::size_t i = 0; i < c.point_count; ++i) {
        if (kCashCurvePoints[c.first_point + i].date == date) {
          found = true;
        }
      }
      if (date < first_quote || date > last_quote) {
        EXPECT_TRUE(found) << "a stub point outside the cash range was dropped";
        ++outside_kept;
      } else {
        EXPECT_FALSE(found) << "a stub point inside the cash range survived";
        ++interior_dropped;
      }
    }
  }
  EXPECT_GT(outside_kept, 50);
  EXPECT_GT(interior_dropped, 0)
      << "no interior stub point was tested, so the rule is only half checked";
}

// Every produced curve is strictly increasing in date with finite rates --
// which a merge of two curves can break without any single rate looking wrong.
TEST(CurveInstrumentsGolden, EveryProducedCurveIsStrictlyIncreasing) {
  int checked = 0;
  const auto check = [&](const auto &cases, const auto &pool) {
    for (const auto &c : cases) {
      if (!c.accepted) {
        continue;
      }
      for (std::size_t i = 0; i < c.point_count; ++i) {
        const RecordedInstrumentPoint &p = pool[c.first_point + i];
        EXPECT_TRUE(std::isfinite(p.rate));
        EXPECT_GT(p.rate, -1.0);
        if (i > 0) {
          EXPECT_GT(p.date, pool[c.first_point + i - 1].date) << "not increasing at " << i;
        }
        ++checked;
      }
    }
  };
  check(kCashCurveCases, kCashCurvePoints);
  check(kSwapCurveCases, kSwapCurvePoints);
  EXPECT_GT(checked, 400);
}

// No instruments: the curve comes back unchanged. Upstream takes that shortcut
// before validating anything, which is why the port does too.
TEST(CurveInstrumentsGolden, NoInstrumentsReturnsTheCurveUnchanged) {
  const std::optional<cdsmodel::ZeroCurve> in = input_curve(1);
  ASSERT_TRUE(in.has_value());

  const std::optional<cdsmodel::ZeroCurve> cash =
      cdsmodel::curve_with_cash(*in, {}, cdsmodel::DayCount::Act360);
  ASSERT_TRUE(cash.has_value());
  EXPECT_EQ(cash->points().size(), in->points().size());

  const std::optional<cdsmodel::ZeroCurve> swaps = cdsmodel::curve_with_swaps(
      *in, std::nullopt, {}, {}, {.interp = cdsmodel::InterpType::FlatForwards},
      cdsmodel::Calendar::none());
  ASSERT_TRUE(swaps.has_value());
  EXPECT_EQ(swaps->points().size(), in->points().size());
}

// The two validators in one upstream file disagree about equal dates: the cash
// one permits them, the swaps one rejects them. Pinned without the oracle.
TEST(CurveInstrumentsGolden, TheSwapsPathRejectsEqualMaturities) {
  const std::optional<cdsmodel::ZeroCurve> in = input_curve(0);
  ASSERT_TRUE(in.has_value());
  const cdsmodel::Date same = *cdsmodel::make_date(2026, 1, 15);
  const std::vector<cdsmodel::Date> dup{same, same};
  const std::vector<double> rates{0.028, 0.028};
  EXPECT_FALSE(cdsmodel::curve_with_swaps(*in, std::nullopt, dup, rates,
                                          {.interp = cdsmodel::InterpType::FlatForwards},
                                          cdsmodel::Calendar::none())
                   .has_value());
  // And a mismatched count is refused, which upstream cannot detect at all:
  // it takes one length for two arrays.
  const std::vector<double> one{0.028};
  EXPECT_FALSE(cdsmodel::curve_with_swaps(*in, std::nullopt, dup, one,
                                          {.interp = cdsmodel::InterpType::FlatForwards},
                                          cdsmodel::Calendar::none())
                   .has_value());
}

} // namespace
