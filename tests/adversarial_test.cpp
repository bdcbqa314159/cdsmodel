// TIER 1 -- adversarial input harness over the port's own seams.
//
// Why this exists, separately from the differential suite:
//
//   * Differential testing compares the port to the oracle. Where BOTH are
//     undefined it passes while both produce garbage. That is how a signed
//     overflow in parse_date_interval survived a green suite -- upstream's
//     atoi overflowed identically, so the two "agreed".
//   * About a third of the port's functions have no upstream counterpart at
//     all: to_tdate, from_tdate, to_upstream_code, weekend_from_upstream,
//     includes, and the string preprocessor. There is no oracle for these.
//   * Recorded coverage is only what was swept by hand. The gaps are
//     invisible by construction.
//
// So this asserts INVARIANTS rather than agreement, and feeds hostile input
// rather than plausible input. Run it under the asan preset: for the totality
// properties the sanitizers are the assertion, and a silent pass here with
// sanitizers off proves much less.
//
// The generator is seeded with a fixed constant. A failure is reproducible,
// and the suite cannot go intermittently red.

#include <cdsmodel/bsearch.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;

/// Fixed seed: hostile but reproducible.
constexpr std::uint64_t kSeed = 0xC0FFEEULL;

/// The widest TDate range the port claims to support: upstream's epoch year
/// through well past any plausible maturity. Used as the domain for the
/// round-trip properties; extremes are probed separately below.
constexpr cdsmodel::TDate kSweepMin = 0;
constexpr cdsmodel::TDate kSweepMax = 300000; // ~2422

/// Bytes chosen to break parsers: the characters that drive upstream's
/// branches, plus whitespace, control codes, digits, signs and high bytes
/// (which are negative on platforms where char is signed).
std::string hostile_string(std::mt19937_64 &rng, std::size_t max_len) {
  static const std::string alphabet =
      "ABEFNDMSQYWUO0123456789/-+ \t\n\r\v\f.,:;*%#\x01\x7f\x80\xfe\xff";
  std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
  std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
  std::string s;
  const std::size_t n = len_dist(rng);
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(alphabet[pick(rng)]);
  }
  return s;
}

// =========================================================================
// Totality: these must return for ANY input, without undefined behaviour.
// Under the asan preset the sanitizers do the asserting.
// =========================================================================

TEST(Adversarial, DayCountParserIsTotalOverHostileStrings) {
  std::mt19937_64 rng{kSeed};
  for (int i = 0; i < 20000; ++i) {
    const std::string s = hostile_string(rng, 48);
    const auto code = cdsmodel::parse_day_count_code(s);
    if (code.has_value()) {
      // A returned code must be one upstream recognises, or one of the two
      // documented non-calculable keywords. Nothing else may escape.
      const bool known = cdsmodel::from_upstream_code(*code).has_value() ||
                         *code == cdsmodel::kDayCountDefaultCode ||
                         *code == cdsmodel::kDayCountNoneCode;
      EXPECT_TRUE(known) << "escaped an unknown code " << *code << " for \"" << s << "\"";
    }
    // Formatting any long must also be total, and never return empty.
    EXPECT_FALSE(cdsmodel::format_day_count_code(code.value_or(0)).empty());
  }
}

TEST(Adversarial, DayCountFormatterIsTotalOverArbitraryCodes) {
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<long> dist(std::numeric_limits<long>::min(),
                                           std::numeric_limits<long>::max());
  for (int i = 0; i < 20000; ++i) {
    const long code = dist(rng);
    EXPECT_FALSE(cdsmodel::format_day_count_code(code).empty()) << "code " << code;
  }
  // And the boundary values explicitly, which a uniform draw will never hit.
  for (long code :
       {std::numeric_limits<long>::min(), -1L, 0L, 1L, 8L, 9L, std::numeric_limits<long>::max()}) {
    EXPECT_FALSE(cdsmodel::format_day_count_code(code).empty()) << "code " << code;
  }
}

