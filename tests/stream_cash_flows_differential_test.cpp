// TIER 2 -- live differential tests for a swap leg's cash flows.
//
// Three jobs here, and the middle one is unusual.
//
//  1. The ordinary comparison: fixed and floating legs, swept over schedules,
//     principal exchanges, day counts and curves.
//
//  2. PROOF THAT THE DROPPED FIELDS ARE DEAD. The port omits `swapType`,
//     `TStubRates firstCoupon/finalCoupon`, `averSoFar`, `compSoFar` and the
//     fixed path's `valueDate`, on the grounds that upstream never reads them.
//     That is a claim about upstream, so it is tested against upstream: each
//     is swept over values that would matter if it were read, and the output
//     is required not to move. Without these tests the omissions are an
//     assertion; with them they are a measurement.
//
//  3. The degenerate case where the two legs disagree, and UPSTREAM-31.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/coupon_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/float_rate.hpp>
#include <cdsmodel/stream_cash_flows.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cfinanci.h"
#include "cgeneral.h"
#include "convert.h"
#include "fltrate.h"
#include "interp.h"
#include "ldate.h"
#include "schedule.h"
#include "streamcf.h"
#include "tcurve.h"
#include "zr2fwd.h"
}

namespace {

class StreamCashFlowsDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

long upstream_interp(cdsmodel::InterpType t) {
  return t == cdsmodel::InterpType::Linear ? JPMCDS_LINEAR_INTERP : JPMCDS_FLAT_FORWARDS;
}

long upstream_flags(cdsmodel::PrincipalFlows f) {
  long out = 0;
  if (f.add_final) {
    out |= JPMCDS_ADD_FINAL;
  }
  if (f.subtract_initial) {
    out |= JPMCDS_SUBTRACT_INITIAL;
  }
  return out;
}

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

struct RefSchedule {
  TCouponDateList *list = nullptr;
  ~RefSchedule() { JpmcdsFreeCouponDateList(list); }
  RefSchedule() = default;
  RefSchedule(const RefSchedule &) = delete;
  RefSchedule &operator=(const RefSchedule &) = delete;
};

struct RefCurve {
  TCurve *curve = nullptr;
  ~RefCurve() { JpmcdsFreeTCurve(curve); }
  RefCurve() = default;
  RefCurve(const RefCurve &) = delete;
  RefCurve &operator=(const RefCurve &) = delete;
};

struct RefFlows {
  TCashFlowList *cfl = nullptr;
  ~RefFlows() { JpmcdsFreeCFL(cfl); }
  RefFlows() = default;
  RefFlows(const RefFlows &) = delete;
  RefFlows &operator=(const RefFlows &) = delete;
  explicit operator bool() const { return cfl != nullptr; }
};

/// One schedule, built on both sides from the same arguments.
struct SchedulePair {
  RefSchedule ref;
  cdsmodel::Expected<cdsmodel::CouponSchedule> port;

  SchedulePair(cdsmodel::Date start, cdsmodel::Date maturity, int months, bool stub_at_end)
      : port(cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not built")) {
    TDateInterval raw = raw_interval({months, cdsmodel::PeriodUnit::Months});
    ref.list = JpmcdsNewCouponDatesSwap(cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity),
                                        &raw, TRUE, FALSE, 0, 0, stub_at_end ? TRUE : FALSE, FALSE,
                                        0, 0, 0, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                                        JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"));
    port = cdsmodel::coupon_schedule({.start = start,
                                      .maturity = maturity,
                                      .coupon_interval = {months, cdsmodel::PeriodUnit::Months},
                                      .stub_position = stub_at_end ? cdsmodel::StubPosition::Back
                                                                   : cdsmodel::StubPosition::Front},
                                     {}, cdsmodel::Calendar::none());
  }

  [[nodiscard]] bool usable() const {
    return ref.list != nullptr && ref.list->fNumItems > 0 && port.has_value();
  }
};

/// A modest rising curve, on both sides.
struct CurvePair {
  RefCurve ref;
  cdsmodel::Expected<cdsmodel::ZeroCurve> port;

