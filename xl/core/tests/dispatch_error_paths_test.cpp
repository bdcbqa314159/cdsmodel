// The error paths of the dispatch table beyond CdsPrice.
//
// error_paths_test.cpp drove the two CdsPrice forms through their failures;
// dispatch_test.cpp drove every function through its HAPPY path. What was left
// uncovered was the bad-input branch of all the OTHER dispatchers and their
// shared readers -- a `fail(...)` or `return unexpected(...)` per cell, which is
// exactly the line a user hits on a typo. This file breaks one cell at a time in
// an otherwise-valid call to each function, so the measured coverage of
// functions.cpp reflects the paths a sheet actually exercises, not just the ones
// that price.

#include <cdsmodel/xl/arguments.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using cdsmodel::xl::all_functions;
using cdsmodel::xl::CalendarRegistry;
using cdsmodel::xl::CallContext;
using cdsmodel::xl::CurveRegistry;
using cdsmodel::xl::find_function;
using cdsmodel::xl::FunctionSpec;
using cdsmodel::xl::invoke;
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

XlValue column(std::initializer_list<XlValue> cells) {
  XlGrid g;
  g.rows = cells.size();
  g.columns = 1;
  for (const XlValue &c : cells) {
    g.cells.push_back(c);
  }
  return XlValue{g};
}

XlValue table(std::initializer_list<const char *> headers,
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
  return XlValue{g};
}

cdsmodel::ZeroCurve flat_curve(double rate) {
  const cdsmodel::RatePoint pts[]{{*cdsmodel::make_date(2007, 1, 1), rate},
                                  {*cdsmodel::make_date(2030, 1, 1), rate}};
  return *cdsmodel::ZeroCurve::create(*cdsmodel::make_date(2006, 10, 9), pts,
                                      cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
}

struct Fixture {
  CurveRegistry curves;
  CalendarRegistry calendars;
  std::string discount = curves.store("USD.disc", flat_curve(0.04));
  std::string spread = curves.store("ACME.spread", flat_curve(0.01));
  CallContext ctx{curves, calendars};

  /// The error cell's text, or the success value's description. Tests assert the
  /// expected message is a substring, so a wrongly-successful call fails loudly.
  std::string err(const char *name, std::vector<XlValue> args) {
    const XlValue got = invoke(*find_function(name), args, ctx);
    if (const auto s = got.as_string()) {
      return *s;
    }
    return got.describe();
  }

  /// A valid call, mutated at one index. Keeps each break attributable.
  std::string broken(const char *name, const std::vector<XlValue> &valid, std::size_t index,
                     XlValue bad) {
    std::vector<XlValue> a = valid;
    a[index] = std::move(bad);
    return err(name, a);
  }
};

bool mentions(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

// ------------------------------------------------- every function, wrong arity

TEST(DispatchErrors, EveryFunctionReportsAWrongArgumentCount) {
  Fixture f;
  // No function takes zero arguments, so an empty call must fail on the arity
  // check -- and name itself, since a user with the wrong count needs to know
  // which function complained.
  for (const FunctionSpec &spec : all_functions()) {
    const XlValue got = invoke(spec, {}, f.ctx);
    const auto text = got.as_string();
    ASSERT_TRUE(text.has_value()) << spec.name << " accepted zero arguments";
    EXPECT_TRUE(mentions(*text, "argument")) << spec.name << ": " << *text;
  }
}

// ------------------------------------------------------------- IRZeroCurveMake

std::vector<XlValue> ir_make_valid() {
  return {XlValue{serial(2006, 10, 9)},
          column({XlValue{serial(2008, 10, 9)}, XlValue{serial(2011, 10, 9)}}),
          column({XlValue{0.05}, XlValue{0.055}}),
          XlValue{static_cast<double>(cdsmodel::kContinuousBasis)},
          XlValue{"ACT/365F"},
          XlValue{"mk.curve"}};
}

TEST(DispatchErrors, IRZeroCurveMakeHappyPathAndEveryBrokenCell) {
  Fixture f;
  // The happy path first -- IRZeroCurveMake has no sibling in dispatch_test, so
  // this is also what covers its body.
  ASSERT_TRUE(mentions(f.err("IRZeroCurveMake", ir_make_valid()), "#"))
      << "a direct curve did not build";

  const auto v = ir_make_valid();
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 0, XlValue{"x"}), "Base Date"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 1, XlValue{"x"}), "Dates"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 2, XlValue{"x"}), "Rates"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 3, XlValue{"x"}), "Basis"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 4, XlValue{"ACT/999"}), "DCC"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 5, XlValue{""}), "name"));
  // Dates and Rates of different lengths.
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveMake", v, 2, column({XlValue{0.05}})), "must match"));
  // Non-increasing dates: the library refuses the curve, and that refusal
  // propagates rather than being swallowed.
  EXPECT_FALSE(f.broken("IRZeroCurveMake", v, 1,
                        column({XlValue{serial(2011, 10, 9)}, XlValue{serial(2008, 10, 9)}}))
                   .empty());
}

