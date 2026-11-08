// The curve-building functions: IRZeroCurveMake, IRZeroCurveBuild,
// CleanSpreadCurveBuild, DiscountFactor, DatesAndRates.
//
// These are the ones that produce the handles everything else consumes, so the
// end-to-end test below -- build a discount curve, bootstrap a credit curve
// off it, then price a CDS against both -- is the one that matters. Until it
// worked, every price in a sheet depended on a handle nothing could create.

#include <cdsmodel/xl/calendars.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
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

/// An empty file on disk, for the distinction the test below draws.
class TempEmpty {
public:
  TempEmpty() {
    path_ = std::filesystem::temp_directory_path() / "cdsmodel_empty_holidays.dat";
    std::ofstream out(path_, std::ios::binary);
  }
  ~TempEmpty() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  TempEmpty(const TempEmpty &) = delete;
  TempEmpty &operator=(const TempEmpty &) = delete;
  [[nodiscard]] std::string name() const { return path_.string(); }

private:
  std::filesystem::path path_;
};

struct Sheet {
  CurveRegistry curves;
  CalendarRegistry calendars;
  CallContext ctx{curves, calendars};

  XlValue call(std::string_view name, std::vector<XlValue> args) {
    const auto *spec = find_function(name);
    if (spec == nullptr) {
      return XlValue{std::string("no function named ") + std::string(name)};
    }
    return invoke(*spec, args, ctx);
  }

  /// A flat 4% discount curve, as a handle.
  XlValue discount(const char *name = "D") {
    return call("IRZeroCurveMake", {XlValue{serial(2006, 10, 9)},
                                    col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}}),
                                    col({XlValue{0.04}, XlValue{0.04}}), XlValue{5000.0},
                                    XlValue{"ACT/365F"}, XlValue{name}});
  }
};

// ------------------------------------------------------------ IRZeroCurveMake

TEST(Curves, IRZeroCurveMakeReturnsAUsableHandle) {
  Sheet s;
  const XlValue handle = s.discount();
  const auto text = handle.as_string();
  ASSERT_TRUE(text.has_value()) << handle.describe();
  EXPECT_EQ(*text, "D#1");

  const auto curve = s.curves.resolve(*text);
  ASSERT_TRUE(curve.has_value()) << curve.error().message;
  // A 4% continuously-compounded curve: the discount factor one year out is
  // exp(-0.04 * 365/365). Checked against the closed form, not a recorded
  // number -- this is the highest rung available for a flat curve.
  const double df = curve->zero_price(*cdsmodel::make_date(2007, 10, 9));
  EXPECT_NEAR(df, std::exp(-0.04 * 365.0 / 365.0), 1e-9);
}

TEST(Curves, IRZeroCurveMakeRefusesMismatchedColumns) {
  Sheet s;
  const XlValue got =
      s.call("IRZeroCurveMake",
             {XlValue{serial(2006, 10, 9)},
              col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}}), // 2 dates
              col({XlValue{0.04}}),                                            // 1 rate
              XlValue{5000.0}, XlValue{"ACT/365F"}, XlValue{"D"}});
  const std::string text = got.describe();
  EXPECT_NE(text.find("2 entries"), std::string::npos) << text;
  EXPECT_NE(text.find("1"), std::string::npos) << text;
}

TEST(Curves, IRZeroCurveMakeNamesWhicheverArgumentIsWrong) {
  Sheet s;
  const std::vector<XlValue> good{XlValue{serial(2006, 10, 9)},
                                  col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}}),
                                  col({XlValue{0.04}, XlValue{0.04}}),
                                  XlValue{5000.0},
                                  XlValue{"ACT/365F"},
                                  XlValue{"D"}};
  ASSERT_TRUE(s.call("IRZeroCurveMake", good).as_string().has_value()) << "the control failed";

  struct Case {
    std::size_t index;
    XlValue bad;
    const char *mentions;
  };
  const Case cases[]{
      {0, XlValue{"nope"}, "Base Date"},
      {1, XlValue{"nope"}, "Dates"},
      {2, XlValue{col({XlValue{"nope"}})}, "Rates"},
      {4, XlValue{"ACT/999"}, "DCC"},
      {5, XlValue{""}, "name"},
  };
  for (const Case &c : cases) {
    std::vector<XlValue> args = good;
    args[c.index] = c.bad;
    const std::string text = s.call("IRZeroCurveMake", args).describe();
    EXPECT_NE(text.find(c.mentions), std::string::npos) << "argument " << c.index << ": " << text;
  }
}