TEST(Adversarial, IntervalParserIsTotalOverHostileStrings) {
  std::mt19937_64 rng{kSeed};
  for (int i = 0; i < 20000; ++i) {
    const std::string s = hostile_string(rng, 48);
    const auto iv = cdsmodel::parse_date_interval(s);
    if (iv.has_value()) {
      // The canonical form is days or months, nothing else.
      EXPECT_TRUE(iv->unit == cdsmodel::PeriodUnit::Days ||
                  iv->unit == cdsmodel::PeriodUnit::Months)
          << "escaped a non-canonical unit for \"" << s << "\"";
      // Formatting what we just parsed must also be total.
      EXPECT_FALSE(cdsmodel::format_date_interval(*iv).empty());
    }
  }
}

TEST(Adversarial, IntervalParserIsTotalOverEveryDigitRunLength) {
  // The length axis specifically: upstream's buffer is 32 bytes and ours
  // bounds at 31, so the interesting lengths are near that boundary -- and
  // every one of them must be handled, not just the ones someone thought of.
  for (std::size_t n = 0; n <= 64; ++n) {
    for (const char *suffix : {"M", "D", "Y", "W", "X", ""}) {
      const std::string s = std::string(n, '9') + suffix;
      const auto iv = cdsmodel::parse_date_interval(s);
      if (iv.has_value()) {
        EXPECT_LE(n, 31u) << "accepted a " << n << "-digit run: \"" << s << "\"";
      }
    }
    // Leading zeros stay within int range however long the run is, so these
    // exercise the digit budget independently of the magnitude check.
    const std::string zeros = std::string(n, '0') + "1M";
    const auto iv = cdsmodel::parse_date_interval(zeros);
    EXPECT_EQ(iv.has_value(), n + 1 <= 31u) << n << " leading zeros";
  }
}

TEST(Adversarial, IntervalConstructorIsTotalOverEveryByteAndExtremeCounts) {
  for (int c = -128; c <= 127; ++c) {
    const char type = static_cast<char>(c);
    for (int periods : {std::numeric_limits<int>::min(), -1000000, -1, 0, 1, 1000000,
                        std::numeric_limits<int>::max()}) {
      // Must return, and must not multiply into overflow for the types that
      // scale (A/Y x12, S x6, Q x3, W x7, U x28).
      const auto iv = cdsmodel::make_date_interval(periods, type);
      if (iv.has_value()) {
        EXPECT_TRUE(iv->unit == cdsmodel::PeriodUnit::Days ||
                    iv->unit == cdsmodel::PeriodUnit::Months);
      }
    }
  }
}

// =========================================================================
// Round trips across the oracle-less interop seam.
// =========================================================================

TEST(Adversarial, TDateRoundTripsAcrossTheWholeSupportedRange) {
  for (cdsmodel::TDate t = kSweepMin; t <= kSweepMax; ++t) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    ASSERT_EQ(cdsmodel::to_tdate(d), t) << "to_tdate(from_tdate(" << t << ")) diverged";
  }
}

TEST(Adversarial, YmdRoundTripsForEveryRepresentableDate) {
  for (cdsmodel::TDate t = kSweepMin; t <= kSweepMax; t += 7) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    const year_month_day ymd = cdsmodel::to_ymd(d);
    const auto rebuilt = cdsmodel::make_date(static_cast<int>(ymd.year()),
                                             static_cast<int>(static_cast<unsigned>(ymd.month())),
                                             static_cast<int>(static_cast<unsigned>(ymd.day())));
    ASSERT_TRUE(rebuilt.has_value()) << "to_ymd produced an unbuildable date at " << t;
    EXPECT_EQ(cdsmodel::to_tdate(*rebuilt), t) << "ymd round trip at " << t;
  }
}

TEST(Adversarial, DayCountCodesRoundTripBothWays) {
  for (cdsmodel::DayCount dcc :
       {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
        cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360,
        cdsmodel::DayCount::EffectiveRate}) {
    EXPECT_EQ(cdsmodel::from_upstream_code(cdsmodel::to_upstream_code(dcc)), dcc);
    EXPECT_EQ(cdsmodel::parse_day_count(cdsmodel::to_string(dcc)), dcc);
  }
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<long> dist(-1000, 1000);
  for (int i = 0; i < 5000; ++i) {
    const long code = dist(rng);
    if (const auto dcc = cdsmodel::from_upstream_code(code)) {
      EXPECT_EQ(cdsmodel::to_upstream_code(*dcc), code) << "code " << code;
    }
  }
}

