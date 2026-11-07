// TIER 2 -- live differential tests for cx.c, the two leg constructors.
//
// A small file with one genuinely subtle behaviour: `JpmcdsContingentLegMake`
// validates its dates AFTER applying the `protectStart` shift, so the flag
// decides whether a zero-length leg is accepted. That is swept on both sides.
//
// `JpmcdsFeeLegMake` is tested through a HAND-WRITTEN prototype, because
// upstream exports it and declares it in no header. That is the finding, and
// the test is written the only way an external caller could write it.

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cmemory.h"
#include "cx.h"
#include "ldate.h"
#include "macros.h"

// NOT declared by any upstream header. cx.h declares JpmcdsFeeLegMakeEmpty and
// JpmcdsFeeLegFree and skips this one, so reaching it requires writing the
// prototype by hand -- which is what this does, and what any external caller
// would have to do.
TFeeLeg *JpmcdsFeeLegMake(int nbDates, TDate *accStartDates, TDate *accEndDates, TDate *payDates,
                          double notional, double couponRate, long dcc,
                          TAccrualPayConv accrualPayConv, TBoolean obsStartOfDay);
}

namespace {

class CxDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct RefCl {
  TContingentLeg *p = nullptr;
  ~RefCl() { FREE(p); }
  RefCl() = default;
  explicit RefCl(TContingentLeg *owned) : p(owned) {}
  RefCl(const RefCl &) = delete;
  RefCl &operator=(const RefCl &) = delete;
  explicit operator bool() const { return p != nullptr; }
};

struct RefFl {
  TFeeLeg *p = nullptr;
  ~RefFl() { JpmcdsFeeLegFree(p); }
  RefFl() = default;
  explicit RefFl(TFeeLeg *owned) : p(owned) {}
  RefFl(const RefFl &) = delete;
  RefFl &operator=(const RefFl &) = delete;
  explicit operator bool() const { return p != nullptr; }
};

// ------------------------------------------------- the contingent leg

TEST_F(CxDifferential, ContingentLegMatchesAcrossSpansAndProtectStart) {
  int compared = 0;
  int refused_by_both = 0;
  const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> spans{
      {d(2024, 3, 20), d(2029, 6, 20)}, // ordinary
      {d(2024, 3, 20), d(2024, 3, 21)}, // one day
      {d(2024, 3, 20), d(2024, 3, 20)}, // ZERO length -- protect_start decides
      {d(2024, 3, 20), d(2024, 3, 19)}, // inverted
      {d(2024, 2, 29), d(2024, 3, 1)},  // across a leap day
  };
  for (const auto &[start, end] : spans) {
    for (const bool protect_start : {false, true}) {
      for (const double notional : {1.0, 1.0e7, -5.0e6}) {
        RefCl ref(JpmcdsContingentLegMake(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end),
                                          notional, PROT_PAY_DEF, protect_start ? TRUE : FALSE));
        const std::optional<cdsmodel::ContingentLeg> got = cdsmodel::make_contingent_leg(
            start, end, notional, cdsmodel::ProtectionPayment::AtDefault, protect_start);

        ASSERT_EQ(static_cast<bool>(ref), got.has_value())
            << "acceptance differs, protect_start=" << protect_start;
        if (!ref) {
          ++refused_by_both;
          continue;
        }
        EXPECT_EQ(ref.p->startDate, cdsmodel::to_tdate(got->start));
        EXPECT_EQ(ref.p->endDate, cdsmodel::to_tdate(got->end));
        EXPECT_DOUBLE_EQ(ref.p->notional, got->notional);
        EXPECT_EQ(ref.p->protectStart == TRUE, got->protect_start);
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 20);
  EXPECT_GT(refused_by_both, 0) << "no span was refused, so the REQUIRE is untested";
}

// The subtle one: the check runs on the SHIFTED start date, so protect_start
// decides whether a zero-length leg is legal. Asserted directly because it is
// the thing a careless port gets wrong.
TEST_F(CxDifferential, ProtectStartDecidesWhetherAZeroLengthLegIsLegal) {
  const cdsmodel::Date same = d(2024, 3, 20);

  RefCl without(JpmcdsContingentLegMake(cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), 1.0,
                                        PROT_PAY_DEF, FALSE));
  EXPECT_FALSE(static_cast<bool>(without))
      << "upstream accepted a zero-length leg without protect_start";
  EXPECT_FALSE(
      cdsmodel::make_contingent_leg(same, same, 1.0, cdsmodel::ProtectionPayment::AtDefault, false)
          .has_value());

  RefCl with(JpmcdsContingentLegMake(cdsmodel::to_tdate(same), cdsmodel::to_tdate(same), 1.0,
                                     PROT_PAY_DEF, TRUE));
  ASSERT_TRUE(static_cast<bool>(with)) << "upstream refused a zero-length leg WITH protect_start";
  EXPECT_EQ(with.p->startDate, cdsmodel::to_tdate(same) - 1);
  const std::optional<cdsmodel::ContingentLeg> got =
      cdsmodel::make_contingent_leg(same, same, 1.0, cdsmodel::ProtectionPayment::AtDefault, true);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->start, same - std::chrono::days{1});
}

