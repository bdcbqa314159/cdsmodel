// C4, C5, C6, C7 and C9 of specs/03_xll.md -- the table, dispatch, handles.
//
// No Excel, no SDK, runs on every platform. The function under test is the
// same one the add-in will call; only the XLOPER12 conversion is missing, and
// that is a separate layer by design (D8).

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
using cdsmodel::xl::XlGrid;
using cdsmodel::xl::XlValue;

/// Excel serial for a Date, which is what a real cell would hold.
double serial(int y, int m, int d) {
  const cdsmodel::Date date = *cdsmodel::make_date(y, m, d);
  return static_cast<double>(date.time_since_epoch().count() + 25569);
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
};

// The one test that is not circular. Every other date here is built with the
// helper above, which shares the production constant -- so a wrong offset
// would shift both sides equally and nothing would fail. These three serials
// were read off Excel, not computed:
//
//     1900-03-01 -> 61      (the first date past Excel's phantom 1900-02-29)
//     1970-01-01 -> 25569   (the Unix epoch, which is the offset itself)
//     2024-03-15 -> 45366
TEST(Registration, TheExcelEpochOffsetIsNotCircular) {
  struct Known {
    double excel_serial;
    int y, m, d;
  };
  const Known known[]{{61, 1900, 3, 1}, {25569, 1970, 1, 1}, {45366, 2024, 3, 15}};
  for (const Known &k : known) {
    const cdsmodel::Date expected = *cdsmodel::make_date(k.y, k.m, k.d);
    const double from_helper = serial(k.y, k.m, k.d);
    EXPECT_EQ(from_helper, k.excel_serial)
        << k.y << "-" << k.m << "-" << k.d << ": the epoch offset is wrong";
    EXPECT_EQ(expected.time_since_epoch().count(), static_cast<long>(k.excel_serial) - 25569);
  }
}

// ------------------------------------------------------------------- C4

TEST(Registration, EveryEntryIsSelfConsistent) {
  ASSERT_FALSE(all_functions().empty());
  for (const FunctionSpec &f : all_functions()) {
    EXPECT_FALSE(f.name.empty());
    EXPECT_FALSE(f.help.empty()) << f.name;
    EXPECT_FALSE(f.category.empty()) << f.name;
    EXPECT_NE(f.dispatch, nullptr) << f.name;
    // The criterion: the arity the type codes declare must equal the number
    // of documented parameters. A mismatch means Excel passes a different
    // number of cells than the dispatcher reads -- which it would do silently.
    EXPECT_EQ(f.declared_arity(), f.params.size())
        << f.name << ": type codes declare " << f.declared_arity() << " arguments but "
        << f.params.size() << " are documented";
    for (const auto &p : f.params) {
      EXPECT_FALSE(p.name.empty()) << f.name;
      EXPECT_FALSE(p.help.empty()) << f.name << " parameter " << p.name;
    }
  }
}

TEST(Registration, BothFormsOfCdsPriceAreRegistered) {
  const FunctionSpec *legacy = find_function("CdsPrice");
  const FunctionSpec *grouped = find_function("CDS.Price");
  ASSERT_NE(legacy, nullptr);
  ASSERT_NE(grouped, nullptr);
  // The whole point, as a number: 16 arguments against 5.
  EXPECT_EQ(legacy->declared_arity(), 16u);
  EXPECT_EQ(grouped->declared_arity(), 5u);
}

// ------------------------------------------------------------------- C5/C9

TEST(CdsPriceDispatch, C5TheLegacyFormPricesATrade) {
  Fixture f;
  const std::vector<XlValue> args{XlValue{serial(2006, 10, 9)},
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
                                  XlValue{f.discount},
                                  XlValue{f.spread},
                                  XlValue{0.4},
                                  XlValue{true}};
  const XlValue got = invoke(*find_function("CdsPrice"), args, f.ctx);
  ASSERT_FALSE(got.as_double().has_value() == false) << got.describe();
  const auto price = got.as_double();
  ASSERT_TRUE(price.has_value()) << got.describe();
  EXPECT_TRUE(std::isfinite(*price));
}