TEST(Curves, BlankCellsBelowAColumnAreIgnored) {
  // Selecting a whole column is the obvious way to call these, and it picks up
  // the empty rows below the data.
  Sheet s;
  const XlValue handle =
      s.call("IRZeroCurveMake",
             {XlValue{serial(2006, 10, 9)},
              col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}, XlValue{}, XlValue{}}),
              col({XlValue{0.04}, XlValue{0.04}, XlValue{}, XlValue{}}), XlValue{5000.0},
              XlValue{"ACT/365F"}, XlValue{"D"}});
  EXPECT_TRUE(handle.as_string().has_value()) << handle.describe();
}

// ----------------------------------------------------------- IRZeroCurveBuild

TEST(Curves, IRZeroCurveBuildBootstrapsFromQuotes) {
  Sheet s;
  const XlValue handle = s.call(
      "IRZeroCurveBuild",
      {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}, XlValue{"M"}, XlValue{"S"}, XlValue{"S"}}),
       col({XlValue{serial(2006, 11, 9)}, XlValue{serial(2007, 1, 9)}, XlValue{serial(2008, 10, 9)},
            XlValue{serial(2011, 10, 9)}}),
       col({XlValue{0.0530}, XlValue{0.0535}, XlValue{0.0540}, XlValue{0.0550}}),
       XlValue{"ACT/360"}, XlValue{"6M"}, XlValue{"6M"}, XlValue{"30/360"}, XlValue{"ACT/360"},
       XlValue{"N"}, XlValue{"NONE"}, XlValue{"IR"}});
  const auto text = handle.as_string();
  ASSERT_TRUE(text.has_value()) << handle.describe();

  const auto curve = s.curves.resolve(*text);
  ASSERT_TRUE(curve.has_value());
  // NOT one point per instrument -- a swap contributes a point per coupon
  // date, so four instruments here produce twelve points. An earlier version
  // of this test asserted four and was wrong about the bootstrap, not about
  // the code. The exact count is already compared against the oracle in
  // tests/ir_zero_curve_differential_test.cpp (`ref->fNumItems ==
  // points.size()`), so duplicating it here would add nothing; what this
  // layer owns is that the cells reached the library at all.
  EXPECT_GE(curve->points().size(), 4u) << "fewer points than instruments";
  double previous = 1.0;
  for (const int year : {2007, 2008, 2009, 2011}) {
    const double df = curve->zero_price(*cdsmodel::make_date(year, 10, 9));
    EXPECT_LT(df, previous) << "the discount factor rose at " << year;
    EXPECT_GT(df, 0.0);
    previous = df;
  }
}

TEST(Curves, IRZeroCurveBuildAcceptsAFrequencyEitherWay) {
  // Upstream states swap frequencies as intervals (6M); a count per year is
  // what the library holds. A sheet written against upstream has the former.
  Sheet s;
  const auto build = [&](XlValue fixed, XlValue flt, const char *name) {
    return s.call("IRZeroCurveBuild",
                  {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}, XlValue{"S"}}),
                   col({XlValue{serial(2006, 11, 9)}, XlValue{serial(2011, 10, 9)}}),
                   col({XlValue{0.053}, XlValue{0.055}}), XlValue{"ACT/360"}, fixed, flt,
                   XlValue{"30/360"}, XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"},
                   XlValue{name}});
  };
  const XlValue as_interval = build(XlValue{"6M"}, XlValue{"6M"}, "A");
  const XlValue as_count = build(XlValue{2.0}, XlValue{2.0}, "B");
  ASSERT_TRUE(as_interval.as_string().has_value()) << as_interval.describe();
  ASSERT_TRUE(as_count.as_string().has_value()) << as_count.describe();

  const auto a = s.curves.resolve(*as_interval.as_string());
  const auto b = s.curves.resolve(*as_count.as_string());
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  // 6M and 2-per-year are the same thing, so the curves must be identical.
  for (const int year : {2007, 2009, 2011}) {
    const cdsmodel::Date d = *cdsmodel::make_date(year, 10, 9);
    EXPECT_EQ(a->zero_price(d), b->zero_price(d)) << year;
  }

  // And a frequency that does not divide 12 is refused rather than rounded.
  const XlValue odd = build(XlValue{"5M"}, XlValue{"6M"}, "C");
  EXPECT_NE(odd.describe().find("dividing 12"), std::string::npos) << odd.describe();
}

