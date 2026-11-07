// TIER 2 -- live differential tests for a swap benchmark's bootstrap flows.
//
// Two functions, so the sweeps are over what varies: spans that do and do not
// divide evenly by the coupon interval, every bad-day convention, a
// substitution table present and absent (UPSTREAM-26 makes those different
// inputs), and the zero-rate special case.
//
// One test asserts a DIVERGENCE: UPSTREAM-32, where upstream writes through
// `fArray[-1]`. Its live reproducer is DISABLED, because running it is a null
// dereference.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/bad_day_map.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/swap_bootstrap_flows.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <span>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "datelist.h"
#include "ldate.h"
#include "zcprvt.h"
}

namespace {

class SwapBootstrapFlowsDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

struct RefFlows {
  TCashFlowList *cfl = nullptr;
  ~RefFlows() { JpmcdsFreeCFL(cfl); }
  RefFlows() = default;
  RefFlows(const RefFlows &) = delete;
  RefFlows &operator=(const RefFlows &) = delete;
  explicit operator bool() const { return cfl != nullptr; }
};

struct RefDates {
  TDateList *dl = nullptr;
  ~RefDates() { JpmcdsFreeDateList(dl); }
  RefDates() = default;
  RefDates(const RefDates &) = delete;
  RefDates &operator=(const RefDates &) = delete;
  explicit operator bool() const { return dl != nullptr; }
};

/// Upstream's TBadDayList viewing a port map, owning the two parallel arrays
/// upstream needs and the port does not. Same shape as the fixture in
/// bad_day_map_differential_test.cpp; upstream has no constructor for this
/// type, so callers fill the struct in by hand.
///
/// A NULL list and an EMPTY one are different inputs -- UPSTREAM-26 branches
/// on the pointer, not the count -- so this keeps the arrays null when the map
/// is empty, exactly as the other fixture does.
struct RefBadDayList {
  std::vector<TDate> bad;
  std::vector<TDate> good;
  TBadDayList list{};

  explicit RefBadDayList(std::span<const cdsmodel::BadDayPair> map) {
    for (const cdsmodel::BadDayPair &pair : map) {
      bad.push_back(cdsmodel::to_tdate(pair.bad));
      good.push_back(cdsmodel::to_tdate(pair.good));
    }
    list.count = static_cast<int>(map.size());
    list.maxAllowed = static_cast<int>(map.size());
    list.badDay = bad.empty() ? nullptr : bad.data();
    list.goodDay = good.empty() ? nullptr : good.data();
  }
};

void expect_flows_match(const TCashFlowList *ref, const std::vector<cdsmodel::CashFlow> &got,
                        const char *what) {
  ASSERT_EQ(ref->fNumItems, static_cast<int>(got.size())) << what;
  for (int i = 0; i < ref->fNumItems; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref->fArray[i].fDate, cdsmodel::to_tdate(got[k].date)) << what << " date " << i;
    // One computation against itself in the same binary, so ULPs.
    EXPECT_NEAR(ref->fArray[i].fAmount, got[k].amount,
                4.0 * std::max(1.0, std::abs(ref->fArray[i].fAmount)) *
                    std::numeric_limits<double>::epsilon())
        << what << " amount " << i;
  }
}

const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> &spans() {
  static const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> v{
      {d(2024, 1, 15), d(2029, 1, 15)}, // on cycle for 6M and 12M
      {d(2024, 1, 15), d(2029, 4, 15)}, // off cycle: a stub
      {d(2024, 1, 15), d(2024, 7, 15)}, // exactly one 6M period
      {d(2024, 2, 29), d(2027, 2, 28)}, // leap day
      {d(2024, 1, 31), d(2026, 7, 31)}, // month ends
      {d(2024, 3, 15), d(2024, 3, 18)}, // shorter than any interval
  };
  return v;
}

