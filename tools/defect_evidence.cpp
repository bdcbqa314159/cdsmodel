// Demonstrates each catalogued upstream defect against the live ISDA library
// and against the port, side by side, and prints a transcript.
//
// This is the evidence behind docs/defect-report.md. It is a tool rather than
// a test because its job is to produce a readable transcript, not to pass.
//
// Build and run (requires a local upstream copy):
//   cmake --build --preset debug --target defect_evidence
//   ./build/debug/defect_evidence
//
// UPSTREAM-1 and UPSTREAM-4 are undefined behaviour. Their memory-safety
// evidence comes from the sanitizer build:
//   cmake --build --preset asan --target defect_evidence
//   ./build/asan/defect_evidence --unsafe

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/version.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "dateconv.h"
#include "ldate.h"
#include "mdydate.h"
#include "yearfrac.h"
}

namespace {

void heading(const char *id, const char *title) {
  std::printf("\n%s\n%s -- %s\n%s\n", std::string(72, '=').c_str(), id, title,
              std::string(72, '=').c_str());
}

void u1(bool unsafe) {
  heading("UPSTREAM-1", "month validated after it indexes a 13-element table");
  std::printf("JpmcdsDate(year, 13, 29) for years outside the 1995-2119 cache:\n\n");
  std::printf("  %-10s %-16s %s\n", "year", "upstream", "port");
  for (int y : {1900, 1950, 2200}) {
    const auto port = cdsmodel::make_date(y, 13, 29);
    const char *upstream = "(not run: reads OOB)";
    if (unsafe) {
      upstream = JpmcdsDate(y, 13, 29) == -1 ? "FAILURE (-1)" : "accepted";
    }
    std::printf("  %-10d %-22s %s\n", y, upstream, port ? "accepted" : "rejected");
  }
  std::printf("\nBoth reject. The difference is HOW: upstream reads leapDays[13],\n"
              "one past the end of a 13-element array, before it checks the month.\n"
              "The port checks the month first, and std::chrono::month cannot hold\n"
              "13 in a way that reaches a table at all.\n"
              "Memory-safety evidence: run --unsafe under the asan preset.\n");
}

void u2() {
  heading("UPSTREAM-2", "JPMCDS_DEFAULT collides with FAILURE");
  std::printf("  JPMCDS_DEFAULT = %d\n  FAILURE        = %d\n  equal?         = %s\n\n",
              JPMCDS_DEFAULT, FAILURE, JPMCDS_DEFAULT == FAILURE ? "YES" : "no");

  long code = 0;
  char buf[] = "Default";
  const int rc = JpmcdsStringToDayCountConv(buf, &code);
  std::printf("Upstream parse of \"Default\": rc=%s, code=%ld\n",
              rc == SUCCESS ? "SUCCESS" : "FAILURE", code);
  std::printf("Upstream JpmcdsDate(1600,1,1) (an invalid date): %ld\n\n",
              (long)JpmcdsDate(1600, 1, 1));
  std::printf("Both are -1. A caller holding a long cannot tell \"the default\n"
              "convention\" from \"the call failed\".\n\n");

  const auto ported = cdsmodel::parse_day_count_code("Default");
  std::printf("Port parse of \"Default\": has_value=%s, value=%ld\n", ported ? "true" : "false",
              ported.value_or(0));
  std::printf("Port make_date(1600,1,1):  has_value=%s\n",
              cdsmodel::make_date(1600, 1, 1) ? "true" : "false");
  std::printf("\nSuccess and value are separate. -1 is an ordinary value, not a signal.\n");
}

void u3() {
  heading("UPSTREAM-3", "lowercase 'y' advances quarterly, not annually");
  const TDate start = JpmcdsDate(2008, 1, 3);
  std::printf("Start date 2008-01-03, one period, via JpmcdsDtFwdAny:\n\n");
  std::printf("  %-6s %-14s %-18s %s\n", "type", "upstream", "months advanced", "port");
  for (char t : {'Y', 'y', 'A', 'a', 'M', 'm', 'S', 's', 'Q', 'q'}) {
    TDateInterval iv;
    iv.prd = 1;
    iv.prd_typ = t;
    iv.flag = 0;
    TDate out = 0;
    char ref[24] = "FAILURE";
    int months = -1;
    if (JpmcdsDtFwdAny(start, &iv, &out) == SUCCESS) {
      TMonthDayYear m{};
      JpmcdsDateToMDY(out, &m);
      std::snprintf(ref, sizeof ref, "%04ld-%02ld-%02ld", m.year, m.month, m.day);
      months = (int)((m.year - 2008) * 12 + (m.month - 1));
    }
    const auto iv_port = cdsmodel::make_date_interval(1, t);
    const int port_months =
        iv_port && iv_port->unit == cdsmodel::PeriodUnit::Months ? iv_port->periods : -1;
    std::printf("  '%c'    %-14s %-18d %d months\n", t, ref, months, port_months);
  }
  std::printf("\n'Y' advances 12 months. 'y' advances 3. Every other period type in\n"
              "the library is case-insensitive; 'y' alone changes meaning with case.\n"
              "Cause: the branch tests toupper(prd_typ) for 'A' but the RAW prd_typ\n"
              "for 'Y', so 'y' matches neither and falls through to quarterly.\n\n");
  std::printf("Port: make_date_interval(1,'y') == make_date_interval(1,'Y') -> %s\n",
              cdsmodel::make_date_interval(1, 'y') == cdsmodel::make_date_interval(1, 'Y')
                  ? "true"
                  : "FALSE");
}

void u4(bool unsafe) {
  heading("UPSTREAM-4", "stack buffer overflow in JpmcdsStringToDateInterval");
  std::printf("numberBuff is char[32]; the digit-copy loop has no bounds check:\n"
              "    while (isdigit((int)*inp)) *nump++ = *inp++;   /* convert.c:288 */\n\n");
  std::printf("  %-14s %-22s %s\n", "digits", "upstream", "port");
  for (int n : {5, 10, 31, 32, 40, 64}) {
    const std::string s = std::string(n, '9') + "M";
    const auto port = cdsmodel::parse_date_interval(s);
    const char *upstream = "(not run: would overflow)";
    if (n <= 31 || unsafe) {
      std::string copy = s;
      TDateInterval iv{};
      const int rc = JpmcdsStringToDateInterval(copy.data(), const_cast<char *>("ev"), &iv);
      upstream = rc == SUCCESS ? "accepted" : "FAILURE";
    }
    std::printf("  %-14d %-22s %s\n", n, upstream, port ? "accepted" : "rejected");
  }
  std::printf("\nAt 32 digits and beyond upstream writes past the buffer. The port\n"
              "bounds the digit count and rejects. This is the ONLY defect whose\n"
              "behaviour the port deliberately does not reproduce.\n");
  if (!unsafe) {
    std::printf("\nRun with --unsafe under the asan preset for the overflow trace.\n");
  }
}

void u5() {
  heading("UPSTREAM-5", "days never format as weeks (missing break)");
  std::printf("  %-10s %-14s %-10s %s\n", "stored", "upstream", "port", "intended");
  for (auto [n, typ] : std::initializer_list<std::pair<int, char>>{
           {12, 'M'}, {18, 'M'}, {6, 'M'}, {7, 'D'}, {14, 'D'}, {28, 'D'}}) {
    TDateInterval iv;
    iv.prd = n;
    iv.prd_typ = typ;
    iv.flag = 0;
    const cdsmodel::DateInterval ported{n, typ == 'M' ? cdsmodel::PeriodUnit::Months
                                                      : cdsmodel::PeriodUnit::Days};
    const char *intended = "same";
    if (typ == 'D' && n % 7 == 0) {
      intended = (n == 7) ? "1W" : (n == 14 ? "2W" : "4W");
    }
    std::printf("  %-10s %-14s %-10s %s\n", (std::to_string(n) + typ).c_str(),
                JpmcdsFormatDateInterval(&iv), cdsmodel::format_date_interval(ported).c_str(),
                intended);
  }
  std::printf("\nMonths collapse (12M -> 1A) because that case has its break. Days\n"
              "never collapse: case 'D' falls through into default:, which\n"
              "overwrites the week form computed just above it.\n\n"
              "The port reproduces this. It is observable output, and abbreviating\n"
              "\"14D\" to \"2W\" would be a behaviour change dressed up as a fix.\n");
}

} // namespace

int main(int argc, char **argv) {
  JpmcdsErrMsgOff();
  bool unsafe = false;
  const char *only = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--unsafe") == 0) {
      unsafe = true;
    } else {
      only = argv[i]; // "u1" .. "u5": run just that section
    }
  }
  const auto want = [only](const char *id) {
    return only == nullptr || std::strcmp(only, id) == 0;
  };

  std::printf("Upstream defect evidence -- ISDA CDS Standard Model 1.8.3 vs cdsmodel %s\n",
              std::string(cdsmodel::version()).c_str());
  if (unsafe) {
    std::printf("\n*** --unsafe: this run deliberately triggers undefined behaviour ***\n");
  }

  // Under a sanitizer build, a section marked --unsafe aborts the process, so
  // pass an id to run just one: `defect_evidence u4 --unsafe`.
  if (want("u1")) {
    u1(unsafe);
  }
  if (want("u2")) {
    u2();
  }
  if (want("u3")) {
    u3();
  }
  if (want("u5")) {
    u5();
  }
  if (want("u4")) {
    u4(unsafe);
  }
  return 0;
}