TEST(Curves, IRZeroCurveBuildRefusesABadInstrumentCode) {
  Sheet s;
  const XlValue got = s.call("IRZeroCurveBuild",
                             {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}, XlValue{"X"}}),
                              col({XlValue{serial(2006, 11, 9)}, XlValue{serial(2011, 10, 9)}}),
                              col({XlValue{0.053}, XlValue{0.055}}), XlValue{"ACT/360"},
                              XlValue{"6M"}, XlValue{"6M"}, XlValue{"30/360"}, XlValue{"ACT/360"},
                              XlValue{"N"}, XlValue{"NONE"}, XlValue{"IR"}});
  const std::string text = got.describe();
  EXPECT_NE(text.find("Types row 2"), std::string::npos) << text;
  EXPECT_NE(text.find("X"), std::string::npos) << text;
}

TEST(Curves, IRZeroCurveBuildRefusesRaggedColumns) {
  Sheet s;
  const XlValue got = s.call("IRZeroCurveBuild",
                             {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}, XlValue{"S"}}),
                              col({XlValue{serial(2006, 11, 9)}}), // one date, two types
                              col({XlValue{0.053}, XlValue{0.055}}), XlValue{"ACT/360"},
                              XlValue{"6M"}, XlValue{"6M"}, XlValue{"30/360"}, XlValue{"ACT/360"},
                              XlValue{"N"}, XlValue{"NONE"}, XlValue{"IR"}});
  EXPECT_NE(got.describe().find("same length"), std::string::npos) << got.describe();
}

// --------------------------------------------------- DiscountFactor / DatesAndRates

TEST(Curves, DiscountFactorReadsACurveByHandle) {
  Sheet s;
  const XlValue handle = s.discount();
  ASSERT_TRUE(handle.as_string().has_value());

  const XlValue df = s.call("DiscountFactor", {handle, XlValue{serial(2007, 10, 9)}});
  const auto value = df.as_double();
  ASSERT_TRUE(value.has_value()) << df.describe();
  EXPECT_NEAR(*value, std::exp(-0.04), 1e-9);

  // At the base date it is exactly 1.
  const XlValue at_base = s.call("DiscountFactor", {handle, XlValue{serial(2006, 10, 9)}});
  EXPECT_EQ(*at_base.as_double(), 1.0);

  // A stale or unknown handle names the argument.
  EXPECT_NE(s.call("DiscountFactor", {XlValue{"D#99"}, XlValue{serial(2007, 1, 1)}})
                .describe()
                .find("Curve"),
            std::string::npos);
}

TEST(Curves, DatesAndRatesSpillsTheCurveAsTwoColumns) {
  Sheet s;
  const XlValue handle = s.discount();
  const XlValue grid = s.call("DatesAndRates", {handle});
  const auto spilled = grid.as_grid();
  ASSERT_TRUE(spilled.has_value()) << grid.describe();
  ASSERT_EQ(spilled->columns, 2u);
  ASSERT_EQ(spilled->rows, 2u);

  // Dates in column 1, rates in column 2, and the serials round-trip.
  EXPECT_EQ(*spilled->at(0, 0).as_double(), serial(2007, 1, 1));
  EXPECT_EQ(*spilled->at(0, 1).as_double(), 0.04);
  EXPECT_EQ(*spilled->at(1, 0).as_double(), serial(2030, 1, 1));
  EXPECT_EQ(*spilled->at(1, 1).as_double(), 0.04);
}

// -------------------------------------------------- CleanSpreadCurveBuild

TEST(Curves, CleanSpreadCurveBuildBootstrapsACreditCurve) {
  Sheet s;
  const XlValue discount = s.discount();
  ASSERT_TRUE(discount.as_string().has_value()) << discount.describe();

  const XlValue handle =
      s.call("CleanSpreadCurveBuild",
             {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
              XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
              col({XlValue{serial(2009, 10, 9)}, XlValue{serial(2011, 10, 9)}}),
              col({XlValue{0.01}, XlValue{0.012}}), XlValue{}, // Include Flags blank = all
              XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"},
              XlValue{"NONE"}, discount, XlValue{0.4}, XlValue{"SP"}});
  const auto text = handle.as_string();
  ASSERT_TRUE(text.has_value()) << handle.describe();

  const auto curve = s.curves.resolve(*text);
  ASSERT_TRUE(curve.has_value());
  EXPECT_EQ(curve->points().size(), 2u) << "one point per included benchmark";
  // Survival probabilities fall with maturity, which is the one invariant a
  // credit curve must satisfy whatever the inputs.
  const double near = curve->zero_price(*cdsmodel::make_date(2009, 10, 9));
  const double far = curve->zero_price(*cdsmodel::make_date(2011, 10, 9));
  EXPECT_LT(far, near);
  EXPECT_GT(far, 0.0);
}

