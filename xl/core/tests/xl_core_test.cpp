// C0 and C10 of specs/03_xll.md.
//
// These run on Windows, macOS and Linux, with no Excel and no SDK. That is
// the entire justification for D8: `cpp_fdm_world` puts this logic behind
// `#ifdef WIN32`, which would leave every decision below verified on one
// machine by hand.

#include <cdsmodel/xl/labelled_range.hpp>
#include <cdsmodel/xl/value.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

using cdsmodel::xl::ascii_from_utf16;
using cdsmodel::xl::LabelledRange;
using cdsmodel::xl::LabelSpec;
using cdsmodel::xl::normalise_label;
using cdsmodel::xl::XlError;
using cdsmodel::xl::XlGrid;
using cdsmodel::xl::XlMissing;
using cdsmodel::xl::XlValue;

XlGrid grid_of(std::initializer_list<std::pair<XlValue, XlValue>> rows) {
  XlGrid g;
  g.rows = rows.size();
  g.columns = 2;
  for (const auto &[label, value] : rows) {
    g.cells.push_back(label);
    g.cells.push_back(value);
  }
  return g;
}

constexpr std::array<LabelSpec, 4> kConventions{{
    {"Coupon Interval", true},
    {"Payment DCC", true},
    {"Bad Day Convention", true},
    {"Holidays", false},
}};

// ---------------------------------------------------------------- XlValue

TEST(XlValue, MissingIsNotZeroAndNotEmptyText) {
  const XlValue missing;
  EXPECT_TRUE(missing.is_missing());
  // The distinction that matters: an omitted argument must not become 0,
  // which is how a default silently replaces an intention.
  EXPECT_FALSE(missing.as_double().has_value());
  EXPECT_FALSE(missing.as_string().has_value());
  EXPECT_NE(missing, XlValue{0.0});
  EXPECT_NE(missing, XlValue{std::string{}});
}

TEST(XlValue, ReadsNumbersIncludingTextThatIsEntirelyANumber) {
  EXPECT_EQ(*XlValue{3.5}.as_double(), 3.5);
  EXPECT_EQ(*XlValue{"3.5"}.as_double(), 3.5);
  EXPECT_EQ(*XlValue{"  3.5  "}.as_double(), 3.5) << "padding should be tolerated";
  EXPECT_EQ(*XlValue{true}.as_double(), 1.0);
}

TEST(XlValue, RefusesTextThatMerelyStartsWithANumber) {
  // `atof` would return 3 here and the trade would price from a number nobody
  // typed. This is the single most valuable refusal in the marshalling layer.
  const auto got = XlValue{"3 months"}.as_double();
  ASSERT_FALSE(got.has_value());
  EXPECT_NE(got.error().message.find("3 months"), std::string::npos)
      << "the message should quote what was found: " << got.error().message;
}

TEST(XlValue, ANumberIsNotSilentlyStringified) {
  // A cell holding 3 where ACT/360 was wanted is a mistake, and "3" is not a
  // day count.
  const auto got = XlValue{3.0}.as_string();
  ASSERT_FALSE(got.has_value());
  EXPECT_NE(got.error().message.find("a number"), std::string::npos) << got.error().message;
}

TEST(XlValue, StringsAreTrimmedSoDownstreamParsersNeedNotBe) {
  // Measured on this port's parsers: " f/s " and " 3M " are both refused, and
  // the interval one refuses with a misleading message. Trimming here is why
  // none of them has to change.
  EXPECT_EQ(*XlValue{"  f/s  "}.as_string(), "f/s");
  EXPECT_EQ(*XlValue{"\tACT/360\n"}.as_string(), "ACT/360");
}

TEST(XlValue, AnErrorCellPropagatesItsOwnText) {
  const auto got = XlValue{XlError{"#DIV/0!"}}.as_double();
  ASSERT_FALSE(got.has_value());
  EXPECT_NE(got.error().message.find("#DIV/0!"), std::string::npos) << got.error().message;
}

TEST(XlValue, ASingleCellReadsAsAOneByOneRange) {
  const auto g = XlValue{4.0}.as_grid();
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->rows, 1u);
  EXPECT_EQ(g->columns, 1u);
  EXPECT_EQ(*g->at(0, 0).as_double(), 4.0);
}

TEST(XlValue, AnOutOfRangeCellIsAnErrorNotUndefinedBehaviour) {
  const XlGrid g = grid_of({{XlValue{"a"}, XlValue{1.0}}});
  EXPECT_TRUE(g.at(5, 5).is_error());
}

// ------------------------------------------------------- ascii_from_utf16
//
// C2: the two cases that justified XLOPER12 over XLOPER. The counted-string
// walk is XCHAR-specific and stays in xl/sdk's oper.cpp, but the narrowing
// rule it feeds lives here so CI exercises it with no Excel and no SDK.

TEST(AsciiFromUtf16, PlainAsciiIsUnchanged) {
  EXPECT_EQ(ascii_from_utf16(u"ACT/360"), "ACT/360");
}

TEST(AsciiFromUtf16, C2StringsOver255CharactersSurviveInFull) {
  // XLOPER capped a string at 255 characters; XLOPER12 does not. A 300-unit
  // string must come back at its full length, every character intact.
  const std::u16string long_name(300, u'x');
  const std::string out = ascii_from_utf16(long_name);
  EXPECT_EQ(out.size(), 300u);
  EXPECT_EQ(out, std::string(300, 'x'));
}