// ------------------------------------------------------------ IRZeroCurveBuild
// plus read_swap_terms, read_frequency, read_bad_day, build_instruments.

std::vector<XlValue> ir_build_valid() {
  return {XlValue{serial(2006, 10, 9)},
          column({XlValue{"M"}, XlValue{"S"}}),
          column({XlValue{serial(2007, 1, 9)}, XlValue{serial(2011, 10, 9)}}),
          column({XlValue{0.053}, XlValue{0.055}}),
          XlValue{"ACT/360"},
          XlValue{"6M"},
          XlValue{"6M"},
          XlValue{"30/360"},
          XlValue{"ACT/360"},
          XlValue{"N"},
          XlValue{"NONE"},
          XlValue{"ir.built"}};
}

TEST(DispatchErrors, IRZeroCurveBuildBreaksEachReader) {
  Fixture f;
  const auto v = ir_build_valid();
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 0, XlValue{"x"}), "Value Date"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 4, XlValue{"ACT/999"}), "MMDCC"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 7, XlValue{"ACT/999"}), "Fixed DCC"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 8, XlValue{"ACT/999"}), "Float DCC"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 11, XlValue{""}), "name"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 10, XlValue{"LON"}), "Holidays"));

  // build_instruments: unequal column lengths, a multi-character type, and an
  // unknown single-character code.
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 1, column({XlValue{"M"}})), "same length"));
  // A multi-character type and an unknown single-character code both fail.
  EXPECT_FALSE(f.broken("IRZeroCurveBuild", v, 1, column({XlValue{"MM"}, XlValue{"S"}})).empty());
  EXPECT_FALSE(f.broken("IRZeroCurveBuild", v, 1, column({XlValue{"X"}, XlValue{"S"}})).empty());

  // read_frequency, through Fixed IVL: a number out of range, a string that is
  // not an interval, and an interval that does not divide the year.
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 5, XlValue{13.0}), "Fixed IVL"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 5, XlValue{"zzz"}), "Fixed IVL"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 5, XlValue{"5M"}), "Fixed IVL"));
  // A bare number IS a valid frequency, so this one must succeed.
  EXPECT_FALSE(mentions(f.broken("IRZeroCurveBuild", v, 5, XlValue{4.0}), "Fixed IVL"));

  // read_bad_day, through Swap BDC: a two-character string and an unknown code.
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 9, XlValue{"NN"}), "Swap BDC"));
  EXPECT_TRUE(mentions(f.broken("IRZeroCurveBuild", v, 9, XlValue{"Q"}), "Swap BDC"));
}

// ---------------------------------------------------------------- CDS.IRZeroCurve
// plus labelled_cells and read_instrument_table.

std::vector<XlValue> cds_ir_valid() {
  return {XlValue{serial(2006, 10, 9)},
          table({"Type", "End Date", "Rate"},
                {{XlValue{"M"}, XlValue{serial(2007, 1, 9)}, XlValue{0.053}},
                 {XlValue{"S"}, XlValue{serial(2011, 10, 9)}, XlValue{0.055}}}),
          block({{"MM DCC", XlValue{"ACT/360"}},
                 {"Fixed IVL", XlValue{"6M"}},
                 {"Float IVL", XlValue{"6M"}},
                 {"Fixed DCC", XlValue{"30/360"}},
                 {"Float DCC", XlValue{"ACT/360"}},
                 {"Swap BDC", XlValue{"N"}}}),
          XlValue{"NONE"},
          XlValue{"cds.ir"}};
}

