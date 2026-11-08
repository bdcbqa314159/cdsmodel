// TIER 2 -- live differential tests for the defaulted-CDS accrual.
//
// This module CORRECTS upstream (UPSTREAM-11), so like the date list suite it
// does two jobs:
//
//   1. assert agreement wherever upstream is right -- which is whenever the
//      credit event and the trade date fall in the same coupon period, the
//      case a quick settlement produces;
//   2. assert the DISAGREEMENT still exists where upstream is wrong, so a
//      future upstream release that fixes it makes our divergence visible
//      rather than leaving a correction nobody revisits.
//
// Job 2 is the one that is easy to forget. A suite that only checked
// agreement would pass forever after upstream changed.

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/defaulted.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cx.h"
#include "dateconv.h"
#include "defaulted.h"
#include "ldate.h"
#include "stub.h"
}

namespace {

class DefaultedDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

cdsmodel::Cds quarterly(cdsmodel::Date start, cdsmodel::Date end,
                        cdsmodel::DayCount dcc = cdsmodel::DayCount::Act360,
                        cdsmodel::BadDayConv bdc = cdsmodel::BadDayConv::None) {
  cdsmodel::Cds cds;
  cds.start = start;
  cds.end = end;
  cds.coupon_rate = 0.01;
  cds.coupon_interval = cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months};
  cds.day_count = dcc;
  cds.bad_day = bdc;
  return cds;
}

std::vector<cdsmodel::Calendar> no_holidays() {
  return {*cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
}

struct Reference {
  bool ok = false;
  double days = 0.0;
  double amount = 0.0;
};

Reference reference_accrual(const cdsmodel::Cds &cds, cdsmodel::Date trade, cdsmodel::Date edd,
                            double notional) {
  TDateInterval iv{cds.coupon_interval.periods, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = cds.stub.position == cdsmodel::StubPosition::Back ? TRUE : FALSE;
  stub.longStub = cds.stub.length == cdsmodel::StubLength::Long ? TRUE : FALSE;
  Reference r;
  r.ok =
      JpmcdsDefaultAccrual(cdsmodel::to_tdate(trade), cdsmodel::to_tdate(edd),
                           cdsmodel::to_tdate(cds.start), cdsmodel::to_tdate(cds.end), &iv, &stub,
                           notional, cds.coupon_rate, cdsmodel::to_upstream_code(cds.day_count),
                           static_cast<long>(static_cast<char>(cds.bad_day)),
                           const_cast<char *>("NONE"), &r.days, &r.amount) == SUCCESS;
  return r;
}

/// True when the credit event and the trade date share a coupon period, which
/// is exactly the regime where upstream's choice of locating date does not
/// matter. Derived from the port's own schedule so the two cannot drift.
bool same_coupon_period(const cdsmodel::Cds &cds, cdsmodel::Date trade, cdsmodel::Date edd,
                        std::span<const cdsmodel::Calendar> cals) {
  cdsmodel::Cds terms = cds;
  terms.pay_accrual_on_default = true;
  terms.protect_start = true;
  const cdsmodel::Expected<cdsmodel::FeeLeg> leg = cdsmodel::cds_fee_leg(terms, 1.0, cals);
  if (!leg) {
    return false;
  }
  for (const cdsmodel::FeePayment &p : leg->payments) {
    const bool has_edd = p.accrual_start <= edd && edd < p.accrual_end;
    const bool has_trade = p.accrual_start <= trade && trade < p.accrual_end;
    if (has_edd && has_trade) {
      return true;
    }
  }
  return false;
}

TEST_F(DefaultedDifferential, AgreesWhereverTheEventAndTradeShareACouponPeriod) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  int agreed = 0;
  for (cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act360, cdsmodel::DayCount::Act365Fixed,
                                 cdsmodel::DayCount::Thirty360}) {
    for (cdsmodel::BadDayConv bdc : {cdsmodel::BadDayConv::None, cdsmodel::BadDayConv::Follow}) {
      const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2029, 3, 20), dcc, bdc);
      for (int event_offset = 0; event_offset <= 1000; event_offset += 11) {
        const cdsmodel::Date edd = cds.start + std::chrono::days{event_offset};
        for (int lag : {0, 1, 3, 10, 25}) {
          const cdsmodel::Date trade = edd + std::chrono::days{lag};
          if (!same_coupon_period(cds, trade, edd, cals)) {
            continue;
          }
          const Reference expected = reference_accrual(cds, trade, edd, 1.0e7);
          const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
              cdsmodel::default_accrual(cds, trade, edd, 1.0e7, cals);
          const std::string where = "edd+" + std::to_string(event_offset) + " lag " +
                                    std::to_string(lag) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(dcc));
          ASSERT_EQ(got.has_value(), expected.ok) << where;
          if (!expected.ok) {
            continue;
          }
          EXPECT_EQ(static_cast<double>(got->days), expected.days) << where;
          // Same libm in this process: anything short of equality would mean
          // the year fraction or the multiplication was reordered.
          EXPECT_EQ(got->amount, expected.amount) << where;
          ++agreed;
        }
      }
    }
  }
  EXPECT_GT(agreed, 300) << "too few agreeing cases: " << agreed;
}

