// TIER 2 -- live differential tests for the date interval layer.
//
// Several tests here assert the presence of upstream DEFECTS rather than
// agreement, because the port deliberately diverges from them. Each names
// the finding it pins, so if a future upstream release fixes one, the test
// fails and tells us the port's divergence is no longer needed.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "convert.h"
#include "ldate.h"
}

namespace {

class DateIntervalDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

/// Upstream's canonical form for an interval, as a port-side value.
cdsmodel::Expected<cdsmodel::DateInterval> as_ported(const TDateInterval &iv) {
  if (iv.prd_typ == 'M') {
    return cdsmodel::DateInterval{iv.prd, cdsmodel::PeriodUnit::Months};
  }
  if (iv.prd_typ == 'D') {
    return cdsmodel::DateInterval{iv.prd, cdsmodel::PeriodUnit::Days};
  }
  return cdsmodel::fail(cdsmodel::ErrorKind::bad_argument, "test fixture could not be built");
}

const char *kAdvanceable = "AaYySsQqMmWwUuDd";
const char *kConstructOnly = "FEBGHIJKLTV";

// --- construction ---------------------------------------------------------

TEST_F(DateIntervalDifferential, MakeMatchesForEveryAdvanceableType) {
  for (const char *t = kAdvanceable; *t; ++t) {
    for (int n : {-24, -3, -1, 0, 1, 2, 3, 6, 7, 12, 13, 28, 100}) {
      TDateInterval reference{};
      const bool reference_ok = JpmcdsMakeDateInterval(n, *t, &reference) == SUCCESS;
      ASSERT_TRUE(reference_ok) << "upstream rejected " << n << *t;

      const auto ported = cdsmodel::make_date_interval(n, *t);
      ASSERT_TRUE(ported.has_value()) << "port rejected " << n << *t;
      EXPECT_EQ(*ported, *as_ported(reference)) << "normalisation for " << n << *t;
      EXPECT_EQ(reference.flag, 0) << "flag is assumed vestigial but was set for " << n << *t;
    }
  }
}

TEST_F(DateIntervalDifferential, ConstructOnlyTypesAreAcceptedUpstreamButInert) {
  // Documents the narrowing: upstream BUILDS these and then cannot advance by
  // them, so the port declines to build them at all.
  const TDate start = JpmcdsDate(2008, 1, 3);
  for (const char *t = kConstructOnly; *t; ++t) {
    TDateInterval reference{};
    EXPECT_EQ(JpmcdsMakeDateInterval(1, *t, &reference), SUCCESS)
        << "upstream unexpectedly rejected type " << *t;
    TDate out = 0;
    EXPECT_NE(JpmcdsDtFwdAny(start, &reference, &out), SUCCESS)
        << "upstream unexpectedly ADVANCED by type " << *t
        << " -- the port's narrowing would now lose functionality";
    EXPECT_FALSE(cdsmodel::make_date_interval(1, *t).has_value()) << "type " << *t;
  }
}

TEST_F(DateIntervalDifferential, InvalidTypesRejectedByBoth) {
  for (char t : {'X', 'Z', '0', '9', '/', ' ', '\t'}) {
    TDateInterval reference{};
    EXPECT_NE(JpmcdsMakeDateInterval(1, t, &reference), SUCCESS) << "type '" << t << "'";
    EXPECT_FALSE(cdsmodel::make_date_interval(1, t).has_value()) << "type '" << t << "'";
  }
}

// --- parsing --------------------------------------------------------------

TEST_F(DateIntervalDifferential, ParseMatchesForWellFormedStrings) {
  std::vector<std::string> inputs{"1Y", "1y", "3M",  "6m",  "1S",   "2Q",  "7D",  "2W",
                                  "1U", "M",  "D",   "Y",   "W",    "-1Y", "-6M", "+3M",
                                  "0M", "0D", "12M", "24M", "100D", "999M"};
  for (const std::string &s : inputs) {
    std::string copy = s;
    TDateInterval reference{};
    const bool reference_ok =
        JpmcdsStringToDateInterval(copy.data(), const_cast<char *>("t"), &reference) == SUCCESS;
    const auto ported = cdsmodel::parse_date_interval(s);

    ASSERT_EQ(ported.has_value(), reference_ok) << "validity for \"" << s << "\"";
    if (reference_ok) {
      EXPECT_EQ(*ported, *as_ported(reference)) << "value for \"" << s << "\"";
    }
  }
}

TEST_F(DateIntervalDifferential, ParseRejectionsMatch) {
  for (std::string s : {"", "1", "12", "-", "+", "1X", "X", "1 Y", " 1Y", "Y1"}) {
    std::string copy = s;
    TDateInterval reference{};
    const bool reference_ok =
        JpmcdsStringToDateInterval(copy.data(), const_cast<char *>("t"), &reference) == SUCCESS;
    const auto ported = cdsmodel::parse_date_interval(s);
    EXPECT_EQ(ported.has_value(), reference_ok) << "validity for \"" << s << "\"";
    if (reference_ok && ported.has_value()) {
      EXPECT_EQ(*ported, *as_ported(reference)) << "value for \"" << s << "\"";
    }
  }
}

// --- advancing ------------------------------------------------------------

TEST_F(DateIntervalDifferential, AdvanceMatchesAcrossDatesAndIntervals) {
  std::vector<cdsmodel::Date> starts;
  for (auto ymd : std::initializer_list<std::array<int, 3>>{{1601, 1, 2},
                                                            {1700, 2, 28},
                                                            {1999, 1, 31},
                                                            {2000, 2, 29},
                                                            {2000, 1, 31},
                                                            {2008, 1, 3},
                                                            {2008, 3, 31},
                                                            {2012, 11, 30},
                                                            {2020, 2, 29},
                                                            {2023, 8, 31},
                                                            {2024, 12, 31},
                                                            {2099, 6, 15}}) {
    starts.push_back(*cdsmodel::make_date(ymd[0], ymd[1], ymd[2]));
  }

  for (cdsmodel::Date start : starts) {
    for (int n : {-120, -13, -12, -6, -1, 0, 1, 2, 3, 6, 11, 12, 13, 60, 120}) {
      for (cdsmodel::PeriodUnit unit : {cdsmodel::PeriodUnit::Days, cdsmodel::PeriodUnit::Months}) {
        const cdsmodel::DateInterval iv{n, unit};
        TDateInterval reference{};
        reference.prd = n;
        reference.prd_typ = unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D';
        reference.flag = 0;

        TDate out = 0;
        const bool reference_ok =
            JpmcdsDtFwdAny(cdsmodel::to_tdate(start), &reference, &out) == SUCCESS;
        const auto ported = cdsmodel::advance(start, iv);

        ASSERT_EQ(ported.has_value(), reference_ok)
            << "validity, start=" << cdsmodel::to_tdate(start) << " n=" << n
            << " unit=" << (unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D');
        if (reference_ok) {
          EXPECT_EQ(cdsmodel::to_tdate(*ported), out)
              << "value, start=" << cdsmodel::to_tdate(start) << " n=" << n
              << " unit=" << (unit == cdsmodel::PeriodUnit::Months ? 'M' : 'D');
        }
      }
    }
  }
}

TEST_F(DateIntervalDifferential, AdvanceRejectsTheEpochDayAsUpstreamDoes) {
  // Upstream requires startDate > 0, so TDate 0 (1601-01-01) cannot be
  // advanced at all -- an off-by-one in its own range check.
  TDateInterval reference{1, 'M', 0};
  TDate out = 0;
  EXPECT_NE(JpmcdsDtFwdAny(0, &reference, &out), SUCCESS);
  EXPECT_FALSE(cdsmodel::advance(cdsmodel::kDateEpoch,
                                 cdsmodel::DateInterval{1, cdsmodel::PeriodUnit::Months})
                   .has_value());
}

// --- upstream defects this port diverges from ----------------------------

TEST_F(DateIntervalDifferential, Upstream3_LowercaseYAdvancesQuarterlyNotAnnually) {
  // JpmcdsDtFwdAny tests `upperPrdTyp == 'A'` but the RAW `prd_typ == 'Y'`,
  // so a hand-built interval with lowercase 'y' falls through to the
  // quarterly branch. Upstream's own comment says this code path exists for
  // callers who build TDateIntervals by hand -- exactly who hits this.
  const TDate start = JpmcdsDate(2008, 1, 3);
  TDate upper = 0;
  TDate lower = 0;
  TDateInterval iv_upper{1, 'Y', 0};
  TDateInterval iv_lower{1, 'y', 0};
  ASSERT_EQ(JpmcdsDtFwdAny(start, &iv_upper, &upper), SUCCESS);
  ASSERT_EQ(JpmcdsDtFwdAny(start, &iv_lower, &lower), SUCCESS);

  TMonthDayYear mu{};
  TMonthDayYear ml{};
  JpmcdsDateToMDY(upper, &mu);
  JpmcdsDateToMDY(lower, &ml);
  EXPECT_EQ(mu.year, 2009) << "'Y' should advance 12 months";
  EXPECT_EQ(ml.year, 2008) << "'y' advances only 3 -- if this now says 2009, upstream fixed it";
  EXPECT_EQ(ml.month, 4) << "'y' lands in April, i.e. quarterly";

  // The port cannot express this: make_date_interval upper-cases first, so
  // both spellings normalise to 12 months.
  EXPECT_EQ(cdsmodel::make_date_interval(1, 'y'), cdsmodel::make_date_interval(1, 'Y'));
}

TEST_F(DateIntervalDifferential, Upstream5_DaysNeverFormatAsWeeks) {
  // FormatDateInterval's `case 'D':` lacks a break and falls into `default:`,
  // which overwrites the week conversion computed just above it.
  for (int days : {7, 14, 21, 28}) {
    TDateInterval iv{days, 'D', 0};
    EXPECT_EQ(std::string{JpmcdsFormatDateInterval(&iv)}, std::to_string(days) + "D")
        << days << " days formatted as weeks -- upstream fixed the missing break";
  }
  // Months, whose case DOES break, collapse as intended.
  TDateInterval year{12, 'M', 0};
  EXPECT_EQ(std::string{JpmcdsFormatDateInterval(&year)}, "1A");
}

// DISABLED deliberately: this test CRASHES under AddressSanitizer, because it
// triggers UPSTREAM-4 -- a stack buffer overflow in upstream's
// JpmcdsStringToDateInterval. It is kept as an executable reproducer rather
// than prose. Run it on purpose with:
//
//   ./build/asan/cdsmodel_differential_tests \
//       --gtest_also_run_disabled_tests \
//       --gtest_filter='*Upstream4*'
//
// Expected output: AddressSanitizer: stack-buffer-overflow, WRITE of size 1,
// in JpmcdsStringToDateInterval at convert.c:288, overflowing 'numberBuff'.
TEST_F(DateIntervalDifferential, DISABLED_Upstream4_LongDigitRunOverflowsStackBuffer) {
  // numberBuff is char[32]; the digit-copy loop has no bounds check.
  std::string overlong = std::string(40, '9') + "M";
  TDateInterval reference{};
  (void)JpmcdsStringToDateInterval(overlong.data(), const_cast<char *>("boom"), &reference);

  // The port rejects the same input instead of overflowing.
  EXPECT_FALSE(cdsmodel::parse_date_interval(overlong).has_value());
}

TEST_F(DateIntervalDifferential, FormatMatchesForCanonicalIntervals) {
  for (int n : {-24, -12, -7, -1, 0, 1, 2, 3, 5, 6, 7, 11, 12, 14, 18, 21, 24, 28, 36, 100}) {
    for (char typ : {'M', 'D'}) {
      TDateInterval reference{n, typ, 0};
      const std::string expected{JpmcdsFormatDateInterval(&reference)};
      const cdsmodel::DateInterval iv{n, typ == 'M' ? cdsmodel::PeriodUnit::Months
                                                    : cdsmodel::PeriodUnit::Days};
      EXPECT_EQ(cdsmodel::format_date_interval(iv), expected) << n << typ;
    }
  }
}

} // namespace