TEST(Adversarial, WeekendMasksRoundTripForEveryRepresentableValue) {
  for (long flags = -300; flags <= 300; ++flags) {
    const auto w = cdsmodel::weekend_from_upstream(flags);
    EXPECT_EQ(w.has_value(), flags >= 0 && flags <= 0x7F) << "flags " << flags;
    if (w.has_value()) {
      EXPECT_EQ(cdsmodel::to_upstream_weekends(*w), flags);
    }
  }
}

// =========================================================================
// Postconditions. These say what a result MEANS, so a plausible-looking but
// wrong answer is caught rather than merely not crashing.
// =========================================================================

TEST(Adversarial, BracketPostconditionsHoldForHostileDomains) {
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<int> size_dist(0, 24);
  std::uniform_real_distribution<double> val_dist(-1e6, 1e6);

  for (int iter = 0; iter < 5000; ++iter) {
    // Strictly increasing domains, since that is bracket's precondition.
    const int n = size_dist(rng);
    std::vector<double> xs;
    double x = val_dist(rng);
    for (int i = 0; i < n; ++i) {
      xs.push_back(x);
      x += std::uniform_real_distribution<double>(1e-9, 100.0)(rng);
    }
    const std::span<const double> view{xs};

    for (int probe = 0; probe < 6; ++probe) {
      const double desired = val_dist(rng);
      const auto b = cdsmodel::bracket(view, desired);
      if (!b.has_value()) {
        EXPECT_TRUE(xs.empty()) << "rejected a valid domain of size " << n;
        continue;
      }
      ASSERT_LT(b->lo, xs.size()) << "lo out of range, n=" << n;
      ASSERT_LT(b->hi, xs.size()) << "hi out of range, n=" << n;
      if (xs.size() == 1) {
        EXPECT_EQ(b->lo, 0u);
        EXPECT_EQ(b->hi, 0u);
      } else {
        EXPECT_LT(b->lo, b->hi) << "lo must precede hi, n=" << n;
      }
    }
  }
}

TEST(Adversarial, DaysInMonthIsAlwaysBetween28And31) {
  for (int y = 1601; y <= 2400; ++y) {
    for (unsigned m = 1; m <= 12; ++m) {
      const unsigned n = cdsmodel::days_in_month(year{y} / month{m});
      EXPECT_GE(n, 28u) << y << "-" << m;
      EXPECT_LE(n, 31u) << y << "-" << m;
    }
  }
}

TEST(Adversarial, EndOfMonthStaysInItsMonthAndIsTheLastDay) {
  for (cdsmodel::TDate t = 1; t <= kSweepMax; t += 13) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    const cdsmodel::Date eom = cdsmodel::end_of_month(d);
    const year_month_day a = cdsmodel::to_ymd(d);
    const year_month_day b = cdsmodel::to_ymd(eom);

    ASSERT_EQ(a.year(), b.year()) << "eom left the year at " << t;
    ASSERT_EQ(a.month(), b.month()) << "eom left the month at " << t;
    EXPECT_GE(eom, d) << "eom moved backwards at " << t;
    EXPECT_EQ(static_cast<unsigned>(b.day()), cdsmodel::days_in_month(a.year() / a.month()))
        << "eom is not the last day at " << t;
    // One day later must be a different month.
    EXPECT_NE(cdsmodel::to_ymd(eom + days{1}).month(), a.month()) << "at " << t;
  }
}