TEST(Curves, IncludeFlagsDropBenchmarks) {
  Sheet s;
  const XlValue discount = s.discount();
  const auto build = [&](XlValue flags, const char *name) {
    return s.call("CleanSpreadCurveBuild",
                  {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                   XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                   col({XlValue{serial(2009, 10, 9)}, XlValue{serial(2011, 10, 9)}}),
                   col({XlValue{0.01}, XlValue{0.012}}), flags, XlValue{true}, XlValue{"3M"},
                   XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"}, discount,
                   XlValue{0.4}, XlValue{name}});
  };

  const XlValue both = build(XlValue{}, "A");
  ASSERT_TRUE(both.as_string().has_value()) << both.describe();
  EXPECT_EQ(s.curves.resolve(*both.as_string())->points().size(), 2u);

  const XlValue one = build(col({XlValue{true}, XlValue{false}}), "B");
  ASSERT_TRUE(one.as_string().has_value()) << one.describe();
  EXPECT_EQ(s.curves.resolve(*one.as_string())->points().size(), 1u)
      << "the excluded benchmark still produced a point";

  // A flag column of the wrong length is refused rather than silently padded.
  const XlValue ragged = build(col({XlValue{true}}), "C");
  EXPECT_NE(ragged.describe().find("Include Flags has 1"), std::string::npos) << ragged.describe();
}

// ------------------------------------------- the refusals, named and tested
//
// Added because the coverage gate caught this slice at 84.6%/80.1% against
// floors of 90/86. Every line below is a cell a user can type wrong across
// functions taking up to sixteen of them, so "something is wrong" without a
// field name is useless.

TEST(Curves, EveryCurveFunctionRefusesTheWrongArgumentCount) {
  Sheet s;
  struct Case {
    const char *function;
    std::size_t args;
    const char *mentions;
  };
  const Case cases[]{{"IRZeroCurveMake", 2, "6 arguments"},
                     {"IRZeroCurveBuild", 3, "12 arguments"},
                     {"CleanSpreadCurveBuild", 4, "16 arguments"},
                     {"DiscountFactor", 1, "2 arguments"},
                     {"DatesAndRates", 0, "1 argument"}};
  for (const Case &c : cases) {
    const std::vector<XlValue> args(c.args, XlValue{1.0});
    const std::string text = s.call(c.function, args).describe();
    EXPECT_NE(text.find(c.mentions), std::string::npos) << c.function << ": " << text;
  }
}

TEST(Curves, IRZeroCurveMakeAndBuildRefuseTheirRemainingCells) {
  Sheet s;
  // A non-numeric basis, which is the one cell IRZeroCurveMake reads as a
  // bare number rather than through a named reader.
  const std::string basis =
      s.call("IRZeroCurveMake", {XlValue{serial(2006, 10, 9)},
                                 col({XlValue{serial(2007, 1, 1)}, XlValue{serial(2030, 1, 1)}}),
                                 col({XlValue{0.04}, XlValue{0.04}}), XlValue{"continuous"},
                                 XlValue{"ACT/365F"}, XlValue{"D"}})
          .describe();
  EXPECT_NE(basis.find("Basis"), std::string::npos) << basis;

  // An unknown calendar handle in IRZeroCurveBuild's Holidays cell.
  const std::string holidays =
      s.call("IRZeroCurveBuild",
             {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}}),
              col({XlValue{serial(2006, 11, 9)}}), col({XlValue{0.053}}), XlValue{"ACT/360"},
              XlValue{"6M"}, XlValue{"6M"}, XlValue{"30/360"}, XlValue{"ACT/360"}, XlValue{"N"},
              XlValue{"LON"}, XlValue{"IR"}})
          .describe();
  EXPECT_NE(holidays.find("Holidays"), std::string::npos) << holidays;

  // A bad day convention that is not one letter, and one that is not a
  // recognised letter.
  const auto with_bdc = [&](const char *bdc) {
    return s
        .call("IRZeroCurveBuild",
              {XlValue{serial(2006, 10, 9)}, col({XlValue{"M"}}),
               col({XlValue{serial(2006, 11, 9)}}), col({XlValue{0.053}}), XlValue{"ACT/360"},
               XlValue{"6M"}, XlValue{"6M"}, XlValue{"30/360"}, XlValue{"ACT/360"}, XlValue{bdc},
               XlValue{"NONE"}, XlValue{"IR"}})
        .describe();
  };
  EXPECT_NE(with_bdc("Follow").find("Swap BDC"), std::string::npos) << with_bdc("Follow");
  EXPECT_NE(with_bdc("Q").find("Swap BDC"), std::string::npos) << with_bdc("Q");
}