TEST_F(DefaultedDifferential, TheTradeDateBeforeTheEventGivesZeroInBoth) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2029, 3, 20));
  for (int lead : {1, 5, 30, 200}) {
    const cdsmodel::Date edd = d(2024, 8, 1);
    const cdsmodel::Date trade = edd - std::chrono::days{lead};
    const Reference expected = reference_accrual(cds, trade, edd, 1.0e7);
    const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
        cdsmodel::default_accrual(cds, trade, edd, 1.0e7, cals);
    ASSERT_TRUE(expected.ok) << lead;
    ASSERT_TRUE(got.has_value()) << lead;
    EXPECT_EQ(expected.days, 0.0);
    EXPECT_EQ(expected.amount, 0.0);
    EXPECT_EQ(got->days, 0);
    EXPECT_EQ(got->amount, 0.0);
  }
}

TEST_F(DefaultedDifferential, AnEventBeforeTheContractStartIsRejectedByBoth) {
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2029, 3, 20));
  for (int before : {1, 30, 400}) {
    const cdsmodel::Date edd = cds.start - std::chrono::days{before};
    const cdsmodel::Date trade = d(2024, 8, 1);
    EXPECT_FALSE(reference_accrual(cds, trade, edd, 1.0e7).ok) << before;
    EXPECT_FALSE(cdsmodel::default_accrual(cds, trade, edd, 1.0e7, cals).has_value()) << before;
  }
}

TEST_F(DefaultedDifferential, Upstream11_UpstreamGoesNegativeOnceSettlementCrossesACouponDate) {
  // The divergence, asserted so it cannot go unnoticed if upstream changes.
  //
  // Quarterly from 20 March 2024, credit event on 1 August 2024, inside the
  // 20 June - 20 September period. The accrual owed is 43 days. Upstream
  // agrees until the TRADE date crosses 20 September, then returns -49 days
  // with SUCCESS, because it locates the period by the trade date and accrues
  // to the event date.
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2029, 3, 20));
  const cdsmodel::Date edd = d(2024, 8, 1);

  const cdsmodel::Expected<cdsmodel::DefaultAccrual> correct =
      cdsmodel::default_accrual(cds, edd, edd, 1.0e7, cals);
  ASSERT_TRUE(correct.has_value());
  EXPECT_EQ(correct->days, 43) << "the port's own answer moved; the rest of this test assumes 43";

  int negative_cases = 0;
  for (int lag : {50, 60, 90, 120, 365}) {
    const cdsmodel::Date trade = edd + std::chrono::days{lag};
    const Reference expected = reference_accrual(cds, trade, edd, 1.0e7);
    ASSERT_TRUE(expected.ok) << "upstream stopped reporting SUCCESS at lag " << lag
                             << " -- it may have been fixed; revisit the correction";
    EXPECT_LT(expected.days, 0.0) << "upstream no longer returns a negative accrual at lag " << lag
                                  << " -- UPSTREAM-11 may be fixed; revisit the correction";
    ++negative_cases;

    // The port is independent of the lag, which is the whole point.
    const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
        cdsmodel::default_accrual(cds, trade, edd, 1.0e7, cals);
    ASSERT_TRUE(got.has_value()) << lag;
    EXPECT_EQ(got->days, correct->days) << "the port's answer varied with the trade date";
    EXPECT_EQ(got->amount, correct->amount) << "the port's answer varied with the trade date";
  }
  EXPECT_EQ(negative_cases, 5);
}

