// TIER 1 -- the curve builder. The invariant first, then replay.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include "golden/zero_curve_builder_cases.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace {

using cdsmodel::DayCount;
using cdsmodel::ZeroCurveBuilder;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

ZeroCurveBuilder make(long basis = 1, DayCount dcc = DayCount::Act365Fixed) {
  return *ZeroCurveBuilder::create(d(2024, 1, 1), 16, basis, dcc);
}

/// The tolerance for a comparison against a RECORDED value.
///
/// Recorded vectors crossed a compiler and a machine on the way here, so the
/// 4-ULP tier does not apply to them. That tier is for two implementations
/// running in the same binary under the same flags, which is the differential
/// suite -- there, contraction is identical on both sides and 4 ULPs is the
/// right bar. Here it is not, and CI said so: Ubuntu release passed while
/// Ubuntu asan failed on the same machine with the same libm, which can only
/// be the flags. `-ffp-contract` turns `a + b*c` into an FMA and changes the
/// value MID-EXPRESSION, so the interpolation feeding `pow` is already a
/// different double before any library function is called.
///
/// So these use the project's accumulating tier, relative 1e-12. Worst
/// measured drift across the four CI platforms was 6.8e-15 relative, on a
/// forward rate -- the end of a chain of interpolate, pow, ratio, reciprocal,
/// pow.
///
/// Relative, not `4 * max(1.0, |want|) * eps`. That earlier form was an
/// absolute tolerance anchored at 1.0, which quietly permitted ~200 ULPs on a
/// discount factor of 0.02 while demanding ~7 on a rate of 6.5 -- loose where
/// it claimed to be tight, and tight where the arithmetic could not deliver.
void expect_recorded(double got, double want, const std::string &where) {
  constexpr double kRelative = 1.0e-12;
  // A recorded zero has to be reproduced as a zero; there is no relative
  // tolerance around it.
  if (want == 0.0) {
    EXPECT_EQ(got, 0.0) << where;
    return;
  }
  EXPECT_NEAR(got, want, kRelative * std::fabs(want)) << where;
}

// =====================================================================
// The invariant: this type caches a discount factor beside every rate,
// so the two must agree for every stored point, by every route in.
// =====================================================================