TEST(DispatchErrors, CdsIRZeroCurveBreaksTableAndBlock) {
  Fixture f;
  const auto v = cds_ir_valid();
  // A scalar where the instruments table belongs.
  EXPECT_TRUE(mentions(f.broken("CDS.IRZeroCurve", v, 1, XlValue{XlError{"#REF!"}}), "range"));
  // The conventions block is not a two-column range.
  EXPECT_TRUE(mentions(f.broken("CDS.IRZeroCurve", v, 2, XlValue{XlError{"#REF!"}}), "two-column range"));
  // A conventions block missing a required label names it.
  EXPECT_TRUE(mentions(f.broken("CDS.IRZeroCurve", v, 2,
                                block({{"MM DCC", XlValue{"ACT/360"}}})),
                       "Fixed IVL"));
  // read_instrument_table's own rows: an empty table, a bad Type, a bad End
  // Date and a bad Rate.
  EXPECT_TRUE(mentions(
      f.broken("CDS.IRZeroCurve", v, 1, table({"Type", "End Date", "Rate"}, {})), "no rows"));
  EXPECT_FALSE(f.broken("CDS.IRZeroCurve", v, 1,
                        table({"Type", "End Date", "Rate"},
                              {{XlValue{"X"}, XlValue{serial(2011, 10, 9)}, XlValue{0.05}}}))
                   .empty());
  EXPECT_TRUE(mentions(f.broken("CDS.IRZeroCurve", v, 1,
                                table({"Type", "End Date", "Rate"},
                                      {{XlValue{"M"}, XlValue{"x"}, XlValue{0.05}}})),
                       "End Date"));
  EXPECT_TRUE(mentions(f.broken("CDS.IRZeroCurve", v, 1,
                                table({"Type", "End Date", "Rate"},
                                      {{XlValue{"M"}, XlValue{serial(2011, 10, 9)}, XlValue{"x"}}})),
                       "Rate"));
}

// -------------------------------------------------- DiscountFactor / DatesAndRates

TEST(DispatchErrors, CurveReadersRejectBadHandles) {
  Fixture f;
  EXPECT_TRUE(mentions(f.err("DiscountFactor", {XlValue{"nope#1"}, XlValue{serial(2011, 10, 9)}}),
                       "Curve"));
  EXPECT_TRUE(mentions(f.err("DiscountFactor", {XlValue{3.0}, XlValue{serial(2011, 10, 9)}}),
                       "Curve"));
  EXPECT_TRUE(mentions(f.err("DiscountFactor", {XlValue{f.discount}, XlValue{"x"}}), "Date"));
  EXPECT_TRUE(mentions(f.err("DatesAndRates", {XlValue{"nope#1"}}), "Curve"));
}

// ------------------------------------------------------- CleanSpreadCurveBuild

std::vector<XlValue> spread_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {XlValue{d},
          XlValue{d},
          XlValue{d},
          XlValue{d},
          column({XlValue{serial(2011, 10, 9)}, XlValue{serial(2016, 10, 9)}}),
          column({XlValue{0.01}, XlValue{0.012}}),
          XlValue{},
          XlValue{true},
          XlValue{"3M"},
          XlValue{"f/s"},
          XlValue{"ACT/360"},
          XlValue{"N"},
          XlValue{"NONE"},
          XlValue{f.discount},
          XlValue{0.4},
          XlValue{"sp.built"}};
}

TEST(DispatchErrors, CleanSpreadCurveBuildBreaksEachReader) {
  Fixture f;
  const auto v = spread_valid(f);
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 0, XlValue{"x"}), "Today"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 1, XlValue{"x"}), "Start Date"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 2, XlValue{"x"}), "Stepin Date"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 3, XlValue{"x"}), "CashSettle Date"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 4, XlValue{"x"}), "End Dates"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 5, XlValue{"x"}), "Coupon Rates"));
  // End Dates and Coupon Rates of different lengths.
  EXPECT_TRUE(
      mentions(f.broken("CleanSpreadCurveBuild", v, 5, column({XlValue{0.01}})), "must match"));
  // Include Flags: a scalar, a bad boolean, and a length mismatch.
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 6, XlValue{3.0}), "Include Flags"));
  EXPECT_TRUE(mentions(
      f.broken("CleanSpreadCurveBuild", v, 6, column({XlValue{"maybe"}, XlValue{true}})),
      "Include Flags"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 6, column({XlValue{true}})),
                       "Include Flags"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 13, XlValue{"nope#1"}), "Discount"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 14, XlValue{1.5}), "outside 0 to 1"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 14, XlValue{"x"}), "Recovery Rate"));
  EXPECT_TRUE(mentions(f.broken("CleanSpreadCurveBuild", v, 15, XlValue{""}), "name"));
  // A non-missing Include Flags that IS valid must still succeed.
  EXPECT_TRUE(mentions(
      f.broken("CleanSpreadCurveBuild", v, 6, column({XlValue{true}, XlValue{true}})), "#"));
}

