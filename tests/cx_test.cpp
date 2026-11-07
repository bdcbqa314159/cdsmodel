// TIER 1 -- replays tests/golden/cx_cases.hpp, so the leg constructors are
// covered in CI, where no oracle is present.
//
// Dates, flags and counts only, so these compare EXACTLY. There is no
// tolerance in this file.

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>

#include "golden/cx_cases.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <vector>

namespace {

using namespace cdsmodel::golden;

TEST(CxGolden, ReplaysEveryRecordedContingentLeg) {
  ASSERT_GE(kContingentLegMakeCases.size(), 50u);
  int matched = 0;
  int refused = 0;
  for (const ContingentLegMakeCase &c : kContingentLegMakeCases) {
    const std::optional<cdsmodel::ContingentLeg> got = cdsmodel::make_contingent_leg(
        cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.end), c.notional,
        cdsmodel::ProtectionPayment::AtDefault, c.protect_start != 0);
    ASSERT_EQ(got.has_value(), c.accepted)
        << "start " << c.start << " end " << c.end << " protect " << c.protect_start;
    if (!c.accepted) {
      ++refused;
      continue;
    }
    EXPECT_EQ(cdsmodel::to_tdate(got->start), c.stored_start);
    EXPECT_EQ(cdsmodel::to_tdate(got->end), c.stored_end);
    EXPECT_DOUBLE_EQ(got->notional, c.notional);
    EXPECT_EQ(got->protect_start, c.stored_protect_start);
    ++matched;
  }
  EXPECT_GT(matched, 100);
  EXPECT_GT(refused, 10) << "the refusal path was barely exercised";
}

// The behaviour the recording exists for, asserted as a property over the
// recorded data rather than on hand-picked cases.
TEST(CxGolden, ProtectStartShiftsTheStoredStartByExactlyOneDay) {
  int shifted = 0;
  int unshifted = 0;
  for (const ContingentLegMakeCase &c : kContingentLegMakeCases) {
    if (!c.accepted) {
      continue;
    }
    if (c.protect_start != 0) {
      EXPECT_EQ(c.stored_start, c.start - 1) << "protect_start did not shift by one day";
      ++shifted;
    } else {
      EXPECT_EQ(c.stored_start, c.start) << "the start date moved without protect_start";
      ++unshifted;
    }
    EXPECT_EQ(c.stored_end, c.end) << "the end date was altered";
  }
  // The exact counts, because they ARE the asymmetry and a threshold hides it.
  // 4 anchors x 3 notionals x the accepted offsets: with protect_start the
  // check is `end > start - 1`, so offsets {0,1,2,90,1827} pass -- five of
  // seven, giving 60. Without it the check is `end > start`, so offset 0 is
  // refused too -- four of seven, giving 48. That difference of 12 is the
  // whole behaviour this module exists to get right.
  EXPECT_EQ(shifted, 4 * 3 * 5) << "the accepted-with-protect_start count moved";
  EXPECT_EQ(unshifted, 4 * 3 * 4) << "the accepted-without-protect_start count moved";
  EXPECT_EQ(shifted - unshifted, 4 * 3)
      << "protect_start no longer admits exactly the zero-length spans";
}

// The acceptance rule, as a property: the check is on the SHIFTED start, so
// `end > start - protect_start` decides. A port that validated before shifting
// would pass the replay on every case except the zero-length ones.
TEST(CxGolden, AcceptanceIsDecidedAfterTheShift) {
  int zero_length_accepted = 0;
  int zero_length_refused = 0;
  for (const ContingentLegMakeCase &c : kContingentLegMakeCases) {
    const cdsmodel::TDate effective_start = c.start - (c.protect_start != 0 ? 1 : 0);
    EXPECT_EQ(c.accepted, c.end > effective_start)
        << "start " << c.start << " end " << c.end << " protect " << c.protect_start;
    if (c.end == c.start) {
      if (c.accepted) {
        ++zero_length_accepted;
      } else {
        ++zero_length_refused;
      }
    }
  }
  // Both must occur: the whole point is that the flag decides.
  EXPECT_GT(zero_length_accepted, 0) << "no zero-length leg was accepted";
  EXPECT_GT(zero_length_refused, 0) << "no zero-length leg was refused";
}

// A fee leg cannot have a count that disagrees with its arrays, because it has
// no count. Upstream's JpmcdsFeeLegMake can, and is unreachable -- see
// fee_leg.hpp.
TEST(CxGolden, AFeeLegsLengthIsItsOwn) {
  const std::vector<cdsmodel::FeePayment> payments{
      {*cdsmodel::make_date(2024, 1, 15), *cdsmodel::make_date(2024, 4, 15),
       *cdsmodel::make_date(2024, 4, 15)},
      {*cdsmodel::make_date(2024, 4, 15), *cdsmodel::make_date(2024, 7, 15),
       *cdsmodel::make_date(2024, 7, 15)}};
  const cdsmodel::FeeLeg leg = cdsmodel::make_fee_leg(
      payments, 1.0e6, 0.01, cdsmodel::DayCount::Act360, cdsmodel::AccrualPayment::All, true);
  EXPECT_EQ(leg.payments.size(), payments.size());
  EXPECT_EQ(leg.payments, payments);
  EXPECT_DOUBLE_EQ(leg.notional, 1.0e6);
  EXPECT_DOUBLE_EQ(leg.coupon_rate, 0.01);
  EXPECT_EQ(leg.day_count, cdsmodel::DayCount::Act360);
  EXPECT_EQ(leg.accrual_pay, cdsmodel::AccrualPayment::All);
  EXPECT_TRUE(leg.observe_start_of_day);

  // An empty leg is permitted, as upstream permits nbDates == 0.
  const std::vector<cdsmodel::FeePayment> none;
  const cdsmodel::FeeLeg empty = cdsmodel::make_fee_leg(
      none, 1.0e6, 0.01, cdsmodel::DayCount::Act360, cdsmodel::AccrualPayment::All, true);
  EXPECT_TRUE(empty.payments.empty());
}

} // namespace