TEST(Adversarial, YearFractionAndDaysDiffAreAntisymmetric) {
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<cdsmodel::TDate> dist(1, kSweepMax);
  constexpr cdsmodel::DayCount kAll[] = {
      cdsmodel::DayCount::Act365,     cdsmodel::DayCount::Act365Fixed,
      cdsmodel::DayCount::Act360,     cdsmodel::DayCount::Thirty360,
      cdsmodel::DayCount::ThirtyE360, cdsmodel::DayCount::EffectiveRate};

  for (int i = 0; i < 20000; ++i) {
    const cdsmodel::Date a = cdsmodel::from_tdate(dist(rng));
    const cdsmodel::Date b = cdsmodel::from_tdate(dist(rng));
    for (cdsmodel::DayCount dcc : kAll) {
      const auto fwd = cdsmodel::year_fraction(a, b, dcc);
      const auto rev = cdsmodel::year_fraction(b, a, dcc);
      ASSERT_EQ(fwd.has_value(), rev.has_value());
      if (fwd.has_value()) {
        EXPECT_DOUBLE_EQ(*fwd, -*rev)
            << "year_fraction not antisymmetric, dcc=" << cdsmodel::to_upstream_code(dcc);
        // Same date must be exactly zero, not merely small.
        if (a == b) {
          EXPECT_EQ(*fwd, 0.0);
        }
      }
      const auto dfwd = cdsmodel::days_diff(a, b, dcc);
      const auto drev = cdsmodel::days_diff(b, a, dcc);
      ASSERT_EQ(dfwd.has_value(), drev.has_value());
      if (dfwd.has_value()) {
        EXPECT_EQ(*dfwd, -*drev) << "days_diff not antisymmetric, dcc="
                                 << cdsmodel::to_upstream_code(dcc);
      }
    }
  }
}

TEST(Adversarial, ActualDayCountsNeverExceedTheCalendarSpan) {
  // An actual/* numerator must equal the real elapsed days; a 30/360 one must
  // not wander implausibly far from it. Catches a sign or scale slip that
  // antisymmetry alone would not.
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<cdsmodel::TDate> dist(1, kSweepMax);
  for (int i = 0; i < 20000; ++i) {
    const cdsmodel::TDate ta = dist(rng);
    const cdsmodel::TDate tb = dist(rng);
    const cdsmodel::Date a = cdsmodel::from_tdate(ta);
    const cdsmodel::Date b = cdsmodel::from_tdate(tb);
    const long actual = tb - ta;

    for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act365, cdsmodel::DayCount::Act365Fixed,
                                   cdsmodel::DayCount::Act360}) {
      EXPECT_EQ(*cdsmodel::days_diff(a, b, dcc), actual)
          << "actual day count diverged from the calendar span";
    }
    // 30/360 counts at most 30 days per month, so it can never exceed the
    // actual span by more than the number of months spanned.
    const long thirty = *cdsmodel::days_diff(a, b, cdsmodel::DayCount::Thirty360);
    EXPECT_LE(std::abs(thirty), std::abs(actual) + 62)
        << "30/360 numerator implausibly far from the actual span";
    EXPECT_EQ(thirty < 0, actual < 0) << "30/360 sign disagrees with the span";
  }
}