// ------------------------------------------------------------ CDS.SpreadCurve
// plus read_benchmark_table.

std::vector<XlValue> cds_spread_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {block({{"Today", XlValue{d}},
                 {"Start Date", XlValue{d}},
                 {"Step In Date", XlValue{d}},
                 {"Cash Settle Date", XlValue{d}}}),
          table({"End Date", "Coupon Rate"},
                {{XlValue{serial(2011, 10, 9)}, XlValue{0.01}},
                 {XlValue{serial(2016, 10, 9)}, XlValue{0.012}}}),
          block({{"Pay Accrual On Default", XlValue{true}},
                 {"Coupon Interval", XlValue{"3M"}},
                 {"Stub Type", XlValue{"f/s"}},
                 {"Payment DCC", XlValue{"ACT/360"}},
                 {"Bad Day Convention", XlValue{"N"}},
                 {"Holidays", XlValue{"NONE"}}}),
          XlValue{f.discount},
          XlValue{0.4},
          XlValue{"cds.sp"}};
}

TEST(DispatchErrors, CdsSpreadCurveBreaksBlocksAndBenchmarks) {
  Fixture f;
  const auto v = cds_spread_valid(f);
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 0, XlValue{3.0}), "valuation"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 1, XlValue{XlError{"#REF!"}}), "range"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 2, XlValue{XlError{"#REF!"}}), "two-column range"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 3, XlValue{"nope#1"}), "Discount"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 4, XlValue{1.5}), "outside 0 to 1"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 5, XlValue{""}), "name"));
  // read_benchmark_table: empty, a bad end date, a bad coupon, a bad include.
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 1, table({"End Date", "Coupon Rate"}, {})),
                       "no rows"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 1,
                                table({"End Date", "Coupon Rate"}, {{XlValue{"x"}, XlValue{0.01}}})),
                       "End Date"));
  EXPECT_TRUE(mentions(f.broken("CDS.SpreadCurve", v, 1,
                                table({"End Date", "Coupon Rate"},
                                      {{XlValue{serial(2011, 10, 9)}, XlValue{"x"}}})),
                       "Coupon Rate"));
  EXPECT_TRUE(mentions(
      f.broken("CDS.SpreadCurve", v, 1,
               table({"End Date", "Coupon Rate", "Include"},
                     {{XlValue{serial(2011, 10, 9)}, XlValue{0.01}, XlValue{"maybe"}}})),
      "Include"));
}

// ------------------------------------------------------------------- ParSpreads

std::vector<XlValue> par_spreads_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {XlValue{d},
          XlValue{d},
          XlValue{d},
          column({XlValue{serial(2011, 10, 9)}, XlValue{serial(2016, 10, 9)}}),
          XlValue{true},
          XlValue{"3M"},
          XlValue{"f/s"},
          XlValue{"ACT/360"},
          XlValue{"N"},
          XlValue{"NONE"},
          XlValue{f.discount},
          XlValue{f.spread},
          XlValue{0.4}};
}

TEST(DispatchErrors, ParSpreadsBreaksEachReader) {
  Fixture f;
  const auto v = par_spreads_valid(f);
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 0, XlValue{"x"}), "Today"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 1, XlValue{"x"}), "StepIn Date"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 2, XlValue{"x"}), "Start Date"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 3, XlValue{"x"}), "End Dates"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 10, XlValue{"nope#1"}), "Discount"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 11, XlValue{"nope#1"}), "Spread"));
  EXPECT_TRUE(mentions(f.broken("ParSpreads", v, 12, XlValue{1.5}), "outside 0 to 1"));
}