// The shift is plain TDate arithmetic with no lower bound, so upstream can
// store a NEGATIVE start date -- which no valid TDate is.
TEST_F(CxDifferential, TheShiftCanPushUpstreamsStartDateBelowItsEpoch) {
  RefCl ref(JpmcdsContingentLegMake(0 /* TDate 0 == 1601-01-01 */,
                                    cdsmodel::to_tdate(d(2024, 1, 15)), 1.0, PROT_PAY_DEF, TRUE));
  ASSERT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.p->startDate, -1) << "upstream gained a lower bound on the shift";

  // `Date` is a sys_days, so a day before the epoch is just an earlier date --
  // there is no sentinel to collide with and nothing to refuse.
  const std::optional<cdsmodel::ContingentLeg> got = cdsmodel::make_contingent_leg(
      cdsmodel::kDateEpoch, d(2024, 1, 15), 1.0, cdsmodel::ProtectionPayment::AtDefault, true);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->start, cdsmodel::kDateEpoch - std::chrono::days{1});
}

// -------------------------------------------------------- the fee leg

TEST_F(CxDifferential, FeeLegMatchesWhenGivenConsistentArrays) {
  std::vector<TDate> starts{cdsmodel::to_tdate(d(2024, 1, 15)), cdsmodel::to_tdate(d(2024, 4, 15)),
                            cdsmodel::to_tdate(d(2024, 7, 15))};
  std::vector<TDate> ends{cdsmodel::to_tdate(d(2024, 4, 15)), cdsmodel::to_tdate(d(2024, 7, 15)),
                          cdsmodel::to_tdate(d(2024, 10, 15))};
  std::vector<TDate> pays = ends;

  RefFl ref(JpmcdsFeeLegMake(3, starts.data(), ends.data(), pays.data(), 1.0e6, 0.01,
                             JPMCDS_ACT_360, ACCRUAL_PAY_ALL, TRUE));
  ASSERT_TRUE(static_cast<bool>(ref));

  std::vector<cdsmodel::FeePayment> payments;
  for (std::size_t i = 0; i < 3; ++i) {
    payments.push_back(cdsmodel::FeePayment{cdsmodel::from_tdate(starts[i]),
                                            cdsmodel::from_tdate(ends[i]),
                                            cdsmodel::from_tdate(pays[i])});
  }
  const cdsmodel::FeeLeg got = cdsmodel::make_fee_leg(
      payments, 1.0e6, 0.01, cdsmodel::DayCount::Act360, cdsmodel::AccrualPayment::All, true);

  ASSERT_EQ(ref.p->nbDates, static_cast<int>(got.payments.size()));
  for (int i = 0; i < ref.p->nbDates; ++i) {
    const auto k = static_cast<std::size_t>(i);
    EXPECT_EQ(ref.p->accStartDates[i], cdsmodel::to_tdate(got.payments[k].accrual_start));
    EXPECT_EQ(ref.p->accEndDates[i], cdsmodel::to_tdate(got.payments[k].accrual_end));
    EXPECT_EQ(ref.p->payDates[i], cdsmodel::to_tdate(got.payments[k].pay));
  }
  EXPECT_DOUBLE_EQ(ref.p->notional, got.notional);
  EXPECT_DOUBLE_EQ(ref.p->couponRate, got.coupon_rate);
  EXPECT_EQ(ref.p->dcc, cdsmodel::to_upstream_code(got.day_count));
  EXPECT_EQ(ref.p->obsStartOfDay == TRUE, got.observe_start_of_day);
}