TEST(CdsPriceDispatch, C9BothFormsGiveTheIdenticalAnswer) {
  Fixture f;
  const std::vector<XlValue> legacy_args{XlValue{serial(2006, 10, 9)},
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
                                         XlValue{f.discount},
                                         XlValue{f.spread},
                                         XlValue{0.4},
                                         XlValue{true}};

  const std::vector<XlValue> grouped_args{block({{"Today", XlValue{serial(2006, 10, 9)}},
                                                 {"Cash Settle Date", XlValue{serial(2006, 10, 9)}},
                                                 {"Step In Date", XlValue{serial(2006, 10, 9)}}}),
                                          block({{"Start Date", XlValue{serial(2006, 10, 9)}},
                                                 {"End Date", XlValue{serial(2011, 10, 9)}},
                                                 {"Coupon Rate", XlValue{0.01}}}),
                                          block({{"Pay Accrual On Default", XlValue{true}},
                                                 {"Coupon Interval", XlValue{"3M"}},
                                                 {"Stub Type", XlValue{"f/s"}},
                                                 {"Payment DCC", XlValue{"ACT/360"}},
                                                 {"Bad Day Convention", XlValue{"N"}},
                                                 {"Holidays", XlValue{"NONE"}}}),
                                          block({{"Discount Curve", XlValue{f.discount}},
                                                 {"Spread Curve", XlValue{f.spread}},
                                                 {"Recovery Rate", XlValue{0.4}}}),
                                          XlValue{true}};

  const XlValue a = invoke(*find_function("CdsPrice"), legacy_args, f.ctx);
  const XlValue b = invoke(*find_function("CDS.Price"), grouped_args, f.ctx);
  ASSERT_FALSE(a.as_double().has_value() == false) << a.describe();
  ASSERT_FALSE(b.as_double().has_value() == false) << b.describe();
  // Bitwise, not near: it is one computation reached two ways. Anything other
  // than exact equality means the readers disagree about an input.
  EXPECT_EQ(*a.as_double(), *b.as_double());
}

TEST(CdsPriceDispatch, AWrongArgumentCountIsReportedNotIndexedPast) {
  Fixture f;
  const std::vector<XlValue> too_few{XlValue{1.0}, XlValue{2.0}};
  const XlValue got = invoke(*find_function("CdsPrice"), too_few, f.ctx);
  ASSERT_TRUE(got.as_double().has_value() == false);
  EXPECT_NE(got.describe().find("16 arguments"), std::string::npos) << got.describe();
}

TEST(CdsPriceDispatch, C3AFailedCallArrivesAsTextNamingTheProblem) {
  Fixture f;
  std::vector<XlValue> args{XlValue{serial(2006, 10, 9)},
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
                            XlValue{f.discount},
                            XlValue{f.spread},
                            XlValue{0.4},
                            XlValue{true}};
  args[9] = XlValue{"ACT/999"}; // a day count that does not exist
  const XlValue got = invoke(*find_function("CdsPrice"), args, f.ctx);
  ASSERT_TRUE(got.as_double().has_value() == false) << "a bad day count was accepted";
  const std::string text = got.describe();
  EXPECT_NE(text.find("Payment DCC"), std::string::npos) << text;
  EXPECT_NE(text.find("ACT/999"), std::string::npos)
      << "the cell should quote what the user typed: " << text;
}

// ------------------------------------------------------------------- C6/C7

TEST(CurveHandles, C6AStaleHandleIsRefusedNotSilentlyResolved) {
  CurveRegistry curves;
  CalendarRegistry calendars;
  const std::string first = curves.store("USD.disc", flat_curve(0.04));
  ASSERT_TRUE(curves.resolve(first).has_value());

  // Rebuilt under the same name: upstream's cache would hand the OLD handle
  // the NEW curve, and every sheet holding it would quietly reprice.
  const std::string second = curves.store("USD.disc", flat_curve(0.09));
  EXPECT_TRUE(curves.resolve(second).has_value());

  const auto stale = curves.resolve(first);
  ASSERT_FALSE(stale.has_value()) << "the stale handle resolved";
  EXPECT_NE(stale.error().message.find("stale"), std::string::npos) << stale.error().message;
}

TEST(CurveHandles, UnknownAndMalformedHandlesSayWhich) {
  CurveRegistry curves;
  CalendarRegistry calendars;
  curves.store("USD.disc", flat_curve(0.04));
  EXPECT_NE(curves.resolve("EUR.disc#1").error().message.find("no curve named"), std::string::npos);
  EXPECT_NE(curves.resolve("USD.disc").error().message.find("not a curve handle"),
            std::string::npos);
  EXPECT_NE(curves.resolve("USD.disc#x").error().message.find("malformed"), std::string::npos);
}

TEST(CurveHandles, C7ClearingTheRegistryInvalidatesEveryHandle) {
  CurveRegistry curves;
  CalendarRegistry calendars;
  const std::string h = curves.store("USD.disc", flat_curve(0.04));
  ASSERT_EQ(curves.size(), 1u);
  curves.clear(); // what xlAutoClose does
  EXPECT_EQ(curves.size(), 0u);
  EXPECT_FALSE(curves.resolve(h).has_value()) << "a handle survived a reload";
}

} // namespace
