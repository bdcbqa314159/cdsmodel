// TIER 2 -- live differential tests for the top-level IR curve build.
//
// This is the function a caller actually reaches, so the sweep is over the
// shapes a caller supplies: money market only, swaps only, both, interleaved,
// lowercase letters, and the degenerate empty list.
//
// Swap rates are solved, so curve rates compare at the solver tier. A separate
// test checks that a money-market-only curve matches to ULPs, since nothing is
// solved there and the loose tolerance would otherwise hide a drift.
//
// One test asserts a divergence: UPSTREAM-35, `toupper` on a signed `char`.
//
// The three-parallel-arrays-to-one-span change has NO test, deliberately.
// Upstream's signature would accept `instrNames` of length 1 with `nInstr` of
// 7 and read six bytes past the end; the port takes one span of structs, so
// there is no second length to disagree with. A test for that would only be
// able to assert that a struct has the members it is declared with, which is
// a tautology dressed as a test. The header carries it instead.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/ir_zero_curve.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "ldate.h"
#include "tcurve.h"
#include "zerocurve.h"
}

namespace {

class IrZeroCurveDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

constexpr double kSolvedRateTolerance = 1.0e-7;

struct RefTC {
  TCurve *tc = nullptr;
  ~RefTC() { JpmcdsFreeTCurve(tc); }
  RefTC() = default;
  explicit RefTC(TCurve *owned) : tc(owned) {}
  RefTC(const RefTC &) = delete;
  RefTC &operator=(const RefTC &) = delete;
  explicit operator bool() const { return tc != nullptr; }
};

void expect_curves_match(const TCurve *ref, const cdsmodel::ZeroCurve &got, const char *what) {
  const std::span<const cdsmodel::RatePoint> points = got.points();
  ASSERT_EQ(ref->fNumItems, static_cast<int>(points.size())) << what;
  EXPECT_EQ(ref->fBaseDate, cdsmodel::to_tdate(got.base_date())) << what << " base date";
  for (int i = 0; i < ref->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->fArray[i].fDate, cdsmodel::to_tdate(points[k].date)) << what << " date " << i;
    EXPECT_NEAR(ref->fArray[i].fRate, points[k].rate, kSolvedRateTolerance)
        << what << " rate " << i;
  }
}

/// One benchmark list, expressed both ways.
struct Benchmarks {
  std::string names;
  std::vector<TDate> dates;
  std::vector<double> rates;
  std::vector<cdsmodel::InstrumentQuote> quotes;

  Benchmarks(const std::string &codes, const std::vector<std::pair<cdsmodel::Date, double>> &data) {
    names = codes;
    for (std::size_t i = 0; i < data.size(); ++i) {
      dates.push_back(cdsmodel::to_tdate(data[i].first));
      rates.push_back(data[i].second);
      const cdsmodel::Expected<cdsmodel::InstrumentKind> kind =
          cdsmodel::instrument_kind_from_code(codes[i]);
      quotes.push_back(cdsmodel::InstrumentQuote{
          kind.value_or(cdsmodel::InstrumentKind::MoneyMarket), data[i].first, data[i].second});
    }
  }
};

const std::vector<std::pair<cdsmodel::Date, double>> &mixed() {
  static const std::vector<std::pair<cdsmodel::Date, double>> v{
      {d(2024, 4, 15), 0.0230}, {d(2024, 7, 15), 0.0245}, {d(2025, 1, 15), 0.0260},
      {d(2026, 1, 15), 0.0280}, {d(2027, 1, 15), 0.0295}, {d(2029, 1, 15), 0.0320},
      {d(2031, 1, 15), 0.0340}};
  return v;
}

