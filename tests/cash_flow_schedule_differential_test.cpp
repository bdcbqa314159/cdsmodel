// TIER 2 -- live differential tests for the cash flow schedule constructors.
//
// Seven booleans across two upstream bitfields, crossed with stub position and
// length, is 2^7 x 4 combinations before any dates are chosen. Most are
// illegal or equivalent, so the sweep is over the ones that change the answer:
// the two stub axes, the three principal flags, and schedules whose span does
// and does not divide evenly.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "dtlist.h"
#include "ldate.h"
#include "stub.h"
}

namespace {

class CashFlowScheduleDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

struct Ref {
  TCashFlowList *cfl = nullptr;
  explicit Ref(TCashFlowList *owned) : cfl(owned) {}
  ~Ref() { JpmcdsFreeCFL(cfl); }
  Ref(const Ref &) = delete;
  Ref &operator=(const Ref &) = delete;
  explicit operator bool() const { return cfl != nullptr; }
};

std::vector<cdsmodel::CashFlow> as_flows(const TCashFlowList *cfl) {
  std::vector<cdsmodel::CashFlow> out;
  for (int i = 0; i < cfl->fNumItems; ++i) {
    out.push_back(
        cdsmodel::CashFlow{cdsmodel::from_tdate(cfl->fArray[i].fDate), cfl->fArray[i].fAmount});
  }
  return out;
}

TDateInterval raw_interval(cdsmodel::DateInterval iv) {
  TDateInterval out{};
  JpmcdsMakeDateInterval(iv.periods, iv.unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D', &out);
  return out;
}

long upstream_flags(const cdsmodel::CashFlowFlags &flags) {
  long out = 0;
  if (flags.add_final) {
    out |= JPMCDS_ADD_FINAL;
  }
  if (flags.subtract_initial) {
    out |= JPMCDS_SUBTRACT_INITIAL;
  }
  if (flags.prestart_zero_payment) {
    out |= JPMCDS_PRESTART_ZERO_PAYMENT;
  }
  if (flags.post_maturity_zero_payment) {
    out |= JPMCDS_POST_MATURITY_ZERO_PAYMENT;
  }
  if (flags.keep_start_date) {
    out |= JPMCDS_KEEP_START_DATE;
  }
  return out;
}

long upstream_stub_placement(cdsmodel::StubPosition position, cdsmodel::StubLength length) {
  long out =
      position == cdsmodel::StubPosition::Back ? JPMCDS_STUB_POSN_BACK : JPMCDS_STUB_POSN_FRONT;
  if (length == cdsmodel::StubLength::Long) {
    out |= JPMCDS_STUB_LENGTH_LONG;
  }
  return out;
}

/// Every single-flag setting plus a few pairs. The illegal pairs are included
/// on purpose -- both sides must refuse them.
std::vector<cdsmodel::CashFlowFlags> flag_sets() {
  using F = cdsmodel::CashFlowFlags;
  return {
      F{},
      F{.add_final = true},
      F{.subtract_initial = true},
      F{.prestart_zero_payment = true},
      F{.post_maturity_zero_payment = true},
      F{.keep_start_date = true},
      F{.subtract_initial = true, .add_final = true},
      F{.add_final = true, .keep_start_date = true},
      F{.subtract_initial = true, .prestart_zero_payment = true}, // illegal
      F{.prestart_zero_payment = true, .keep_start_date = true},  // illegal
      F{.add_final = true, .post_maturity_zero_payment = true},   // illegal
  };
}

TEST_F(CashFlowScheduleDifferential, MakeCflMatchesAcrossFlagsStubsAndSchedules) {
  const std::vector<std::pair<cdsmodel::Date, cdsmodel::Date>> spans{
      {d(2024, 1, 15), d(2027, 1, 15)}, // divides evenly by 3M, 6M, 12M
      {d(2024, 2, 20), d(2027, 5, 10)}, // divides by nothing tidy
      {d(2024, 1, 31), d(2026, 7, 31)}, // month ends
      {d(2024, 2, 29), d(2027, 2, 28)}, // the leap-day span
      {d(2024, 1, 15), d(2024, 4, 15)}, // one interval exactly
  };
  int compared = 0;
  int refused = 0;
  for (const auto &[start, maturity] : spans) {
    for (const int periods : {3, 6, 12}) {
      const cdsmodel::DateInterval iv{periods, cdsmodel::PeriodUnit::Months};
      for (const cdsmodel::StubPosition position :
           {cdsmodel::StubPosition::Front, cdsmodel::StubPosition::Back}) {
        for (const cdsmodel::StubLength length :
             {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
          for (const cdsmodel::CashFlowFlags &flags : flag_sets()) {
            TDateInterval raw = raw_interval(iv);
            const Ref ref(JpmcdsMakeCFL(
                0.05, cdsmodel::to_tdate(start), &raw, cdsmodel::to_tdate(maturity),
                JPMCDS_ACT_365F, upstream_stub_placement(position, length), upstream_flags(flags),
                JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE")));

            const cdsmodel::CashFlowConventions conventions{
                .rate = 0.05,
                .day_count = cdsmodel::DayCount::Act365Fixed,
                .stub_length = length,
                .accrual_bad_day = cdsmodel::BadDayConv::None,
                .pay_bad_day = cdsmodel::BadDayConv::None};
            const std::optional<std::vector<cdsmodel::CashFlow>> got =
                cdsmodel::cash_flows_from_schedule(start, maturity, iv, conventions, position,
                                                   flags, {});

            const std::string where = std::to_string(cdsmodel::to_tdate(start)) + ".." +
                                      std::to_string(cdsmodel::to_tdate(maturity)) + " iv " +
                                      std::to_string(periods) + " stub " +
                                      std::to_string(upstream_stub_placement(position, length)) +
                                      " flags " + std::to_string(upstream_flags(flags));
            ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
            if (got) {
              // Amounts are rate * yearFrac, one multiplication each, so
              // bit-identical.
              EXPECT_EQ(*got, as_flows(ref.cfl)) << where;
            } else {
              ++refused;
            }
            ++compared;
          }
        }
      }
    }
  }
  EXPECT_GT(compared, 600) << compared;
  EXPECT_GT(refused, 0) << "the illegal flag combinations must actually be exercised";
}

TEST_F(CashFlowScheduleDifferential, MakeCflMatchesUnderBusinessDayAdjustment) {
  // Upstream resolves a holiday FILE per side; "NONE" is the name both sides
  // agree on without a file on disk -- weekends only.
  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  int compared = 0;
  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous,
        cdsmodel::BadDayConv::Modified, cdsmodel::BadDayConv::None}) {
    for (const cdsmodel::StubPosition position :
         {cdsmodel::StubPosition::Front, cdsmodel::StubPosition::Back}) {
      const cdsmodel::DateInterval iv{3, cdsmodel::PeriodUnit::Months};
      TDateInterval raw = raw_interval(iv);
      const Ref ref(JpmcdsMakeCFL(0.05, cdsmodel::to_tdate(d(2024, 1, 6)), &raw,
                                  cdsmodel::to_tdate(d(2026, 7, 4)), JPMCDS_ACT_365F,
                                  upstream_stub_placement(position, cdsmodel::StubLength::Short),
                                  JPMCDS_ADD_FINAL, cdsmodel::to_upstream_code(conv),
                                  cdsmodel::to_upstream_code(conv), const_cast<char *>("NONE")));

      const cdsmodel::CashFlowConventions conventions{.rate = 0.05,
                                                      .day_count = cdsmodel::DayCount::Act365Fixed,
                                                      .stub_length = cdsmodel::StubLength::Short,
                                                      .accrual_bad_day = conv,
                                                      .pay_bad_day = conv};
      const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::cash_flows_from_schedule(
          d(2024, 1, 6), d(2026, 7, 4), iv, conventions, position, {.add_final = true}, calendars);
      const std::string where = std::string{static_cast<char>(conv)} + " stub " +
                                std::to_string(static_cast<int>(position));
      ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
      if (got) {
        EXPECT_EQ(*got, as_flows(ref.cfl)) << where;
      }
      ++compared;
    }
  }
  EXPECT_EQ(compared, 8);
}

TEST_F(CashFlowScheduleDifferential, FromDatesMatchesForGridsThatOverhangEitherEnd) {
  // cash_flows_from_dates is public on both sides, so the grid need not be one
  // the schedule generator would produce. These overhang the start, the
  // maturity, both, and neither.
  const cdsmodel::Date start = d(2024, 2, 20);
  const cdsmodel::Date maturity = d(2027, 5, 10);
  const std::vector<std::vector<cdsmodel::Date>> grids{
      {d(2024, 2, 20), d(2025, 2, 20), d(2026, 2, 20), d(2027, 5, 10)},  // neither
      {d(2024, 1, 10), d(2025, 1, 10), d(2026, 1, 10), d(2027, 5, 10)},  // front only
      {d(2024, 2, 20), d(2025, 2, 20), d(2026, 2, 20), d(2027, 12, 10)}, // back only
      {d(2024, 1, 10), d(2025, 1, 10), d(2026, 1, 10), d(2027, 12, 10)}, // both
      {d(2024, 1, 10), d(2027, 12, 10)},                                 // both, two dates
  };
  int compared = 0;
  for (const std::vector<cdsmodel::Date> &grid : grids) {
    for (const cdsmodel::StubLength length :
         {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
      for (const bool use_pre_start : {false, true}) {
        TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(grid.size()));
        ASSERT_NE(dl, nullptr);
        for (std::size_t i = 0; i < grid.size(); ++i) {
          dl->fArray[i] = cdsmodel::to_tdate(grid[i]);
        }
        const Ref ref(JpmcdsNewCFLFromDL3(
            dl, cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), 0.05, JPMCDS_ACT_365F,
            length == cdsmodel::StubLength::Long ? TRUE : FALSE, JPMCDS_BAD_DAY_NONE,
            JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"), const_cast<char *>("NONE"),
            use_pre_start ? TRUE : FALSE));
        JpmcdsFreeDateList(dl);

        const cdsmodel::CashFlowConventions conventions{
            .rate = 0.05,
            .day_count = cdsmodel::DayCount::Act365Fixed,
            .stub_length = length,
            .accrual_bad_day = cdsmodel::BadDayConv::None,
            .pay_bad_day = cdsmodel::BadDayConv::None};
        const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::cash_flows_from_dates(
            grid, start, maturity, conventions, {}, {}, use_pre_start);
        const std::string where = "grid " + std::to_string(grid.size()) + " long " +
                                  std::to_string(static_cast<int>(length)) + " pre " +
                                  std::to_string(use_pre_start);
        ASSERT_EQ(got.has_value(), static_cast<bool>(ref)) << where;
        if (got) {
          EXPECT_EQ(*got, as_flows(ref.cfl)) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 15) << compared;
}

TEST_F(CashFlowScheduleDifferential, AnEmptyGridGivesAnEmptyStreamOnBothSides) {
  TDateList *dl = JpmcdsNewEmptyDateList(0);
  ASSERT_NE(dl, nullptr);
  const Ref ref(JpmcdsNewCFLFromDL3(dl, cdsmodel::to_tdate(d(2024, 1, 15)),
                                    cdsmodel::to_tdate(d(2027, 1, 15)), 0.05, JPMCDS_ACT_365F,
                                    FALSE, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                                    const_cast<char *>("NONE"), const_cast<char *>("NONE"), FALSE));
  JpmcdsFreeDateList(dl);
  ASSERT_TRUE(ref);
  EXPECT_EQ(ref.cfl->fNumItems, 0);

  const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::cash_flows_from_dates(
      {}, d(2024, 1, 15), d(2027, 1, 15), {.rate = 0.05}, {}, {}, false);
  ASSERT_TRUE(got.has_value()) << "an empty grid is not an error";
  EXPECT_TRUE(got->empty());
}

TEST_F(CashFlowScheduleDifferential, DISABLED_Upstream25_AOneDateGridWithAStubOverflowsTheHeap) {
  // Disabled: AddressSanitizer aborts the process. With numDates == 1 the
  // front-stub branch reads accrualDates->fArray[1] and writes
  // cfl->fArray[1].fAmount, both past a one-element allocation; the back-stub
  // branch reads fArray[-1], before it.
  //
  // Live evidence: ./build/debug/defect_evidence u25 --unsafe
  TDateList *dl = JpmcdsNewEmptyDateList(1);
  dl->fArray[0] = cdsmodel::to_tdate(d(2024, 1, 1));
  TCashFlowList *cfl =
      JpmcdsNewCFLFromDL3(dl, cdsmodel::to_tdate(d(2024, 6, 1)), cdsmodel::to_tdate(d(2024, 12, 1)),
                          0.05, JPMCDS_ACT_365F, FALSE, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                          const_cast<char *>("NONE"), const_cast<char *>("NONE"), FALSE);
  JpmcdsFreeCFL(cfl);
  JpmcdsFreeDateList(dl);
}

TEST_F(CashFlowScheduleDifferential, Upstream25_ThePortRefusesAOneDateGridWithAStub) {
  const std::vector<cdsmodel::Date> one{d(2024, 1, 1)};
  const cdsmodel::CashFlowConventions conventions{.rate = 0.05};

  // A front stub: the start is after the only grid date.
  EXPECT_FALSE(cdsmodel::cash_flows_from_dates(one, d(2024, 6, 1), d(2024, 12, 1), conventions, {},
                                               {}, false)
                   .has_value());
  // A back stub: the maturity is before it.
  EXPECT_FALSE(
      cdsmodel::cash_flows_from_dates(one, d(2023, 1, 1), d(2023, 6, 1), conventions, {}, {}, false)
          .has_value());
  // No stub at all: the one date is fine, and matches upstream.
  TDateList *dl = JpmcdsNewEmptyDateList(1);
  ASSERT_NE(dl, nullptr);
  dl->fArray[0] = cdsmodel::to_tdate(d(2024, 1, 1));
  const Ref ref(JpmcdsNewCFLFromDL3(dl, cdsmodel::to_tdate(d(2024, 1, 1)),
                                    cdsmodel::to_tdate(d(2024, 1, 1)), 0.05, JPMCDS_ACT_365F, FALSE,
                                    JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                                    const_cast<char *>("NONE"), const_cast<char *>("NONE"), FALSE));
  JpmcdsFreeDateList(dl);
  const std::optional<std::vector<cdsmodel::CashFlow>> got = cdsmodel::cash_flows_from_dates(
      one, d(2024, 1, 1), d(2024, 1, 1), conventions, {}, {}, false);
  ASSERT_EQ(got.has_value(), static_cast<bool>(ref));
  if (got) {
    EXPECT_EQ(*got, as_flows(ref.cfl)) << "no stub, so no overflow and no divergence";
  }
}

TEST_F(CashFlowScheduleDifferential, TheLongStubCoalescingMatchesWhereBothStubsExist) {
  // The case that makes upstream's second memcpy copy a buffer onto itself.
  // Recorded as an observation rather than a defect -- the result is correct
  // and neither ASan nor UBSan reports it -- so what this asserts is that the
  // port reproduces the ANSWER.
  const std::vector<cdsmodel::Date> grid{d(2024, 1, 10), d(2024, 7, 10), d(2025, 1, 10),
                                         d(2025, 7, 10), d(2027, 12, 10)};
  const cdsmodel::Date start = d(2024, 2, 20);
  const cdsmodel::Date maturity = d(2027, 5, 10);
  ASSERT_GT(start, grid.front()) << "the premise: a front stub";
  ASSERT_LT(maturity, grid.back()) << "and a back stub";

  for (const cdsmodel::StubLength length :
       {cdsmodel::StubLength::Short, cdsmodel::StubLength::Long}) {
    TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(grid.size()));
    ASSERT_NE(dl, nullptr);
    for (std::size_t i = 0; i < grid.size(); ++i) {
      dl->fArray[i] = cdsmodel::to_tdate(grid[i]);
    }
    const Ref ref(JpmcdsNewCFLFromDL3(
        dl, cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity), 0.05, JPMCDS_ACT_365F,
        length == cdsmodel::StubLength::Long ? TRUE : FALSE, JPMCDS_BAD_DAY_NONE,
        JPMCDS_BAD_DAY_NONE, const_cast<char *>("NONE"), const_cast<char *>("NONE"), FALSE));
    JpmcdsFreeDateList(dl);
    ASSERT_TRUE(ref) << static_cast<int>(length);

    const cdsmodel::CashFlowConventions conventions{
        .rate = 0.05, .day_count = cdsmodel::DayCount::Act365Fixed, .stub_length = length};
    const std::optional<std::vector<cdsmodel::CashFlow>> got =
        cdsmodel::cash_flows_from_dates(grid, start, maturity, conventions, {}, {}, false);
    ASSERT_TRUE(got.has_value()) << static_cast<int>(length);
    EXPECT_EQ(*got, as_flows(ref.cfl)) << static_cast<int>(length);
    // A long stub coalesces both ends, so five grid dates become three flows.
    if (length == cdsmodel::StubLength::Long) {
      EXPECT_EQ(got->size(), 3u);
    } else {
      EXPECT_EQ(got->size(), 5u);
    }
  }
}

} // namespace
