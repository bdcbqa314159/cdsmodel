// Emits tests/golden/date_interval_cases.hpp -- upstream's answers for the
// date interval layer, recorded so CI can verify the port without the oracle.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_date_interval_golden
//   ./build/debug/gen_date_interval_golden > tests/golden/date_interval_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "convert.h"
#include "ldate.h"
}

namespace {

std::string quoted(const std::string &s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out + "\"";
}

/// 0 = days, 1 = months, -1 = not a canonical interval
int unit_of(const TDateInterval &iv) {
  if (iv.prd_typ == 'D') {
    return 0;
  }
  if (iv.prd_typ == 'M') {
    return 1;
  }
  return -1;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 date interval layer (convert.c, ldate.c), so this port can be\n"
              "// verified where the upstream sources are not available (CI). Computed\n"
              "// data, exempt from the upstream licence's distribution terms under its\n"
              "// section 4(b).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_date_interval_golden\n"
              "//   ./build/debug/gen_date_interval_golden > tests/golden/date_interval_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <string_view>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// unit: 0 = days, 1 = months\n"
              "struct IntervalMakeCase {\n  int periods;\n  char type;\n  bool ok;\n"
              "  int out_periods;\n  int out_unit;\n};\n\n");

  std::vector<std::string> rows;
  const std::string all_types = "AaYySsQqMmWwUuDdFEBGHIJKLTVXZ0/ ";
  for (char t : all_types) {
    for (int n : {-24, -3, -1, 0, 1, 2, 3, 6, 7, 12, 13, 28, 100}) {
      TDateInterval iv{};
      const bool ok = JpmcdsMakeDateInterval(n, t, &iv) == SUCCESS;
      const int unit = ok ? unit_of(iv) : -1;
      char buf[128];
      std::snprintf(buf, sizeof buf, "    {%d, '%s%c', %s, %d, %d},", n,
                    (t == '\'' || t == '\\') ? "\\" : "", t, (ok && unit >= 0) ? "true" : "false",
                    (ok && unit >= 0) ? iv.prd : 0, unit);
      rows.emplace_back(buf);
    }
  }
  std::printf("inline constexpr std::array<IntervalMakeCase, %zu> kIntervalMakeCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- parsing ----------------------------------------------------------
  std::printf("struct IntervalParseCase {\n  std::string_view input;\n  bool ok;\n"
              "  int periods;\n  int unit;\n};\n\n");
  rows.clear();
  std::vector<std::string> inputs{
      "1Y",  "1y",  "3M",  "6m", "1S",  "2Q",  "7D",   "2W",   "1U",  "M",  "D",  "Y", "W", "-1Y",
      "-6M", "+3M", "0M",  "0D", "12M", "24M", "100D", "999M", "",    "1",  "12", "-", "+", "1X",
      "X",   "1 Y", " 1Y", "Y1", "18M", "36M", "5M",   "14D",  "21D", "1E", "1I", "1F"};
  for (const std::string &s : inputs) {
    std::string copy = s;
    TDateInterval iv{};
    const bool ok =
        JpmcdsStringToDateInterval(copy.data(), const_cast<char *>("g"), &iv) == SUCCESS;
    const int unit = ok ? unit_of(iv) : -1;
    rows.push_back("    {" + quoted(s) + ", " + ((ok && unit >= 0) ? "true" : "false") + ", " +
                   std::to_string((ok && unit >= 0) ? iv.prd : 0) + ", " + std::to_string(unit) +
                   "},");
  }
  std::printf("inline constexpr std::array<IntervalParseCase, %zu> kIntervalParseCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- advancing --------------------------------------------------------
  std::printf("struct IntervalAdvanceCase {\n  long start;\n  int periods;\n  int unit;\n"
              "  bool ok;\n  long result;\n};\n\n");
  rows.clear();
  std::vector<long> starts;
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
    starts.push_back(static_cast<long>(JpmcdsDate(ymd[0], ymd[1], ymd[2])));
  }
  starts.push_back(0); // the epoch day, which upstream refuses to advance
  for (long start : starts) {
    for (int n : {-120, -13, -12, -6, -1, 0, 1, 2, 3, 6, 11, 12, 13, 60, 120}) {
      for (int unit = 0; unit <= 1; ++unit) {
        TDateInterval iv{};
        iv.prd = n;
        iv.prd_typ = unit == 1 ? 'M' : 'D';
        iv.flag = 0;
        TDate out = 0;
        const bool ok = JpmcdsDtFwdAny(static_cast<TDate>(start), &iv, &out) == SUCCESS;
        char buf[160];
        std::snprintf(buf, sizeof buf, "    {%ld, %d, %d, %s, %ld},", start, n, unit,
                      ok ? "true" : "false", ok ? static_cast<long>(out) : 0L);
        rows.emplace_back(buf);
      }
    }
  }
  std::printf("inline constexpr std::array<IntervalAdvanceCase, %zu> kIntervalAdvanceCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- formatting -------------------------------------------------------
  std::printf("struct IntervalFormatCase {\n  int periods;\n  int unit;\n"
              "  std::string_view text;\n};\n\n");
  rows.clear();
  for (int n :
       {-24, -12, -7, -6, -3, -1, 0, 1, 2, 3, 5, 6, 7, 11, 12, 14, 18, 21, 24, 28, 30, 36, 100}) {
    for (int unit = 0; unit <= 1; ++unit) {
      TDateInterval iv{};
      iv.prd = n;
      iv.prd_typ = unit == 1 ? 'M' : 'D';
      iv.flag = 0;
      rows.push_back("    {" + std::to_string(n) + ", " + std::to_string(unit) + ", " +
                     quoted(JpmcdsFormatDateInterval(&iv)) + "},");
    }
  }
  std::printf("inline constexpr std::array<IntervalFormatCase, %zu> kIntervalFormatCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
