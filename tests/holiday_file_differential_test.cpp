// TIER 2 -- H1, H2, H4, H5, H6 of specs/04_holiday_calendars.md.
//
// The oracle is `JpmcdsHolidayListRead` over the files upstream ships, plus
// generated files for every directive and every malformed shape. Dates and
// the weekend rule are both compared -- a parser that reads the right dates
// and the wrong weekend rule gets every Saturday wrong.

#include <cdsmodel/business_days.hpp>
#include <cdsmodel/holiday_file.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "ldate.h"
THolidayList *JpmcdsHolidayListRead(char *fileName);
}

namespace {

/// Where upstream's own holiday files live. Skipped rather than failed if the
/// oracle tree is absent -- the same discipline as every other tier-2 test.
std::filesystem::path shipped(const char *name) {
#ifdef CDSMODEL_UPSTREAM_PATH
  return std::filesystem::path(CDSMODEL_UPSTREAM_PATH) / "examples" / "excel" / name;
#else
  (void)name;
  return {};
#endif
}

std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

/// A temp file holding `contents`, because the oracle only takes a path.
class TempFile {
public:
  explicit TempFile(std::string_view contents) {
    path_ = std::filesystem::temp_directory_path() /
            ("cdsmodel_hol_" + std::to_string(++counter_) + ".dat");
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

/// A TDate from a calendar date.
///
/// Computed, never hardcoded. An earlier revision of this file wrote Excel
/// serials (45366 for 2024-03-15) where TDates belong -- and a TDate counts
/// from 1601, so 45366 is a date in 1725 with a different weekday. The tests
/// "passed" against the wrong days until one of them disagreed.
TDate tdate_of(int y, int m, int d) { return cdsmodel::to_tdate(*cdsmodel::make_date(y, m, d)); }

/// Whether upstream calls `date` a business day under the file at `path`.
bool upstream_is_business_day(const std::string &path, TDate date, bool *loaded) {
  std::string mutable_path = path;
  THolidayList *hl = JpmcdsHolidayListRead(mutable_path.data());
  *loaded = hl != nullptr;
  if (hl == nullptr) {
    return false;
  }
  TBoolean is_business = FALSE;
  JpmcdsHolidayListIsBusinessDay(date, hl, &is_business);
  return is_business != FALSE;
}

class HolidayFileDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

// ----------------------------------------------------------------- H1

TEST_F(HolidayFileDifferential, H1ShippedFilesAgreeEveryDayForTwentyYears) {
  for (const char *name : {"NYC.dat", "TRG.dat"}) {
    const std::filesystem::path path = shipped(name);
    if (!std::filesystem::exists(path)) {
      GTEST_SKIP() << path << " is absent";
    }
    const cdsmodel::Expected<cdsmodel::Calendar> ours =
        cdsmodel::parse_holiday_file(read_file(path));
    ASSERT_TRUE(ours.has_value()) << name << ": " << ours.error().message;

    std::string mutable_path = path.string();
    THolidayList *theirs = JpmcdsHolidayListRead(mutable_path.data());
    ASSERT_NE(theirs, nullptr) << name << ": upstream refused its own file";

    // Every day from 1996 to 2016. The dates in these files run to 2049, but
    // twenty years of every single day is already 7,305 comparisons per file
    // and covers each weekday against each holiday rule.
    int compared = 0;
    int holidays_seen = 0;
    const long first = tdate_of(1996, 1, 1);
    for (long serial = first; serial < first + 7305; ++serial) {
      const cdsmodel::Date day = cdsmodel::from_tdate(static_cast<TDate>(serial));
      TBoolean their_answer = FALSE;
      JpmcdsHolidayListIsBusinessDay(static_cast<TDate>(serial), theirs, &their_answer);
      const bool our_answer = ours->is_business_day(day);
      ASSERT_EQ(our_answer, their_answer != FALSE) << name << " disagrees on TDate " << serial;
      ++compared;
      if (!our_answer) {
        ++holidays_seen;
      }
    }
    EXPECT_EQ(compared, 7305);
    // A positive control: a sweep where nothing is ever a holiday would agree
    // trivially and prove nothing about the dates being read at all.
    EXPECT_GT(holidays_seen, 2000) << name << ": weekends alone should exceed this";
  }
}

// ----------------------------------------------------------------- H2

TEST_F(HolidayFileDifferential, H2EveryDirectiveHasUpstreamsEffect) {
  struct Directive {
    const char *line;
    TDate weekday_tdate; // a date falling on the weekday the directive names
    const char *what;
  };
  // 2024-03-15 is a Friday, so the six days after it run Saturday to Thursday.
  const Directive directives[]{
      {"# SATURDAY_NOT_ALWAYS_HOLIDAY", tdate_of(2024, 3, 16), "Saturday"},
      {"# SUNDAY_NOT_ALWAYS_HOLIDAY", tdate_of(2024, 3, 17), "Sunday"},
      {"# MONDAY_ALWAYS_HOLIDAY", tdate_of(2024, 3, 18), "Monday"},
      {"# TUESDAY_ALWAYS_HOLIDAY", tdate_of(2024, 3, 19), "Tuesday"},
      {"# WEDNESDAY_ALWAYS_HOLIDAY", tdate_of(2024, 3, 20), "Wednesday"},
      {"# THURSDAY_ALWAYS_HOLIDAY", tdate_of(2024, 3, 21), "Thursday"},
      {"# FRIDAY_ALWAYS_HOLIDAY", tdate_of(2024, 3, 15), "Friday"},
  };

  for (const Directive &d : directives) {
    const std::string contents = std::string(d.line) + "\n19960101\n";
    const TempFile file(contents);

    bool loaded = false;
    const bool theirs = upstream_is_business_day(file.name(), d.weekday_tdate, &loaded);
    ASSERT_TRUE(loaded) << d.line << ": upstream refused the file";

    const cdsmodel::Expected<cdsmodel::Calendar> ours = cdsmodel::parse_holiday_file(contents);
    ASSERT_TRUE(ours.has_value()) << d.line << ": " << ours.error().message;
    const bool mine = ours->is_business_day(cdsmodel::from_tdate(d.weekday_tdate));

    EXPECT_EQ(mine, theirs) << d.line << " changed " << d.what << " differently";
  }
}

TEST_F(HolidayFileDifferential, H2DirectivesCombine) {
  // Both weekend days released at once: every day becomes a business day
  // except the listed holiday.
  const std::string contents =
      "# SATURDAY_NOT_ALWAYS_HOLIDAY\n# SUNDAY_NOT_ALWAYS_HOLIDAY\n20240318\n";
  const TempFile file(contents);
  const cdsmodel::Expected<cdsmodel::Calendar> ours = cdsmodel::parse_holiday_file(contents);
  ASSERT_TRUE(ours.has_value()) << ours.error().message;

  // Saturday, Sunday, and the Monday that is the listed holiday.
  for (const TDate serial : {tdate_of(2024, 3, 16), tdate_of(2024, 3, 17), tdate_of(2024, 3, 18)}) {
    bool loaded = false;
    const bool theirs = upstream_is_business_day(file.name(), static_cast<TDate>(serial), &loaded);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(ours->is_business_day(cdsmodel::from_tdate(static_cast<TDate>(serial))), theirs)
        << "TDate " << serial;
  }
}

// ----------------------------------------------------------------- H4

TEST_F(HolidayFileDifferential, H4OutOfOrderDatesAreRefusedByBothSides) {
  const std::string contents = "19960101\n19960704\n19960301\n";
  const TempFile file(contents);

  std::string path = file.name();
  EXPECT_EQ(JpmcdsHolidayListRead(path.data()), nullptr) << "upstream accepted out-of-order dates";

  const cdsmodel::Expected<cdsmodel::Calendar> ours = cdsmodel::parse_holiday_file(contents);
  ASSERT_FALSE(ours.has_value());
  // Ours additionally says WHICH line, which upstream's message does not.
  EXPECT_NE(ours.error().message.find("3"), std::string::npos) << ours.error().message;
}

TEST_F(HolidayFileDifferential, H4DuplicateDatesAreRefusedByBothSides) {
  // Upstream's check is `>=`, so an equal date is out of order too.
  const std::string contents = "19960101\n19960101\n";
  const TempFile file(contents);
  std::string path = file.name();
  EXPECT_EQ(JpmcdsHolidayListRead(path.data()), nullptr);
  EXPECT_FALSE(cdsmodel::parse_holiday_file(contents).has_value());
}

// ------------------------------------------------- H5 and UPSTREAM-41 (H3)

TEST_F(HolidayFileDifferential, H5MalformedDateLinesDivergeDeliberately) {
  struct Case {
    const char *contents;
    const char *why;
  };
  const Case cases[]{
      {"19960101\nabc\n", "text upstream silently treats as a comment"},
      {"19960101\n19960704x\n", "atol stops at the x and yields 1996-07-04"},
      {"19960101\n1996010\n", "seven digits -- a typo, not a date"},
      {"19960101\n19961301\n", "month 13"},
      {"19960101\n19960230\n", "30 February"},
  };
  for (const Case &c : cases) {
    const TempFile file(c.contents);
    std::string path = file.name();
    THolidayList *theirs = JpmcdsHolidayListRead(path.data());

    const cdsmodel::Expected<cdsmodel::Calendar> ours = cdsmodel::parse_holiday_file(c.contents);
    EXPECT_FALSE(ours.has_value()) << "we accepted a malformed line: " << c.why;

    // Where upstream ALSO refuses, there is no divergence to document.
    // Where it accepts, the refusal is deliberate -- D2.
    if (theirs != nullptr) {
      EXPECT_FALSE(ours.has_value())
          << "upstream accepted \"" << c.contents << "\" (" << c.why << ") and we must refuse it";
    }
  }
}

TEST_F(HolidayFileDifferential, H3UpstreamIgnoresALowerCaseDirectiveAndWeRefuseIt) {
  // UPSTREAM-41, from both sides. Upstream builds an upper-cased copy of each
  // comment line and then compares the ORIGINAL, so the directives are
  // case-sensitive and a lower-case one is silently dropped.
  const std::string lower = "# saturday_not_always_holiday\n19960101\n";
  const std::string upper = "# SATURDAY_NOT_ALWAYS_HOLIDAY\n19960101\n";
  const TempFile lower_file(lower);
  const TempFile upper_file(upper);

  const TDate kSaturday = tdate_of(2024, 3, 16);
  bool loaded = false;

  const bool upper_business = upstream_is_business_day(upper_file.name(), kSaturday, &loaded);
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(upper_business) << "upstream stopped honouring the upper-case directive";

  const bool lower_business = upstream_is_business_day(lower_file.name(), kSaturday, &loaded);
  ASSERT_TRUE(loaded) << "upstream refused the lower-case file; UPSTREAM-41 may be fixed";
  EXPECT_FALSE(lower_business)
      << "upstream now honours a lower-case directive; UPSTREAM-41 is fixed and "
         "this port's refusal should be revisited";

  // Ours: the upper-case form behaves identically, and the lower-case form is
  // refused rather than silently ignored.
  const cdsmodel::Expected<cdsmodel::Calendar> ours_upper = cdsmodel::parse_holiday_file(upper);
  ASSERT_TRUE(ours_upper.has_value()) << ours_upper.error().message;
  EXPECT_TRUE(ours_upper->is_business_day(cdsmodel::from_tdate(kSaturday)));

  const cdsmodel::Expected<cdsmodel::Calendar> ours_lower = cdsmodel::parse_holiday_file(lower);
  ASSERT_FALSE(ours_lower.has_value()) << "we silently ignored it, as upstream does";
  EXPECT_NE(ours_lower.error().message.find("saturday_not_always_holiday"), std::string::npos)
      << "the message should quote the line: " << ours_lower.error().message;
}

// ----------------------------------------------------------------- H6

TEST_F(HolidayFileDifferential, H6TwoReadsInOneProcessDoNotInterfere) {
  // Upstream's reader fed a process-wide cache, which is why loading it was a
  // declared non-port. There is no state here to interfere, and this is what
  // proves it.
  const std::string sat_released = "# SATURDAY_NOT_ALWAYS_HOLIDAY\n19960101\n";
  const std::string plain = "19960101\n";

  const cdsmodel::Expected<cdsmodel::Calendar> first = cdsmodel::parse_holiday_file(sat_released);
  const cdsmodel::Expected<cdsmodel::Calendar> second = cdsmodel::parse_holiday_file(plain);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  const TDate kSaturday = tdate_of(2024, 3, 16);
  EXPECT_TRUE(first->is_business_day(cdsmodel::from_tdate(kSaturday)));
  EXPECT_FALSE(second->is_business_day(cdsmodel::from_tdate(kSaturday)))
      << "the second read inherited the first's weekend rule";

  // And the first is unchanged by the second having happened.
  EXPECT_TRUE(first->is_business_day(cdsmodel::from_tdate(kSaturday)));
}

} // namespace