TEST(ZeroCurveBuilder, EveryStoredPointsRateAndDiscountAgree) {
  int checked = 0;
  for (long basis : {1L, 2L, 4L, 12L, 365L, 5000L}) {
    for (DayCount dcc : {DayCount::Act365Fixed, DayCount::Act360}) {
      ZeroCurveBuilder b = make(basis, dcc);
      for (int years : {1, 3, 7, 12, 20}) {
        const cdsmodel::Date at = d(2024 + years, 1, 1);
        if (!b.add_rate(at, 0.03 + 0.001 * years)) {
          continue;
        }
      }
      for (const ZeroCurveBuilder::Point &p : b.points()) {
        const std::optional<double> recomputed = b.compute_discount(p.date, p.rate);
        ASSERT_TRUE(recomputed.has_value())
            << "basis " << basis << " at " << cdsmodel::to_tdate(p.date);
        EXPECT_EQ(*recomputed, p.discount)
            << "basis " << basis << " at " << cdsmodel::to_tdate(p.date)
            << ": the cached discount does not match the stored rate";
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 40) << checked;
}

TEST(ZeroCurveBuilder, TheInvariantHoldsWhenPointsArriveAsDiscountFactors) {
  // add_discount_factor recovers the rate, so the pair must still agree --
  // and the recovered rate must reproduce the factor it came from.
  ZeroCurveBuilder b = make();
  for (int years : {1, 3, 7}) {
    const cdsmodel::Date at = d(2024 + years, 1, 1);
    const double factor = std::pow(1.04, -static_cast<double>(years));
    ASSERT_TRUE(b.add_discount_factor(at, factor)) << years;
  }
  ASSERT_EQ(b.points().size(), 3u);
  for (const ZeroCurveBuilder::Point &p : b.points()) {
    const std::optional<double> recomputed = b.compute_discount(p.date, p.rate);
    ASSERT_TRUE(recomputed.has_value());
    // A round trip through the rate, so relative rather than exact.
    EXPECT_NEAR(*recomputed, p.discount, 1.0e-12 * std::max(1.0, std::fabs(p.discount)));
  }
}

TEST(ZeroCurveBuilder, PointsAreAlwaysSortedByDateHoweverTheyArrive) {
  ZeroCurveBuilder b = make();
  // Pathologically unsorted.
  for (int years : {20, 1, 12, 3, 7, 2, 15, 5}) {
    ASSERT_TRUE(b.add_rate(d(2024 + years, 1, 1), 0.03 + 0.001 * years)) << years;
  }
  ASSERT_EQ(b.points().size(), 8u);
  for (std::size_t i = 1; i < b.points().size(); ++i) {
    EXPECT_LT(b.points()[i - 1].date, b.points()[i].date) << "i=" << i;
  }
}

TEST(ZeroCurveBuilder, ADuplicateIsIdempotentWithinOneTenthOfAMicron) {
  // Upstream's tolerance is a hardcoded 1e-7 on the RATE, not the discount.
  ZeroCurveBuilder b = make();
  const cdsmodel::Date at = d(2029, 1, 1);
  ASSERT_TRUE(b.add_rate(at, 0.05));
  ASSERT_EQ(b.points().size(), 1u);

  EXPECT_TRUE(b.add_rate(at, 0.05));
  EXPECT_TRUE(b.add_rate(at, 0.05 + 9.0e-8)) << "inside 1e-7";
  EXPECT_EQ(b.points().size(), 1u);
  EXPECT_EQ(b.points()[0].rate, 0.05) << "the first value must win, not the last";

  EXPECT_FALSE(b.add_rate(at, 0.05 + 2.0e-7)) << "outside 1e-7 is a conflict";
  EXPECT_EQ(b.points().size(), 1u) << "a rejected conflict must not change anything";
}

TEST(ZeroCurveBuilder, AFailedAddCommitsNothing) {
  // The port's answer to UPSTREAM-16: every route commits only after all its
  // conversions have succeeded, so no failure can leave a point behind.
  ZeroCurveBuilder b = make();
  ASSERT_TRUE(b.add_rate(d(2029, 1, 1), 0.05));
  const std::size_t before = b.points().size();

  // rate <= -basis: rejected by the conversion.
  EXPECT_FALSE(b.add_rate(d(2030, 1, 1), -1.0));
  EXPECT_EQ(b.points().size(), before);
  // A non-positive discount factor has no rate.
  EXPECT_FALSE(b.add_discount_factor(d(2031, 1, 1), 0.0));
  EXPECT_EQ(b.points().size(), before);
  EXPECT_FALSE(b.add_discount_factor(d(2031, 1, 1), -0.5));
  EXPECT_EQ(b.points().size(), before);
  // A rate quoted in terms the conversion cannot handle.
  EXPECT_FALSE(b.add_general_rate(d(2032, 1, 1), -3.0, 2, DayCount::Act365Fixed));
  EXPECT_EQ(b.points().size(), before);
}

TEST(ZeroCurveBuilder, TheTwoDiscountPathsAgreeEverywhereIncludingTheBoundary) {
  // The port's answer to UPSTREAM-17. The fast path is only taken where it
  // agrees with the general one, so no input can make them differ.
  ZeroCurveBuilder fast = make(1, DayCount::Act365Fixed); // fast path eligible
  ZeroCurveBuilder slow = make(2, DayCount::Act365Fixed); // never eligible
  const cdsmodel::Date at = d(2029, 1, 1);

  for (double rate : {0.05, 0.0, -0.5, -0.9, -0.999, -0.9999999}) {
    const std::optional<double> via_fast = fast.compute_discount(at, rate);
    const std::optional<double> via_general =
        cdsmodel::rate_to_discount(rate, d(2024, 1, 1), at, DayCount::Act365Fixed, 1);
    ASSERT_TRUE(via_fast.has_value()) << rate;
    ASSERT_TRUE(via_general.has_value()) << rate;
    EXPECT_EQ(*via_fast, *via_general) << "the fast path diverged at rate " << rate;
  }
  // At the boundary both refuse, where upstream's fast path returns infinity.
  EXPECT_FALSE(fast.compute_discount(at, -1.0).has_value());
  EXPECT_FALSE(slow.compute_discount(at, -2.0).has_value());
  // And nothing infinite can be stored.
  EXPECT_FALSE(fast.add_rate(at, -1.0));
  EXPECT_TRUE(fast.empty());
}

TEST(ZeroCurveBuilder, PrefixTakesOnlyEarlierPointsAndSuffixOnlyLater) {
  const auto seeded = [](long basis, std::initializer_list<int> years) {
    ZeroCurveBuilder b = *ZeroCurveBuilder::create(d(2024, 1, 1), 16, basis, DayCount::Act365Fixed);
    for (int y : years) {
      [[maybe_unused]] const bool ok = b.add_rate(d(2024 + y, 1, 1), 0.03 + 0.001 * y);
    }
    return b;
  };

  ZeroCurveBuilder target = seeded(1, {5, 7, 10});
  const ZeroCurveBuilder source = seeded(1, {1, 2, 3, 5, 12, 15});
  ASSERT_TRUE(target.add_prefix(source));
  // Only 1, 2, 3 precede the target's first date of 2029; 5 collides and 12,
  // 15 are later, so neither is taken.
  EXPECT_EQ(target.points().size(), 6u);
  EXPECT_EQ(target.points().front().date, d(2025, 1, 1));
  EXPECT_EQ(target.points().back().date, d(2034, 1, 1));

  ZeroCurveBuilder other = seeded(1, {5, 7, 10});
  ASSERT_TRUE(other.add_suffix(source));
  // Only 12 and 15 follow the target's last date of 2034.
  EXPECT_EQ(other.points().size(), 5u);
  EXPECT_EQ(other.points().back().date, d(2039, 1, 1));

  // An empty target takes everything from either direction.
  ZeroCurveBuilder empty_prefix = seeded(1, {});
  ASSERT_TRUE(empty_prefix.add_prefix(source));
  EXPECT_EQ(empty_prefix.points().size(), source.points().size());
  ZeroCurveBuilder empty_suffix = seeded(1, {});
  ASSERT_TRUE(empty_suffix.add_suffix(source));
  EXPECT_EQ(empty_suffix.points().size(), source.points().size());
}

TEST(ZeroCurveBuilder, MismatchedValueDatesAreRefused) {
  // Upstream's comment requires the value dates to match and nothing checks
  // it. The port checks it.
  ZeroCurveBuilder a = make();
  ZeroCurveBuilder b = *ZeroCurveBuilder::create(d(2024, 6, 1), 8, 1, DayCount::Act365Fixed);
  ASSERT_TRUE(b.add_rate(d(2030, 1, 1), 0.04));
  EXPECT_FALSE(a.add_prefix(b));
  EXPECT_FALSE(a.add_suffix(b));
}

TEST(ZeroCurveBuilder, RoundTripsThroughAFinishedCurve) {
  ZeroCurveBuilder b = make();
  for (int years : {1, 2, 5, 10}) {
    ASSERT_TRUE(b.add_rate(d(2024 + years, 1, 1), 0.03 + 0.002 * years));
  }
  const std::optional<cdsmodel::ZeroCurve> curve = b.to_zero_curve();
  ASSERT_TRUE(curve.has_value());
  EXPECT_EQ(curve->points().size(), b.points().size());
  EXPECT_EQ(curve->basis(), 1.0);

  const std::optional<ZeroCurveBuilder> back = ZeroCurveBuilder::from_zero_curve(*curve);
  ASSERT_TRUE(back.has_value());
  ASSERT_EQ(back->points().size(), b.points().size());
  for (std::size_t i = 0; i < b.points().size(); ++i) {
    EXPECT_EQ(back->points()[i].date, b.points()[i].date) << "i=" << i;
    EXPECT_EQ(back->points()[i].rate, b.points()[i].rate) << "i=" << i;
    // The discount cache is recomputed on the way back in, as upstream does.
    EXPECT_EQ(back->points()[i].discount, b.points()[i].discount) << "i=" << i;
  }
}

TEST(ZeroCurveBuilder, AnEmptyBuilderYieldsNoCurve) {
  // ZeroCurve::create refuses an empty curve -- see UPSTREAM-15 -- so a
  // builder with nothing in it cannot produce one.
  const ZeroCurveBuilder b = make();
  EXPECT_TRUE(b.empty());
  EXPECT_FALSE(b.to_zero_curve().has_value());
  EXPECT_FALSE(b.find_closest(d(2029, 1, 1)).has_value());
  EXPECT_FALSE(b.find_exact(d(2029, 1, 1)).has_value());
}

// =====================================================================
// Replay.
// =====================================================================

TEST(ZeroCurveBuilder, MatchesRecordedConstructionVerdicts) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kBuilderCreateCases) {
    const std::optional<DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    if (!dcc) {
      continue; // a day count the port's enum cannot hold
    }
    const bool accepted =
        ZeroCurveBuilder::create(d(2024, 1, 1), c.expected_points, c.basis, *dcc).has_value();
    EXPECT_EQ(accepted, c.accepted)
        << "n " << c.expected_points << " basis " << c.basis << " dcc " << c.day_count;
    ++checked;
  }
  EXPECT_GT(checked, 100) << checked;
}