  explicit CurvePair(cdsmodel::Date base)
      : port(cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "not built")) {
    std::vector<cdsmodel::RatePoint> points;
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (int i = 1; i <= 8; ++i) {
      const cdsmodel::Date day = d(2024 + 5 * i, 1, 15);
      const double rate = 0.02 + 0.003 * static_cast<double>(i);
      points.push_back(cdsmodel::RatePoint{day, rate});
      dates.push_back(cdsmodel::to_tdate(day));
      rates.push_back(rate);
    }
    ref.curve = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(), 8, 1.0,
                                 JPMCDS_ACT_365F);
    port = cdsmodel::ZeroCurve::create(base, points, 1.0, cdsmodel::DayCount::Act365Fixed);
  }
};

void expect_flows_match(const TCashFlowList *ref, const std::vector<cdsmodel::CashFlow> &got,
                        const char *what) {
  ASSERT_EQ(ref->fNumItems, static_cast<int>(got.size())) << what;
  for (int i = 0; i < ref->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->fArray[i].fDate, cdsmodel::to_tdate(got[k].date)) << what << " date " << i;
    // One computation compared against itself in the same binary, so ULPs.
    EXPECT_NEAR(ref->fArray[i].fAmount, got[k].amount,
                4.0 * std::max(1.0, std::abs(ref->fArray[i].fAmount)) *
                    std::numeric_limits<double>::epsilon())
        << what << " amount " << i;
  }
}

const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> &spans() {
  static const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> v{
      {d(2024, 1, 15), d(2029, 1, 15)}, // on cycle, no stub
      {d(2024, 1, 15), d(2029, 4, 15)}, // off cycle, one stub
      {d(2024, 2, 29), d(2027, 2, 28)}, // leap day
      {d(2024, 1, 31), d(2026, 7, 31)}, // month ends
  };
  return v;
}

// ---------------------------------------------------------------- fixed leg