// The count that lies. Reached through the hand-written prototype, because
// nothing declares the function -- which is why this is recorded rather than
// filed as a defect.
TEST_F(CxDifferential, FeeLegMakeAcceptsACountThatDisagreesWithItsArrays) {
  std::vector<TDate> ends{cdsmodel::to_tdate(d(2024, 4, 15)), cdsmodel::to_tdate(d(2024, 7, 15)),
                          cdsmodel::to_tdate(d(2024, 10, 15))};
  std::vector<TDate> pays = ends;

  RefFl bad(JpmcdsFeeLegMake(3, nullptr, ends.data(), pays.data(), 1.0e6, 0.01, JPMCDS_ACT_360,
                             ACCRUAL_PAY_ALL, TRUE));
  ASSERT_TRUE(static_cast<bool>(bad)) << "upstream started refusing a NULL array";
  EXPECT_EQ(bad.p->nbDates, 3) << "the count is kept";
  EXPECT_EQ(bad.p->accStartDates, nullptr) << "while the array it counts is NULL";
  EXPECT_NE(bad.p->accEndDates, nullptr) << "and the others were copied";
  // JpmcdsFeeLegPV reads fl->accStartDates[0] whenever nbDates > 1. Not called
  // here: that read is the hazard, and the register entry it would deserve is
  // withheld only because nothing declares the constructor.

  RefFl negative(JpmcdsFeeLegMake(-5, nullptr, nullptr, nullptr, 1.0e6, 0.01, JPMCDS_ACT_360,
                                  ACCRUAL_PAY_ALL, TRUE));
  ASSERT_TRUE(static_cast<bool>(negative));
  EXPECT_EQ(negative.p->nbDates, -5) << "a negative count is stored as given";

  // Unrepresentable in the port: one vector cannot disagree with its own
  // length, and there is no count to pass.
  const std::vector<cdsmodel::FeePayment> none;
  const cdsmodel::FeeLeg empty = cdsmodel::make_fee_leg(
      none, 1.0e6, 0.01, cdsmodel::DayCount::Act360, cdsmodel::AccrualPayment::All, true);
  EXPECT_TRUE(empty.payments.empty());
}

// JpmcdsFeeLegMakeEmpty is the only DECLARED constructor, and it hands back
// three zeroed arrays for the caller to fill in by hand -- which is what
// cds.c:166 then does at line 192.
TEST_F(CxDifferential, FeeLegMakeEmptyReturnsZeroedArraysForTheCallerToFill) {
  RefFl ref(JpmcdsFeeLegMakeEmpty(3));
  ASSERT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.p->nbDates, 3);
  ASSERT_NE(ref.p->accStartDates, nullptr);
  for (int i = 0; i < 3; ++i) {
    // JpmcdsMallocSafe memsets, so the dates are TDate 0 -- 1601-01-01, a real
    // date rather than an obviously-unset one.
    EXPECT_EQ(ref.p->accStartDates[i], 0) << "slot " << i;
  }

  RefFl zero(JpmcdsFeeLegMakeEmpty(0));
  ASSERT_TRUE(static_cast<bool>(zero));
  EXPECT_EQ(zero.p->nbDates, 0);
  EXPECT_EQ(zero.p->accStartDates, nullptr) << "zero length leaves the arrays NULL";
}

} // namespace
