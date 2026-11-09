// The last five: ParSpreads, UpfrontFlat, ParSpreadFlat, DefaultAccrual,
// FeeLegFlows. With these the add-in covers every function upstream's XLL
// exposed, minus the six error-log ones that Expected<T> made unnecessary.
//
// Two of the tests here are real oracles rather than smoke checks:
// UpfrontFlat and ParSpreadFlat are inverses, and ParSpreads at a maturity
// whose own quote built the curve must return that quote back.

#include <cdsmodel/xl/calendars.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cdsmodel::xl::CalendarRegistry;
using cdsmodel::xl::CallContext;
using cdsmodel::xl::CurveRegistry;
using cdsmodel::xl::find_function;
using cdsmodel::xl::invoke;
using cdsmodel::xl::XlGrid;
using cdsmodel::xl::XlValue;

double serial(int y, int m, int d) {
  return static_cast<double>(cdsmodel::make_date(y, m, d)->time_since_epoch().count() + 25569);
}

XlValue col(std::initializer_list<XlValue> cells) {
  XlGrid g;
  g.rows = cells.size();
  g.columns = 1;
  g.cells.assign(cells.begin(), cells.end());
  return XlValue{g};
}

/// A sheet with both curves already built, the way a real one would be.
struct Book {
  CurveRegistry curves;
  CalendarRegistry calendars;
  CallContext ctx{curves, calendars};
  XlValue discount;
  XlValue spread;

  Book() {
    discount =
        call("IRZeroCurveMake", {XlValue{serial(2006, 10, 9)},
                                 col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}}),
                                 col({XlValue{0.04}, XlValue{0.04}}), XlValue{5000.0},
                                 XlValue{"ACT/365F"}, XlValue{"USD"}});
    spread = call("CleanSpreadCurveBuild",
                  {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                   XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                   col({XlValue{serial(2011, 10, 9)}}), col({XlValue{0.01}}), XlValue{},
                   XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"},
                   XlValue{"NONE"}, discount, XlValue{0.4}, XlValue{"ACME"}});
  }

  XlValue call(std::string_view name, std::vector<XlValue> args) {
    const auto *spec = find_function(name);
    if (spec == nullptr) {
      return XlValue{std::string("no function named ") + std::string(name)};
    }
    return invoke(*spec, args, ctx);
  }

  /// The seven cells shared by UpfrontFlat and ParSpreadFlat, with the
  /// 15th left for the caller to fill.
  std::vector<XlValue> flat_args(XlValue fifteenth) {
    return {XlValue{serial(2006, 10, 9)},
            XlValue{serial(2006, 10, 11)},
            XlValue{serial(2006, 10, 9)},
            XlValue{serial(2006, 10, 10)},
            XlValue{serial(2006, 10, 9)},
            XlValue{serial(2011, 10, 9)},
            XlValue{0.01},
            XlValue{true},
            XlValue{"3M"},
            XlValue{"f/s"},
            XlValue{"ACT/360"},
            XlValue{"N"},
            XlValue{"NONE"},
            discount,
            std::move(fifteenth),
            XlValue{0.4},
            XlValue{true}};
  }
};

// ---------------------------------------------------------------- ParSpreads

TEST(Pricing, ParSpreadsReturnsOnePerMaturity) {
  Book b;
  ASSERT_TRUE(b.spread.as_string().has_value()) << b.spread.describe();

  const XlValue got =
      b.call("ParSpreads", {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                            XlValue{serial(2006, 10, 9)},
                            col({XlValue{serial(2008, 10, 9)}, XlValue{serial(2011, 10, 9)},
                                 XlValue{serial(2013, 10, 9)}}),
                            XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"},
                            XlValue{"N"}, XlValue{"NONE"}, b.discount, b.spread, XlValue{0.4}});
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value()) << got.describe();
  ASSERT_EQ(grid->rows, 3u);
  ASSERT_EQ(grid->columns, 1u);

  for (std::size_t i = 0; i < 3; ++i) {
    const auto value = grid->at(i, 0).as_double();
    ASSERT_TRUE(value.has_value()) << "row " << i << ": " << grid->at(i, 0).describe();
    EXPECT_GT(*value, 0.0) << "row " << i;
    EXPECT_LT(*value, 1.0) << "row " << i;
  }
}