TEST_F(StreamCashFlowsDifferential, FixedLegMatchesAcrossSchedulesFlagsAndRates) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const int months : {3, 6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        SchedulePair sched(start, maturity, months, stub_at_end);
        if (!sched.usable()) {
          continue;
        }
        for (const cdsmodel::DayCount dcc :
             {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
              cdsmodel::DayCount::Thirty360}) {
          for (const bool subtract_initial : {false, true}) {
            for (const bool add_final : {false, true}) {
              for (const double fixed_rate : {0.0, 0.04, -0.005}) {
                const cdsmodel::PrincipalFlows flows{.subtract_initial = subtract_initial,
                                                     .add_final = add_final};
                TStreamFixed *rs = JpmcdsNewStreamFixed(
                    sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6, cdsmodel::to_upstream_code(dcc),
                    upstream_flags(flows), 0.03, 0.05, fixed_rate);
                ASSERT_NE(rs, nullptr);
                RefFlows ref;
                ref.cfl = JpmcdsNewStreamFixedCFL(cdsmodel::to_tdate(start), rs);

                const cdsmodel::FixedStream stream{.schedule = *sched.port,
                                                   .principal = 1.0e6,
                                                   .pay_day_count = dcc,
                                                   .principal_flows = flows,
                                                   .first_coupon_rate = 0.03,
                                                   .final_coupon_rate = 0.05,
                                                   .fixed_rate = fixed_rate};
                const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
                    cdsmodel::fixed_stream_cash_flows(stream);

                if (!ref) {
                  EXPECT_FALSE(got.has_value());
                } else {
                  ASSERT_TRUE(got.has_value());
                  expect_flows_match(ref.cfl, *got, "fixed");
                  ++compared;
                }
                JpmcdsFreeTStreamFixed(rs);
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 300) << "the fixed sweep compared almost nothing";
}

TEST_F(StreamCashFlowsDifferential, FixedCouponRatesMatchIncludingStubRates) {
  int compared = 0;
  for (const auto &[start, maturity] : spans()) {
    for (const bool stub_at_end : {false, true}) {
      SchedulePair sched(start, maturity, 6, stub_at_end);
      if (!sched.usable()) {
        continue;
      }
      TStreamFixed *rs = JpmcdsNewStreamFixed(sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6,
                                              JPMCDS_ACT_360, 0, 0.011, 0.022, 0.033);
      ASSERT_NE(rs, nullptr);
      std::vector<double> ref(static_cast<std::size_t>(sched.ref.list->fNumItems), -1.0);
      ASSERT_EQ(JpmcdsGetFixedCouponRates(cdsmodel::to_tdate(start), rs, ref.data()), SUCCESS);

      const cdsmodel::Expected<std::vector<double>> got =
          cdsmodel::fixed_coupon_rates({.schedule = *sched.port,
                                        .principal = 1.0e6,
                                        .pay_day_count = cdsmodel::DayCount::Act360,
                                        .first_coupon_rate = 0.011,
                                        .final_coupon_rate = 0.022,
                                        .fixed_rate = 0.033});
      ASSERT_TRUE(got.has_value());
      ASSERT_EQ(ref.size(), got->size());
      for (std::size_t i = 0; i < ref.size(); ++i) {
        EXPECT_DOUBLE_EQ(ref[i], (*got)[i]) << "rate " << i;
      }
      ++compared;
      JpmcdsFreeTStreamFixed(rs);
    }
  }
  EXPECT_GT(compared, 4);
}

// --------------------------------------------------------------- float leg

TEST_F(StreamCashFlowsDifferential, FloatLegMatchesAcrossSchedulesFlagsAndInterpolation) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair curve(base);
  ASSERT_NE(curve.ref.curve, nullptr);
  ASSERT_TRUE(curve.port.has_value());

  int compared = 0;
  int refused_by_both = 0;
  for (const auto &[start, maturity] : spans()) {
    if (start < base) {
      continue;
    }
    for (const int months : {3, 6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        SchedulePair sched(start, maturity, months, stub_at_end);
        if (!sched.usable()) {
          continue;
        }
        for (const cdsmodel::InterpType interp :
             {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
          for (const bool subtract_initial : {false, true}) {
            for (const bool add_final : {false, true}) {
              for (const int spot : {0, 2}) {
                for (const double spread : {0.0, 0.0025}) {
                  const cdsmodel::PrincipalFlows flows{.subtract_initial = subtract_initial,
                                                       .add_final = add_final};
                  TDateInterval mat = raw_interval({months, cdsmodel::PeriodUnit::Months});
                  TDateInterval pay = raw_interval({months, cdsmodel::PeriodUnit::Months});
                  TFloatRate rfr{};
                  ASSERT_EQ(JpmcdsFloatRateSet(&mat, &pay, JPMCDS_ACT_360, spot, spread, 1.0, &rfr),
                            SUCCESS);
                  TStubRates first{0.0, mat};
                  TStubRates final_{0.0, mat};
                  TStreamFloat *rs = JpmcdsNewStreamFloat(
                      sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6, JPMCDS_ACT_360,
                      upstream_flags(flows), &first, &final_, 0.0, 0.0, &rfr);
                  ASSERT_NE(rs, nullptr);
                  RefFlows ref;
                  ref.cfl =
                      JpmcdsNewStreamFloatCFLGen(curve.ref.curve, upstream_interp(interp), rs,
                                                 JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"));

                  const cdsmodel::FloatStream stream{
                      .schedule = *sched.port,
                      .principal = 1.0e6,
                      .pay_day_count = cdsmodel::DayCount::Act360,
                      .principal_flows = flows,
                      .rate = {.maturity_interval = {months, cdsmodel::PeriodUnit::Months},
                               .pay_interval = {months, cdsmodel::PeriodUnit::Months},
                               .day_count = cdsmodel::DayCount::Act360,
                               .spot_offset = cdsmodel::calendar_day_offset(spot),
                               .spread = spread,
                               .weight = 1.0}};
                  const cdsmodel::Expected<std::vector<cdsmodel::CashFlow>> got =
                      cdsmodel::float_stream_cash_flows(*curve.port, interp, stream,
                                                        cdsmodel::BadDayConv::None,
                                                        cdsmodel::Calendar::none());

                  if (!ref) {
                    EXPECT_FALSE(got.has_value()) << "port accepted what upstream refused";
                    ++refused_by_both;
                  } else {
                    ASSERT_TRUE(got.has_value()) << "port refused what upstream accepted";
                    expect_flows_match(ref.cfl, *got, "float");
                    ++compared;
                  }
                  JpmcdsFreeTStreamFloat(rs);
                }
              }
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 200) << "the float sweep compared almost nothing";
}

TEST_F(StreamCashFlowsDifferential, FloatingCashFlowMatchesOnItsOwn) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair curve(base);
  ASSERT_NE(curve.ref.curve, nullptr);
  ASSERT_TRUE(curve.port.has_value());

  int compared = 0;
  for (const int start_months : {0, 6, 24, 60}) {
    for (const int tenor_months : {3, 6, 12}) {
      const cdsmodel::Expected<cdsmodel::Date> index_start = cdsmodel::advance(
          base, {start_months == 0 ? 1 : start_months, cdsmodel::PeriodUnit::Months});
      ASSERT_TRUE(index_start.has_value());
      const cdsmodel::Expected<cdsmodel::Date> index_mat =
          cdsmodel::advance(*index_start, {tenor_months, cdsmodel::PeriodUnit::Months});
      ASSERT_TRUE(index_mat.has_value());
      for (const cdsmodel::InterpType interp :
           {cdsmodel::InterpType::Linear, cdsmodel::InterpType::FlatForwards}) {
        for (const double spread : {0.0, 0.0025, -0.001}) {
          const cdsmodel::CouponDates coupon{.reset = *index_start,
                                             .pay = *index_mat,
                                             .accrual_start = *index_start,
                                             .accrual_end = *index_mat};
          TCouponDates ref_coupon{};
          ref_coupon.resetDate = cdsmodel::to_tdate(coupon.reset);
          ref_coupon.payDate = cdsmodel::to_tdate(coupon.pay);
          ref_coupon.accrueStartDate = cdsmodel::to_tdate(coupon.accrual_start);
          ref_coupon.accrueEndDate = cdsmodel::to_tdate(coupon.accrual_end);
          TDateInterval pay = raw_interval({tenor_months, cdsmodel::PeriodUnit::Months});

          double ref_amount = 0.0;
          const bool accepted =
              JpmcdsFloatingCashFlowGen(
                  curve.ref.curve, upstream_interp(interp), cdsmodel::to_tdate(*index_start),
                  cdsmodel::to_tdate(*index_mat), pay, &ref_coupon, JPMCDS_ACT_360, JPMCDS_ACT_360,
                  1.0e6, spread, &ref_amount) == SUCCESS;
          const cdsmodel::Expected<double> got = cdsmodel::floating_cash_flow(
              *curve.port, interp, *index_start, *index_mat,
              {tenor_months, cdsmodel::PeriodUnit::Months}, coupon, cdsmodel::DayCount::Act360,
              cdsmodel::DayCount::Act360, 1.0e6, spread);

          ASSERT_EQ(accepted, got.has_value());
          if (accepted) {
            EXPECT_NEAR(ref_amount, *got,
                        4.0 * std::max(1.0, std::abs(ref_amount)) *
                            std::numeric_limits<double>::epsilon());
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 40);
}

// ------------------------------------- the dropped fields, proven dead

// The port's FixedStream has no value date. Upstream takes one and passes it
// to CheckFixedStreamDetails, which never reads it.
TEST_F(StreamCashFlowsDifferential, TheFixedPathIgnoresItsValueDate) {
  SchedulePair sched(d(2024, 1, 15), d(2029, 4, 15), 6, false);
  ASSERT_TRUE(sched.usable());
  TStreamFixed *rs = JpmcdsNewStreamFixed(sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6,
                                          JPMCDS_ACT_360, JPMCDS_ADD_FINAL, 0.03, 0.05, 0.04);
  ASSERT_NE(rs, nullptr);

  RefFlows baseline;
  baseline.cfl = JpmcdsNewStreamFixedCFL(cdsmodel::to_tdate(d(2024, 1, 15)), rs);
  ASSERT_TRUE(static_cast<bool>(baseline));

  // A century of value dates, including ones long after the swap matures.
  for (const int year : {1950, 2000, 2024, 2030, 2099}) {
    RefFlows other;
    other.cfl = JpmcdsNewStreamFixedCFL(cdsmodel::to_tdate(d(year, 7, 1)), rs);
    ASSERT_TRUE(static_cast<bool>(other)) << year;
    ASSERT_EQ(baseline.cfl->fNumItems, other.cfl->fNumItems) << year;
    for (int i = 0; i < baseline.cfl->fNumItems; ++i) {
      EXPECT_EQ(baseline.cfl->fArray[i].fDate, other.cfl->fArray[i].fDate) << year << " " << i;
      EXPECT_DOUBLE_EQ(baseline.cfl->fArray[i].fAmount, other.cfl->fArray[i].fAmount)
          << year << " " << i;
    }
  }
  JpmcdsFreeTStreamFixed(rs);
}

// The port's FloatStream has no stub-rate fields, no averSoFar and no
// compSoFar. Sweep each over values that would change the answer if read.
TEST_F(StreamCashFlowsDifferential, TheFloatPathIgnoresStubRatesAndTheSoFarFields) {
  const cdsmodel::Date base = d(2024, 1, 15);
  CurvePair curve(base);
  ASSERT_NE(curve.ref.curve, nullptr);

  // A FRONT stub and a BACK stub, so both GetFrontStub and GetBackStub run.
  for (const bool stub_at_end : {false, true}) {
    SchedulePair sched(base, d(2029, 4, 15), 6, stub_at_end);
    ASSERT_TRUE(sched.usable());
    ASSERT_NE(sched.ref.list->stubLocation, JPMCDS_NO_STUB) << "need a stub to exercise this";

    TDateInterval mat = raw_interval({6, cdsmodel::PeriodUnit::Months});
    TDateInterval pay = raw_interval({6, cdsmodel::PeriodUnit::Months});
    TFloatRate rfr{};
    ASSERT_EQ(JpmcdsFloatRateSet(&mat, &pay, JPMCDS_ACT_360, 0, 0.0, 1.0, &rfr), SUCCESS);

    TDateInterval one_day = raw_interval({1, cdsmodel::PeriodUnit::Days});
    TDateInterval ten_year = raw_interval({120, cdsmodel::PeriodUnit::Months});

    struct Variant {
      const char *label;
      TStubRates first;
      TStubRates final_;
      double aver;
      double comp;
    };
    const Variant variants[] = {
        {"baseline", {0.0, mat}, {0.0, mat}, 0.0, 0.0},
        {"huge stub rates", {9.5, mat}, {-0.5, mat}, 0.0, 0.0},
        {"one-day stub intervals", {0.0, one_day}, {0.0, one_day}, 0.0, 0.0},
        {"ten-year stub intervals", {0.0, ten_year}, {0.0, ten_year}, 0.0, 0.0},
        {"soFar fields set", {0.0, mat}, {0.0, mat}, 0.77, -0.33},
        {"everything at once", {9.5, one_day}, {-0.5, ten_year}, 1.5, 2.5},
    };

    std::vector<double> baseline;
    for (const Variant &v : variants) {
      TStubRates first = v.first;
      TStubRates final_ = v.final_;
      TStreamFloat *rs =
          JpmcdsNewStreamFloat(sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6, JPMCDS_ACT_360, 0,
                               &first, &final_, v.aver, v.comp, &rfr);
      ASSERT_NE(rs, nullptr) << v.label;
      RefFlows ref;
      ref.cfl = JpmcdsNewStreamFloatCFLGen(curve.ref.curve, JPMCDS_FLAT_FORWARDS, rs,
                                           JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"));
      ASSERT_TRUE(static_cast<bool>(ref)) << v.label;

      std::vector<double> amounts;
      for (int i = 0; i < ref.cfl->fNumItems; ++i) {
        amounts.push_back(ref.cfl->fArray[i].fAmount);
      }
      if (baseline.empty()) {
        baseline = amounts;
        ASSERT_FALSE(baseline.empty());
      } else {
        ASSERT_EQ(baseline.size(), amounts.size()) << v.label;
        for (std::size_t i = 0; i < baseline.size(); ++i) {
          EXPECT_DOUBLE_EQ(baseline[i], amounts[i])
              << v.label << " changed flow " << i << " -- a field the port drops is actually read";
        }
      }
      JpmcdsFreeTStreamFloat(rs);
    }
  }
}

// `TSwapType` has one enumerator, so the field is not a choice -- which is why
// the port drops it. Upstream's guard is one-sided: `swapType >
// JPMCDS_SINGLE_REFIX`, and that constant is 0, so on its face a negative
// value passes.
//
// It does not, and the reason is worth recording. An enum whose enumerators
// are all non-negative gets an UNSIGNED compatible type in C, so inside the
// library's own translation unit `(TSwapType)(-1)` is 4294967295 and the
// comparison catches it. In C++ the implementation picks, and clang++ picks
// signed `int` -- so this very test file sees a signed enum while the oracle
// it links against sees an unsigned one. The comparison happens on the C side,
// so the C answer is the one that counts.
//
// A guard that is correct only because of implementation-defined enum
// signedness, differing between the two languages that share the header, is
// not a guard anyone should rely on. The port has no such field, so there is
// nothing to rely on.
TEST_F(StreamCashFlowsDifferential, SwapTypeHasExactlyOneUsableValue) {
  SchedulePair sched(d(2024, 1, 15), d(2029, 4, 15), 6, false);
  ASSERT_TRUE(sched.usable());

  for (const int type : {-1000, -1, 1, 2}) {
    TStreamFixed *rs = JpmcdsNewStreamFixed(sched.ref.list, static_cast<TSwapType>(type), 1.0e6,
                                            JPMCDS_ACT_360, 0, 0.03, 0.05, 0.04);
    EXPECT_EQ(rs, nullptr) << "swapType " << type
                           << " was accepted; the one-sided guard has started leaking";
    JpmcdsFreeTStreamFixed(rs);
  }
  TStreamFixed *ok = JpmcdsNewStreamFixed(sched.ref.list, JPMCDS_SINGLE_REFIX, 1.0e6,
                                          JPMCDS_ACT_360, 0, 0.03, 0.05, 0.04);
  EXPECT_NE(ok, nullptr) << "the only valid swap type was refused";
  JpmcdsFreeTStreamFixed(ok);
}

// ------------------------------------------- the legs' disagreement

// A one-period Both-stub schedule is valid by upstream's own validator and
// constructible through its public API. The fixed path lets the FRONT rate
// win; the float path lets the BACK one win.
TEST_F(StreamCashFlowsDifferential, OnePeriodTwoStubResolvesOppositelyOnTheTwoLegs) {
  TCouponDateList *dl = JpmcdsNewEmptyCouponDateList(1);
  ASSERT_NE(dl, nullptr);
  dl->stubLocation = JPMCDS_TWO_STUB;
  dl->fArray[0].accrueStartDate = cdsmodel::to_tdate(d(2024, 1, 15));
  dl->fArray[0].accrueEndDate = cdsmodel::to_tdate(d(2024, 7, 15));
  dl->fArray[0].payDate = cdsmodel::to_tdate(d(2024, 7, 15));
  dl->fArray[0].resetDate = cdsmodel::to_tdate(d(2024, 1, 15));
  ASSERT_EQ(JpmcdsCouponDateListCheck(dl), SUCCESS)
      << "upstream's validator accepts a one-period two-stub schedule";

  TStreamFixed *rs = JpmcdsNewStreamFixed(dl, JPMCDS_SINGLE_REFIX, 1.0e6, JPMCDS_ACT_360, 0,
                                          0.011 /*first*/, 0.022 /*final*/, 0.033);
  ASSERT_NE(rs, nullptr);
  double rate = -1.0;
  ASSERT_EQ(JpmcdsGetFixedCouponRates(cdsmodel::to_tdate(d(2024, 1, 15)), rs, &rate), SUCCESS);
  EXPECT_DOUBLE_EQ(rate, 0.011) << "the fixed path assigns final then first, so first wins";

  // The port reproduces it, from the same schedule.
  cdsmodel::CouponSchedule schedule;
  schedule.stub = cdsmodel::StubLocation::Both;
  schedule.periods.push_back(cdsmodel::CouponDates{.reset = d(2024, 1, 15),
                                                   .pay = d(2024, 7, 15),
                                                   .accrual_start = d(2024, 1, 15),
                                                   .accrual_end = d(2024, 7, 15)});
  const cdsmodel::Expected<std::vector<double>> got =
      cdsmodel::fixed_coupon_rates({.schedule = schedule,
                                    .principal = 1.0e6,
                                    .pay_day_count = cdsmodel::DayCount::Act360,
                                    .first_coupon_rate = 0.011,
                                    .final_coupon_rate = 0.022,
                                    .fixed_rate = 0.033});
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->size(), 1u);
  EXPECT_DOUBLE_EQ(got->front(), 0.011) << "the port must resolve it upstream's way";

  JpmcdsFreeTStreamFixed(rs);
  JpmcdsFreeCouponDateList(dl);
}

// ------------------------------------------------------------ UPSTREAM-31

// JpmcdsForwardFromZCurve does not write its output on failure, which is the
// precondition for GetFloatStubRate reading an indeterminate double. Asserted
// so the register entry rests on a measurement rather than a reading.
TEST_F(StreamCashFlowsDifferential, Upstream31ForwardFromZCurveLeavesItsOutputUnwritten) {
  const cdsmodel::Date base = d(2024, 1, 15);
  TDate point = cdsmodel::to_tdate(d(2300, 1, 15));
  double rate = 1.0e2;
  RefCurve curve;
  curve.curve = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), &point, &rate, 1, 1.0, JPMCDS_ACT_365F);
  ASSERT_NE(curve.curve, nullptr);

  int failures = 0;
  int left_unwritten = 0;
  for (int year = 150; year <= 180; ++year) {
    constexpr double kSentinel = 12345.0;
    double out = kSentinel;
    const int rc = JpmcdsForwardFromZCurve(
        curve.curve, JPMCDS_FLAT_FORWARDS, cdsmodel::to_tdate(d(2024 + year, 1, 15)),
        cdsmodel::to_tdate(d(2024 + year, 7, 15)), JPMCDS_ACT_360, JPMCDS_SIMPLE_BASIS, &out);
    if (rc != SUCCESS) {
      ++failures;
      if (out == kSentinel) {
        ++left_unwritten;
      }
    }
  }
  ASSERT_GT(failures, 0) << "no failure found, so the precondition is untested";
  EXPECT_EQ(failures, left_unwritten)
      << "JpmcdsForwardFromZCurve now writes its output on failure, so UPSTREAM-31's "
         "indeterminate read may be fixed";
}

// Reaching GetFloatStubRate's failure path, which performs the read. Not
// DISABLED: the read's value is discarded by both callers, so running this is
// safe -- it is the UB inside upstream that cannot be observed, which is the
// point of the register entry.
TEST_F(StreamCashFlowsDifferential, Upstream31TheStubPathIsReachable) {
  const cdsmodel::Date base = d(2024, 1, 15);
  TDate point = cdsmodel::to_tdate(d(2300, 1, 15));
  double rate = 1.0e2;
  RefCurve curve;
  curve.curve = JpmcdsMakeTCurve(cdsmodel::to_tdate(base), &point, &rate, 1, 1.0, JPMCDS_ACT_365F);
  ASSERT_NE(curve.curve, nullptr);

  SchedulePair sched(base, d(2185, 6, 15), 6, true);
  ASSERT_TRUE(sched.usable());
  ASSERT_EQ(sched.ref.list->stubLocation, JPMCDS_BACK_STUB);

  const TCouponDates last = sched.ref.list->fArray[sched.ref.list->fNumItems - 1];
  constexpr double kSentinel = 12345.0;
  double out = kSentinel;
  // Exactly the call GetFloatStubRate makes, on exactly the period GetBackStub
  // hands it.
  EXPECT_NE(JpmcdsForwardFromZCurve(curve.curve, JPMCDS_FLAT_FORWARDS, last.accrueStartDate,
                                    last.accrueEndDate, JPMCDS_ACT_360, JPMCDS_SIMPLE_BASIS, &out),
            SUCCESS);
  EXPECT_EQ(out, kSentinel) << "the output was written, so `rate` would not be indeterminate";

  // And the port refuses rather than returning a value derived from nothing.
  const cdsmodel::FloatStream stream{
      .schedule = *sched.port,
      .principal = 1.0e6,
      .pay_day_count = cdsmodel::DayCount::Act360,
      .rate = {.maturity_interval = {6, cdsmodel::PeriodUnit::Months},
               .pay_interval = {6, cdsmodel::PeriodUnit::Months},
               .day_count = cdsmodel::DayCount::Act360}};
  const std::vector<cdsmodel::RatePoint> port_points{cdsmodel::RatePoint{d(2300, 1, 15), 1.0e2}};
  const cdsmodel::Expected<cdsmodel::ZeroCurve> port_curve =
      cdsmodel::ZeroCurve::create(base, port_points, 1.0, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(port_curve.has_value());
  EXPECT_FALSE(cdsmodel::float_stream_cash_flows(*port_curve, cdsmodel::InterpType::FlatForwards,
                                                 stream, cdsmodel::BadDayConv::None,
                                                 cdsmodel::Calendar::none())
                   .has_value());
}

} // namespace