TEST(AsciiFromUtf16, C2NonAsciiNarrowsToQuestionMarkRatherThanTruncating) {
  // Deliberately lossy (D10): every string this add-in reads is ASCII by
  // construction, so a non-ASCII unit is a user mistake shown as '?', not data
  // to preserve. The length is still kept -- nothing is dropped.
  const std::u16string mixed = u"A" + std::u16string(1, u'\x00E9') // e-acute
                               + u"B" + std::u16string(1, u'\x4E2D'); // CJK
  EXPECT_EQ(ascii_from_utf16(mixed), "A?B?");
}

TEST(AsciiFromUtf16, TheBoundaryIsExactlyOneTwentyEight) {
  EXPECT_EQ(ascii_from_utf16(std::u16string(1, u'\x007F')), std::string(1, '\x7F'));
  EXPECT_EQ(ascii_from_utf16(std::u16string(1, u'\x0080')), "?");
}

// ---------------------------------------------------------- LabelledRange

TEST(LabelledRange, ReadsRowsInAnyOrder) {
  const XlGrid g = grid_of({{XlValue{"Payment DCC"}, XlValue{"ACT/360"}},
                            {XlValue{"Holidays"}, XlValue{"NONE"}},
                            {XlValue{"Bad Day Convention"}, XlValue{"F"}},
                            {XlValue{"Coupon Interval"}, XlValue{"3M"}}});
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_EQ(*r->value_of("Coupon Interval")->as_string(), "3M");
  EXPECT_EQ(*r->value_of("Payment DCC")->as_string(), "ACT/360");
}

TEST(LabelledRange, LabelMatchingIgnoresCaseAndSpacingButNotSpelling) {
  EXPECT_EQ(normalise_label("  Payment   DCC "), "payment dcc");
  EXPECT_EQ(normalise_label("PAYMENT DCC"), "payment dcc");
  EXPECT_NE(normalise_label("Payment DC"), "payment dcc") << "near misses must stay distinct";
}

// C10, the criterion this reader exists for.
TEST(LabelledRange, C10AnUnknownLabelIsReportedNotIgnored) {
  const XlGrid g = grid_of({{XlValue{"Coupon Interval"}, XlValue{"3M"}},
                            {XlValue{"Payment DC"}, XlValue{"ACT/360"}}, // typo
                            {XlValue{"Bad Day Convention"}, XlValue{"F"}}});
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_FALSE(r.has_value()) << "a misspelled label was silently ignored, so the sheet "
                                 "would price with a default nobody chose";
  EXPECT_NE(r.error().message.find("Payment DC"), std::string::npos) << r.error().message;
  EXPECT_NE(r.error().message.find("Payment DCC"), std::string::npos)
      << "the message should list what IS accepted: " << r.error().message;
}

TEST(LabelledRange, C10ADuplicatedLabelIsReported) {
  const XlGrid g = grid_of({{XlValue{"Coupon Interval"}, XlValue{"3M"}},
                            {XlValue{"Payment DCC"}, XlValue{"ACT/360"}},
                            {XlValue{"Bad Day Convention"}, XlValue{"F"}},
                            {XlValue{"Coupon Interval"}, XlValue{"6M"}}});
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_FALSE(r.has_value()) << "two rows disagree and the sheet still priced";
  EXPECT_NE(r.error().message.find("more than once"), std::string::npos) << r.error().message;
}

TEST(LabelledRange, AMissingRequiredLabelIsReportedByName) {
  const XlGrid g = grid_of(
      {{XlValue{"Coupon Interval"}, XlValue{"3M"}}, {XlValue{"Payment DCC"}, XlValue{"ACT/360"}}});
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("Bad Day Convention"), std::string::npos) << r.error().message;
}

TEST(LabelledRange, AnOptionalLabelMayBeAbsent) {
  const XlGrid g = grid_of({{XlValue{"Coupon Interval"}, XlValue{"3M"}},
                            {XlValue{"Payment DCC"}, XlValue{"ACT/360"}},
                            {XlValue{"Bad Day Convention"}, XlValue{"F"}}});
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_TRUE(r.has_value()) << r.error().message;
  EXPECT_FALSE(r->has("Holidays"));
  EXPECT_FALSE(r->value_of("Holidays").has_value()) << "asking for it should say so";
}

TEST(LabelledRange, BlankSpacerRowsAreSkippedButAValueWithNoLabelIsNot) {
  const XlGrid with_spacer = grid_of({{XlValue{"Coupon Interval"}, XlValue{"3M"}},
                                      {XlValue{}, XlValue{}},
                                      {XlValue{"Payment DCC"}, XlValue{"ACT/360"}},
                                      {XlValue{"Bad Day Convention"}, XlValue{"F"}}});
  EXPECT_TRUE(LabelledRange::read(with_spacer, kConventions).has_value());

  const XlGrid orphan = grid_of({{XlValue{"Coupon Interval"}, XlValue{"3M"}},
                                 {XlValue{}, XlValue{"ACT/360"}}, // meant to be filled in
                                 {XlValue{"Payment DCC"}, XlValue{"ACT/360"}},
                                 {XlValue{"Bad Day Convention"}, XlValue{"F"}}});
  const auto r = LabelledRange::read(orphan, kConventions);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("no label"), std::string::npos) << r.error().message;
}

TEST(LabelledRange, AOneColumnRangeIsRefusedWithTheReason) {
  XlGrid g;
  g.rows = 2;
  g.columns = 1;
  g.cells = {XlValue{"Coupon Interval"}, XlValue{"Payment DCC"}};
  const auto r = LabelledRange::read(g, kConventions);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("two columns"), std::string::npos) << r.error().message;
}

} // namespace