TEST_F(SwapBootstrapFlowsDifferential, CouponDatesMatchAcrossSpansAndConventions) {
  int compared = 0;
  int refused = 0;
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  for (const auto &[value_date, maturity] : spans()) {
    for (const int months : {3, 6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        for (const cdsmodel::BadDayConv conv :
             {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
              cdsmodel::BadDayConv::Previous, cdsmodel::BadDayConv::Modified}) {
          TDateInterval raw = raw_interval({months, cdsmodel::PeriodUnit::Months});
          RefDates ref;
          ref.dl =
              JpmcdsZCGetSwapCouponDL(cdsmodel::to_tdate(value_date), cdsmodel::to_tdate(maturity),
                                      stub_at_end ? TRUE : FALSE, &raw, nullptr,
                                      cdsmodel::to_upstream_code(conv), const_cast<char *>("NONE"));

          const std::optional<std::vector<cdsmodel::Date>> got = cdsmodel::swap_coupon_dates(
              value_date, maturity, {months, cdsmodel::PeriodUnit::Months}, stub_at_end,
              {.conv = conv}, calendars);

          if (!ref) {
            EXPECT_FALSE(got.has_value());
            ++refused;
            continue;
          }
          ASSERT_TRUE(got.has_value());
          ASSERT_EQ(ref.dl->fNumItems, static_cast<int>(got->size()));
          for (int i = 0; i < ref.dl->fNumItems; ++i) {
            EXPECT_EQ(ref.dl->fArray[i], cdsmodel::to_tdate((*got)[static_cast<std::size_t>(i)]))
                << "date " << i;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 100) << "the coupon-date sweep compared almost nothing";
}

TEST_F(SwapBootstrapFlowsDifferential, CashFlowsMatchAcrossSpansRatesAndConventions) {
  int compared = 0;
  int refused_by_both = 0;
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  for (const auto &[value_date, maturity] : spans()) {
    for (const int months : {6, 12}) {
      for (const bool stub_at_end : {false, true}) {
        for (const cdsmodel::DayCount dcc :
             {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
              cdsmodel::DayCount::Thirty360}) {
          for (const cdsmodel::BadDayConv conv :
               {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
                cdsmodel::BadDayConv::Modified}) {
            for (const double rate : {0.0, 0.04, -0.01}) {
              TDateInterval raw = raw_interval({months, cdsmodel::PeriodUnit::Months});
              RefFlows ref;
              ref.cfl = JpmcdsZCGetSwapCFL(
                  cdsmodel::to_tdate(value_date), cdsmodel::to_tdate(maturity),
                  stub_at_end ? TRUE : FALSE, rate, &raw, cdsmodel::to_upstream_code(dcc), nullptr,
                  cdsmodel::to_upstream_code(conv), const_cast<char *>("NONE"));

              const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::swap_cash_flows(
                  value_date, maturity, stub_at_end, rate, {months, cdsmodel::PeriodUnit::Months},
                  dcc, {.conv = conv}, calendars);

              // UPSTREAM-32: upstream writes through fArray[-1] rather than
              // refusing, so this input is covered by its own test and must
              // not be exercised here.
              if (value_date == maturity) {
                continue;
              }
              if (!ref) {
                EXPECT_FALSE(got.has_value()) << "port accepted what upstream refused";
                ++refused_by_both;
                continue;
              }
              ASSERT_TRUE(got.has_value()) << "port refused what upstream accepted";
              expect_flows_match(ref.cfl, *got, "swap cfl");
              ++compared;
            }
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 200) << "the cash-flow sweep compared almost nothing";
}

// A substitution table present and a table absent are different inputs, and an
// EMPTY table is different again -- UPSTREAM-26 branches on the pointer.
TEST_F(SwapBootstrapFlowsDifferential, MatchesWithASubstitutionTable) {
  const cdsmodel::Date value_date = d(2024, 1, 15);
  const cdsmodel::Date maturity = d(2027, 1, 15);
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};

  // Move two of the coupon dates somewhere unrelated, so a table that is read
  // produces visibly different flows from one that is not.
  const std::vector<cdsmodel::BadDayPair> pairs{
      cdsmodel::BadDayPair{d(2025, 1, 15), d(2025, 2, 3)},
      cdsmodel::BadDayPair{d(2026, 1, 15), d(2026, 3, 9)}};

  int compared = 0;
  for (const std::vector<cdsmodel::BadDayPair> &table :
       {std::vector<cdsmodel::BadDayPair>{}, pairs}) {
    RefBadDayList ref_table(table);
    for (const cdsmodel::BadDayConv conv :
         {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
      for (const double rate : {0.0, 0.04}) {
        TDateInterval raw = raw_interval({12, cdsmodel::PeriodUnit::Months});
        RefFlows ref;
        ref.cfl = JpmcdsZCGetSwapCFL(cdsmodel::to_tdate(value_date), cdsmodel::to_tdate(maturity),
                                     TRUE, rate, &raw, JPMCDS_ACT_360, &ref_table.list,
                                     cdsmodel::to_upstream_code(conv), const_cast<char *>("NONE"));
        const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::swap_cash_flows(
            value_date, maturity, true, rate, {12, cdsmodel::PeriodUnit::Months},
            cdsmodel::DayCount::Act360,
            {.substitutions = std::span<const cdsmodel::BadDayPair>{table}, .conv = conv},
            calendars);
        ASSERT_TRUE(static_cast<bool>(ref));
        ASSERT_TRUE(got.has_value());
        expect_flows_match(ref.cfl, *got, "with table");
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, 8);
}

// The zero-rate path returns one flow of 1.0 at the adjusted maturity, and no
// coupons at all -- so it is the one input where the schedule plays no part.
TEST_F(SwapBootstrapFlowsDifferential, TheZeroRatePathIsASingleRedemption) {
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  int compared = 0;
  for (const auto &[value_date, maturity] : spans()) {
    for (const cdsmodel::BadDayConv conv :
         {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow,
          cdsmodel::BadDayConv::Previous}) {
      TDateInterval raw = raw_interval({6, cdsmodel::PeriodUnit::Months});
      RefFlows ref;
      ref.cfl = JpmcdsZCGetSwapCFL(cdsmodel::to_tdate(value_date), cdsmodel::to_tdate(maturity),
                                   TRUE, 0.0, &raw, JPMCDS_ACT_360, nullptr,
                                   cdsmodel::to_upstream_code(conv), const_cast<char *>("NONE"));
      ASSERT_TRUE(static_cast<bool>(ref));
      EXPECT_EQ(ref.cfl->fNumItems, 1);
      EXPECT_DOUBLE_EQ(ref.cfl->fArray[0].fAmount, 1.0);

      const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::swap_cash_flows(
          value_date, maturity, true, 0.0, {6, cdsmodel::PeriodUnit::Months},
          cdsmodel::DayCount::Act360, {.conv = conv}, calendars);
      ASSERT_TRUE(got.has_value());
      expect_flows_match(ref.cfl, *got, "zero rate");
      ++compared;
    }
  }
  EXPECT_GT(compared, 15);
  // Even the degenerate maturity is safe here, which is the irony of
  // UPSTREAM-32: the bizarre RATE got a special path and the bizarre MATURITY
  // did not.
  const std::optional<std::vector<cdsmodel::CashFlow>> degenerate = cdsmodel::swap_cash_flows(
      d(2024, 1, 15), d(2024, 1, 15), true, 0.0, {6, cdsmodel::PeriodUnit::Months},
      cdsmodel::DayCount::Act360, {}, calendars);
  ASSERT_TRUE(degenerate.has_value());
  EXPECT_EQ(degenerate->size(), 1u);
}

// ------------------------------------------------------------ UPSTREAM-32

// The precondition, measured: JpmcdsNewPayDates drops its first element, so a
// one-date list becomes a zero-date one, and JpmcdsNewEmptyCFL(0) has a NULL
// array. Both halves asserted, so the register entry rests on measurement.
TEST_F(SwapBootstrapFlowsDifferential, Upstream32AnEmptyPayDateListWithANullArray) {
  TDateInterval raw = raw_interval({6, cdsmodel::PeriodUnit::Months});
  const TDate same = cdsmodel::to_tdate(d(2024, 1, 15));

  RefDates raw_list;
  raw_list.dl = JpmcdsNewDateList(same, same, &raw, TRUE);
  ASSERT_TRUE(static_cast<bool>(raw_list));
  EXPECT_EQ(raw_list.dl->fNumItems, 1) << "the span start == end gives one date";

  RefDates pay;
  pay.dl = JpmcdsNewPayDates(same, same, &raw, TRUE);
  ASSERT_TRUE(static_cast<bool>(pay));
  EXPECT_EQ(pay.dl->fNumItems, 0) << "JpmcdsNewPayDates drops the first element";

  TCashFlowList *empty = JpmcdsNewEmptyCFL(0);
  ASSERT_NE(empty, nullptr);
  EXPECT_EQ(empty->fNumItems, 0);
  EXPECT_EQ(empty->fArray, nullptr)
      << "fArray is non-null, so fArray[-1] would not be a null dereference";
  JpmcdsFreeCFL(empty);

  // The port reaches the same empty list -- the date generation is faithful --
  // and then refuses to build flows from it. The refusal is in
  // swap_cash_flows, guarding on the OUTCOME rather than on
  // `maturity == value_date`.
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  const std::optional<std::vector<cdsmodel::Date>> port_dates = cdsmodel::swap_coupon_dates(
      d(2024, 1, 15), d(2024, 1, 15), {6, cdsmodel::PeriodUnit::Months}, true, {}, calendars);
  ASSERT_TRUE(port_dates.has_value()) << "the date generation itself does not fail";
  EXPECT_TRUE(port_dates->empty()) << "and it reaches the same empty list upstream does";

  EXPECT_FALSE(cdsmodel::swap_cash_flows(d(2024, 1, 15), d(2024, 1, 15), true, 0.04,
                                         {6, cdsmodel::PeriodUnit::Months},
                                         cdsmodel::DayCount::Act360, {}, calendars)
                   .has_value())
      << "a non-zero rate on an empty schedule must be refused, not written through";
}

TEST_F(SwapBootstrapFlowsDifferential, DISABLED_Upstream32WritesThroughANullPointer) {
  TDateInterval raw = raw_interval({6, cdsmodel::PeriodUnit::Months});
  const TDate same = cdsmodel::to_tdate(d(2024, 1, 15));
  // `cfl->fArray[cfl->fNumItems-1].fAmount += 1.0` with fArray == NULL and
  // fNumItems == 0. UBSan: "applying non-zero offset ... to null pointer";
  // ASan: SEGV on 0xfffffffffffffff8. Never enabled.
  RefFlows ref;
  ref.cfl = JpmcdsZCGetSwapCFL(same, same, TRUE, 0.04, &raw, JPMCDS_ACT_360, nullptr,
                               JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"));
}

} // namespace