TEST(ZeroCurveBuilder, MatchesRecordedBuilderStates) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kBuilderCases) {
    const std::optional<DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value()) << c.day_count;
    std::optional<ZeroCurveBuilder> b =
        ZeroCurveBuilder::create(cdsmodel::from_tdate(c.value_date), 16, c.basis, *dcc);
    ASSERT_TRUE(b.has_value()) << "basis " << c.basis;

    for (std::size_t i = 0; i < c.nb; ++i) {
      const cdsmodel::Date at = cdsmodel::from_tdate(c.dates[i]);
      if (c.mode == 0) {
        (void)b->add_rate(at, c.inputs[i]);
      } else if (c.mode == 1) {
        (void)b->add_discount_factor(at, c.inputs[i]);
      } else {
        const std::optional<DayCount> entry = cdsmodel::from_upstream_code(c.entry_day_count);
        ASSERT_TRUE(entry.has_value());
        (void)b->add_general_rate(at, c.inputs[i], c.entry_basis, *entry);
      }
    }

    const std::string where = "basis " + std::to_string(c.basis) + " dcc " +
                              std::to_string(c.day_count) + " mode " + std::to_string(c.mode) +
                              " entry basis " + std::to_string(c.entry_basis);
    ASSERT_EQ(b->points().size(), c.n) << where;
    for (std::size_t i = 0; i < c.n; ++i) {
      EXPECT_EQ(cdsmodel::to_tdate(b->points()[i].date), c.out_dates[i]) << where << " i=" << i;
      // Rates and cached discounts come out of pow/exp, so the
      // non-accumulating tier applies cross-platform.
      expect_recorded(b->points()[i].rate, c.out_rates[i], where + " rate i=" + std::to_string(i));
      expect_recorded(b->points()[i].discount, c.out_discounts[i],
                      where + " discount i=" + std::to_string(i));
      ++checked;
    }
  }
  EXPECT_GT(checked, 200) << checked;
}

