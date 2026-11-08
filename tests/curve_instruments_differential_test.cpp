// TIER 2 -- live differential tests for adding instruments to a zero curve.
//
// The two entry points are what a caller actually drives, so the sweeps are
// over the shapes a caller supplies: a stub curve present and absent, quotes
// before and after the stub's end, benchmark strips that overlap the stub and
// strips that do not.
//
// Swap rates are ROOTS, so curve rates compare at the solver tier, as in
// swap_bootstrap_differential_test.cpp. Cash rates are not solved and compare
// tightly.
//
// One test asserts a divergence: UPSTREAM-34, where both validators detect a
// NULL curve and then dereference it. Its live reproducers are DISABLED.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/curve_instruments.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "gtozc.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

class CurveInstrumentsDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

/// Swap rates are solved, so this is the solver tier. Cash rates are not, but
/// one tolerance for the whole curve is simpler than two and the cash points
/// agree far inside it -- asserted separately below.
constexpr double kCurveRateTolerance = 1.0e-7;

struct RefTC {
  TCurve *tc = nullptr;
  ~RefTC() { JpmcdsFreeTCurve(tc); }
  RefTC() = default;
  explicit RefTC(TCurve *owned) : tc(owned) {}
  RefTC(const RefTC &) = delete;
  RefTC &operator=(const RefTC &) = delete;
  explicit operator bool() const { return tc != nullptr; }
};

/// Build the same curve on both sides.
struct CurvePair {
  RefTC ref;
  cdsmodel::Expected<cdsmodel::ZeroCurve> port;

  CurvePair(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points)
      : port(cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not built")) {
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (const cdsmodel::RatePoint &p : points) {
      dates.push_back(cdsmodel::to_tdate(p.date));
      rates.push_back(p.rate);
    }
    ref.tc = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.empty() ? nullptr : dates.data(),
                              rates.empty() ? nullptr : rates.data(),
                              static_cast<int>(points.size()), 1.0, JPMCDS_ACT_365F);
    port = cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  }
};

void expect_curves_match(const TCurve *ref, const cdsmodel::ZeroCurve &got, const char *what) {
  const std::span<const cdsmodel::RatePoint> points = got.points();
  ASSERT_EQ(ref->fNumItems, static_cast<int>(points.size())) << what;
  EXPECT_EQ(ref->fBaseDate, cdsmodel::to_tdate(got.base_date())) << what << " base date";
  for (int i = 0; i < ref->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->fArray[i].fDate, cdsmodel::to_tdate(points[k].date)) << what << " date " << i;
    EXPECT_NEAR(ref->fArray[i].fRate, points[k].rate, kCurveRateTolerance) << what << " rate " << i;
  }
}

// ------------------------------------------------------------------- cash