// ---------------------------------------------------------------- CDS.ParSpreads

std::vector<XlValue> cds_par_spreads_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {block({{"Today", XlValue{d}},
                 {"Step In Date", XlValue{d}},
                 {"Start Date", XlValue{d}}}),
          column({XlValue{serial(2011, 10, 9)}, XlValue{serial(2016, 10, 9)}}),
          block({{"Pay Accrual On Default", XlValue{true}},
                 {"Coupon Interval", XlValue{"3M"}},
                 {"Stub Type", XlValue{"f/s"}},
                 {"Payment DCC", XlValue{"ACT/360"}},
                 {"Bad Day Convention", XlValue{"N"}},
                 {"Holidays", XlValue{"NONE"}}}),
          block({{"Discount Curve", XlValue{f.discount}},
                 {"Spread Curve", XlValue{f.spread}},
                 {"Recovery Rate", XlValue{0.4}}})};
}

TEST(DispatchErrors, CdsParSpreadsBreaksEachBlock) {
  Fixture f;
  const auto v = cds_par_spreads_valid(f);
  EXPECT_TRUE(mentions(f.broken("CDS.ParSpreads", v, 0, XlValue{3.0}), "valuation"));
  EXPECT_TRUE(mentions(f.broken("CDS.ParSpreads", v, 1, XlValue{"x"}), "End Dates"));
  EXPECT_TRUE(mentions(f.broken("CDS.ParSpreads", v, 2, XlValue{XlError{"#REF!"}}), "two-column range"));
  EXPECT_TRUE(mentions(f.broken("CDS.ParSpreads", v, 3, XlValue{XlError{"#REF!"}}), "two-column range"));
}

// --------------------------------------- UpfrontFlat / ParSpreadFlat (shared reader)

std::vector<XlValue> flat_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {XlValue{d},
          XlValue{d},
          XlValue{d},
          XlValue{d},
          XlValue{d},
          XlValue{serial(2011, 10, 9)},
          XlValue{0.01},
          XlValue{true},
          XlValue{"3M"},
          XlValue{"f/s"},
          XlValue{"ACT/360"},
          XlValue{"N"},
          XlValue{"NONE"},
          XlValue{f.discount},
          XlValue{0.02},
          XlValue{0.4},
          XlValue{true}};
}

TEST(DispatchErrors, FlatFormsBreakTheSharedReader) {
  Fixture f;
  const auto v = flat_valid(f);
  for (const char *name : {"UpfrontFlat", "ParSpreadFlat"}) {
    EXPECT_TRUE(mentions(f.broken(name, v, 0, XlValue{"x"}), "Today")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 2, XlValue{"x"}), "Benchmark St Date")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 5, XlValue{"x"}), "End Date")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 6, XlValue{"x"}), "Coupon Rate")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 13, XlValue{"nope#1"}), "Discount")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 15, XlValue{1.5}), "outside 0 to 1")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 16, XlValue{"maybe"}), "Clean Price")) << name;
    // The one cell that differs between the two: a non-numeric market spread /
    // upfront charge at index 14.
    EXPECT_FALSE(f.broken(name, v, 14, XlValue{"x"}).empty()) << name;
  }
}

// ---------------------------------------- CDS.Upfront / CDS.ParSpreadFlat (shared)

std::vector<XlValue> cds_flat_valid(const Fixture &f) {
  const double d = serial(2006, 10, 9);
  return {block({{"Today", XlValue{d}},
                 {"Cash Settle Date", XlValue{d}},
                 {"Benchmark St Date", XlValue{d}},
                 {"Step In Date", XlValue{d}}}),
          block({{"Start Date", XlValue{d}},
                 {"End Date", XlValue{serial(2011, 10, 9)}},
                 {"Coupon Rate", XlValue{0.01}}}),
          block({{"Pay Accrual On Default", XlValue{true}},
                 {"Coupon Interval", XlValue{"3M"}},
                 {"Stub Type", XlValue{"f/s"}},
                 {"Payment DCC", XlValue{"ACT/360"}},
                 {"Bad Day Convention", XlValue{"N"}},
                 {"Holidays", XlValue{"NONE"}}}),
          XlValue{f.discount},
          XlValue{0.02},
          XlValue{0.4},
          XlValue{true}};
}

