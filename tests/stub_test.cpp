// TIER 1 -- stub method parsing. Port semantics plus recorded vectors.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include "golden/schedule_cases.hpp"
#include "golden/stub_cases.hpp"

#include <optional>
#include <string>
#include <utility>

namespace {

using cdsmodel::parse_stub_method;
using cdsmodel::StubLength;
using cdsmodel::StubMethod;
using cdsmodel::StubPosition;

TEST(Stub, CanonicalForms) {
  EXPECT_EQ(parse_stub_method("f/s"), (StubMethod{StubPosition::Front, StubLength::Short}));
  EXPECT_EQ(parse_stub_method("f/l"), (StubMethod{StubPosition::Front, StubLength::Long}));
  EXPECT_EQ(parse_stub_method("b/s"), (StubMethod{StubPosition::Back, StubLength::Short}));
  EXPECT_EQ(parse_stub_method("b/l"), (StubMethod{StubPosition::Back, StubLength::Long}));
}

TEST(Stub, CaseInsensitive) {
  EXPECT_EQ(parse_stub_method("F/S"), parse_stub_method("f/s"));
  EXPECT_EQ(parse_stub_method("B/L"), parse_stub_method("b/l"));
}

TEST(Stub, OnlyTheFirstCharacterOfEachFieldIsRead) {
  // Upstream's leniency, preserved: whole words work, and so does nonsense
  // that happens to start with the right letter.
  EXPECT_EQ(parse_stub_method("front/short"), parse_stub_method("f/s"));
  EXPECT_EQ(parse_stub_method("back/long"), parse_stub_method("b/l"));
  EXPECT_EQ(parse_stub_method("fromage/sausage"), parse_stub_method("f/s"));
}

TEST(Stub, MissingLengthDefaultsToShortButEmptyLengthFails) {
  // The asymmetry is upstream's: a missing field is a null pointer, an empty
  // one is "" whose first character falls through to the error branch.
  EXPECT_EQ(parse_stub_method("f"), (StubMethod{StubPosition::Front, StubLength::Short}));
  EXPECT_EQ(parse_stub_method("b"), (StubMethod{StubPosition::Back, StubLength::Short}));
  EXPECT_FALSE(parse_stub_method("f/").has_value());
  EXPECT_FALSE(parse_stub_method("b/").has_value());
}

TEST(Stub, ThirdFieldIsSilentlyIgnored) {
  // Upstream's error text documents LOCATION/TYPE/[LENGTH], but the parser
  // splits on one '/' and reads field two as LENGTH. There is no TYPE field.
  EXPECT_EQ(parse_stub_method("f/s/l"), (StubMethod{StubPosition::Front, StubLength::Short}));
  EXPECT_EQ(parse_stub_method("b/l/whatever"), (StubMethod{StubPosition::Back, StubLength::Long}));
  // So the format the message describes is rejected: 'n' is not a length.
  EXPECT_FALSE(parse_stub_method("f/n/l").has_value());
}

TEST(Stub, Rejected) {
  for (std::string_view bad : {"", "/", "/s", "x", "x/s", " f/s", "1/2"}) {
    EXPECT_FALSE(parse_stub_method(bad).has_value()) << "accepted " << bad;
  }
}

TEST(Stub, NonAsciiDoesNotInvokeUndefinedBehaviour) {
  // Upstream calls toupper() on a possibly-negative char here. The port
  // compares characters directly, so a high byte is simply not a match.
  EXPECT_FALSE(parse_stub_method("\xe9/s").has_value());
  EXPECT_FALSE(parse_stub_method("f/\xe9").has_value());
}

TEST(Stub, ConstexprUsable) {
  static constexpr auto parsed = parse_stub_method("b/l");
  static_assert(parsed.has_value());
  static_assert(parsed->position == StubPosition::Back);
  static_assert(parsed->length == StubLength::Long);
}

TEST(Stub, MatchesRecordedVectors) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kStubParseCases) {
    const auto got = parse_stub_method(c.name);
    ASSERT_EQ(got.has_value(), c.ok) << "input \"" << c.name << "\"";
    if (c.ok) {
      EXPECT_EQ(static_cast<int>(got->position), c.position) << "input \"" << c.name << "\"";
      EXPECT_EQ(static_cast<int>(got->length), c.length) << "input \"" << c.name << "\"";
    }
    ++checked;
  }
  EXPECT_EQ(checked, static_cast<int>(cdsmodel::golden::kStubParseCases.size()));
}

} // namespace

