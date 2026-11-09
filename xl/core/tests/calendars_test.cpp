// H7 and H8 of specs/04_holiday_calendars.md -- calendars reaching a formula.
//
// All of this runs with no Excel and no SDK. The file reading lives in
// `xl/core` rather than `xl/interface` precisely so these tests exist: one
// layer up it would be Windows-only and verified by hand.

#include <cdsmodel/holiday_file.hpp>
#include <cdsmodel/xl/calendars.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using cdsmodel::xl::calendar_dates_grid;
using cdsmodel::xl::calendar_from_range;
using cdsmodel::xl::CalendarRegistry;
using cdsmodel::xl::CallContext;
using cdsmodel::xl::CurveRegistry;
using cdsmodel::xl::find_function;
using cdsmodel::xl::invoke;
using cdsmodel::xl::read_calendar_file;
using cdsmodel::xl::XlGrid;
using cdsmodel::xl::XlValue;

double serial(int y, int m, int d) {
  return static_cast<double>(cdsmodel::make_date(y, m, d)->time_since_epoch().count() + 25569);
}

XlValue column(std::initializer_list<XlValue> cells) {
  XlGrid g;
  g.rows = cells.size();
  g.columns = 1;
  g.cells.assign(cells.begin(), cells.end());
  return XlValue{g};
}

class TempFile {
public:
  explicit TempFile(std::string_view contents) {
    path_ = std::filesystem::temp_directory_path() /
            ("cdsmodel_cal_" + std::to_string(++counter_) + ".dat");
    std::ofstream out(path_, std::ios::binary);
    out << contents;
  }
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;
  [[nodiscard]] std::string name() const { return path_.string(); }

private:
  std::filesystem::path path_;
  static int counter_;
};
int TempFile::counter_ = 0;