TEST(Pricing, ParSpreadsAtTheCurvesOwnBenchmarkReturnsItsQuote) {
  // The oracle. The credit curve was bootstrapped from a single benchmark
  // maturing 2011-10-09 quoted at 100bp, so asking for the par spread at that
  // maturity must give 100bp back -- the bootstrap and the par-spread
  // calculation are inverses, and this is the only input where that is
  // checkable without a second implementation.
  Book b;
  const XlValue got =
      b.call("ParSpreads", {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                            XlValue{serial(2006, 10, 9)}, col({XlValue{serial(2011, 10, 9)}}),
                            XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"},
                            XlValue{"N"}, XlValue{"NONE"}, b.discount, b.spread, XlValue{0.4}});
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value()) << got.describe();
  const auto value = grid->at(0, 0).as_double();
  ASSERT_TRUE(value.has_value()) << grid->at(0, 0).describe();
  // Bounded by the bootstrap's own solver tolerance, not by an ULP count.
  EXPECT_NEAR(*value, 0.01, 1e-7) << "got " << *value;
}

TEST(Pricing, OneBadMaturityDoesNotLoseTheWholeColumn) {
  // A column where one cell is wrong and the rest are right is more useful
  // than a single error replacing all of them.
  Book b;
  const XlValue got = b.call(
      "ParSpreads",
      {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
       col({XlValue{serial(2011, 10, 9)}, XlValue{serial(2005, 1, 1)}}), // before the start date
       XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"},
       XlValue{"NONE"}, b.discount, b.spread, XlValue{0.4}});
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value()) << got.describe();
  ASSERT_EQ(grid->rows, 2u);
  EXPECT_TRUE(grid->at(0, 0).as_double().has_value()) << "the good row was lost";
  EXPECT_FALSE(grid->at(1, 0).as_double().has_value()) << "the bad row priced anyway";
}

// -------------------------------------------- UpfrontFlat / ParSpreadFlat

TEST(Pricing, UpfrontFlatAndParSpreadFlatAreInverses) {
  // The real oracle for this pair. Convert a quoted spread to an upfront
  // charge, then convert that charge back, and the spread must return. Any
  // disagreement means one direction is wrong, and neither needs a recorded
  // number to show it.
  Book b;
  for (const double quoted : {0.005, 0.01, 0.02, 0.05}) {
    const XlValue charge = b.call("UpfrontFlat", b.flat_args(XlValue{quoted}));
    const auto upfront = charge.as_double();
    ASSERT_TRUE(upfront.has_value()) << "spread " << quoted << ": " << charge.describe();

    const XlValue back = b.call("ParSpreadFlat", b.flat_args(XlValue{*upfront}));
    const auto recovered = back.as_double();
    ASSERT_TRUE(recovered.has_value()) << "charge " << *upfront << ": " << back.describe();

    // Bounded by the solver's stopping tolerance, which is what decides how
    // close a round trip through a root-find can get.
    EXPECT_NEAR(*recovered, quoted, 1e-7)
        << "quoted " << quoted << " -> upfront " << *upfront << " -> " << *recovered;
  }
}

TEST(Pricing, AnUpfrontChargeIsZeroWhenTheCouponEqualsTheQuote) {
  // A trade paying exactly its own market spread costs nothing upfront. The
  // flat_args fixture pays 100bp, so quoting 100bp must give ~0.
  Book b;
  const XlValue charge = b.call("UpfrontFlat", b.flat_args(XlValue{0.01}));
  const auto value = charge.as_double();
  ASSERT_TRUE(value.has_value()) << charge.describe();
  EXPECT_NEAR(*value, 0.0, 1e-6) << "got " << *value;
}

