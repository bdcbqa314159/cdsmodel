// TIER 2 -- live differential tests for the rest of stub.c.
//
// Four functions whose answers are small -- a double, a bool, an enum, a pair
// -- and whose interest is entirely in which inputs they REFUSE. So the sweep
// is over the refusal boundaries: stub windows that touch one end of a coupon
// period, both, neither, or fall outside it; and packed values whose two
// halves are independently valid or not.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "date_sup.h"
#include "ldate.h"
#include "stub.h"
}

namespace {

class StubDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TEST_F(StubDifferential, StubPaymentMatchesAcrossTheWholeAcceptanceBoundary) {
  const cdsmodel::Date prev = d(2024, 1, 15);
  const cdsmodel::Date next = d(2024, 7, 15);
  // Windows that touch the start, the end, both, neither, and that fall
  // outside the period on either side.
  const cdsmodel::Date boundaries[] = {d(2023, 12, 1), d(2024, 1, 15), d(2024, 2, 29),
                                       d(2024, 4, 15), d(2024, 7, 15), d(2024, 9, 1)};
  int compared = 0;
  int refused = 0;
  for (const cdsmodel::Date stub_start : boundaries) {
    for (const cdsmodel::Date stub_end : boundaries) {
      for (const cdsmodel::DayCount dcc :
           {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
            cdsmodel::DayCount::Thirty360, cdsmodel::DayCount::ThirtyE360}) {
        for (const double rate : {0.0, 0.05, -0.02, 1.5}) {
          double want = 0.0;
          const int rc = JpmcdsStubPayment(
              cdsmodel::to_tdate(prev), cdsmodel::to_tdate(next), cdsmodel::to_tdate(stub_start),
              cdsmodel::to_tdate(stub_end), rate, cdsmodel::to_upstream_code(dcc), &want);
          const std::optional<double> got =
              cdsmodel::stub_payment(prev, next, stub_start, stub_end, rate, dcc);
          const std::string where = std::to_string(cdsmodel::to_tdate(stub_start)) + ".." +
                                    std::to_string(cdsmodel::to_tdate(stub_end)) + " dcc " +
                                    std::to_string(cdsmodel::to_upstream_code(dcc)) + " rate " +
                                    std::to_string(rate);
          ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
          if (got) {
            // One multiplication of a year fraction, so bit-identical.
            EXPECT_EQ(*got, want) << where;
          } else {
            ++refused;
          }
          ++compared;
        }
      }
    }
  }
  EXPECT_GT(compared, 300) << compared;
  EXPECT_GT(refused, 0) << "the sweep must actually cross the refusal boundary";
}

TEST_F(StubDifferential, AnInvertedOrDegenerateCouponPeriodIsRefusedByBoth) {
  const cdsmodel::Date a = d(2024, 1, 15);
  const cdsmodel::Date b = d(2024, 7, 15);
  for (const auto &[prev, next] : {std::pair{b, a}, std::pair{a, a}}) {
    double want = 0.0;
    const int rc = JpmcdsStubPayment(cdsmodel::to_tdate(prev), cdsmodel::to_tdate(next),
                                     cdsmodel::to_tdate(prev), cdsmodel::to_tdate(next), 0.05,
                                     JPMCDS_ACT_365F, &want);
    EXPECT_EQ(rc, FAILURE);
    EXPECT_FALSE(
        cdsmodel::stub_payment(prev, next, prev, next, 0.05, cdsmodel::DayCount::Act365Fixed)
            .has_value());
  }
}

TEST_F(StubDifferential, IsEndStubMatchesForEveryPositionAndSchedule) {
  int compared = 0;
  const long positions[] = {JPMCDS_STUB_POS_DEFAULT_AUTO, JPMCDS_STUB_POS_DEFAULT_FRONT,
                            JPMCDS_STUB_POS_DEFAULT_BACK};
  const std::pair<cdsmodel::Date, cdsmodel::Date> spans[] = {
      {d(2024, 1, 15), d(2027, 1, 15)}, // divides evenly by 3M, 6M, 12M
      {d(2024, 1, 15), d(2024, 2, 20)}, // shorter than one quarter
      {d(2024, 2, 29), d(2027, 2, 28)}, // the leap-day span
      {d(2024, 3, 1), d(2029, 11, 30)}, // divides by nothing tidy
      {d(2024, 1, 15), d(2024, 1, 15)}, // zero length
  };
  for (const auto &[start, maturity] : spans) {
    for (const int periods : {1, 2, 3, 4, 6, 7, 12}) {
      TDateInterval raw{};
      ASSERT_EQ(JpmcdsMakeDateInterval(periods, 'M', &raw), SUCCESS);
      const cdsmodel::DateInterval iv{periods, cdsmodel::PeriodUnit::Months};
      for (const long position : positions) {
        TBoolean want = FALSE;
        const int rc = JpmcdsIsEndStub(cdsmodel::to_tdate(start), cdsmodel::to_tdate(maturity),
                                       &raw, static_cast<TStubPos>(position), &want);
        const std::optional<cdsmodel::StubPosDefault> ported =
            cdsmodel::stub_pos_from_upstream(position);
        ASSERT_TRUE(ported.has_value()) << position;
        const std::optional<bool> got = cdsmodel::is_end_stub(start, maturity, iv, *ported);
        const std::string where = std::to_string(cdsmodel::to_tdate(start)) + " iv " +
                                  std::to_string(periods) + " pos " + std::to_string(position);
        ASSERT_EQ(got.has_value(), rc == SUCCESS) << where;
        if (got) {
          EXPECT_EQ(*got, want == TRUE) << where;
        }
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 90) << compared;
}

TEST_F(StubDifferential, AnUnknownStubPositionIsRefusedByBoth) {
  // Upstream's switch has no default arm; `status` stays FAILURE and the
  // output is never written. The port's enum cannot hold the value at all, so
  // this checks the upstream half -- that these really are rejections.
  TDateInterval quarterly{};
  ASSERT_EQ(JpmcdsMakeDateInterval(3, 'M', &quarterly), SUCCESS);
  for (const long bogus : {-1L, 3L, 7L, 99L}) {
    TBoolean out = FALSE;
    EXPECT_EQ(JpmcdsIsEndStub(cdsmodel::to_tdate(d(2024, 1, 15)),
                              cdsmodel::to_tdate(d(2027, 1, 15)), &quarterly,
                              static_cast<TStubPos>(bogus), &out),
              FAILURE)
        << bogus;
    EXPECT_FALSE(cdsmodel::stub_pos_from_upstream(bogus).has_value()) << bogus;
  }
  // And the three real values round-trip.
  for (const long code : {0L, 1L, 2L}) {
    const std::optional<cdsmodel::StubPosDefault> pos = cdsmodel::stub_pos_from_upstream(code);
    ASSERT_TRUE(pos.has_value()) << code;
    EXPECT_EQ(cdsmodel::to_upstream_code(*pos), code);
    TStubPos reference{};
    EXPECT_EQ(JpmcdsStubPosMake(code, &reference), SUCCESS) << code;
    EXPECT_EQ(static_cast<long>(reference), code);
  }
}

TEST_F(StubDifferential, Upstream_TheSplitValidatesOnlyOneOfItsTwoOutputs) {
  // Not a defect -- recorded as an observation in stub.hpp. The stub position
  // is checked; the bad day convention is masked out and assigned unchecked,
  // though JpmcdsBadDayConvValid is in the same library.
  int accepted_invalid = 0;
  for (const long packed :
       {0x0046L, 0x0146L, 0x0246L, 0x0346L, 0x0041L, 0x0000L, 0x00FFL, 0x0150L, 0x024EL, 0x014DL}) {
    long conv = -1;
    TStubPos position{};
    const int rc =
        JpmcdsBadDayAndStubPosSplit(static_cast<TBadDayAndStubPos>(packed), &conv, &position);
    const std::optional<std::pair<cdsmodel::BadDayConv, cdsmodel::StubPosDefault>> got =
        cdsmodel::split_bad_day_and_stub_pos(packed);
    const std::string where = std::to_string(packed);

    if (rc == SUCCESS) {
      const bool conv_valid = cdsmodel::bad_day_conv_from_upstream(conv).has_value();
      // The stub position always agrees.
      ASSERT_TRUE(cdsmodel::stub_pos_from_upstream(static_cast<long>(position)).has_value())
          << where;
      if (conv_valid) {
        ASSERT_TRUE(got.has_value()) << where;
        EXPECT_EQ(cdsmodel::to_upstream_code(got->first), conv) << where;
        EXPECT_EQ(cdsmodel::to_upstream_code(got->second), static_cast<long>(position)) << where;
      } else {
        ++accepted_invalid;
        // The deliberate difference: upstream succeeds here, the port does not.
        EXPECT_FALSE(got.has_value())
            << where << ": the port refuses a convention it cannot represent";
        // And upstream's own validator disagrees with upstream's own split.
        EXPECT_EQ(JpmcdsBadDayConvValid(const_cast<char *>("test"), conv), FAILURE) << where;
      }
    } else {
      EXPECT_FALSE(got.has_value()) << where;
    }
  }
  EXPECT_GT(accepted_invalid, 0) << "the sweep must include a convention upstream lets through";
}

TEST_F(StubDifferential, AnUnvalidatedConventionStillFailsAtFirstUseWhateverTheDate) {
  // The reason the above is an observation and not a defect. My first reading
  // was that a bogus convention would travel until some date happened to need
  // adjusting, making detection depend on the data. It does not:
  // JpmcdsBusinessDay validates the convention BEFORE asking whether the date
  // is a business day, so the same value fails on a Monday and a Saturday
  // alike. Measured here rather than asserted in prose.
  const cdsmodel::Date monday = d(2024, 1, 8);
  const cdsmodel::Date saturday = d(2024, 1, 6);
  for (const long conv : {long{'A'}, 0L, 255L, long{'f'}}) {
    ASSERT_EQ(JpmcdsBadDayConvValid(const_cast<char *>("test"), conv), FAILURE) << conv;
    for (const cdsmodel::Date date : {monday, saturday}) {
      TDate out = 0;
      EXPECT_EQ(JpmcdsBusinessDay(cdsmodel::to_tdate(date), conv, const_cast<char *>("NONE"), &out),
                FAILURE)
          << conv << " on " << cdsmodel::to_tdate(date);
    }
  }
  // And the four real conventions succeed on both.
  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous, cdsmodel::BadDayConv::None,
        cdsmodel::BadDayConv::Modified}) {
    for (const cdsmodel::Date date : {monday, saturday}) {
      TDate out = 0;
      EXPECT_EQ(JpmcdsBusinessDay(cdsmodel::to_tdate(date), cdsmodel::to_upstream_code(conv),
                                  const_cast<char *>("NONE"), &out),
                SUCCESS)
          << static_cast<char>(conv);
    }
  }
}

TEST_F(StubDifferential, TheCouponPeriodDoesNotEnterTheAnswerOnEitherSide) {
  // The dead `coupon` and `stubFrac` locals mean the result is rate *
  // stubYears and nothing else. Shown by moving the period's far end and
  // watching both implementations not change.
  const cdsmodel::Date prev = d(2024, 1, 15);
  const cdsmodel::Date stub_start = prev;
  const cdsmodel::Date stub_end = d(2024, 4, 15);
  std::optional<double> first_upstream;
  std::optional<double> first_port;
  for (const cdsmodel::Date next : {d(2024, 7, 15), d(2025, 1, 15), d(2030, 6, 30)}) {
    double want = 0.0;
    ASSERT_EQ(JpmcdsStubPayment(cdsmodel::to_tdate(prev), cdsmodel::to_tdate(next),
                                cdsmodel::to_tdate(stub_start), cdsmodel::to_tdate(stub_end), 0.05,
                                JPMCDS_ACT_365F, &want),
              SUCCESS);
    const std::optional<double> got = cdsmodel::stub_payment(prev, next, stub_start, stub_end, 0.05,
                                                             cdsmodel::DayCount::Act365Fixed);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, want);
    if (!first_upstream) {
      first_upstream = want;
      first_port = *got;
    }
    EXPECT_EQ(want, *first_upstream) << "upstream's answer moved with the coupon period";
    EXPECT_EQ(*got, *first_port) << "the port's answer moved with the coupon period";
  }
}

} // namespace