/// A flat continuously-compounded curve, for the share-lifetime test below.
cdsmodel::ZeroCurve flat(double rate) {
  const cdsmodel::RatePoint pts[]{{*cdsmodel::make_date(2007, 1, 1), rate},
                                  {*cdsmodel::make_date(2030, 1, 1), rate}};
  return *cdsmodel::ZeroCurve::create(*cdsmodel::make_date(2006, 10, 9), pts,
                                      cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
}

struct Ctx {
  CurveRegistry curves;
  CalendarRegistry calendars;
  CallContext ctx{curves, calendars};

  XlValue call(std::string_view name, std::vector<XlValue> args) {
    const auto *spec = find_function(name);
    if (spec == nullptr) {
      return XlValue{std::string("no such function")};
    }
    return invoke(*spec, args, ctx);
  }
};

// --------------------------------------------------------------- the range form

TEST(Calendars, H7ARangeOfDatesBecomesACalendar) {
  Ctx c;
  const XlValue handle =
      c.call("CDS.Calendar", {column({XlValue{serial(2024, 1, 1)}, XlValue{serial(2024, 7, 4)}}),
                              XlValue{"STANDARD"}, XlValue{"TEST"}});
  const auto text = handle.as_string();
  ASSERT_TRUE(text.has_value()) << handle.describe();
  EXPECT_EQ(*text, "TEST#1") << "the handle should name the calendar and its generation";

  const auto resolved = c.calendars.resolve(*text);
  ASSERT_TRUE(resolved.has_value()) << resolved.error().message;
  EXPECT_FALSE((*resolved)->is_business_day(*cdsmodel::make_date(2024, 1, 1)));
  EXPECT_FALSE((*resolved)->is_business_day(*cdsmodel::make_date(2024, 7, 4)));
  EXPECT_TRUE((*resolved)->is_business_day(*cdsmodel::make_date(2024, 1, 2)));
}

TEST(Calendars, H7DatesMayBeInAnyOrderAndBlanksAreSkipped) {
  // A sheet is not a file. Upstream's increasing-order rule constrains the
  // FILE FORMAT; a user selecting a column should not have to sort it, and
  // selecting a whole column picks up the empty rows below.
  const XlValue unsorted =
      column({XlValue{serial(2024, 7, 4)}, XlValue{}, XlValue{serial(2024, 1, 1)}, XlValue{}});
  const auto cal = calendar_from_range(unsorted, XlValue{"STANDARD"});
  ASSERT_TRUE(cal.has_value()) << cal.error().message;
  EXPECT_FALSE(cal->is_business_day(*cdsmodel::make_date(2024, 1, 1)));
  EXPECT_FALSE(cal->is_business_day(*cdsmodel::make_date(2024, 7, 4)));
}

TEST(Calendars, H7ADuplicateInTheRangeIsRefused) {
  // Sorting is a convenience; a duplicate is the sheet disagreeing with
  // itself and stays an error.
  const XlValue twice = column({XlValue{serial(2024, 1, 1)}, XlValue{serial(2024, 1, 1)}});
  const auto cal = calendar_from_range(twice, XlValue{"STANDARD"});
  ASSERT_FALSE(cal.has_value());
  EXPECT_NE(cal.error().message.find("more than once"), std::string::npos) << cal.error().message;
}

TEST(Calendars, H7TheWeekendRuleAcceptsEverySpellingAndRefusesNonsense) {
  const XlValue dates = column({XlValue{serial(2024, 1, 1)}});
  // 2024-03-16 is a Saturday, 2024-03-18 a Monday.
  const cdsmodel::Date sat = *cdsmodel::make_date(2024, 3, 16);
  const cdsmodel::Date mon = *cdsmodel::make_date(2024, 3, 18);

  const auto standard = calendar_from_range(dates, XlValue{"STANDARD"});
  ASSERT_TRUE(standard.has_value());
  EXPECT_FALSE(standard->is_business_day(sat));

  const auto none = calendar_from_range(dates, XlValue{"NONE"});
  ASSERT_TRUE(none.has_value());
  EXPECT_TRUE(none->is_business_day(sat)) << "NONE means no weekend at all";

  const auto blank = calendar_from_range(dates, XlValue{});
  ASSERT_TRUE(blank.has_value());
  EXPECT_FALSE(blank->is_business_day(sat)) << "omitted should mean STANDARD";

  const auto listed = calendar_from_range(dates, XlValue{"Monday, Sunday"});
  ASSERT_TRUE(listed.has_value()) << listed.error().message;
  EXPECT_FALSE(listed->is_business_day(mon)) << "Monday was listed";
  EXPECT_TRUE(listed->is_business_day(sat)) << "Saturday was not listed";

  const auto bad = calendar_from_range(dates, XlValue{"Someday"});
  ASSERT_FALSE(bad.has_value());
  EXPECT_NE(bad.error().message.find("Someday"), std::string::npos) << bad.error().message;
}

TEST(Calendars, H7AnEmptyRangeIsRefusedButOneCellIsFine) {
  // All blanks: nothing to build from.
  EXPECT_FALSE(calendar_from_range(column({XlValue{}, XlValue{}}), XlValue{}).has_value());
  // A single cell is a 1x1 range, which `XlValue::as_grid` already models --
  // and a one-holiday calendar is perfectly legitimate, so this succeeds. An
  // earlier version of this test asserted it should fail, which was wrong
  // about the feature rather than about the code.
  const auto one = calendar_from_range(XlValue{serial(2024, 1, 1)}, XlValue{});
  ASSERT_TRUE(one.has_value()) << one.error().message;
  EXPECT_FALSE(one->is_business_day(*cdsmodel::make_date(2024, 1, 1)));
  // Text where dates belong is still refused.
  EXPECT_FALSE(calendar_from_range(XlValue{"not a date"}, XlValue{}).has_value());
}

// ---------------------------------------------------------------- the file form

TEST(Calendars, H7LoadHolidaysReadsAFileAndReturnsAHandle) {
  Ctx c;
  const TempFile file("# SATURDAY_NOT_ALWAYS_HOLIDAY\n20240101\n20240704\n");
  const XlValue handle = c.call("LoadHolidays", {XlValue{file.name()}, XlValue{"NYC"}});
  const auto text = handle.as_string();
  ASSERT_TRUE(text.has_value()) << handle.describe();
  EXPECT_EQ(*text, "NYC#1");

  const auto resolved = c.calendars.resolve(*text);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_FALSE((*resolved)->is_business_day(*cdsmodel::make_date(2024, 1, 1)));
  EXPECT_TRUE((*resolved)->is_business_day(*cdsmodel::make_date(2024, 3, 16)))
      << "the directive should have released Saturday";
}

TEST(Calendars, H7AMissingFileAndAMalformedFileSayDifferentThings) {
  // Two problems with different fixes. Upstream reports both as FAILURE.
  const auto missing = read_calendar_file("/nonexistent/nowhere.dat");
  ASSERT_FALSE(missing.has_value());
  EXPECT_NE(missing.error().message.find("no holiday file at"), std::string::npos)
      << missing.error().message;

  const TempFile bad("20240101\nnot-a-date\n");
  const auto malformed = read_calendar_file(bad.name());
  ASSERT_FALSE(malformed.has_value());
  EXPECT_NE(malformed.error().message.find("line 2"), std::string::npos)
      << malformed.error().message;
  // And the path, because a sheet may load several files.
  EXPECT_NE(malformed.error().message.find(".dat"), std::string::npos) << malformed.error().message;
}

// -------------------------------------------------------------- spilling back

TEST(Calendars, H7CalendarDatesRoundTripsThroughCells) {
  Ctx c;
  const TempFile file("20240101\n20240704\n20241225\n");
  const XlValue handle = c.call("LoadHolidays", {XlValue{file.name()}, XlValue{"X"}});
  ASSERT_TRUE(handle.as_string().has_value()) << handle.describe();

  const XlValue spilled = c.call("CDS.CalendarDates", {handle});
  const auto grid = spilled.as_grid();
  ASSERT_TRUE(grid.has_value()) << spilled.describe();
  ASSERT_EQ(grid->columns, 1u);
  ASSERT_EQ(grid->rows, 3u) << "three holidays, none on a weekend";

  // And the spilled serials rebuild the same calendar -- which is the
  // migration path off files, so it has to actually work.
  const auto rebuilt = calendar_from_range(spilled, XlValue{"STANDARD"});
  ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error().message;
  const auto original = c.calendars.resolve(*handle.as_string());
  ASSERT_TRUE(original.has_value());
  for (const int day : {1, 2, 3, 4, 5}) {
    const cdsmodel::Date d = *cdsmodel::make_date(2024, 7, day);
    EXPECT_EQ(rebuilt->is_business_day(d), (*original)->is_business_day(d)) << "2024-07-0" << day;
  }
}

TEST(Calendars, H7AWeekendOnlyCalendarSaysSoRatherThanSpillingNothing) {
  const cdsmodel::Calendar weekends_only = cdsmodel::Calendar::none();
  const XlValue spilled = calendar_dates_grid(weekends_only);
  const auto grid = spilled.as_grid();
  ASSERT_TRUE(grid.has_value());
  EXPECT_EQ(grid->rows, 1u);
  EXPECT_NE(grid->at(0, 0).describe().find("no listed holidays"), std::string::npos);
}

// ------------------------------------------------------------- stale handles

TEST(Calendars, AResolvedValueOutlivesARebuildOfItsName) {
  // What `shared_ptr<const T>` buys beyond speed. A cell can hold a resolved
  // curve while ANOTHER cell rebuilds that name -- Excel decides the order --
  // and the first cell must keep reading the object it resolved rather than
  // a freed one. Returning a reference into the map would have dangled here;
  // returning a copy was correct but copied the whole curve per cell.
  CurveRegistry curves;
  const std::string first = curves.store("USD", flat(0.04));
  const auto held = curves.resolve(first);
  ASSERT_TRUE(held.has_value());
  const double before = (*held)->zero_price(*cdsmodel::make_date(2010, 1, 1));

  // Overwrite the name twice, and clear the registry entirely.
  (void)curves.store("USD", flat(0.09));
  (void)curves.store("USD", flat(0.15));
  curves.clear();
  EXPECT_EQ(curves.size(), 0u);

  // The value resolved earlier is untouched by all of that.
  EXPECT_EQ((*held)->zero_price(*cdsmodel::make_date(2010, 1, 1)), before);
  EXPECT_NE(held->use_count(), 0);
}

TEST(Calendars, ConcurrentStoresAndResolvesDoNotRace) {
  // The registry is locked now rather than documented as "safe because no
  // function registers with $". Excel decides when and on which thread it
  // recalculates, and `$` is a one-character change away.
  //
  // Run under the asan preset, this is also a TSan-shaped check: without the
  // mutex, concurrent insert_or_assign on an unordered_map corrupts it.
  CalendarRegistry calendars;
  const auto weekends = cdsmodel::Calendar::none();
  constexpr int kRounds = 200;

  std::atomic<int> resolved{0};
  std::thread writer([&] {
    for (int i = 0; i < kRounds; ++i) {
      (void)calendars.store("NYC", weekends);
      (void)calendars.store("LON", weekends);
    }
  });
  std::thread reader([&] {
    for (int i = 0; i < kRounds; ++i) {
      if (calendars.resolve("NYC#1").has_value()) {
        ++resolved;
      }
      (void)calendars.size();
    }
  });
  writer.join();
  reader.join();

  // The count is not asserted -- it depends on interleaving, which is the
  // point. What is asserted is that the registry survived and still works.
  EXPECT_EQ(calendars.size(), 2u);
  EXPECT_TRUE(calendars.resolve("NYC#" + std::to_string(kRounds)).has_value());
  EXPECT_GE(resolved.load(), 0);
}

TEST(Calendars, H7AStaleCalendarHandleIsRefused) {
  Ctx c;
  const TempFile first("20240101\n");
  const TempFile second("20240101\n20240704\n");
  const XlValue h1 = c.call("LoadHolidays", {XlValue{first.name()}, XlValue{"NYC"}});
  const XlValue h2 = c.call("LoadHolidays", {XlValue{second.name()}, XlValue{"NYC"}});
  ASSERT_EQ(*h1.as_string(), "NYC#1");
  ASSERT_EQ(*h2.as_string(), "NYC#2");

  EXPECT_TRUE(c.calendars.resolve("NYC#2").has_value());
  const auto stale = c.calendars.resolve("NYC#1");
  ASSERT_FALSE(stale.has_value());
  EXPECT_NE(stale.error().message.find("stale"), std::string::npos) << stale.error().message;
  // The noun must be right -- the registry is templated and a copy-paste
  // would have said "curve".
  EXPECT_NE(c.calendars.resolve("EUR#1").error().message.find("calendar"), std::string::npos);
  EXPECT_NE(c.curves.resolve("EUR#1").error().message.find("curve"), std::string::npos);
}

// ------------------------------------------------------- the point of it all

TEST(Calendars, H7APriceCanNowUseARealCalendar) {
  Ctx c;
  const TempFile file("20111007\n"); // a Friday near the trade's maturity
  const XlValue handle = c.call("LoadHolidays", {XlValue{file.name()}, XlValue{"NYC"}});
  ASSERT_TRUE(handle.as_string().has_value()) << handle.describe();

  const cdsmodel::RatePoint pts[]{{*cdsmodel::make_date(2007, 1, 1), 0.04},
                                  {*cdsmodel::make_date(2030, 1, 1), 0.04}};
  const auto curve =
      cdsmodel::ZeroCurve::create(*cdsmodel::make_date(2006, 10, 9), pts,
                                  cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
  ASSERT_TRUE(curve.has_value());
  const std::string disc = c.curves.store("D", *curve);
  const std::string sprd = c.curves.store("S", *curve);

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
                                  XlValue{"F"},
                                  *handle.as_string(), // <- a real calendar
                                  XlValue{disc},
                                  XlValue{sprd},
                                  XlValue{0.4},
                                  XlValue{true}};

  const XlValue priced = c.call("CdsPrice", args);
  const auto value = priced.as_double();
  ASSERT_TRUE(value.has_value()) << "pricing with a named calendar failed: " << priced.describe();
  EXPECT_TRUE(std::isfinite(*value));

  // And a bad handle is refused, naming the field.
  std::vector<XlValue> bad = args;
  bad[11] = XlValue{"NOPE#1"};
  const XlValue refused = c.call("CdsPrice", bad);
  EXPECT_FALSE(refused.as_double().has_value());
  EXPECT_NE(refused.describe().find("Holidays"), std::string::npos) << refused.describe();
}

// ------------------------------------------- the calendar functions' refusals
//
// Added because the coverage gate caught these at 88.9% against a floor of 90.
// Every one is a cell a user can type wrong, which makes them the paths that
// matter most and were least tested -- the same lesson as artifact #03.

TEST(Calendars, EveryCalendarFunctionRefusesBadArgumentsByName) {
  Ctx c;
  struct Case {
    const char *function;
    std::vector<XlValue> args;
    const char *must_mention;
  };
  const XlValue dates = column({XlValue{serial(2024, 1, 1)}});
  const std::vector<Case> cases{
      // wrong arity
      {"LoadHolidays", {XlValue{"x"}}, "2 arguments"},
      {"CDS.Calendar", {dates, XlValue{"STANDARD"}}, "3 arguments"},
      {"CDS.CalendarDates", {}, "1 argument"},
      // a number where text belongs
      {"LoadHolidays", {XlValue{3.0}, XlValue{"N"}}, "Filename"},
      {"LoadHolidays", {XlValue{"/nope.dat"}, XlValue{3.0}}, "Name"},
      {"CDS.CalendarDates", {XlValue{3.0}}, "Calendar"},
      // an empty name, which would make the handle unreferenceable
      {"LoadHolidays", {XlValue{"/nope.dat"}, XlValue{""}}, "Name"},
      {"CDS.Calendar", {dates, XlValue{"STANDARD"}, XlValue{""}}, "Name"},
      // a file that does not exist, and an unknown handle
      {"LoadHolidays", {XlValue{"/nonexistent/x.dat"}, XlValue{"N"}}, "no holiday file"},
      {"CDS.CalendarDates", {XlValue{"NOPE#1"}}, "no calendar named"},
  };
  for (const Case &k : cases) {
    const XlValue got = c.call(k.function, k.args);
    const std::string text = got.describe();
    EXPECT_NE(text.find(k.must_mention), std::string::npos)
        << k.function << " should have mentioned \"" << k.must_mention << "\": " << text;
  }
}

TEST(Calendars, CalendarFromFileIsTheSameFunctionUnderAnotherName) {
  Ctx c;
  const TempFile file("20240101\n20240704\n");
  const XlValue viaLoad = c.call("LoadHolidays", {XlValue{file.name()}, XlValue{"A"}});
  const XlValue viaFile = c.call("CDS.CalendarFromFile", {XlValue{file.name()}, XlValue{"B"}});
  ASSERT_TRUE(viaLoad.as_string().has_value()) << viaLoad.describe();
  ASSERT_TRUE(viaFile.as_string().has_value()) << viaFile.describe();

  const auto a = c.calendars.resolve(*viaLoad.as_string());
  const auto b = c.calendars.resolve(*viaFile.as_string());
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  // Same file, so the same answer on every day of a month.
  for (int day = 1; day <= 31; ++day) {
    const cdsmodel::Date d = *cdsmodel::make_date(2024, 1, day);
    EXPECT_EQ((*a)->is_business_day(d), (*b)->is_business_day(d)) << "2024-01-" << day;
  }
}

TEST(Calendars, AFractionalSerialInTheRangeIsRefusedNotTruncated) {
  // Excel serials carry a time of day. Truncating silently would build a
  // calendar from dates the sheet does not show.
  const XlValue withTime = column({XlValue{serial(2024, 1, 1) + 0.25}});
  const auto cal = calendar_from_range(withTime, XlValue{});
  ASSERT_FALSE(cal.has_value());
  EXPECT_NE(cal.error().message.find("whole number of days"), std::string::npos)
      << cal.error().message;
}

TEST(Calendars, TheWeekendRuleToleratesSpacingAndAnEmptyEntry) {
  const XlValue dates = column({XlValue{serial(2024, 1, 1)}});
  // Spacing around names, and a trailing comma -- both things a human types.
  const auto spaced = calendar_from_range(dates, XlValue{" Monday ,  Sunday , "});
  ASSERT_TRUE(spaced.has_value()) << spaced.error().message;
  EXPECT_FALSE(spaced->is_business_day(*cdsmodel::make_date(2024, 3, 18))) << "Monday";
  EXPECT_FALSE(spaced->is_business_day(*cdsmodel::make_date(2024, 3, 17))) << "Sunday";
  EXPECT_TRUE(spaced->is_business_day(*cdsmodel::make_date(2024, 3, 16))) << "Saturday";

  // A number where the rule belongs.
  EXPECT_FALSE(calendar_from_range(dates, XlValue{3.0}).has_value());
}

// ----------------------------------------------------------------------- H8

TEST(Calendars, H8ATenThousandLineFileReadsQuickly) {
  // Upstream reads the file TWICE -- once to count, once to parse -- because
  // it sizes an array first. One read should win comfortably.
  std::string contents;
  contents.reserve(90000);
  cdsmodel::Date day = *cdsmodel::make_date(1990, 1, 1);
  for (int i = 0; i < 10000; ++i) {
    const auto ymd = cdsmodel::to_ymd(day);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d\n", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
    contents += buf;
    day += std::chrono::days{3}; // strictly increasing, as the format needs
  }

  const auto start = std::chrono::steady_clock::now();
  const auto parsed = cdsmodel::parse_holiday_file(contents);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(parsed.has_value()) << parsed.error().message;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  EXPECT_LT(ms, 50) << "10,000 lines took " << ms << " ms";
  // A positive control: a parse that silently read nothing would be instant.
  EXPECT_GT(parsed->holidays().size(), 6000u)
      << "every third day over 82 years, minus those falling on a weekend";
}

} // namespace