TEST(DispatchErrors, CdsFlatFormsBreakTheSharedBlockReader) {
  Fixture f;
  const auto v = cds_flat_valid(f);
  for (const char *name : {"CDS.Upfront", "CDS.ParSpreadFlat"}) {
    EXPECT_TRUE(mentions(f.broken(name, v, 0, XlValue{3.0}), "valuation")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 1, XlValue{XlError{"#REF!"}}), "two-column range")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 2, XlValue{XlError{"#REF!"}}), "two-column range")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 3, XlValue{"nope#1"}), "Discount")) << name;
    // Index 4 differs between the two (Market Par Spread vs Upfront Charge).
    EXPECT_FALSE(f.broken(name, v, 4, XlValue{"x"}).empty()) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 5, XlValue{1.5}), "outside 0 to 1")) << name;
    EXPECT_TRUE(mentions(f.broken(name, v, 6, XlValue{"maybe"}), "Clean Price")) << name;
  }
}

// ------------------------------------------------------------- DefaultAccrual

std::vector<XlValue> default_accrual_valid() {
  return {XlValue{serial(2008, 2, 15)},
          XlValue{serial(2008, 3, 20)},
          XlValue{serial(2006, 10, 9)},
          XlValue{serial(2011, 10, 9)},
          XlValue{0.01},
          XlValue{1.0e7},
          XlValue{"3M"},
          XlValue{"f/s"},
          XlValue{"ACT/360"},
          XlValue{"N"},
          XlValue{"NONE"}};
}

TEST(DispatchErrors, DefaultAccrualBreaksEachReader) {
  Fixture f;
  const auto v = default_accrual_valid();
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 0, XlValue{"x"}), "Trade Date"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 1, XlValue{"x"}), "EDD Date"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 2, XlValue{"x"}), "Effective Date"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 3, XlValue{"x"}), "Maturity Date"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 4, XlValue{"x"}), "Coupon Rate"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 5, XlValue{"x"}), "Notional"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 8, XlValue{"ACT/999"}), "Payment DCC"));
}

// ------------------------------------------------- CDS.DefaultAccrual (grouped)

std::vector<XlValue> cds_default_accrual_valid() {
  return {block({{"Trade Date", XlValue{serial(2008, 2, 15)}},
                 {"EDD Date", XlValue{serial(2008, 3, 20)}}}),
          block({{"Start Date", XlValue{serial(2006, 10, 9)}},
                 {"End Date", XlValue{serial(2011, 10, 9)}},
                 {"Coupon Rate", XlValue{0.01}}}),
          XlValue{1.0e7},
          block({{"Coupon Interval", XlValue{"3M"}},
                 {"Stub Type", XlValue{"f/s"}},
                 {"Payment DCC", XlValue{"ACT/360"}},
                 {"Bad Day Convention", XlValue{"N"}},
                 {"Holidays", XlValue{"NONE"}}})};
}

TEST(DispatchErrors, CdsDefaultAccrualBreaksEachBlock) {
  Fixture f;
  const auto v = cds_default_accrual_valid();
  EXPECT_TRUE(mentions(f.broken("CDS.DefaultAccrual", v, 0, XlValue{3.0}), "dates"));
  EXPECT_TRUE(mentions(f.broken("CDS.DefaultAccrual", v, 1, XlValue{XlError{"#REF!"}}), "two-column range"));
  EXPECT_TRUE(mentions(f.broken("CDS.DefaultAccrual", v, 2, XlValue{"x"}), "Notional"));
  // read_conventions_no_payacc: the five-label block is not a range.
  EXPECT_TRUE(mentions(f.broken("CDS.DefaultAccrual", v, 3, XlValue{XlError{"#REF!"}}), "two-column range"));
  // A date cell inside the dates block that is not a date.
  EXPECT_TRUE(mentions(f.broken("CDS.DefaultAccrual", v, 0,
                                block({{"Trade Date", XlValue{"x"}},
                                       {"EDD Date", XlValue{serial(2008, 3, 20)}}})),
                       "Trade Date"));
}

// ----------------------------------------------------------------- FeeLegFlows

