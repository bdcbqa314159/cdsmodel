// TIER 1 -- stub method parsing. Port semantics plus recorded vectors.

#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include "golden/schedule_cases.hpp"

#include <string>

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