TEST(Adversarial, CalendarInvariantsHoldForRandomHolidaySets) {
  std::mt19937_64 rng{kSeed};
  std::uniform_int_distribution<cdsmodel::TDate> day_dist(1, 60000);
  std::uniform_int_distribution<int> count_dist(0, 40);
  std::uniform_int_distribution<long> mask_dist(0, 0x7F);

  for (int iter = 0; iter < 2000; ++iter) {
    // Strictly increasing holiday dates, since that is the precondition.
    std::vector<cdsmodel::Date> holidays;
    cdsmodel::TDate t = day_dist(rng);
    const int n = count_dist(rng);
    for (int i = 0; i < n; ++i) {
      holidays.push_back(cdsmodel::from_tdate(t));
      t += std::uniform_int_distribution<cdsmodel::TDate>(1, 40)(rng);
    }
    const auto w = cdsmodel::weekend_from_upstream(mask_dist(rng));
    ASSERT_TRUE(w.has_value());

    const auto cal = cdsmodel::Calendar::create(holidays, *w);
    ASSERT_TRUE(cal.has_value()) << "rejected a strictly increasing list of " << n;

    // Stored holidays: strictly increasing, and never on a weekend.
    const std::span<const cdsmodel::Date> kept = cal->holidays();
    EXPECT_TRUE(std::ranges::is_sorted(kept, std::ranges::less{}))
        << "stored holidays are not sorted";
    EXPECT_EQ(std::ranges::adjacent_find(kept), kept.end())
        << "stored holidays contain a duplicate";
    for (cdsmodel::Date h : kept) {
      EXPECT_FALSE(cal->is_weekend(h)) << "a weekend date survived construction";
      EXPECT_TRUE(cal->is_holiday(h)) << "a stored holiday is not reported as one";
      EXPECT_FALSE(cal->is_business_day(h)) << "a stored holiday is a business day";
    }
    EXPECT_LE(kept.size(), holidays.size()) << "construction invented holidays";

    // The predicate identity, over a window around the holiday set.
    for (int i = 0; i < 60; ++i) {
      const cdsmodel::Date d = cdsmodel::from_tdate(day_dist(rng));
      EXPECT_EQ(cal->is_business_day(d), !cal->is_weekend(d) && !cal->is_holiday(d))
          << "is_business_day is not weekend-or-holiday";
    }
  }
}

TEST(Adversarial, AdvanceIsTotalOverExtremeIntervals) {
  // DateInterval is an aggregate, so a caller can fill it with any int
  // without going through make_date_interval. advance must therefore be safe
  // on its own, not merely safe for intervals the constructor produces.
  for (cdsmodel::TDate t : {1, 2, 1000, 148655, 250000}) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    for (int periods : {std::numeric_limits<int>::min(), std::numeric_limits<int>::min() + 1,
                        -2000000000, -100000, -1, 0, 1, 100000, 2000000000,
                        std::numeric_limits<int>::max() - 1, std::numeric_limits<int>::max()}) {
      for (cdsmodel::PeriodUnit unit : {cdsmodel::PeriodUnit::Days, cdsmodel::PeriodUnit::Months}) {
        const auto r = cdsmodel::advance(d, {periods, unit});
        // No claim about the value -- only that it returns without undefined
        // behaviour, and that a success is a date we can round-trip.
        if (r.has_value()) {
          EXPECT_EQ(cdsmodel::from_tdate(cdsmodel::to_tdate(*r)), *r)
              << "advance produced an unrepresentable date, periods=" << periods;
        }
      }
    }
  }
}

TEST(Adversarial, NormalizeIsTotalOverExtremeMonths) {
  for (int year : {1601, 2024, 9999}) {
    for (int month : {std::numeric_limits<int>::min(), -2000000000, -13, 0, 1, 12, 13, 2000000000,
                      std::numeric_limits<int>::max()}) {
      for (int day : {0, 1, 28, 31, 32}) {
        const auto r = cdsmodel::normalize_ymd(year, month, day);
        if (r.has_value()) {
          EXPECT_GE(static_cast<unsigned>(r->month()), 1u);
          EXPECT_LE(static_cast<unsigned>(r->month()), 12u);
          EXPECT_GE(static_cast<unsigned>(r->day()), 1u);
          EXPECT_LE(static_cast<unsigned>(r->day()), 31u);
        }
      }
    }
  }
}