TEST(Pricing, TheFlatFunctionsNameWhicheverCellIsWrong) {
  Book b;
  ASSERT_TRUE(b.call("UpfrontFlat", b.flat_args(XlValue{0.01})).as_double().has_value())
      << "the control failed";

  struct Case {
    std::size_t index;
    XlValue bad;
    const char *mentions;
  };
  const Case cases[]{
      {0, XlValue{"nope"}, "Today"},
      {1, XlValue{"nope"}, "CashSettle Date"},
      {2, XlValue{"nope"}, "Benchmark St Date"},
      {3, XlValue{"nope"}, "StepIn Date"},
      {4, XlValue{"nope"}, "Start Date"},
      {5, XlValue{"nope"}, "End Date"},
      {6, XlValue{"nope"}, "Coupon Rate"},
      {13, XlValue{"NOPE#1"}, "Discount Curve"},
      {14, XlValue{"nope"}, "Market Par Spread"},
      {15, XlValue{2.0}, "Recovery Rate"},
      {16, XlValue{"maybe"}, "Clean Price"},
  };
  for (const Case &c : cases) {
    std::vector<XlValue> args = b.flat_args(XlValue{0.01});
    args[c.index] = c.bad;
    const std::string text = b.call("UpfrontFlat", args).describe();
    EXPECT_NE(text.find(c.mentions), std::string::npos) << "argument " << c.index << ": " << text;
  }

  // And ParSpreadFlat's 15th cell is an upfront charge, not a spread.
  std::vector<XlValue> args = b.flat_args(XlValue{"nope"});
  EXPECT_NE(b.call("ParSpreadFlat", args).describe().find("Upfront Charge"), std::string::npos);
}

// ------------------------------------------------------------ DefaultAccrual

