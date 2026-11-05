// TIER 1 -- the curve builder. The invariant first, then replay.

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/rate_conversion.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <gtest/gtest.h>

#include "golden/zero_curve_builder_cases.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace {

using cdsmodel::DayCount;
using cdsmodel::ZeroCurveBuilder;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

ZeroCurveBuilder make(long basis = 1, DayCount dcc = DayCount::Act365Fixed) {
  return *ZeroCurveBuilder::create(d(2024, 1, 1), 16, basis, dcc);
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
      const double eps = std::numeric_limits<double>::epsilon();
      EXPECT_NEAR(b->points()[i].rate, c.out_rates[i],
                  4.0 * std::max(1.0, std::fabs(c.out_rates[i])) * eps)
          << where << " i=" << i;
      EXPECT_NEAR(b->points()[i].discount, c.out_discounts[i],
                  4.0 * std::max(1.0, std::fabs(c.out_discounts[i])) * eps)
          << where << " i=" << i;
      ++checked;
    }
  }
  EXPECT_GT(checked, 200) << checked;
}

} // namespace