TEST(Adversarial, TDateRepresentabilityIsExplicitAndExact) {
  // The domain is IMPLEMENTATION-DEPENDENT in two independent ways, which an
  // earlier version of this test got wrong by asserting Apple's answer as a
  // literal:
  //
  //   * std::chrono::days::rep is 32-bit on libc++ and 64-bit on libstdc++.
  //   * `long`, and therefore TDate, is 32-bit on Windows and 64-bit
  //     elsewhere.
  //
  // So everything here is a property of the bounds, never a magic number.
  static_assert(cdsmodel::is_representable_tdate(0));
  static_assert(cdsmodel::is_representable_tdate(cdsmodel::kMaxTDate));
  static_assert(cdsmodel::is_representable_tdate(cdsmodel::kMinTDate));

  // Just outside -- but only where "just outside" is expressible without
  // overflowing TDate itself, which it is not when the bound already sits at
  // the type's own limit.
  if (cdsmodel::kMaxTDate < std::numeric_limits<cdsmodel::TDate>::max()) {
    EXPECT_FALSE(cdsmodel::is_representable_tdate(cdsmodel::kMaxTDate + 1));
  }
  if (cdsmodel::kMinTDate > std::numeric_limits<cdsmodel::TDate>::min()) {
    EXPECT_FALSE(cdsmodel::is_representable_tdate(cdsmodel::kMinTDate - 1));
  }

  // The range is asymmetric on every platform: every conversion carries the
  // 1601-to-1970 offset, so the lower bound is shifted while the upper is not.
  EXPECT_NE(cdsmodel::kMinTDate, -cdsmodel::kMaxTDate - 1)
      << "the domain looks symmetric, which would mean the epoch offset was ignored";
  EXPECT_LT(cdsmodel::kMinTDate, cdsmodel::TDate{0});
  EXPECT_GT(cdsmodel::kMaxTDate, cdsmodel::TDate{0});

  // Every real date is comfortably inside, whatever the platform decided.
  // TDate 300000 is the year 2422; this is the claim that actually matters.
  EXPECT_TRUE(cdsmodel::is_representable_tdate(0));
  EXPECT_TRUE(cdsmodel::is_representable_tdate(300000));

  // Round trip holds exactly across the whole domain, bounds included -- the
  // property the named limits exist to guarantee.
  for (cdsmodel::TDate t : {cdsmodel::TDate{0}, cdsmodel::TDate{1}, cdsmodel::TDate{300000},
                            cdsmodel::TDate{100000000}, cdsmodel::kMaxTDate - 1,
                            cdsmodel::kMaxTDate, cdsmodel::kMinTDate, cdsmodel::kMinTDate + 1}) {
    ASSERT_TRUE(cdsmodel::is_representable_tdate(t)) << "at " << t;
    EXPECT_EQ(cdsmodel::to_tdate(cdsmodel::from_tdate(t)), t) << "round trip at " << t;
  }
}

TEST(Adversarial, IntervalScalingRejectsCountsThatWouldOverflow) {
  // 'Y' scales by 12, 'U' by 28. Upstream wraps silently here.
  EXPECT_FALSE(cdsmodel::make_date_interval(std::numeric_limits<int>::max(), 'Y').has_value());
  EXPECT_FALSE(cdsmodel::make_date_interval(std::numeric_limits<int>::min(), 'Y').has_value());
  EXPECT_FALSE(cdsmodel::make_date_interval(999999999, 'Y').has_value());
  EXPECT_FALSE(cdsmodel::make_date_interval(std::numeric_limits<int>::max(), 'U').has_value());

  // The largest count that still fits, and one past it.
  EXPECT_TRUE(cdsmodel::make_date_interval(std::numeric_limits<int>::max() / 12, 'Y').has_value());
  EXPECT_FALSE(
      cdsmodel::make_date_interval(std::numeric_limits<int>::max() / 12 + 1, 'Y').has_value());
  // Unscaled types accept the full range.
  EXPECT_TRUE(cdsmodel::make_date_interval(std::numeric_limits<int>::max(), 'M').has_value());
  EXPECT_TRUE(cdsmodel::make_date_interval(std::numeric_limits<int>::min(), 'D').has_value());
}

TEST(Adversarial, AdvanceByZeroIsIdentityWhereItSucceeds) {
  for (cdsmodel::TDate t = 1; t <= kSweepMax; t += 101) {
    const cdsmodel::Date d = cdsmodel::from_tdate(t);
    for (cdsmodel::PeriodUnit unit : {cdsmodel::PeriodUnit::Days, cdsmodel::PeriodUnit::Months}) {
      const auto same = cdsmodel::advance(d, {0, unit});
      ASSERT_TRUE(same.has_value()) << "advance by zero failed at " << t;
      EXPECT_EQ(cdsmodel::to_tdate(*same), t) << "advance by zero moved the date at " << t;
    }
  }
}

} // namespace
