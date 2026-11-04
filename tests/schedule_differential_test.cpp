// TIER 2 -- live differential tests for the schedule generator and the stub
// parser, against the 2009 C compiled into this same binary.
//
// The schedule is where small conventions compound: every fee payment date
// comes from here, so a single date off by one day moves an accrual and the
// price with it. These tests sweep the whole cross product of span, interval
// and stub rather than spot-checking, because the interesting cases are the
// ones nobody thinks to spot-check -- month-end clamping, long stubs with
// exactly two dates, spans shorter than one period.

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/schedule.hpp>
#include <cdsmodel/stub.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cxdatelist.h"
#include "dateconv.h"
#include "dtlist.h"
#include "stub.h"
}

namespace {

class ScheduleDifferential : public ::testing::Test {
protected:
  void SetUp() override { JpmcdsErrMsgOff(); }
};

std::vector<long> reference_schedule(long start, long end, int periods, char unit, bool back,
                                     bool long_stub) {
  TDateInterval iv{periods, unit, 0};
  TStubMethod stub;
  stub.stubAtEnd = back ? TRUE : FALSE;
  stub.longStub = long_stub ? TRUE : FALSE;
  TDateList *dl = JpmcdsDateListMakeRegular(start, end, &iv, &stub);
  std::vector<long> out;
  if (dl != nullptr) {
    for (int i = 0; i < dl->fNumItems; ++i) {
      out.push_back(dl->fArray[i]);
    }
    JpmcdsFreeDateList(dl);
  }
  return out;
}

std::vector<long> port_schedule(long start, long end, int periods, char unit, bool back,
                                bool long_stub) {
  const cdsmodel::DateInterval iv{periods, unit == 'M' ? cdsmodel::PeriodUnit::Months
                                                       : cdsmodel::PeriodUnit::Days};
  const cdsmodel::StubMethod stub{
      back ? cdsmodel::StubPosition::Back : cdsmodel::StubPosition::Front,
      long_stub ? cdsmodel::StubLength::Long : cdsmodel::StubLength::Short};
  const auto got = cdsmodel::make_regular_date_list(cdsmodel::from_tdate(start),
                                                    cdsmodel::from_tdate(end), iv, stub);
  std::vector<long> out;
  if (got) {
    for (cdsmodel::Date date : *got) {
      out.push_back(cdsmodel::to_tdate(date));
    }
  }
  return out;
}

struct Span {
  int y0, m0, d0, y1, m1, d1;
};

TEST_F(ScheduleDifferential, MakeRegularMatchesAcrossSpansIntervalsAndStubs) {
  // Month-end starts, leap-year ends, IMM-like dates, and spans both shorter
  // and longer than the interval.
  const std::vector<Span> spans{
      {2024, 1, 31, 2029, 1, 31},  {2024, 1, 31, 2026, 6, 30},  {2024, 2, 29, 2027, 2, 28},
      {2023, 3, 15, 2028, 3, 20},  {2024, 6, 20, 2024, 9, 20},  {2024, 6, 20, 2024, 7, 15},
      {2024, 6, 20, 2024, 6, 21},  {2024, 12, 31, 2030, 3, 20}, {2024, 5, 30, 2029, 11, 30},
      {2020, 2, 29, 2024, 2, 29},  {2024, 8, 15, 2025, 2, 10},  {2024, 1, 1, 2024, 3, 1},
      {2024, 3, 20, 2034, 3, 20},  {1999, 12, 31, 2001, 1, 31}, {2024, 4, 30, 2025, 4, 30},
      {2024, 11, 29, 2026, 2, 28},
  };
  struct Ivl {
    int periods;
    char unit;
  };
  const std::vector<Ivl> intervals{{1, 'M'},  {2, 'M'}, {3, 'M'}, {4, 'M'},  {5, 'M'},  {6, 'M'},
                                   {12, 'M'}, {1, 'D'}, {7, 'D'}, {30, 'D'}, {91, 'D'}, {182, 'D'}};

  int compared = 0;
  for (const Span &s : spans) {
    const long start = JpmcdsDate(s.y0, s.m0, s.d0);
    const long end = JpmcdsDate(s.y1, s.m1, s.d1);
    for (const Ivl &iv : intervals) {
      for (bool back : {false, true}) {
        for (bool long_stub : {false, true}) {
          const std::string label = std::to_string(start) + ".." + std::to_string(end) + " " +
                                    std::to_string(iv.periods) + iv.unit +
                                    (back ? " back" : " front") + (long_stub ? " long" : " short");
          const std::vector<long> expected =
              reference_schedule(start, end, iv.periods, iv.unit, back, long_stub);
          const std::vector<long> got =
              port_schedule(start, end, iv.periods, iv.unit, back, long_stub);
          // Dates are integers: nothing less than exact equality will do.
          ASSERT_EQ(got, expected) << label;
          ++compared;
        }
      }
    }
  }
  EXPECT_EQ(compared, static_cast<int>(spans.size() * intervals.size() * 4));
  EXPECT_GT(compared, 700) << "sweep thinner than expected: " << compared;
}

TEST_F(ScheduleDifferential, RejectedInputsAreRejectedByBoth) {
  const long start = JpmcdsDate(2024, 1, 15);
  for (long end : {start, start - 1, start - 400}) {
    EXPECT_TRUE(reference_schedule(start, end, 3, 'M', false, false).empty()) << end;
    EXPECT_TRUE(port_schedule(start, end, 3, 'M', false, false).empty()) << end;
  }
  for (int periods : {0, -1, -3}) {
    EXPECT_TRUE(reference_schedule(start, start + 400, periods, 'M', false, false).empty())
        << periods;
    EXPECT_TRUE(port_schedule(start, start + 400, periods, 'M', false, false).empty()) << periods;
  }
}

TEST_F(ScheduleDifferential, StubParserMatchesForWellFormedLengthFields) {
  // Every input here has LENGTH either absent or starting with s/S/l/L.
  // A malformed LENGTH reaches UPSTREAM-9 -- see the DISABLED test below.
  for (const char *name :
       {"f/s",         "f/l",       "F/S",       "F/L",     "b/s",   "b/l",   "B/S",
        "B/L",         "f",         "b",         "F",       "B",     "front", "back",
        "front/short", "back/long", "f/sausage", "b/lemon", "f/s/l", "b/l/x", "fb",
        "bf",          "x",         "x/s",       "",        "/",     "/s",    "1"}) {
    TStubMethod reference{};
    const bool ref_ok = JpmcdsStringToStubMethod(const_cast<char *>(name), &reference) == SUCCESS;
    const auto got = cdsmodel::parse_stub_method(name);
    ASSERT_EQ(got.has_value(), ref_ok) << "input \"" << name << "\"";
    if (!ref_ok) {
      continue;
    }
    EXPECT_EQ(got->position == cdsmodel::StubPosition::Back, reference.stubAtEnd != FALSE)
        << "input \"" << name << "\"";
    EXPECT_EQ(got->length == cdsmodel::StubLength::Long, reference.longStub != FALSE)
        << "input \"" << name << "\"";
  }
}

// DISABLED deliberately: this test executes UPSTREAM-9, an out-of-bounds read
// in upstream's JpmcdsStringToStubMethod. It is NOT disabled because it
// crashes here -- it does not. The read takes whatever occupies an unpushed
// varargs slot, so whether it faults depends on the caller's register and
// stack state: it faults in a minimal standalone frame and does not fault in
// this binary or in defect_evidence. It stays disabled because running known
// undefined behaviour in a CI suite means a future libc or sanitizer can
// start failing the build for a reason nobody will connect to this test.
//
// The reportable evidence is the source and the threshold measurement in
// tools/defect_evidence.cpp (sections u9 and u10), not a crash.
//
//   ./build/asan/cdsmodel_differential_tests \
//       --gtest_also_run_disabled_tests \
//       --gtest_filter='*Upstream9*'
TEST_F(ScheduleDifferential, DISABLED_Upstream9_MalformedLengthFormatsWithAMissingArgument) {
  // invalidFormatMsg has two %s conversions. The LOCATION error branch passes
  // routine AND name; the LENGTH error branch passes routine only, so
  // vsprintf consumes an indeterminate varargs slot and dereferences it as a
  // string. Any input whose LENGTH field starts with something other than
  // s/S/l/L reaches it.
  JpmcdsErrMsgOn();
  TStubMethod reference{};
  (void)JpmcdsStringToStubMethod(const_cast<char *>("f/X"), &reference);

  // The port rejects the same input and formats nothing.
  EXPECT_FALSE(cdsmodel::parse_stub_method("f/X").has_value());
}

} // namespace