TEST_F(IrZeroCurveDifferential, MatchesAcrossInstrumentMixesAndConventions) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  int compared = 0;
  int refused_by_both = 0;

  // Every split of the seven benchmarks into a money-market prefix and a swap
  // suffix, which is the only arrangement the dates allow -- plus the two
  // degenerate ends.
  for (std::size_t cash_count = 0; cash_count <= mixed().size(); ++cash_count) {
    std::string codes(mixed().size(), 'S');
    for (std::size_t i = 0; i < cash_count; ++i) {
      codes[i] = 'M';
    }
    const Benchmarks bench(codes, mixed());

    for (const cdsmodel::DayCount mm_dcc :
         {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed}) {
      for (const long freq : {1L, 2L, 4L}) {
        for (const cdsmodel::BadDayConv conv :
             {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
          std::vector<TDate> raw_dates = bench.dates;
          std::vector<double> raw_rates = bench.rates;
          std::string raw_names = bench.names;
          RefTC out(JpmcdsBuildIRZeroCurve(
              cdsmodel::to_tdate(value_date), raw_names.data(), raw_dates.data(), raw_rates.data(),
              static_cast<long>(raw_dates.size()), cdsmodel::to_upstream_code(mm_dcc), freq, freq,
              JPMCDS_ACT_360, JPMCDS_ACT_360, cdsmodel::to_upstream_code(conv),
              const_cast<char *>("NONE")));

          const cdsmodel::Expected<cdsmodel::ZeroCurve> got =
              cdsmodel::build_ir_zero_curve(value_date, bench.quotes, mm_dcc,
                                            {.fixed_frequency = freq,
                                             .float_frequency = freq,
                                             .interp = cdsmodel::InterpType::FlatForwards,
                                             .bad_day = conv},
                                            cdsmodel::Calendar::none());

          ASSERT_EQ(static_cast<bool>(out), got.has_value())
              << "acceptance differs: " << codes << " freq " << freq;
          if (out) {
            expect_curves_match(out.tc, *got, codes.c_str());
            ++compared;
          } else {
            ++refused_by_both;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 70) << "the mix sweep compared almost nothing";
}

// Lowercase is accepted, which is the only reason toupper is in the function.
TEST_F(IrZeroCurveDifferential, LowercaseInstrumentCodesAreAccepted) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const Benchmarks upper("MMMSSSS", mixed());
  const Benchmarks lower("mmmssss", mixed());

  std::vector<TDate> ud = upper.dates, ld = lower.dates;
  std::vector<double> ur = upper.rates, lr = lower.rates;
  std::string un = upper.names, ln = lower.names;

  RefTC a(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), un.data(), ud.data(), ur.data(), 7,
                                 JPMCDS_ACT_360, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360,
                                 JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")));
  RefTC b(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), ln.data(), ld.data(), lr.data(), 7,
                                 JPMCDS_ACT_360, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360,
                                 JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")));
  ASSERT_TRUE(a);
  ASSERT_TRUE(b) << "upstream stopped accepting lowercase instrument codes";
  ASSERT_EQ(a.tc->fNumItems, b.tc->fNumItems);
  for (int i = 0; i < a.tc->fNumItems; ++i) {
    EXPECT_EQ(a.tc->fArray[i].fDate, b.tc->fArray[i].fDate) << i;
    EXPECT_DOUBLE_EQ(a.tc->fArray[i].fRate, b.tc->fArray[i].fRate) << i;
  }

  // The port's parser agrees on all four characters and refuses the rest.
  EXPECT_EQ(cdsmodel::instrument_kind_from_code('M'), cdsmodel::InstrumentKind::MoneyMarket);
  EXPECT_EQ(cdsmodel::instrument_kind_from_code('m'), cdsmodel::InstrumentKind::MoneyMarket);
  EXPECT_EQ(cdsmodel::instrument_kind_from_code('S'), cdsmodel::InstrumentKind::Swap);
  EXPECT_EQ(cdsmodel::instrument_kind_from_code('s'), cdsmodel::InstrumentKind::Swap);
  for (const char code : {'X', 'x', '0', ' ', '\0'}) {
    EXPECT_FALSE(cdsmodel::instrument_kind_from_code(code).has_value()) << "code " << int{code};
  }
}

// A money-market-only curve involves no solve, so it should match to ULPs
// rather than to the solver tolerance the swap cases need.
TEST_F(IrZeroCurveDifferential, AMoneyMarketOnlyCurveMatchesToUlps) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const std::vector<std::pair<cdsmodel::Date, double>> cash{
      {d(2024, 4, 15), 0.0230}, {d(2024, 7, 15), 0.0245}, {d(2025, 1, 15), 0.0260}};
  const Benchmarks bench("MMM", cash);
  std::vector<TDate> dates = bench.dates;
  std::vector<double> rates = bench.rates;
  std::string names = bench.names;

  for (const cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
                                       cdsmodel::DayCount::Thirty360}) {
    RefTC out(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), names.data(), dates.data(),
                                     rates.data(), 3, cdsmodel::to_upstream_code(dcc), 2, 2,
                                     JPMCDS_ACT_360, JPMCDS_ACT_360, JPMCDS_BAD_DAY_NONE,
                                     const_cast<char *>("NONE")));
    const cdsmodel::Expected<cdsmodel::ZeroCurve> got = cdsmodel::build_ir_zero_curve(
        value_date, bench.quotes, dcc, {.interp = cdsmodel::InterpType::FlatForwards},
        cdsmodel::Calendar::none());
    ASSERT_EQ(static_cast<bool>(out), got.has_value());
    ASSERT_TRUE(out);
    ASSERT_EQ(out.tc->fNumItems, static_cast<int>(got->points().size()));
    for (int i = 0; i < out.tc->fNumItems; ++i) {
      const auto k = static_cast<std::size_t>(i);
      EXPECT_NEAR(out.tc->fArray[i].fRate, got->points()[k].rate,
                  4.0 * std::max(1.0, std::abs(out.tc->fArray[i].fRate)) *
                      std::numeric_limits<double>::epsilon())
          << "rate " << i << " is not a bit-level match";
    }
  }
}