TEST_F(DefaultedDifferential, Upstream11_UpstreamFailsOnLateSettlementWhereThePortAnswers) {
  // The second behavioural consequence of the same root cause. A trade date
  // past the final accrual end matches no coupon period, so upstream fails --
  // even though the credit event itself sits comfortably inside the schedule
  // and the debt is well defined.
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2026, 3, 20));
  const cdsmodel::Date edd = d(2025, 7, 1);
  const cdsmodel::Date late_trade = d(2026, 9, 1); // after the last accrual end

  EXPECT_FALSE(reference_accrual(cds, late_trade, edd, 1.0e7).ok)
      << "upstream now answers for a late settlement -- revisit the correction";
  const cdsmodel::Expected<cdsmodel::DefaultAccrual> got =
      cdsmodel::default_accrual(cds, late_trade, edd, 1.0e7, cals);
  ASSERT_TRUE(got.has_value());
  EXPECT_GT(got->days, 0);
  // And it equals the answer from asking on the event date itself.
  const cdsmodel::Expected<cdsmodel::DefaultAccrual> prompt =
      cdsmodel::default_accrual(cds, edd, edd, 1.0e7, cals);
  ASSERT_TRUE(prompt.has_value());
  EXPECT_EQ(got->days, prompt->days);
  EXPECT_EQ(got->amount, prompt->amount);
}

TEST_F(DefaultedDifferential, TheScopeOfTheDivergenceIsWhatWasMeasured) {
  // 62% of a settlement-lag grid disagrees and 60% goes negative. Recorded as
  // a test so the claim in the defect report is checked rather than asserted.
  const std::vector<cdsmodel::Calendar> cals = no_holidays();
  const cdsmodel::Cds cds = quarterly(d(2024, 3, 20), d(2029, 3, 20));
  int total = 0;
  int differ = 0;
  int negative = 0;
  for (int month = 4; month <= 12; ++month) {
    for (int day = 1; day <= 28; day += 3) {
      const cdsmodel::Date edd = d(2024, month, day);
      if (edd < cds.start) {
        continue;
      }
      for (int lag = 0; lag <= 120; lag += 5) {
        const cdsmodel::Date trade = edd + std::chrono::days{lag};
        const Reference actual = reference_accrual(cds, trade, edd, 1.0e7);
        const Reference prompt = reference_accrual(cds, edd, edd, 1.0e7);
        if (!actual.ok || !prompt.ok) {
          continue;
        }
        ++total;
        if (actual.days != prompt.days) {
          ++differ;
        }
        if (actual.days < 0.0) {
          ++negative;
        }
      }
    }
  }
  ASSERT_GT(total, 2000) << "grid smaller than the one measured: " << total;
  const double differ_pct = 100.0 * differ / total;
  const double negative_pct = 100.0 * negative / total;
  EXPECT_GT(differ_pct, 55.0) << "measured 62%, got " << differ_pct << "%";
  EXPECT_LT(differ_pct, 70.0) << "measured 62%, got " << differ_pct << "%";
  EXPECT_GT(negative_pct, 50.0) << "measured 60%, got " << negative_pct << "%";
}

} // namespace
