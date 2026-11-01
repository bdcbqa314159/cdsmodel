// Emits tests/golden/day_count_string_cases.hpp -- upstream's answers for the
// day count convention parser and formatter, recorded so CI can verify the
// port without possessing the oracle.
//
// Licence note: computed data, exempt under section 4(b) of the upstream
// licence. The code that produced it is not committed.
//
//   cmake --build --preset debug --target gen_day_count_string_golden
//   ./build/debug/gen_day_count_string_golden > tests/golden/day_count_string_cases.hpp

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "yearfrac.h"
}

namespace {

/// Render a C++ string literal, escaping what must be escaped.
std::string quoted(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\v':
      out += "\\v";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      out += c;
    }
  }
  return out + "\"";
}

std::optional<long> reference_parse(const std::string &s) {
  long code = 0;
  std::string mutable_copy = s; // upstream takes char*
  if (JpmcdsStringToDayCountConv(mutable_copy.data(), &code) != SUCCESS) {
    return std::nullopt;
  }
  return code;
}

std::vector<std::string> corpus() {
  std::vector<std::string> v{"Actual/365",
                             "Actual/365F",
                             "Actual/360",
                             "30/360",
                             "30E/360",
                             "Effective",
                             "Default",
                             "ACTUAL/365",
                             "actual/365",
                             "aCtUaL/365f",
                             "effective",
                             "EFFECTIVE",
                             "default",
                             "NONE",
                             "none",
                             "None",
                             "Act/Act",
                             "ACT/ACT",
                             "A/A",
                             "A",
                             "ACT/360",
                             "ACT/365",
                             "ACT/365F",
                             "B30/360",
                             "B30E/360",
                             " ACT/360",
                             "   ACT/360",
                             "\tACT/360",
                             "ACT /360",
                             "ACT / 360",
                             "ACT/360 ",
                             " 30/360 ",
                             "",
                             " ",
                             "   ",
                             "\t\n",
                             "GARBAGE365",
                             "A360",
                             "A365",
                             "B360",
                             "30360",
                             "XA360X",
                             "FA365",
                             "EFFORT",
                             "BANANA",
                             "DEFAULTS",
                             "ADEFAULT",
                             "NONEX",
                             "XNONE",
                             "30/Actual",
                             "30/ACTUAL",
                             "B",
                             "E",
                             "F",
                             "N",
                             "D",
                             "3",
                             "0",
                             "6",
                             "/",
                             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA365",
                             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA365",
                             "                                 ACT/360",
                             "30/360AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                             "ACT-360",
                             "ACT_365",
                             "30 / 360",
                             "-1",
                             "0",
                             "8"};

  // Every 1-, 2- and 3-character string over the alphabet that drives
  // upstream's strstr chain. This is the part that pins the leniency.
  const std::string alphabet = "ABEFND306/";
  for (char a : alphabet) {
    v.push_back(std::string{a});
    for (char b : alphabet) {
      v.push_back(std::string{a, b});
      for (char c : alphabet) {
        v.push_back(std::string{a, b, c});
      }
    }
  }
  return v;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf(
      "// GENERATED FILE -- DO NOT EDIT.\n"
      "//\n"
      "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
      "// 1.8.3 day count string layer (yearfrac.c), so this port can be verified\n"
      "// where the upstream sources are not available (CI). Computed data, exempt\n"
      "// from the upstream licence's distribution terms under its section 4(b).\n"
      "//\n"
      "// Regenerate with a local upstream copy present:\n"
      "//   cmake --build --preset debug --target gen_day_count_string_golden\n"
      "//   ./build/debug/gen_day_count_string_golden > tests/golden/day_count_string_cases.hpp\n"
      "\n#pragma once\n\n#include <array>\n#include <string_view>\n\n"
      "namespace cdsmodel::golden {\n\n"
      "/// One string handed to the parser and what upstream made of it.\n"
      "struct ParseCase {\n  std::string_view input;\n  bool ok;\n  long code;\n};\n\n");

  std::vector<std::string> rows;
  for (const std::string &s : corpus()) {
    const std::optional<long> got = reference_parse(s);
    rows.push_back("    {" + quoted(s) + ", " + (got ? "true" : "false") + ", " +
                   std::to_string(got.value_or(0)) + "},");
  }
  std::printf("inline constexpr std::array<ParseCase, %zu> kParseCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("/// One convention code and the name upstream formats it as.\n"
              "struct FormatCase {\n  long code;\n  std::string_view text;\n};\n\n");
  rows.clear();
  for (long code = -6; code <= 14; ++code) {
    rows.push_back("    {" + std::to_string(code) + ", " + quoted(JpmcdsFormatDayCountConv(code)) +
                   "},");
  }
  std::printf("inline constexpr std::array<FormatCase, %zu> kFormatCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