TEST_F(CurveInstrumentsDifferential, CashMatchesWithAndWithoutAStubCurve) {
  const cdsmodel::Date base = d(2024, 1, 15);
  int compared = 0;
  const std::vector<std::vector<cdsmodel::RatePoint>> stubs{
      // An EMPTY input curve is not in this sweep: the port's `ZeroCurve` is
      // non-empty by construction (decided in the zero_curve slice), so
      // building from nothing goes through ZeroCurveBuilder instead. Upstream
      // handles it here with an explicit `if (zeroCurve->fNumItems > 0)`.
      {{d(2024, 2, 15), 0.0220}},                           // one point, before the cash
      {{d(2024, 2, 15), 0.0220}, {d(2025, 7, 15), 0.0270}}, // straddles the cash
      {{d(2026, 1, 15), 0.0300}},                           // entirely after the cash
  };
  const std::vector<std::vector<cdsmodel::RatePoint>> quote_sets{
      {{d(2024, 4, 15), 0.0230}},
      {{d(2024, 4, 15), 0.0230}, {d(2024, 7, 15), 0.0245}, {d(2025, 1, 15), 0.0260}},
      {{d(2024, 2, 20), 0.0215}, {d(2024, 3, 20), 0.0225}},
  };

  for (const auto &stub : stubs) {
    for (const auto &quotes : quote_sets) {
      for (const cdsmodel::DayCount dcc :
           {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
            cdsmodel::DayCount::Thirty360}) {
        CurvePair pair(base, stub);
        ASSERT_TRUE(pair.ref);
        ASSERT_TRUE(pair.port.has_value());

        std::vector<TDate> raw_dates;
        std::vector<double> raw_rates;
        for (const cdsmodel::RatePoint &q : quotes) {
          raw_dates.push_back(cdsmodel::to_tdate(q.date));
          raw_rates.push_back(q.rate);
        }
        RefTC out(JpmcdsZCCash(pair.ref.tc, raw_dates.data(), raw_rates.data(),
                               static_cast<int>(quotes.size()), cdsmodel::to_upstream_code(dcc)));
        const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
            cdsmodel::curve_with_cash(*pair.port, quotes, dcc);

        ASSERT_EQ(static_cast<bool>(out), got.has_value()) << "acceptance differs";
        if (out) {
          expect_curves_match(out.tc, *got, "cash");
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 25) << "the cash sweep compared almost nothing";
}

// No quotes: the curve comes back unchanged, and upstream takes that shortcut
// BEFORE validating -- so a day count it would otherwise reject is accepted.
TEST_F(CurveInstrumentsDifferential, NoCashQuotesReturnsTheCurveUnvalidated) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair pair(base, {{d(2024, 4, 15), 0.023}});
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  TDate date = cdsmodel::to_tdate(d(2024, 4, 15));
  double rate = 0.023;
  // 9999 is not a day count convention JpmcdsDayCountValid accepts.
  RefTC out(JpmcdsZCCash(pair.ref.tc, &date, &rate, 0, 9999));
  ASSERT_TRUE(out) << "upstream validated despite having nothing to add";
  const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
      cdsmodel::curve_with_cash(*pair.port, {}, cdsmodel::DayCount::Act360);
  ASSERT_TRUE(got.has_value());
  expect_curves_match(out.tc, *got, "empty cash");
}

// Cash rates are not solved, so they should agree far more tightly than the
// curve-wide tolerance admits. Asserted so a real drift cannot hide inside the
// slack the swap solve needs.
TEST_F(CurveInstrumentsDifferential, CashRatesAgreeFarInsideTheSolverTolerance) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair pair(base, {{d(2024, 2, 15), 0.0220}});
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  const std::vector<cdsmodel::RatePoint> quotes{
      {d(2024, 4, 15), 0.0230}, {d(2024, 7, 15), 0.0245}, {d(2025, 1, 15), 0.0260}};
  std::vector<TDate> raw_dates;
  std::vector<double> raw_rates;
  for (const cdsmodel::RatePoint &q : quotes) {
    raw_dates.push_back(cdsmodel::to_tdate(q.date));
    raw_rates.push_back(q.rate);
  }
  RefTC out(JpmcdsZCCash(pair.ref.tc, raw_dates.data(), raw_rates.data(), 3, JPMCDS_ACT_360));
  ASSERT_TRUE(out);
  const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
      cdsmodel::curve_with_cash(*pair.port, quotes, cdsmodel::DayCount::Act360);
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(out.tc->fNumItems, static_cast<int>(got->points().size()));
  for (int i = 0; i < out.tc->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_NEAR(out.tc->fArray[i].fRate, got->points()[k].rate,
                4.0 * std::max(1.0, std::abs(out.tc->fArray[i].fRate)) *
                    std::numeric_limits<double>::epsilon())
        << "cash rate " << i << " is not a bit-level match";
  }
}

// ------------------------------------------------------------------ swaps

TEST_F(CurveInstrumentsDifferential, SwapsMatchAcrossStripsAndStubOverlap) {
  const cdsmodel::Date base = d(2024, 1, 15);
  int compared = 0;
  const std::vector<std::vector<cdsmodel::RatePoint>> stubs{
      {{d(2024, 4, 15), 0.0230}, {d(2025, 1, 15), 0.0260}},
      {{d(2024, 4, 15), 0.0230}, {d(2027, 1, 15), 0.0310}}, // overlaps the strip
  };
  const std::vector<std::vector<cdsmodel::Date>> strips{
      {d(2026, 1, 15), d(2027, 1, 15), d(2029, 1, 15), d(2031, 1, 15)},
      {d(2026, 4, 20), d(2027, 7, 20), d(2029, 10, 20)}, // off cycle
  };
  const std::vector<double> rates{0.0280, 0.0295, 0.0320, 0.0340};

  for (const auto &stub : stubs) {
    for (const auto &strip : strips) {
      for (const long freq : {1L, 2L, 4L}) {
        for (const cdsmodel::BadDayConv conv :
             {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
          CurvePair pair(base, stub);
          ASSERT_TRUE(pair.ref);
          ASSERT_TRUE(pair.port.has_value());

          std::vector<TDate> raw_dates;
          for (const cdsmodel::Date date : strip) {
            raw_dates.push_back(cdsmodel::to_tdate(date));
          }
          const std::span<const double> strip_rates{rates.data(), strip.size()};
          std::vector<double> raw_rates(strip_rates.begin(), strip_rates.end());

          RefTC out(JpmcdsZCSwaps(pair.ref.tc, nullptr, raw_dates.data(), raw_rates.data(),
                                  static_cast<int>(strip.size()), static_cast<int>(freq),
                                  static_cast<int>(freq), JPMCDS_ACT_360, JPMCDS_ACT_360, 'M',
                                  static_cast<TBadDayAndStubPos>(cdsmodel::to_upstream_code(conv)),
                                  const_cast<char *>("NONE")));

          const cdsmodel::SwapConventions conventions{.fixed_frequency = freq,
                                                      .float_frequency = freq,
                                                      .interp = cdsmodel::InterpType::FlatForwards,
                                                      .bad_day = conv};
          const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
              cdsmodel::curve_with_swaps(*pair.port, std::nullopt, strip, strip_rates, conventions,
                                         cdsmodel::Calendar::none());

          ASSERT_EQ(static_cast<bool>(out), got.has_value()) << "acceptance differs, freq " << freq;
          if (out) {
            expect_curves_match(out.tc, *got, "swaps");
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 15) << "the swaps sweep compared almost nothing";
}

// The skip loop uses `<`, so a benchmark maturing exactly on the stub's last
// date is KEPT. Asserted, because the boundary is what decides whether
// UPSTREAM-33's trigger is reachable from here.
TEST_F(CurveInstrumentsDifferential, ABenchmarkOnTheStubsLastDateIsKept) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const cdsmodel::Date boundary = d(2027, 1, 15);
  CurvePair pair(base, {{d(2024, 4, 15), 0.0230}, {boundary, 0.0310}});
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  const std::vector<cdsmodel::Date> strip{boundary, d(2029, 1, 15)};
  const std::vector<double> rates{0.0300, 0.0320};
  std::vector<TDate> raw_dates{cdsmodel::to_tdate(strip[0]), cdsmodel::to_tdate(strip[1])};
  std::vector<double> raw_rates = rates;

  RefTC out(JpmcdsZCSwaps(pair.ref.tc, nullptr, raw_dates.data(), raw_rates.data(), 2, 2, 2,
                          JPMCDS_ACT_360, JPMCDS_ACT_360, 'M',
                          static_cast<TBadDayAndStubPos>(JPMCDS_BAD_DAY_NONE),
                          const_cast<char *>("NONE")));
  const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::curve_with_swaps(
      *pair.port, std::nullopt, strip, rates, {.interp = cdsmodel::InterpType::FlatForwards},
      cdsmodel::Calendar::none());
  ASSERT_EQ(static_cast<bool>(out), got.has_value());
  if (out) {
    expect_curves_match(out.tc, *got, "boundary");
    // The boundary benchmark was not dropped, so the curve grew past it.
    EXPECT_GT(out.tc->fArray[out.tc->fNumItems - 1].fDate, cdsmodel::to_tdate(boundary));
  }
}

TEST_F(CurveInstrumentsDifferential, NoSwapsReturnsTheCurveUnchanged) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair pair(base, {{d(2024, 4, 15), 0.023}});
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  TDate date = cdsmodel::to_tdate(d(2026, 1, 15));
  double rate = 0.03;
  RefTC out(JpmcdsZCSwaps(pair.ref.tc, nullptr, &date, &rate, 0, 2, 2, JPMCDS_ACT_360,
                          JPMCDS_ACT_360, 'M', static_cast<TBadDayAndStubPos>(JPMCDS_BAD_DAY_NONE),
                          const_cast<char *>("NONE")));
  ASSERT_TRUE(out);
  const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::curve_with_swaps(
      *pair.port, std::nullopt, {}, {}, {.interp = cdsmodel::InterpType::FlatForwards},
      cdsmodel::Calendar::none());
  ASSERT_TRUE(got.has_value());
  expect_curves_match(out.tc, *got, "empty swaps");
}

// The two validators in one file disagree about equal dates: the cash one
// permits them (`>`), the swaps one rejects them (`<=`). Both reproduced.
TEST_F(CurveInstrumentsDifferential, TheTwoValidatorsDisagreeAboutEqualDates) {
  const cdsmodel::Date base = d(2024, 1, 15);
  const cdsmodel::Date same = d(2025, 1, 15);
  CurvePair pair(base, {{d(2024, 2, 15), 0.0220}});
  ASSERT_TRUE(pair.ref);
  ASSERT_TRUE(pair.port.has_value());

  TDate dup[2] = {cdsmodel::to_tdate(same), cdsmodel::to_tdate(same)};
  double two[2] = {0.026, 0.026};

  // Cash: equal dates pass the ORDER check (the curve may still refuse them
  // later, so compare acceptance rather than assuming success).
  RefTC cash(JpmcdsZCCash(pair.ref.tc, dup, two, 2, JPMCDS_ACT_360));
  const std::vector<cdsmodel::RatePoint> quotes{{same, 0.026}, {same, 0.026}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> got_cash =
      cdsmodel::curve_with_cash(*pair.port, quotes, cdsmodel::DayCount::Act360);
  EXPECT_EQ(static_cast<bool>(cash), got_cash.has_value())
      << "the cash path disagrees about duplicate dates";

  // Swaps: equal dates are rejected outright by the validator.
  RefTC swaps(JpmcdsZCSwaps(pair.ref.tc, nullptr, dup, two, 2, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360,
                            'M', static_cast<TBadDayAndStubPos>(JPMCDS_BAD_DAY_NONE),
                            const_cast<char *>("NONE")));
  EXPECT_FALSE(static_cast<bool>(swaps)) << "the swaps validator now permits equal dates";
  const std::vector<cdsmodel::Date> dup_dates{same, same};
  const std::vector<double> dup_rates{0.026, 0.026};
  EXPECT_FALSE(cdsmodel::curve_with_swaps(*pair.port, std::nullopt, dup_dates, dup_rates,
                                          {.interp = cdsmodel::InterpType::FlatForwards},
                                          cdsmodel::Calendar::none())
                   .has_value());
}

// ------------------------------------------------------------ UPSTREAM-34

// The count-zero shortcut runs before validation, so a NULL curve is handled
// there -- by JpmcdsCopyCurve, which tolerates it. With a positive count the
// validator is reached and dereferences the NULL it just reported.
TEST_F(CurveInstrumentsDifferential, Upstream34ANullCurveIsSafeOnlyWithNoInstruments) {
  TDate date = cdsmodel::to_tdate(d(2025, 1, 15));
  double rate = 0.03;
  EXPECT_EQ(JpmcdsZCCash(nullptr, &date, &rate, 0, JPMCDS_ACT_360), nullptr)
      << "the zero-count shortcut stopped tolerating a NULL curve";
  EXPECT_EQ(JpmcdsZCSwaps(nullptr, nullptr, &date, &rate, 0, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360,
                          'M', static_cast<TBadDayAndStubPos>(JPMCDS_BAD_DAY_NONE),
                          const_cast<char *>("NONE")),
            nullptr);
  // The port cannot express the input: both take a reference.
}

TEST_F(CurveInstrumentsDifferential, DISABLED_Upstream34CashValidatorDereferencesNull) {
  TDate dates[2] = {cdsmodel::to_tdate(d(2025, 1, 15)), cdsmodel::to_tdate(d(2026, 1, 15))};
  double rates[2] = {0.03, 0.035};
  // CheckZCCashInputs reports the NULL curve and then evaluates
  // `dateArr[i] <= zeroCurve->fBaseDate`. UBSan: member access within null
  // pointer, gtozc.c:345. ASan: SEGV on 0x10. Never enabled.
  JpmcdsFreeTCurve(JpmcdsZCCash(nullptr, dates, rates, 2, JPMCDS_ACT_360));
}

TEST_F(CurveInstrumentsDifferential, DISABLED_Upstream34SwapsValidatorDereferencesNull) {
  TDate dates[2] = {cdsmodel::to_tdate(d(2025, 1, 15)), cdsmodel::to_tdate(d(2026, 1, 15))};
  double rates[2] = {0.03, 0.035};
  // CheckZCSwapsInputs reports the NULL curve and then calls
  // `JpmcdsIsBusinessDay(zeroCurve->fBaseDate, ...)` unconditionally.
  // gtozc.c:464. Never enabled.
  JpmcdsFreeTCurve(JpmcdsZCSwaps(
      nullptr, nullptr, dates, rates, 2, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360, 'M',
      static_cast<TBadDayAndStubPos>(JPMCDS_BAD_DAY_NONE), const_cast<char *>("NONE")));
}

} // namespace
