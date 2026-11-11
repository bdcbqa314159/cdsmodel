// The error paths of xl/core, which are what a user actually hits.
//
// Measured before this file existed: xl/core sat at 54.1% branch coverage with
// `arguments.cpp` at 60.4% of lines, and nearly every uncovered line was a
// `fail(...)` or a `to_cell(x.error())`. D8 built a seam so this layer could be
// tested without Excel, and then only the happy paths used it -- which is the
// worse half of the bargain, because a cell with a typo in it is the common
// case, not the exceptional one.

#include <cdsmodel/xl/arguments.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>
#include <cdsmodel/xl/labelled_range.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

namespace {

using cdsmodel::xl::CalendarRegistry;
using cdsmodel::xl::CallContext;
using cdsmodel::xl::Conventions;
using cdsmodel::xl::CurveRegistry;
using cdsmodel::xl::find_function;
using cdsmodel::xl::invoke;
using cdsmodel::xl::LabelledRange;
using cdsmodel::xl::LabelledTable;
using cdsmodel::xl::LabelSpec;
using cdsmodel::xl::MarketData;
using cdsmodel::xl::TradeTerms;
using cdsmodel::xl::Valuation;
using cdsmodel::xl::XlError;
using cdsmodel::xl::XlGrid;
using cdsmodel::xl::XlValue;

double serial(int y, int m, int d) {
  return static_cast<double>(cdsmodel::make_date(y, m, d)->time_since_epoch().count() + 25569);
}

XlValue block(std::initializer_list<std::pair<const char *, XlValue>> rows) {
  XlGrid g;
  g.rows = rows.size();
  g.columns = 2;
  for (const auto &[label, value] : rows) {
    g.cells.emplace_back(std::string(label));
    g.cells.push_back(value);
  }
  return XlValue{g};
}

cdsmodel::ZeroCurve flat(double rate) {
  const cdsmodel::RatePoint pts[]{{*cdsmodel::make_date(2007, 1, 1), rate},
                                  {*cdsmodel::make_date(2030, 1, 1), rate}};
  return *cdsmodel::ZeroCurve::create(*cdsmodel::make_date(2006, 10, 9), pts,
                                      cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
}

/// A complete, working 16-argument call. Each test below breaks exactly one
/// cell, so the failure is attributable to that cell and nothing else.
struct Call {
  CurveRegistry curves;
  CalendarRegistry calendars;
  std::string discount = curves.store("USD.disc", flat(0.04));
  std::string spread = curves.store("ACME.spread", flat(0.01));
  CallContext ctx{curves, calendars};

  std::vector<XlValue> args() const {
    return {XlValue{serial(2006, 10, 9)},
            XlValue{serial(2006, 10, 9)},
            XlValue{serial(2006, 10, 9)},
            XlValue{serial(2006, 10, 9)},
            XlValue{serial(2011, 10, 9)},
            XlValue{0.01},
            XlValue{true},
            XlValue{"3M"},
            XlValue{"f/s"},
            XlValue{"ACT/360"},
            XlValue{"N"},
            XlValue{"NONE"},
            XlValue{discount},
            XlValue{spread},
            XlValue{0.4},
            XlValue{true}};
  }

  std::string fails_with(std::size_t index, XlValue bad) const {
    std::vector<XlValue> a = args();
    a[index] = std::move(bad);
    const XlValue got = invoke(*find_function("CdsPrice"), a, const_cast<CallContext &>(ctx));
    return got.as_double().has_value() == false ? got.describe() : std::string{};
  }
};

// ----------------------------------------------- every cell, broken in turn

TEST(XlErrorPaths, EveryPositionalCellReportsItsOwnNameWhenWrong) {
  const Call c;
  struct Case {
    std::size_t index;
    XlValue bad;
    const char *must_mention;
  };
  // One per argument. The point is not that each fails -- it is that the
  // message names the FIELD, so a user with 16 cells knows which one.
  const std::array<Case, 11> cases{{
      {0, XlValue{"not a date"}, "Today"},
      {1, XlValue{"not a date"}, "Cash Settle Date"},
      {2, XlValue{"not a date"}, "Step In Date"},
      {3, XlValue{"not a date"}, "Start Date"},
      {4, XlValue{"not a date"}, "End Date"},
      {5, XlValue{"not a number"}, "Coupon Rate"},
      {7, XlValue{"nonsense"}, "Coupon Interval"},
      {8, XlValue{"zzz"}, "Stub Type"},
      {9, XlValue{"ACT/999"}, "Payment DCC"},
      {10, XlValue{"Q"}, "Bad Day Convention"},
      {14, XlValue{"not a number"}, "Recovery Rate"},
  }};
  for (const Case &k : cases) {
    const std::string text = c.fails_with(k.index, k.bad);
    ASSERT_FALSE(text.empty()) << "argument " << k.index << " was accepted when wrong";
    EXPECT_NE(text.find(k.must_mention), std::string::npos)
        << "argument " << k.index << " failed without naming itself: " << text;
  }
}

TEST(XlErrorPaths, ADateWithATimeOfDayIsRefusedNotTruncated) {
  const Call c;
  // Excel serials carry fractions for times. Truncating silently would price
  // a trade from a date the sheet does not show.
  const std::string text = c.fails_with(0, XlValue{serial(2006, 10, 9) + 0.5});
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("whole number of days"), std::string::npos) << text;
}

TEST(XlErrorPaths, ABadCurveHandleNamesWhichCurve) {
  const Call c;
  EXPECT_NE(c.fails_with(12, XlValue{"nope#1"}).find("Discount Curve"), std::string::npos);
  EXPECT_NE(c.fails_with(13, XlValue{"nope#1"}).find("Spread Curve"), std::string::npos);
  // A number where a handle belongs: the message should say what it got.
  EXPECT_NE(c.fails_with(12, XlValue{3.0}).find("Discount Curve"), std::string::npos);
}

TEST(XlErrorPaths, RecoveryOutsideZeroToOneIsRefused) {
  const Call c;
  EXPECT_NE(c.fails_with(14, XlValue{1.5}).find("outside 0 to 1"), std::string::npos);
  EXPECT_NE(c.fails_with(14, XlValue{-0.1}).find("outside 0 to 1"), std::string::npos);
}

TEST(XlErrorPaths, AnUnknownCalendarIsRefusedRatherThanIgnored) {
  // This asserted `only NONE is available` until artifact #4 built the reader.
  // A bare name is now read as a HANDLE, so it is refused for the right
  // reason -- no such calendar was loaded -- rather than because calendars
  // were unsupported. Either way it is never silently ignored, which is the
  // behaviour that matters: pricing against the wrong calendar without saying
  // so is the failure this guards.
  const Call c;
  const std::string text = c.fails_with(11, XlValue{"LON"});
  ASSERT_FALSE(text.empty()) << "an unknown calendar was silently ignored";
  EXPECT_NE(text.find("Holidays"), std::string::npos) << text;
  EXPECT_NE(text.find("LON"), std::string::npos) << text;

  // A stale one is refused too, which only became possible now that a
  // calendar can exist at all.
  const std::string stale = c.fails_with(11, XlValue{"NYC#99"});
  EXPECT_FALSE(stale.empty());
}

TEST(XlErrorPaths, CleanPriceMustBeABoolean) {
  const Call c;
  EXPECT_NE(c.fails_with(15, XlValue{"maybe"}).find("Clean Price"), std::string::npos);
}

TEST(XlErrorPaths, AnErrorCellPropagatesIntoTheResult) {
  const Call c;
  const std::string text = c.fails_with(5, XlValue{XlError{"#REF!"}});
  ASSERT_FALSE(text.empty());
  EXPECT_NE(text.find("Coupon Rate"), std::string::npos) << text;
}

// --------------------------------------------------- the grouped form's paths

TEST(XlErrorPaths, TheGroupedFormRejectsEachBlockByName) {
  Call c;
  const XlValue good_valuation = block({{"Today", XlValue{serial(2006, 10, 9)}},
                                        {"Cash Settle Date", XlValue{serial(2006, 10, 9)}},
                                        {"Step In Date", XlValue{serial(2006, 10, 9)}}});
  const XlValue good_trade = block({{"Start Date", XlValue{serial(2006, 10, 9)}},
                                    {"End Date", XlValue{serial(2011, 10, 9)}},
                                    {"Coupon Rate", XlValue{0.01}}});
  const XlValue good_conv = block({{"Pay Accrual On Default", XlValue{true}},
                                   {"Coupon Interval", XlValue{"3M"}},
                                   {"Stub Type", XlValue{"f/s"}},
                                   {"Payment DCC", XlValue{"ACT/360"}},
                                   {"Bad Day Convention", XlValue{"N"}}});
  const XlValue good_market = block({{"Discount Curve", XlValue{c.discount}},
                                     {"Spread Curve", XlValue{c.spread}},
                                     {"Recovery Rate", XlValue{0.4}}});

  const auto run = [&](std::vector<XlValue> a) {
    const XlValue got = invoke(*find_function("CDS.Price"), a, c.ctx);
    return got.as_double().has_value() == false ? got.describe() : std::string{};
  };

  // The control: all four blocks good.
  EXPECT_TRUE(run({good_valuation, good_trade, good_conv, good_market, XlValue{true}}).empty());

  // Each block replaced by something that is not a two-column range. The
  // message must say WHICH block, or a user with four ranges is guessing.
  EXPECT_NE(
      run({XlValue{3.0}, good_trade, good_conv, good_market, XlValue{true}}).find("valuation"),
      std::string::npos);
  EXPECT_NE(
      run({good_valuation, XlValue{3.0}, good_conv, good_market, XlValue{true}}).find("trade"),
      std::string::npos);
  EXPECT_NE(run({good_valuation, good_trade, XlValue{3.0}, good_market, XlValue{true}})
                .find("conventions"),
            std::string::npos);
  EXPECT_NE(
      run({good_valuation, good_trade, good_conv, XlValue{3.0}, XlValue{true}}).find("market"),
      std::string::npos);

  // Wrong arity, and a non-boolean flag.
  EXPECT_NE(run({good_valuation, good_trade}).find("5 arguments"), std::string::npos);
  EXPECT_NE(run({good_valuation, good_trade, good_conv, good_market, XlValue{"maybe"}})
                .find("Clean Price"),
            std::string::npos);
}

TEST(XlErrorPaths, AMissingRowInsideABlockNamesTheRow) {
  Call c;
  const XlValue short_valuation =
      block({{"Today", XlValue{serial(2006, 10, 9)}},
             {"Cash Settle Date", XlValue{serial(2006, 10, 9)}}}); // no Step In Date
  const auto r = Valuation::read_block(short_valuation);
  ASSERT_FALSE(r.has_value());
  EXPECT_NE(r.error().message.find("Step In Date"), std::string::npos) << r.error().message;
  EXPECT_NE(r.error().message.find("valuation"), std::string::npos) << r.error().message;
}

TEST(XlErrorPaths, WrongCellCountToAPositionalReaderIsReported) {
  const std::array<XlValue, 2> two{XlValue{1.0}, XlValue{2.0}};
  EXPECT_FALSE(Valuation::read_positional(two).has_value());
  EXPECT_FALSE(TradeTerms::read_positional(two).has_value());
  CurveRegistry curves;
  CalendarRegistry cals;
  EXPECT_FALSE(Conventions::read_positional(two, cals).has_value());
  EXPECT_FALSE(MarketData::read_positional(two, curves).has_value());
}

// --------------------------------------------------- XlValue's remaining paths

TEST(XlErrorPaths, BooleansAreReadFromEverySpellingExcelUses) {
  EXPECT_EQ(*XlValue{true}.as_bool(), true);
  EXPECT_EQ(*XlValue{1.0}.as_bool(), true) << "Excel treats non-zero as TRUE";
  EXPECT_EQ(*XlValue{0.0}.as_bool(), false);
  for (const char *yes : {"TRUE", "true", "True"}) {
    EXPECT_EQ(*XlValue{yes}.as_bool(), true) << yes;
  }
  for (const char *no : {"FALSE", "false", "False"}) {
    EXPECT_EQ(*XlValue{no}.as_bool(), false) << no;
  }
  EXPECT_FALSE(XlValue{"yes"}.as_bool().has_value());
  EXPECT_FALSE(XlValue{XlError{"#N/A"}}.as_bool().has_value());
  EXPECT_FALSE(XlValue{}.as_bool().has_value());
}

TEST(XlErrorPaths, DescribeNamesEveryKindOfCell) {
  EXPECT_EQ(XlValue{}.describe(), "empty");
  EXPECT_EQ(XlValue{1.0}.describe(), "a number");
  EXPECT_EQ(XlValue{true}.describe(), "TRUE/FALSE");
  EXPECT_NE(XlValue{"hi"}.describe().find("\"hi\""), std::string::npos);
  XlGrid g;
  g.rows = 2;
  g.columns = 3;
  g.cells.resize(6);
  EXPECT_NE(XlValue{g}.describe().find("2x3"), std::string::npos);
  EXPECT_NE(XlValue{XlError{"#DIV/0!"}}.describe().find("#DIV/0!"), std::string::npos);
}

TEST(XlErrorPaths, AnErrorCellIsRefusedByEveryReader) {
  const XlValue bad{XlError{"#VALUE!"}};
  EXPECT_FALSE(bad.as_double().has_value());
  EXPECT_FALSE(bad.as_string().has_value());
  EXPECT_FALSE(bad.as_bool().has_value());
  EXPECT_FALSE(bad.as_grid().has_value());
  // And is reported as a failure by the dispatcher convention.
  EXPECT_FALSE(bad.as_double().has_value());
  // The cell's own error text survives into the description, so a message
  // built from it tells the user which cell upstream of theirs broke.
  EXPECT_NE(bad.describe().find("#VALUE!"), std::string::npos) << bad.describe();
}

TEST(XlErrorPaths, GridsCompareByShapeAndContent) {
  XlGrid a;
  a.rows = 1;
  a.columns = 2;
  a.cells = {XlValue{1.0}, XlValue{2.0}};
  XlGrid same = a;
  EXPECT_EQ(a, same);
  XlGrid different = a;
  different.cells[1] = XlValue{3.0};
  EXPECT_NE(a, different);
  XlGrid reshaped = a;
  reshaped.rows = 2;
  reshaped.columns = 1;
  EXPECT_NE(a, reshaped) << "shape is part of identity";
}

TEST(XlErrorPaths, AnAllWhitespaceStringTrimsToEmpty) {
  const auto got = XlValue{"   \t  "}.as_string();
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(got->empty());
}

// ------------------------------------------- LabelledRange's remaining paths

TEST(XlErrorPaths, AnEmptyBlockAndANonTextLabelAreBothReported) {
  constexpr std::array<LabelSpec, 1> labels{{{"Coupon Interval", true}}};

  XlGrid empty;
  empty.rows = 0;
  empty.columns = 2;
  const auto r1 = LabelledRange::read(empty, labels);
  ASSERT_FALSE(r1.has_value());
  EXPECT_NE(r1.error().message.find("empty"), std::string::npos) << r1.error().message;

  XlGrid numeric_label;
  numeric_label.rows = 1;
  numeric_label.columns = 2;
  numeric_label.cells = {XlValue{7.0}, XlValue{"3M"}};
  const auto r2 = LabelledRange::read(numeric_label, labels);
  ASSERT_FALSE(r2.has_value());
  EXPECT_NE(r2.error().message.find("row 1"), std::string::npos) << r2.error().message;
}

// ------------------------------------------- LabelledTable's C10 rules

namespace {
// A header-row grid: `headers` then one row of `cells` per inner list.
XlGrid table_grid(std::initializer_list<const char *> headers,
                  std::initializer_list<std::initializer_list<XlValue>> rows) {
  XlGrid g;
  g.columns = headers.size();
  g.rows = rows.size() + 1;
  for (const char *h : headers) {
    g.cells.emplace_back(std::string(h));
  }
  for (const auto &row : rows) {
    for (const XlValue &c : row) {
      g.cells.push_back(c);
    }
  }
  return g;
}
} // namespace

TEST(XlErrorPaths, ATableReportsUnknownDuplicateAndMissingColumns) {
  constexpr std::array<LabelSpec, 3> columns{
      {{"End Date", true}, {"Coupon Rate", true}, {"Include", false}}};

  // An unknown header is named, never ignored.
  const auto unknown = LabelledTable::read(
      table_grid({"End Date", "Spread"}, {{XlValue{1.0}, XlValue{0.01}}}), columns);
  ASSERT_FALSE(unknown.has_value());
  EXPECT_NE(unknown.error().message.find("Spread"), std::string::npos) << unknown.error().message;

  // A duplicated header is refused -- two columns would disagree.
  const auto duplicate = LabelledTable::read(
      table_grid({"End Date", "End Date"}, {{XlValue{1.0}, XlValue{2.0}}}), columns);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_NE(duplicate.error().message.find("more than once"), std::string::npos)
      << duplicate.error().message;

  // A missing REQUIRED column is reported; a missing optional one is not.
  const auto missing =
      LabelledTable::read(table_grid({"End Date"}, {{XlValue{1.0}}}), columns);
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().message.find("Coupon Rate"), std::string::npos)
      << missing.error().message;
}

TEST(XlErrorPaths, ATableReadsColumnsInAnyOrderAndSkipsOptionalAbsent) {
  constexpr std::array<LabelSpec, 3> columns{
      {{"End Date", true}, {"Coupon Rate", true}, {"Include", false}}};
  // Reversed order, optional Include absent, two data rows.
  const auto ok = LabelledTable::read(
      table_grid({"Coupon Rate", "End Date"}, {{XlValue{0.01}, XlValue{1.0}},
                                               {XlValue{0.02}, XlValue{2.0}}}),
      columns);
  ASSERT_TRUE(ok.has_value()) << (ok.has_value() ? "" : ok.error().message);
  EXPECT_EQ(ok->rows(), 2u);
  EXPECT_FALSE(ok->has("Include"));
  const auto ends = ok->column("End Date");
  ASSERT_TRUE(ends.has_value());
  ASSERT_EQ(ends->size(), 2u);
  EXPECT_EQ((*ends)[1].as_double(), 2.0);
  // Asking for an absent optional column is an error that names it.
  const auto absent = ok->column("Include");
  EXPECT_FALSE(absent.has_value());
}

} // namespace
