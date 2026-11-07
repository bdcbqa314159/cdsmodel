// Emits tests/golden/date_from_string_cases.hpp.
//
// A parser's recorded domain is strings, so each case is the input text and
// what upstream did with it: accepted-with-a-date, or refused. Both outcomes
// are recorded, because a parser is as much defined by what it rejects.
//
// Two groups of input are deliberately NOT recorded:
//   - the UPSTREAM-39 inputs (an unparseable year, silently invented), where
//     the port refuses by design. Recording upstream's answer would freeze a
//     divergence into a file the port is checked against.
//   - the UPSTREAM-38 input, which overflows a stack buffer. Running the
//     generator over it would be running the overflow.
// Both are covered by the live differential tests instead, which is the only
// place a deliberate divergence can honestly live.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_date_from_string_golden
//   ./build/debug/gen_date_from_string_golden > tests/golden/date_from_string_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "ldate.h"
#include "mdydate.h"
}

namespace {

// Everything the live sweep covers, minus the two excluded groups above.
std::vector<std::string> inputs() {
  std::vector<std::string> out{
      "20240315",
      "03/15/2024",
      "Mar-15-2024",
      "15-Mar-2024",
      "03.15.2024",
      "03 15 2024",
      "03-15-2024",
      "3/15/2024",
      "03/15-2024",
      "03.15/2024",
      "jan-15-2024",
      "JAN-15-2024",
      "Jan-15-2024",
      "jAn-15-2024",
      "January-15-2024",
      "ja-15-2024",
      "j-15-2024",
      "janu-15-2024",
      "feb-29-2024",
      "feb-29-2023",
      "dec-31-2024",
      "xyz-15-2024",
      "Jan-Feb-2024",
      "1/1/59",
      "1/1/60",
      "1/1/99",
      "1/1/00",
      "1/1/01",
      "1/1/100",
      "1/1/150",
      "1/1/1599",
      "1/1/1600",
      "1/1/1601",
      "1/1/1602",
      "1-1-2024-GARBAGE",
      "1/1/2024/99/99",
      "1-1-2024-",
      "1-1",
      "1",
      "",
      "-",
      "---",
      "1--",
      "2024031",
      "202403150",
      "0240315",
      "00240315",
      "20240000",
      "20241315",
      "20240230",
      "20240229",
      "20230229",
      "20241131",
      "20241130",
      "00001015",
      "20240015",
      "20240300",
      "aa/15/2024",
      "03/aa/2024",
      "abcdefgh",
      " 03 15 2024 ",
      "03  15  2024",
      "13/15/2024",
      "03/32/2024",
      "00/15/2024",
      "03/00/2024",
      "-3/15/2024",
      "3/-15/2024",
      "16010101",
      "16001231",
      "1-1-1601",
      "1/1/-",
  };
  // Every day of several years through all four documented formats, which is
  // the part a shape list cannot cover: the arithmetic.
  static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  for (const long year : {2023, 2024, 2100, 2000, 1999}) {
    for (long month = 1; month <= 12; ++month) {
      for (long day = 1; day <= 31; ++day) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%04ld%02ld%02ld", year, month, day);
        out.emplace_back(buf);
        std::snprintf(buf, sizeof buf, "%02ld/%02ld/%04ld", month, day, year);
        out.emplace_back(buf);
        std::snprintf(buf, sizeof buf, "%s-%02ld-%04ld", kMonths[month - 1], day, year);
        out.emplace_back(buf);
        std::snprintf(buf, sizeof buf, "%02ld-%s-%04ld", day, kMonths[month - 1], year);
        out.emplace_back(buf);
      }
    }
  }
  return out;
}

void escape(const std::string &text) {
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      std::printf("\\%c", c);
    } else {
      std::printf("%c", c);
    }
  }
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT. Emitted by tools/gen_date_from_string_golden.cpp.\n");
  std::printf("//\n");
  std::printf("// Recorded behaviour of JpmcdsStringToDate: the input text, whether it\n");
  std::printf("// was accepted, and the resulting date. See the generator for the two\n");
  std::printf("// groups of input deliberately excluded (UPSTREAM-38 and UPSTREAM-39).\n");
  std::printf("//\n");
  std::printf("// Derived data under licence section 4(b); no upstream source here.\n\n");
  std::printf("#pragma once\n\n");
  std::printf("#include <cstdint>\n\n");
  std::printf("namespace cdsmodel::golden {\n\n");
  std::printf("struct DateFromStringCase {\n");
  std::printf("  const char *input;\n");
  std::printf("  bool accepted;\n");
  std::printf("  std::int32_t tdate; // meaningful only when accepted\n");
  std::printf("};\n\n");
  std::printf("inline constexpr DateFromStringCase kDateFromStringCases[] = {\n");

  int accepted = 0;
  int refused = 0;
  const std::vector<std::string> all = inputs();
  for (const std::string &input : all) {
    std::string copy = input; // upstream takes char* and calls strtok
    TDate date = 0;
    const bool ok = JpmcdsStringToDate(copy.data(), &date) == SUCCESS;
    if (ok) {
      ++accepted;
    } else {
      ++refused;
      date = 0;
    }
    std::printf("    {\"");
    escape(input);
    std::printf("\", %s, %ld},\n", ok ? "true" : "false", static_cast<long>(date));
  }

  std::printf("};\n\n");
  std::printf("// %d accepted, %d refused.\n", accepted, refused);
  std::printf("\n} // namespace cdsmodel::golden\n");

  std::fprintf(stderr, "%zu cases: %d accepted, %d refused\n", all.size(), accepted, refused);
  return 0;
}