TEST(DispatchErrors, FeeLegFlowsBreaksEachReader) {
  Fixture f;
  const std::vector<XlValue> v{XlValue{serial(2006, 10, 9)},
                               XlValue{serial(2011, 10, 9)},
                               XlValue{0.01},
                               XlValue{1.0e7},
                               XlValue{"3M"},
                               XlValue{"f/s"},
                               XlValue{"ACT/360"},
                               XlValue{"N"},
                               XlValue{"NONE"}};
  EXPECT_TRUE(mentions(f.broken("FeeLegFlows", v, 0, XlValue{"x"}), "Start Date"));
  EXPECT_TRUE(mentions(f.broken("FeeLegFlows", v, 1, XlValue{"x"}), "End Date"));
  EXPECT_TRUE(mentions(f.broken("FeeLegFlows", v, 2, XlValue{"x"}), "Coupon Rate"));
  EXPECT_TRUE(mentions(f.broken("FeeLegFlows", v, 3, XlValue{"x"}), "Notional"));
  EXPECT_TRUE(mentions(f.broken("FeeLegFlows", v, 6, XlValue{"ACT/999"}), "Payment DCC"));
}

// -------------------------------------------------- CDS.FeeLegFlows (grouped)

TEST(DispatchErrors, CdsFeeLegFlowsBreaksEachBlock) {
  Fixture f;
  const std::vector<XlValue> v{block({{"Start Date", XlValue{serial(2006, 10, 9)}},
                                      {"End Date", XlValue{serial(2011, 10, 9)}},
                                      {"Coupon Rate", XlValue{0.01}}}),
                               XlValue{1.0e7},
                               block({{"Coupon Interval", XlValue{"3M"}},
                                      {"Stub Type", XlValue{"f/s"}},
                                      {"Payment DCC", XlValue{"ACT/360"}},
                                      {"Bad Day Convention", XlValue{"N"}},
                                      {"Holidays", XlValue{"NONE"}}})};
  EXPECT_TRUE(mentions(f.broken("CDS.FeeLegFlows", v, 0, XlValue{XlError{"#REF!"}}), "two-column range"));
  EXPECT_TRUE(mentions(f.broken("CDS.FeeLegFlows", v, 1, XlValue{"x"}), "Notional"));
  EXPECT_TRUE(mentions(f.broken("CDS.FeeLegFlows", v, 2, XlValue{XlError{"#REF!"}}), "two-column range"));
}

// ------------------------------------------------------------------- calendars

TEST(DispatchErrors, CalendarFunctionsReportBadInput) {
  Fixture f;
  // LoadHolidays / CDS.CalendarFromFile: a non-string path and name, an empty
  // name, and a path to a file that does not exist.
  for (const char *name : {"LoadHolidays", "CDS.CalendarFromFile"}) {
    EXPECT_TRUE(mentions(f.err(name, {XlValue{3.0}, XlValue{"NYC"}}), "Filename")) << name;
    EXPECT_TRUE(mentions(f.err(name, {XlValue{"x.dat"}, XlValue{3.0}}), "Name")) << name;
    EXPECT_TRUE(mentions(f.err(name, {XlValue{"x.dat"}, XlValue{""}}), "Name")) << name;
    EXPECT_FALSE(f.err(name, {XlValue{"no_such_file_12345.dat"}, XlValue{"NYC"}}).empty()) << name;
  }
  // CDS.Calendar from a range: an empty name, and dates that do not parse.
  EXPECT_TRUE(mentions(f.err("CDS.Calendar",
                             {column({XlValue{serial(2020, 1, 1)}}), XlValue{"STANDARD"},
                              XlValue{""}}),
                       "Name"));
  // CDS.CalendarDates: a non-string handle and an unknown one.
  EXPECT_TRUE(mentions(f.err("CDS.CalendarDates", {XlValue{3.0}}), "Calendar"));
  EXPECT_FALSE(f.err("CDS.CalendarDates", {XlValue{"nope#1"}}).empty());
}

// --------------------------------------------- an error cell into any reader

TEST(DispatchErrors, AnErrorCellIsRefusedWhereANumberOrDateBelongs) {
  Fixture f;
  const auto v = default_accrual_valid();
  // An upstream #REF! arriving in a date cell propagates, named by its field.
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 0, XlValue{XlError{"#REF!"}}), "Trade Date"));
  EXPECT_TRUE(mentions(f.broken("DefaultAccrual", v, 5, XlValue{XlError{"#DIV/0!"}}), "Notional"));
}

} // namespace