TEST(Curves, CleanSpreadCurveBuildRefusesItsOwnCellsByName) {
  Sheet s;
  const XlValue discount = s.discount();
  const auto build = [&](std::size_t index, XlValue bad) {
    std::vector<XlValue> args{XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2006, 10, 9)},
                              XlValue{serial(2006, 10, 9)},
                              col({XlValue{serial(2011, 10, 9)}}),
                              col({XlValue{0.01}}),
                              XlValue{},
                              XlValue{true},
                              XlValue{"3M"},
                              XlValue{"f/s"},
                              XlValue{"ACT/360"},
                              XlValue{"N"},
                              XlValue{"NONE"},
                              discount,
                              XlValue{0.4},
                              XlValue{"SP"}};
    args[index] = std::move(bad);
    return s.call("CleanSpreadCurveBuild", args).describe();
  };

  struct Case {
    std::size_t index;
    XlValue bad;
    const char *mentions;
  };
  const Case cases[]{
      {0, XlValue{"nope"}, "Today"},
      {1, XlValue{"nope"}, "Start Date"},
      {2, XlValue{"nope"}, "Stepin Date"},
      {3, XlValue{"nope"}, "CashSettle Date"},
      {4, XlValue{col({XlValue{"nope"}})}, "End Dates"},
      {5, XlValue{col({XlValue{"nope"}})}, "Coupon Rates"},
      // An error cell is the only way Include Flags fails to read as a range:
      // any ordinary single value is a 1x1 range, and with one benchmark that
      // is a legitimate single flag. An earlier version of this test passed
      // `3.0` expecting a refusal and got a curve -- because Excel treats
      // non-zero as TRUE, which `as_bool` deliberately implements.
      {6, XlValue{cdsmodel::xl::XlError{"#REF!"}}, "Include Flags"},
      {6, XlValue{col({XlValue{"maybe"}})}, "Include Flags row"},
      {13, XlValue{"NOPE#1"}, "Discount Curve"},
      {14, XlValue{"lots"}, "Recovery Rate"},
      {14, XlValue{1.5}, "Recovery Rate"},
      {15, XlValue{""}, "name"},
  };
  for (const Case &c : cases) {
    const std::string text = build(c.index, c.bad);
    EXPECT_NE(text.find(c.mentions), std::string::npos) << "argument " << c.index << ": " << text;
  }
}

TEST(Curves, MismatchedBenchmarkColumnsAreRefused) {
  Sheet s;
  const XlValue discount = s.discount();
  const std::string text =
      s.call("CleanSpreadCurveBuild",
             {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
              XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
              col({XlValue{serial(2009, 10, 9)}, XlValue{serial(2011, 10, 9)}}), // 2 dates
              col({XlValue{0.01}}),                                              // 1 coupon
              XlValue{}, XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"},
              XlValue{"N"}, XlValue{"NONE"}, discount, XlValue{0.4}, XlValue{"SP"}})
          .describe();
  EXPECT_NE(text.find("End Dates has 2"), std::string::npos) << text;
}