// =====================================================================
// EVALUATION.
// =====================================================================

ZeroCurveBuilder seeded(long basis = 1, DayCount dcc = DayCount::Act365Fixed) {
  ZeroCurveBuilder b = *ZeroCurveBuilder::create(d(2024, 1, 1), 16, basis, dcc);
  for (int years : {1, 2, 3, 5, 7, 10, 15, 20, 30}) {
    [[maybe_unused]] const bool ok = b.add_rate(d(2024 + years, 1, 1), 0.02 + 0.004 * years);
  }
  return b;
}

TEST(ZeroCurveBuilderEval, TheBracketAgreesWithThePortedBinarySearch) {
  // `bracket_dates` searches the points directly, where `bracket` searches a
  // span -- two implementations of one thing, which is the mistake UPSTREAM-17
  // is about. This checks them against each other THROUGH the interpolation,
  // which is the only way the bracket is ever used.
  //
  // Only dates strictly inside the curve and not equal to any stored date, so
  // none of interpolate's early returns fire and the answer depends purely on
  // which pair was chosen.
  const ZeroCurveBuilder b = seeded();
  std::vector<cdsmodel::TDate> dates;
  for (const ZeroCurveBuilder::Point &p : b.points()) {
    dates.push_back(cdsmodel::to_tdate(p.date));
  }
  ASSERT_GE(dates.size(), 2u);

  int checked = 0;
  for (cdsmodel::TDate at = dates.front() + 1; at < dates.back(); at += 7) {
    if (std::ranges::find(dates, at) != dates.end()) {
      continue;
    }
    const std::optional<cdsmodel::Bracket> want =
        cdsmodel::bracket(std::span<const cdsmodel::TDate>{dates}, static_cast<double>(at));
    ASSERT_TRUE(want.has_value()) << at;
    const ZeroCurveBuilder::Point &lo = b.points()[want->lo];
    const ZeroCurveBuilder::Point &hi = b.points()[want->hi];
    const double expected =
        lo.rate + ((hi.rate - lo.rate) / static_cast<double>(dates[want->hi] - dates[want->lo])) *
                      static_cast<double>(at - dates[want->lo]);

    const std::optional<double> got =
        b.interpolate(cdsmodel::from_tdate(at), cdsmodel::InterpType::Linear);
    ASSERT_TRUE(got.has_value()) << at;
    // Bit-exact: if the bracket differed anywhere, the rate would too.
    EXPECT_EQ(*got, expected) << "at " << at;
    ++checked;
  }
  EXPECT_GT(checked, 1000) << checked;
}