TEST(Pricing, DefaultAccrualReturnsDaysAndAmount) {
  Book b;
  const XlValue got =
      b.call("DefaultAccrual", {XlValue{serial(2007, 2, 15)}, XlValue{serial(2007, 2, 15)},
                                XlValue{serial(2006, 10, 9)}, XlValue{serial(2011, 10, 9)},
                                XlValue{0.01}, XlValue{1.0e7}, XlValue{"3M"}, XlValue{"f/s"},
                                XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"}});
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value()) << got.describe();
  ASSERT_EQ(grid->rows, 1u);
  ASSERT_EQ(grid->columns, 2u);

  const auto days = grid->at(0, 0).as_double();
  const auto amount = grid->at(0, 1).as_double();
  ASSERT_TRUE(days.has_value());
  ASSERT_TRUE(amount.has_value());

  // The event is 2007-02-15; the coupon period containing it began
  // 2007-01-09, so the accrual runs 09 Jan through 15 Feb inclusive = 38 days.
  EXPECT_EQ(*days, 38.0) << "day count";
  // notional * rate * days/360.
  EXPECT_NEAR(*amount, 1.0e7 * 0.01 * 38.0 / 360.0, 1e-6);
  EXPECT_EQ(*days, static_cast<double>(static_cast<long>(*days))) << "a day count must be integral";
}

TEST(Pricing, DefaultAccrualRefusesAnEventBeforeTheTradeStarts) {
  Book b;
  const XlValue got = b.call(
      "DefaultAccrual",
      {XlValue{serial(2006, 1, 1)}, XlValue{serial(2006, 1, 1)}, // before the effective date
       XlValue{serial(2006, 10, 9)}, XlValue{serial(2011, 10, 9)}, XlValue{0.01}, XlValue{1.0e7},
       XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"}});
  // NOT `as_grid().has_value()` -- any single value is a 1x1 range, so a
  // failure STRING passes that check. The same trap caught the Include Flags
  // test one slice ago; the real question is whether two columns came back.
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value());
  EXPECT_NE(grid->columns, 2u) << "an impossible event produced an accrual";
  EXPECT_NE(got.describe().find("event"), std::string::npos) << got.describe();
}

// -------------------------------------------------------------- FeeLegFlows

TEST(Pricing, FeeLegFlowsListsEveryPayment) {
  Book b;
  const XlValue got =
      b.call("FeeLegFlows", {XlValue{serial(2006, 10, 9)}, XlValue{serial(2011, 10, 9)},
                             XlValue{0.01}, XlValue{1.0e7}, XlValue{"3M"}, XlValue{"f/s"},
                             XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"}});
  const auto grid = got.as_grid();
  ASSERT_TRUE(grid.has_value()) << got.describe();
  ASSERT_EQ(grid->columns, 2u);
  // Five years, quarterly: twenty payments.
  EXPECT_EQ(grid->rows, 20u);

  double previous_date = 0.0;
  double total = 0.0;
  for (std::size_t i = 0; i < grid->rows; ++i) {
    const auto date = grid->at(i, 0).as_double();
    const auto amount = grid->at(i, 1).as_double();
    ASSERT_TRUE(date.has_value()) << "row " << i;
    ASSERT_TRUE(amount.has_value()) << "row " << i;
    EXPECT_GT(*date, previous_date) << "row " << i << ": dates must advance";
    previous_date = *date;
    EXPECT_GT(*amount, 0.0) << "row " << i;
    total += *amount;
  }
  // Twenty quarterly coupons at 1% on 10m is about five years of coupon:
  // 1e7 * 0.01 * 5 = 500,000, give or take the day-count convention.
  EXPECT_NEAR(total, 1.0e7 * 0.01 * 5.0, 1.0e4) << "total " << total;
}

TEST(Pricing, FeeLegFlowsAndDefaultAccrualNameTheirOwnCells) {
  Book b;
  const auto flows = [&](std::size_t index, XlValue bad) {
    std::vector<XlValue> args{XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2011, 10, 9)},
                              XlValue{0.01},
                              XlValue{1.0e7},
                              XlValue{"3M"},
                              XlValue{"f/s"},
                              XlValue{"ACT/360"},
                              XlValue{"N"},
                              XlValue{"NONE"}};
    args[index] = std::move(bad);
    return b.call("FeeLegFlows", args).describe();
  };
  EXPECT_NE(flows(0, XlValue{"nope"}).find("Start Date"), std::string::npos);
  EXPECT_NE(flows(1, XlValue{"nope"}).find("End Date"), std::string::npos);
  EXPECT_NE(flows(2, XlValue{"nope"}).find("Coupon Rate"), std::string::npos);
  EXPECT_NE(flows(3, XlValue{"nope"}).find("Notional"), std::string::npos);
  EXPECT_NE(flows(6, XlValue{"ACT/999"}).find("Payment DCC"), std::string::npos);

  // Wrong arity on each of the five.
  for (const auto &[name, mentions] :
       std::vector<std::pair<const char *, const char *>>{{"ParSpreads", "13 arguments"},
                                                          {"UpfrontFlat", "17 arguments"},
                                                          {"ParSpreadFlat", "17 arguments"},
                                                          {"DefaultAccrual", "11 arguments"},
                                                          {"FeeLegFlows", "9 arguments"}}) {
    const std::string text = b.call(name, {XlValue{1.0}}).describe();
    EXPECT_NE(text.find(mentions), std::string::npos) << name << ": " << text;
  }
}

TEST(Pricing, TheRemainingCellsOfParSpreadsAndTheGridFunctionsAreNamed) {
  // The last few refusals the coverage gate found untested. Each is a cell a
  // user can get wrong in a function taking up to seventeen of them.
  Book b;
  const auto par = [&](std::size_t index, XlValue bad) {
    std::vector<XlValue> args{XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2006, 10, 9)},
                              col({XlValue{serial(2011, 10, 9)}}),
                              XlValue{true},
                              XlValue{"3M"},
                              XlValue{"f/s"},
                              XlValue{"ACT/360"},
                              XlValue{"N"},
                              XlValue{"NONE"},
                              b.discount,
                              b.spread,
                              XlValue{0.4}};
    args[index] = std::move(bad);
    return b.call("ParSpreads", args).describe();
  };
  EXPECT_NE(par(9, XlValue{"LON"}).find("Holidays"), std::string::npos) << par(9, XlValue{"LON"});
  EXPECT_NE(par(11, XlValue{"NOPE#1"}).find("Spread Curve"), std::string::npos);
  EXPECT_NE(par(12, XlValue{"lots"}).find("Recovery Rate"), std::string::npos);
  EXPECT_NE(par(12, XlValue{-0.5}).find("Recovery Rate"), std::string::npos);

  // DefaultAccrual's own two numeric cells, and its Holidays.
  const auto accrual = [&](std::size_t index, XlValue bad) {
    std::vector<XlValue> args{XlValue{serial(2007, 2, 15)},
                              XlValue{serial(2007, 2, 15)},
                              XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2011, 10, 9)},
                              XlValue{0.01},
                              XlValue{1.0e7},
                              XlValue{"3M"},
                              XlValue{"f/s"},
                              XlValue{"ACT/360"},
                              XlValue{"N"},
                              XlValue{"NONE"}};
    args[index] = std::move(bad);
    return b.call("DefaultAccrual", args).describe();
  };
  EXPECT_NE(accrual(4, XlValue{"nope"}).find("Coupon Rate"), std::string::npos);
  EXPECT_NE(accrual(5, XlValue{"nope"}).find("Notional"), std::string::npos);
  EXPECT_NE(accrual(10, XlValue{"LON"}).find("Holidays"), std::string::npos);
}