// =====================================================================
// The rest of stub.c: payment, the end-stub decision, and the split.
// =====================================================================

namespace {

cdsmodel::Date sd(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

} // namespace

TEST(Stub, APaymentScalesLinearlyInTheRateAndIsZeroOverNoTime) {
  // The whole function is `rate * stubYears`, so these two properties pin it
  // without reference to any recorded number: doubling the rate doubles the
  // payment, and a zero-length stub accrues nothing.
  const cdsmodel::Date prev = sd(2024, 1, 15);
  const cdsmodel::Date next = sd(2024, 7, 15);
  for (const cdsmodel::DayCount dcc : {cdsmodel::DayCount::Act365Fixed, cdsmodel::DayCount::Act360,
                                       cdsmodel::DayCount::Thirty360}) {
    const std::optional<double> single =
        cdsmodel::stub_payment(prev, next, prev, sd(2024, 4, 15), 0.05, dcc);
    const std::optional<double> doubled =
        cdsmodel::stub_payment(prev, next, prev, sd(2024, 4, 15), 0.10, dcc);
    ASSERT_TRUE(single.has_value());
    ASSERT_TRUE(doubled.has_value());
    EXPECT_EQ(*doubled, 2.0 * *single) << static_cast<int>(dcc);

    // Zero rate, zero payment.
    const std::optional<double> free =
        cdsmodel::stub_payment(prev, next, prev, sd(2024, 4, 15), 0.0, dcc);
    ASSERT_TRUE(free.has_value());
    EXPECT_EQ(*free, 0.0) << static_cast<int>(dcc);

    // Zero-length stub at the period start: no time, no accrual.
    const std::optional<double> instant = cdsmodel::stub_payment(prev, next, prev, prev, 0.05, dcc);
    ASSERT_TRUE(instant.has_value()) << "touching prevCoupon at both ends is accepted";
    EXPECT_EQ(*instant, 0.0) << static_cast<int>(dcc);
  }
}

TEST(Stub, APaymentIsRefusedUnlessTheStubTouchesTheCouponPeriodAtAnEnd) {
  const cdsmodel::Date prev = sd(2024, 1, 15);
  const cdsmodel::Date next = sd(2024, 7, 15);
  const cdsmodel::DayCount dcc = cdsmodel::DayCount::Act365Fixed;

  // Accepted: touching the start, the end, or both.
  EXPECT_TRUE(cdsmodel::stub_payment(prev, next, prev, sd(2024, 4, 15), 0.05, dcc).has_value());
  EXPECT_TRUE(cdsmodel::stub_payment(prev, next, sd(2024, 4, 15), next, 0.05, dcc).has_value());
  EXPECT_TRUE(cdsmodel::stub_payment(prev, next, prev, next, 0.05, dcc).has_value());

  // Refused: strictly inside, so neither boundary matches -- even though
  // rate * stubYears is perfectly computable for it.
  EXPECT_FALSE(
      cdsmodel::stub_payment(prev, next, sd(2024, 2, 15), sd(2024, 4, 15), 0.05, dcc).has_value());
  // Refused: outside the period on either side.
  EXPECT_FALSE(
      cdsmodel::stub_payment(prev, next, sd(2023, 12, 1), sd(2024, 4, 15), 0.05, dcc).has_value());
  EXPECT_FALSE(
      cdsmodel::stub_payment(prev, next, sd(2024, 4, 15), sd(2024, 9, 1), 0.05, dcc).has_value());
  // Refused: a coupon period that is inverted or has no length.
  EXPECT_FALSE(cdsmodel::stub_payment(next, prev, next, prev, 0.05, dcc).has_value());
  EXPECT_FALSE(cdsmodel::stub_payment(prev, prev, prev, prev, 0.05, dcc).has_value());
}

TEST(Stub, TheEndStubDecisionFollowsTheDefaultOrTheSchedule) {
  const cdsmodel::DateInterval quarterly{3, cdsmodel::PeriodUnit::Months};
  const cdsmodel::Date start = sd(2024, 1, 15);

  // Front and Back answer without looking at anything.
  for (const cdsmodel::Date maturity : {sd(2027, 1, 15), sd(2024, 2, 20), sd(2029, 11, 30)}) {
    EXPECT_EQ(cdsmodel::is_end_stub(start, maturity, quarterly, cdsmodel::StubPosDefault::Back),
              std::optional{true});
    EXPECT_EQ(cdsmodel::is_end_stub(start, maturity, quarterly, cdsmodel::StubPosDefault::Front),
              std::optional{false});
  }

  // Auto: an evenly divided span has no stub, so true; otherwise false.
  EXPECT_EQ(
      cdsmodel::is_end_stub(start, sd(2027, 1, 15), quarterly, cdsmodel::StubPosDefault::Auto),
      std::optional{true})
      << "twelve whole quarters, so no stub";
  EXPECT_EQ(
      cdsmodel::is_end_stub(start, sd(2024, 2, 20), quarterly, cdsmodel::StubPosDefault::Auto),
      std::optional{false})
      << "shorter than one quarter, so a stub, and it goes to the front";

  // And the Auto answer must agree with count_dates, which is what decides it.
  for (const cdsmodel::Date maturity :
       {sd(2027, 1, 15), sd(2024, 2, 20), sd(2029, 11, 30), sd(2026, 7, 15)}) {
    const std::optional<cdsmodel::DateCount> span =
        cdsmodel::count_dates(start, maturity, quarterly);
    const std::optional<bool> decision =
        cdsmodel::is_end_stub(start, maturity, quarterly, cdsmodel::StubPosDefault::Auto);
    ASSERT_EQ(decision.has_value(), span.has_value());
    if (decision) {
      EXPECT_EQ(*decision, span->extra_days <= 0) << cdsmodel::to_tdate(maturity);
    }
  }
}

TEST(Stub, TheSplitRoundTripsEveryRepresentablePairing) {
  // Four conventions times three stub defaults, packed and unpacked.
  for (const cdsmodel::BadDayConv conv :
       {cdsmodel::BadDayConv::Follow, cdsmodel::BadDayConv::Previous, cdsmodel::BadDayConv::None,
        cdsmodel::BadDayConv::Modified}) {
    for (const cdsmodel::StubPosDefault position :
         {cdsmodel::StubPosDefault::Auto, cdsmodel::StubPosDefault::Front,
          cdsmodel::StubPosDefault::Back}) {
      const long packed =
          (cdsmodel::to_upstream_code(position) << 8) | cdsmodel::to_upstream_code(conv);
      const std::optional<std::pair<cdsmodel::BadDayConv, cdsmodel::StubPosDefault>> split =
          cdsmodel::split_bad_day_and_stub_pos(packed);
      ASSERT_TRUE(split.has_value()) << packed;
      EXPECT_EQ(split->first, conv) << packed;
      EXPECT_EQ(split->second, position) << packed;
    }
  }
  // An unrepresentable half on either side is refused.
  EXPECT_FALSE(cdsmodel::split_bad_day_and_stub_pos(0x0341).has_value()) << "both bad";
  EXPECT_FALSE(cdsmodel::split_bad_day_and_stub_pos(0x0346).has_value()) << "stub position bad";
  EXPECT_FALSE(cdsmodel::split_bad_day_and_stub_pos(0x0041).has_value()) << "convention bad";
  // High bits above the two bytes are ignored, as upstream's masks ignore them.
  EXPECT_EQ(cdsmodel::split_bad_day_and_stub_pos(0x00FF0146),
            cdsmodel::split_bad_day_and_stub_pos(0x0146))
      << "only the low two bytes are read";
}

// =====================================================================
// Replay.
// =====================================================================

TEST(Stub, MatchesRecordedStubPayments) {
  int checked = 0;
  int refused = 0;
  for (const auto &c : cdsmodel::golden::kStubPaymentCases) {
    const std::optional<cdsmodel::DayCount> dcc = cdsmodel::from_upstream_code(c.day_count);
    if (!dcc) {
      continue;
    }
    const std::optional<double> got = cdsmodel::stub_payment(
        cdsmodel::from_tdate(c.prev_coupon), cdsmodel::from_tdate(c.next_coupon),
        cdsmodel::from_tdate(c.stub_start), cdsmodel::from_tdate(c.stub_end), c.rate, *dcc);
    const std::string where = std::to_string(c.stub_start) + ".." + std::to_string(c.stub_end) +
                              " dcc " + std::to_string(c.day_count) + " rate " +
                              std::to_string(c.rate);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      // One multiplication, so exact.
      EXPECT_EQ(*got, c.payment) << where;
    } else {
      ++refused;
    }
    ++checked;
  }
  EXPECT_GT(checked, 500) << checked;
  EXPECT_GT(refused, 100) << "most of the sweep is refused, which is the point: " << refused;
}