// An empty instrument list fails on BOTH sides -- upstream through
// JpmcdsMallocSafe(0), which returns NULL by design and reports bytes rather
// than instruments.
TEST_F(IrZeroCurveDifferential, AnEmptyInstrumentListIsRefusedByBoth) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  TDate date = cdsmodel::to_tdate(d(2025, 1, 15));
  double rate = 0.03;
  char name = 'M';
  EXPECT_EQ(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), &name, &date, &rate, 0,
                                   JPMCDS_ACT_360, 2, 2, JPMCDS_ACT_360, JPMCDS_ACT_360,
                                   JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")),
            nullptr)
      << "upstream started accepting an empty instrument list";
  EXPECT_FALSE(cdsmodel::build_ir_zero_curve(value_date, {}, cdsmodel::DayCount::Act360,
                                             {.interp = cdsmodel::InterpType::FlatForwards},
                                             cdsmodel::Calendar::none())
                   .has_value());
}

// An unknown instrument letter is refused by both.
TEST_F(IrZeroCurveDifferential, AnUnknownInstrumentCodeIsRefusedByBoth) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const std::vector<std::pair<cdsmodel::Date, double>> data{{d(2024, 4, 15), 0.023},
                                                            {d(2026, 1, 15), 0.028}};
  std::vector<TDate> dates{cdsmodel::to_tdate(data[0].first), cdsmodel::to_tdate(data[1].first)};
  std::vector<double> rates{data[0].second, data[1].second};
  std::string names = "MX";
  EXPECT_EQ(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), names.data(), dates.data(),
                                   rates.data(), 2, JPMCDS_ACT_360, 2, 2, JPMCDS_ACT_360,
                                   JPMCDS_ACT_360, JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")),
            nullptr);
  // The port cannot be handed an unknown kind: InstrumentKind is an enum and
  // the parser refuses the letter before a quote can be built.
  EXPECT_FALSE(cdsmodel::instrument_kind_from_code('X').has_value());
}

// ------------------------------------------------------------ UPSTREAM-35

// `toupper` receives a negative value for any high-bit byte, which C does not
// permit. Measured on the argument rather than on an outcome: there is no
// outcome, because the instrument check rejects the character immediately
// after the undefined behaviour has already happened.
TEST_F(IrZeroCurveDifferential, Upstream35ToupperReceivesANegativeChar) {
  // The precondition: `char` is signed, so a high-bit byte is negative.
  EXPECT_LT(static_cast<int>(static_cast<char>(0xE9)), 0)
      << "char is unsigned on this platform, so UPSTREAM-35 does not arise here";
  EXPECT_NE(static_cast<int>(static_cast<char>(0xE9)), EOF)
      << "the one negative value C permits is EOF";

  // And the call is reached: upstream refuses the list, but only after
  // toupper has already been handed the value.
  const cdsmodel::Date value_date = d(2024, 1, 15);
  std::vector<TDate> dates{cdsmodel::to_tdate(d(2024, 4, 15)), cdsmodel::to_tdate(d(2026, 1, 15))};
  std::vector<double> rates{0.023, 0.028};
  char names[2] = {'M', static_cast<char>(0xE9)};
  EXPECT_EQ(JpmcdsBuildIRZeroCurve(cdsmodel::to_tdate(value_date), names, dates.data(),
                                   rates.data(), 2, JPMCDS_ACT_360, 2, 2, JPMCDS_ACT_360,
                                   JPMCDS_ACT_360, JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")),
            nullptr);

  // The port's parser takes the same byte and is defined for it.
  EXPECT_FALSE(cdsmodel::instrument_kind_from_code(static_cast<char>(0xE9)).has_value());
  for (int byte = 0; byte < 256; ++byte) {
    const char code = static_cast<char>(byte);
    const cdsmodel::Expected<cdsmodel::InstrumentKind> kind =
        cdsmodel::instrument_kind_from_code(code);
    const bool expected = code == 'M' || code == 'm' || code == 'S' || code == 's';
    EXPECT_EQ(kind.has_value(), expected) << "byte " << byte;
  }
}

} // namespace