// --------------------------------------------------------- the whole surface

TEST(Pricing, EveryTypeCodeIsUNotQ) {
  // `xlcall.h`, directly above the XLOPER12 definition: "Use \"U\" as the
  // argument type in the REGISTER function." `Q` is the pre-2007 XLOPER, and
  // registering it against a function taking LPXLOPER12 hands Excel the wrong
  // struct for every argument.
  //
  // This is a string, so no compiler can catch it and the typechecker could
  // not either -- it was found by reading the SDK header. This test is what
  // stops it coming back.
  for (const cdsmodel::xl::FunctionSpec &f : cdsmodel::xl::all_functions()) {
    EXPECT_EQ(f.type_codes.find('Q'), std::string_view::npos)
        << f.name << " registers type code Q (XLOPER); XLOPER12 needs U";
    for (const char c : f.type_codes) {
      EXPECT_EQ(c, 'U') << f.name << " has an unexpected type code '" << c << "'";
    }
  }
}

TEST(Pricing, EveryFunctionUpstreamExposedIsNowRegistered) {
  // Upstream's XLL registered 18 functions. Six managed a global error log and
  // are gone, because Expected<T> carries the reason to the cell that asked.
  // The other twelve, plus Version, must all be here.
  for (const char *name :
       {"CdsPrice", "ParSpreads", "ParSpreadFlat", "UpfrontFlat", "DefaultAccrual", "FeeLegFlows",
        "CleanSpreadCurveBuild", "IRZeroCurveBuild", "IRZeroCurveMake", "DiscountFactor",
        "DatesAndRates", "LoadHolidays"}) {
    EXPECT_NE(find_function(name), nullptr) << name << " is not registered";
  }
  // And the error-log functions are deliberately absent.
  for (const char *gone : {"ErrorLogStatus", "ErrorLogContents", "ErrorLogFilename",
                           "SetErrorLogFilename", "SetErrorLogStatus"}) {
    EXPECT_EQ(find_function(gone), nullptr)
        << gone << " was reproduced; Expected<T> makes it unnecessary";
  }
}

} // namespace