TEST(Curves, TheColumnReadersRefuseEmptyAndNonRangeInput) {
  // The three readers every curve function leans on. An all-blank column is
  // the realistic mistake -- a user selects the wrong column -- and an error
  // cell is how `as_grid` itself fails, since any ordinary value is a 1x1
  // range.
  const XlValue all_blank = col({XlValue{}, XlValue{}});
  const XlValue error_cell{cdsmodel::xl::XlError{"#REF!"}};

  EXPECT_NE(cdsmodel::xl::read_numbers(all_blank, "Rates").error().message.find("empty"),
            std::string::npos);
  EXPECT_NE(cdsmodel::xl::read_dates(all_blank, "Dates").error().message.find("empty"),
            std::string::npos);
  EXPECT_NE(cdsmodel::xl::read_strings(all_blank, "Types").error().message.find("empty"),
            std::string::npos);

  for (const auto &[reader, what] :
       std::vector<std::pair<int, const char *>>{{0, "Rates"}, {1, "Dates"}, {2, "Types"}}) {
    const auto message = reader == 0   ? cdsmodel::xl::read_numbers(error_cell, what).error()
                         : reader == 1 ? cdsmodel::xl::read_dates(error_cell, what).error()
                                       : cdsmodel::xl::read_strings(error_cell, what).error();
    EXPECT_NE(message.message.find(what), std::string::npos) << message.message;
  }

  // And a fractional serial in a date column.
  const auto fractional =
      cdsmodel::xl::read_dates(col({XlValue{serial(2024, 1, 1) + 0.5}}), "Dates");
  ASSERT_FALSE(fractional.has_value());
  EXPECT_NE(fractional.error().message.find("whole number of days"), std::string::npos)
      << fractional.error().message;
}

TEST(Curves, ADirectoryIsNotAHolidayFile) {
  // Found by writing this test: `ifstream` opens a directory successfully on
  // macOS and reads zero bytes, and an EMPTY holiday file is legitimate --
  // upstream accepts one and produces weekends only. So pointing at the wrong
  // path silently yielded a working calendar with no holidays, which is the
  // same silent-wrong-calendar failure UPSTREAM-41 produces by another route.
  const auto as_dir =
      cdsmodel::xl::read_calendar_file(std::filesystem::temp_directory_path().string());
  ASSERT_FALSE(as_dir.has_value()) << "a directory was read as a holiday file";
  EXPECT_NE(as_dir.error().message.find("is not a file"), std::string::npos)
      << as_dir.error().message;

  // An empty FILE is still accepted, because upstream accepts one.
  const TempEmpty empty;
  const auto blank = cdsmodel::xl::read_calendar_file(empty.name());
  ASSERT_TRUE(blank.has_value()) << blank.error().message;
  EXPECT_TRUE(blank->holidays().empty());
  EXPECT_FALSE(blank->is_business_day(*cdsmodel::make_date(2024, 3, 16))) << "Saturday";
}

TEST(Curves, DatesAndRatesAndDiscountFactorRefuseANonHandle) {
  Sheet s;
  EXPECT_NE(s.call("DatesAndRates", {XlValue{3.0}}).describe().find("Curve"), std::string::npos);
  EXPECT_NE(s.call("DatesAndRates", {XlValue{"NOPE#1"}}).describe().find("Curve"),
            std::string::npos);
  EXPECT_NE(s.call("DiscountFactor", {s.discount(), XlValue{"nope"}}).describe().find("Date"),
            std::string::npos);
}

// ------------------------------------------------------------ end to end

TEST(Curves, TheWholeChainWorksFromCellsToAPrice) {
  // This is the test the slice exists for. Before it, a sheet could not build
  // either curve, so every price depended on a handle nothing could create.
  Sheet s;

  const XlValue discount = s.discount("USD");
  ASSERT_TRUE(discount.as_string().has_value()) << discount.describe();

  const XlValue spread = s.call(
      "CleanSpreadCurveBuild",
      {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
       XlValue{serial(2006, 10, 9)}, col({XlValue{serial(2011, 10, 9)}}), col({XlValue{0.01}}),
       XlValue{}, XlValue{true}, XlValue{"3M"}, XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"},
       XlValue{"NONE"}, discount, XlValue{0.4}, XlValue{"ACME"}});
  ASSERT_TRUE(spread.as_string().has_value()) << spread.describe();

  const XlValue price =
      s.call("CdsPrice", {XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                          XlValue{serial(2006, 10, 9)}, XlValue{serial(2006, 10, 9)},
                          XlValue{serial(2011, 10, 9)}, XlValue{0.01}, XlValue{true}, XlValue{"3M"},
                          XlValue{"f/s"}, XlValue{"ACT/360"}, XlValue{"N"}, XlValue{"NONE"},
                          discount, spread, XlValue{0.4}, XlValue{true}});
  const auto value = price.as_double();
  ASSERT_TRUE(value.has_value()) << "the chain broke: " << price.describe();

  // The benchmark was quoted at 100bp and the trade pays 100bp, so a trade
  // priced off its own bootstrapping benchmark is worth about nothing. That
  // is a closed-form expectation, not a recorded number.
  EXPECT_NEAR(*value, 0.0, 1e-6) << "priced " << *value;
}

} // namespace
