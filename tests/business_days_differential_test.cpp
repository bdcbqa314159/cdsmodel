// TIER 2 -- live differential tests for business day counting.

#include <cdsmodel/business_days.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <utility>
#include <vector>

extern "C" {
#include "buscache.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
}

namespace {

class BusinessDaysDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
  void TearDown() override { JpmcdsHolidayEmptyCache(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

THolidayList *build(const std::vector<cdsmodel::Date> &holidays, long weekends,
                    std::vector<TDate> &storage) {
  storage.clear();
  for (cdsmodel::Date h : holidays) {
    storage.push_back(cdsmodel::to_tdate(h));
  }
  TDateList dl{};
  dl.fNumItems = static_cast<int>(storage.size());
  dl.fArray = storage.empty() ? nullptr : storage.data();
  return JpmcdsHolidayListNewGeneral(storage.empty() ? nullptr : &dl, weekends);
}

const long kWeekendSets[] = {0x00, 0x60, 0x40, 0x30, 0x01, 0x20, 0x7E};

std::vector<cdsmodel::Date> holiday_set() {
  return {d(2023, 12, 25), d(2023, 12, 26), d(2024, 1, 1),   d(2024, 3, 29),
          d(2024, 4, 1),   d(2024, 5, 6),   d(2024, 5, 27),  d(2024, 7, 4),
          d(2024, 8, 26),  d(2024, 11, 28), d(2024, 12, 25), d(2024, 12, 26)};
}

TEST_F(BusinessDaysDifferential, MatchesForEveryWeekendSetBothDirections) {
  const auto holidays = holiday_set();
  std::vector<TDate> storage;
  for (long w : kWeekendSets) {
    THolidayList *hl = build(holidays, w, storage);
    ASSERT_NE(hl, nullptr) << "weekends=0x" << std::hex << w;
    const auto cal = cdsmodel::Calendar::create(holidays, *cdsmodel::weekend_from_upstream(w));
    ASSERT_TRUE(cal.has_value());

    // Anchors spread across the holiday set, with offsets covering sub-week,
    // exact-week and multi-month spans in both directions. Exact multiples of
    // 7 matter: that is where upstream's forward/backward branch flips.
    for (int anchor : {0, 17, 31, 60, 120, 200, 300}) {
      const cdsmodel::Date a = d(2023, 12, 1) + std::chrono::days{anchor};
      for (int offset :
           {-200, -91, -35, -14, -8, -7, -6, -3, -1, 0, 1, 3, 6, 7, 8, 14, 35, 91, 200}) {
        const cdsmodel::Date b = a + std::chrono::days{offset};
        long reference = 0;
        const bool reference_ok =
            JpmcdsHolidayListBusinessDaysDiff(cdsmodel::to_tdate(a), cdsmodel::to_tdate(b), hl,
                                              &reference) == SUCCESS;
        const auto ported = cdsmodel::business_days_between(a, b, *cal);

        ASSERT_EQ(ported.has_value(), reference_ok)
            << "validity, weekends=0x" << std::hex << w << std::dec
            << " from=" << cdsmodel::to_tdate(a) << " offset=" << offset;
        if (reference_ok) {
          EXPECT_EQ(*ported, reference) << "value, weekends=0x" << std::hex << w << std::dec
                                        << " from=" << cdsmodel::to_tdate(a)
                                        << " to=" << cdsmodel::to_tdate(b) << " offset=" << offset;
        }
      }
    }
    JpmcdsHolidayListDelete(hl);
  }
}

TEST_F(BusinessDaysDifferential, MatchesAcrossEveryWeekdayPairWithinOneWeek) {
  // The lookup tables are indexed by weekday pair, so every one of the 49
  // combinations must be hit, in both directions, to exercise both tables.
  std::vector<TDate> storage;
  THolidayList *hl = build({}, 0x60, storage); // standard weekends, no holidays
  ASSERT_NE(hl, nullptr);
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();

  for (int i = 0; i < 7; ++i) {
    for (int j = 0; j < 7; ++j) {
      // Forward within a week, and the mirrored backward pair.
      const cdsmodel::Date a = d(2024, 6, 3) + std::chrono::days{i}; // Mon + i
      const cdsmodel::Date b = d(2024, 6, 10) + std::chrono::days{j};
      for (auto [from, to] : {std::pair{a, b}, std::pair{b, a}}) {
        long reference = 0;
        ASSERT_EQ(JpmcdsHolidayListBusinessDaysDiff(cdsmodel::to_tdate(from),
                                                    cdsmodel::to_tdate(to), hl, &reference),
                  SUCCESS);
        EXPECT_EQ(*cdsmodel::business_days_between(from, to, cal), reference)
            << "weekday pair " << i << "," << j << " from=" << cdsmodel::to_tdate(from);
      }
    }
  }
  JpmcdsHolidayListDelete(hl);
}

TEST_F(BusinessDaysDifferential, IsNotAntisymmetricAndUpstreamAgrees) {
  // Pins the documented asymmetry. If upstream ever makes the tables mirror
  // each other, this fails and the port's transcription can be revisited.
  std::vector<TDate> storage;
  THolidayList *hl = build({}, 0x60, storage);
  ASSERT_NE(hl, nullptr);
  const cdsmodel::Calendar cal = cdsmodel::Calendar::none();

  int asymmetric_pairs = 0;
  for (int i = 0; i < 28; ++i) {
    for (int j = 0; j < 28; ++j) {
      const cdsmodel::Date a = d(2024, 6, 1) + std::chrono::days{i};
      const cdsmodel::Date b = d(2024, 6, 1) + std::chrono::days{j};
      long fwd = 0;
      long rev = 0;
      ASSERT_EQ(
          JpmcdsHolidayListBusinessDaysDiff(cdsmodel::to_tdate(a), cdsmodel::to_tdate(b), hl, &fwd),
          SUCCESS);
      ASSERT_EQ(
          JpmcdsHolidayListBusinessDaysDiff(cdsmodel::to_tdate(b), cdsmodel::to_tdate(a), hl, &rev),
          SUCCESS);
      if (fwd != -rev) {
        ++asymmetric_pairs;
      }
      // Whatever upstream says, the port says.
      EXPECT_EQ(*cdsmodel::business_days_between(a, b, cal), fwd);
      EXPECT_EQ(*cdsmodel::business_days_between(b, a, cal), rev);
    }
  }
  EXPECT_GT(asymmetric_pairs, 0) << "upstream's forward and backward tables now mirror each other; "
                                    "the transcription in business_days.hpp can be reconsidered";
  JpmcdsHolidayListDelete(hl);
}

TEST_F(BusinessDaysDifferential, HolidayAdjustmentMatchesOverTheClosedInterval) {
  // The holiday subtraction uses a closed interval that differs by direction:
  // [from+1, to] forwards, [to, from-1] backwards. An off-by-one there shows
  // up only when an endpoint IS a holiday, so anchor directly on holidays.
  const auto holidays = holiday_set();
  std::vector<TDate> storage;
  THolidayList *hl = build(holidays, 0x60, storage);
  ASSERT_NE(hl, nullptr);
  const auto cal = cdsmodel::Calendar::create(holidays, cdsmodel::Weekend::Standard);
  ASSERT_TRUE(cal.has_value());

  for (cdsmodel::Date h : cal->holidays()) {
    for (int offset : {-10, -5, -2, -1, 0, 1, 2, 5, 10}) {
      const cdsmodel::Date other = h + std::chrono::days{offset};
      for (auto [from, to] : {std::pair{h, other}, std::pair{other, h}}) {
        long reference = 0;
        ASSERT_EQ(JpmcdsHolidayListBusinessDaysDiff(cdsmodel::to_tdate(from),
                                                    cdsmodel::to_tdate(to), hl, &reference),
                  SUCCESS);
        EXPECT_EQ(*cdsmodel::business_days_between(from, to, *cal), reference)
            << "from=" << cdsmodel::to_tdate(from) << " to=" << cdsmodel::to_tdate(to);
      }
    }
  }
  JpmcdsHolidayListDelete(hl);
}

} // namespace