TEST(ZeroCurveBuilderEval, AStoredDateReturnsItsStoredRateExactly) {
  // Upstream short-circuits an exact hit rather than interpolating to it, so
  // the result at a stored date is the stored value and not an arithmetic
  // reconstruction of it.
  for (cdsmodel::InterpType interp :
       {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
    const ZeroCurveBuilder b = seeded();
    for (const ZeroCurveBuilder::Point &p : b.points()) {
      const std::optional<double> got = b.interpolate(p.date, interp);
      ASSERT_TRUE(got.has_value());
      EXPECT_EQ(*got, p.rate) << "at " << cdsmodel::to_tdate(p.date);
    }
  }
}

TEST(ZeroCurveBuilderEval, ExtrapolationIsFlatBackwardsAndSlopedForwards) {
  const ZeroCurveBuilder b = seeded();
  const double first = b.points().front().rate;
  for (int years : {-20, -5, -1, 0}) {
    const std::optional<double> got =
        b.interpolate(d(2025 + years, 1, 1), cdsmodel::InterpType::Linear);
    ASSERT_TRUE(got.has_value()) << years;
    EXPECT_EQ(*got, first) << "backwards extrapolation must be flat, at " << years;
  }
  // Forward is NOT flat -- the last pair's slope continues.
  const double last = b.points().back().rate;
  const std::optional<double> beyond = b.interpolate(d(2070, 1, 1), cdsmodel::InterpType::Linear);
  ASSERT_TRUE(beyond.has_value());
  EXPECT_GT(*beyond, last) << "forward extrapolation follows the final slope";
}

TEST(ZeroCurveBuilderEval, TheValueDateDiscountsByNothingEvenWithNoPoints) {
  const ZeroCurveBuilder empty =
      *ZeroCurveBuilder::create(d(2024, 1, 1), 4, 1, DayCount::Act365Fixed);
  const std::optional<double> at_value_date =
      empty.discount_factor(d(2024, 1, 1), cdsmodel::InterpType::Linear);
  ASSERT_TRUE(at_value_date.has_value()) << "it short-circuits before interpolating";
  EXPECT_EQ(*at_value_date, 1.0);
  // Anywhere else an empty curve has nothing to say.
  EXPECT_FALSE(empty.discount_factor(d(2029, 1, 1), cdsmodel::InterpType::Linear).has_value());
  EXPECT_FALSE(empty.interpolate(d(2029, 1, 1), cdsmodel::InterpType::Linear).has_value());
}

TEST(ZeroCurveBuilderEval, AForwardFromTheValueDateIsJustTheDiscountFactor) {
  // df(valueDate) is 1.0, so the composition has to collapse. Stated as a
  // property because it is the cheapest check that the forward machinery is
  // wired up the right way round.
  const ZeroCurveBuilder b = seeded();
  for (cdsmodel::InterpType interp :
       {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
    for (const cdsmodel::Date at : {d(2026, 4, 1), d(2031, 1, 1), d(2045, 9, 30)}) {
      const std::optional<double> df = b.discount_factor(at, interp);
      const std::optional<double> fwd = b.forward_discount_factor(d(2024, 1, 1), at, interp);
      ASSERT_TRUE(df.has_value());
      ASSERT_TRUE(fwd.has_value());
      EXPECT_EQ(*fwd, *df) << "at " << cdsmodel::to_tdate(at);
    }
    // And a zero-width forward discounts by nothing.
    const std::optional<double> none =
        b.forward_discount_factor(d(2031, 1, 1), d(2031, 1, 1), interp);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(*none, 1.0);
  }
}

TEST(ZeroCurveBuilderEval, TheMoneyMarketRateIsTheSimpleForwardFromTheValueDate) {
  const ZeroCurveBuilder b = seeded();
  for (cdsmodel::InterpType interp :
       {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
    for (DayCount dcc : {DayCount::Act360, DayCount::Act365Fixed}) {
      for (const cdsmodel::Date at : {d(2025, 1, 1), d(2029, 7, 1), d(2040, 1, 1)}) {
        const std::optional<double> mm = b.money_market_rate(at, dcc, interp);
        const std::optional<double> fwd = b.forward_rate(d(2024, 1, 1), at, 0, dcc, interp);
        ASSERT_EQ(mm.has_value(), fwd.has_value());
        if (mm) {
          EXPECT_EQ(*mm, *fwd) << "at " << cdsmodel::to_tdate(at);
        }
      }
    }
  }
}

TEST(ZeroCurveBuilderEval, NothingEvaluatesToNaN) {
  // The headline oracle-free property, and the answer to UPSTREAM-19. Every
  // evaluation either refuses or returns a number. Infinity is allowed --
  // UPSTREAM-15 settled that it is a value, not an error -- but NaN is not,
  // because a double-returning conversion has nowhere to report an error and
  // a NaN that escapes becomes a price.
  int checked = 0;
  for (long basis : {1L, 2L, 12L, 365L, 5000L}) {
    for (DayCount dcc : {DayCount::Act365Fixed, DayCount::Act360}) {
      for (double rate : {-24.0, -1.0, -0.5, 0.0, 0.05, 1.0, 25.0, 1e30}) {
        std::optional<ZeroCurveBuilder> b = ZeroCurveBuilder::create(d(2024, 1, 1), 8, basis, dcc);
        ASSERT_TRUE(b.has_value());
        if (!b->add_rate(d(2054, 1, 1), rate) || !b->add_rate(d(2055, 1, 1), rate)) {
          continue; // refused on the way in, which is also an answer
        }
        for (cdsmodel::InterpType interp :
             {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
          const std::string where = "basis " + std::to_string(basis) + " rate " +
                                    std::to_string(rate) + " interp " +
                                    std::to_string(static_cast<int>(interp));
          for (const cdsmodel::Date at :
               {d(2030, 1, 1), d(2054, 1, 1), d(2054, 7, 1), d(2055, 1, 1), d(2070, 1, 1)}) {
            const std::optional<double> r = b->interpolate(at, interp);
            const std::optional<double> df = b->discount_factor(at, interp);
            const std::optional<double> pv = b->present_value(1e6, at, interp);
            if (r) {
              EXPECT_FALSE(std::isnan(*r)) << where;
            }
            if (df) {
              EXPECT_FALSE(std::isnan(*df)) << where;
              EXPECT_GE(*df, 0.0) << where;
            }
            if (pv) {
              EXPECT_FALSE(std::isnan(*pv)) << where;
            }
            ++checked;
          }
          const std::optional<double> fwd_df =
              b->forward_discount_factor(d(2054, 1, 1), d(2055, 1, 1), interp);
          const std::optional<double> fwd =
              b->forward_rate(d(2054, 1, 1), d(2055, 1, 1), 0, dcc, interp);
          if (fwd_df) {
            EXPECT_TRUE(std::isfinite(*fwd_df)) << where << ": a forward factor must be finite";
            // NOT strictly positive. Measured at basis 365, rate 25: the
            // factor at the far date underflows to zero while the near one is
            // still subnormal, so the ratio is exactly 0 -- a forward over
            // which everything becomes worthless. That is a value, and the
            // first draft of this test asserted it away. Upstream returns the
            // same 0 and then refuses to convert it, which is what the port
            // does too, asserted just below.
            EXPECT_GE(*fwd_df, 0.0) << where;
            if (*fwd_df == 0.0) {
              EXPECT_FALSE(
                  b->forward_rate(d(2054, 1, 1), d(2055, 1, 1), 0, dcc, interp).has_value())
                  << where << ": a zero forward factor has no rate";
            }
          }
          if (fwd) {
            EXPECT_FALSE(std::isnan(*fwd)) << where;
          }
        }
      }
    }
  }
  EXPECT_GT(checked, 200) << checked;
}

TEST(ZeroCurveBuilderEval, MatchesRecordedEvaluations) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kEvalCases) {
    const std::optional<DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    ASSERT_TRUE(dcc.has_value()) << c.day_count;
    const cdsmodel::InterpType interp =
        c.interp == 0 ? cdsmodel::InterpType::Linear : cdsmodel::InterpType::FlatForwards;
    std::optional<ZeroCurveBuilder> b =
        ZeroCurveBuilder::create(cdsmodel::from_tdate(c.value_date), 16, c.basis, *dcc);
    ASSERT_TRUE(b.has_value()) << "basis " << c.basis;
    for (int years : {1, 2, 3, 5, 7, 10, 15, 20, 30}) {
      // The same seeding the generator used, so the recorded answers are
      // answers to the same curve.
      ASSERT_TRUE(b->add_rate(d(2024 + years, 1, 1), 0.02 + 0.004 * years));
    }
    const cdsmodel::Date at = cdsmodel::from_tdate(c.at);
    const std::string where = "basis " + std::to_string(c.basis) + " dcc " +
                              std::to_string(c.day_count) + " interp " + std::to_string(c.interp) +
                              " at " + std::to_string(c.at);

    const std::optional<double> rate = b->interpolate(at, interp);
    ASSERT_EQ(rate.has_value(), c.rate_ok) << where;
    if (rate) {
      expect_recorded(*rate, c.rate, where + " rate");
    }
    const std::optional<double> df = b->discount_factor(at, interp);
    ASSERT_EQ(df.has_value(), c.discount_ok) << where;
    if (df) {
      expect_recorded(*df, c.discount, where + " discount");
    }
    const std::optional<double> fwd =
        b->forward_rate(cdsmodel::from_tdate(c.value_date), at, 0, DayCount::Act365Fixed, interp);
    ASSERT_EQ(fwd.has_value(), c.forward_ok) << where;
    if (fwd) {
      expect_recorded(*fwd, c.forward, where + " forward");
    }
    ++checked;
  }
  EXPECT_GT(checked, 200) << checked;
}

} // namespace