TEST(Stub, MatchesRecordedEndStubDecisions) {
  int checked = 0;
  for (const auto &c : cdsmodel::golden::kEndStubCases) {
    const std::optional<cdsmodel::StubPosDefault> position =
        cdsmodel::stub_pos_from_upstream(c.position);
    ASSERT_TRUE(position.has_value()) << c.position;
    const cdsmodel::DateInterval iv{c.periods, cdsmodel::PeriodUnit::Months};
    const std::optional<bool> got = cdsmodel::is_end_stub(
        cdsmodel::from_tdate(c.start), cdsmodel::from_tdate(c.maturity), iv, *position);
    const std::string where = std::to_string(c.start) + " iv " + std::to_string(c.periods) +
                              " pos " + std::to_string(c.position);
    ASSERT_EQ(got.has_value(), c.accepted) << where;
    if (got) {
      EXPECT_EQ(*got, c.is_end_stub) << where;
    }
    ++checked;
  }
  EXPECT_GT(checked, 150) << checked;
}

TEST(Stub, MatchesRecordedSplitsIncludingTheDeliberateDivergence) {
  int agreed = 0;
  int diverged = 0;
  for (const auto &c : cdsmodel::golden::kSplitCases) {
    const std::optional<std::pair<cdsmodel::BadDayConv, cdsmodel::StubPosDefault>> got =
        cdsmodel::split_bad_day_and_stub_pos(c.packed);
    const std::string where = std::to_string(c.packed);
    if (c.accepted && c.conv_valid) {
      ASSERT_TRUE(got.has_value()) << where;
      EXPECT_EQ(cdsmodel::to_upstream_code(got->first), c.conv) << where;
      EXPECT_EQ(cdsmodel::to_upstream_code(got->second), c.position) << where;
      ++agreed;
    } else if (c.accepted) {
      // Upstream accepted a convention its own validator rejects. The port
      // cannot represent it, so it refuses here instead of at first use.
      EXPECT_FALSE(got.has_value()) << where << ": the deliberate difference";
      ++diverged;
    } else {
      EXPECT_FALSE(got.has_value()) << where;
    }
  }
  EXPECT_GT(agreed, 10) << agreed;
  EXPECT_GT(diverged, 10) << "the recorded cases upstream lets through: " << diverged;
}
