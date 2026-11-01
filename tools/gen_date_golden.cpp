// Emits tests/golden/date_cases.hpp -- the upstream date layer's answers,
// recorded so CI can verify the port without possessing the oracle.
//
// Licence note: these are "data derived from calculations that have been
// attained by using the ISDA CDS Standard Model", exempt under section 4(b).
// The code that produced them is not committed.
//
//   cmake --build --preset debug --target gen_date_golden
//   ./build/debug/gen_date_golden > tests/golden/date_cases.hpp

#include <cdsmodel/date.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "dateconv.h"
#include "mdydate.h"
}

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 date layer (dateconv.c), so this port can be verified where the\n"
              "// upstream sources are not available (CI). Computed data, exempt from the\n"
              "// upstream licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_date_golden\n"
              "//   ./build/debug/gen_date_golden > tests/golden/date_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n\nnamespace cdsmodel::golden {\n\n");

  // ---- TDate -> y/m/d, weekday, end-of-month ----------------------------
  std::printf("/// One TDate and everything upstream reports about it.\n"
              "struct DateCase {\n"
              "  long tdate;\n  int year;\n  unsigned month;\n  unsigned day;\n"
              "  long weekday;\n  long eom;\n};\n\n");

  std::vector<std::string> rows;
  for (long t = 0; t < 219000; t += 397) {
    TMonthDayYear mdy{};
    long dow = -1;
    TDate eom = -1;
    if (JpmcdsDateToMDY(t, &mdy) != SUCCESS) {
      continue;
    }
    if (JpmcdsDayOfWeek(t, &dow) != SUCCESS) {
      continue;
    }
    if (JpmcdsDateToEOM(t, &eom) != SUCCESS) {
      continue;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "    {%ld, %ld, %lu, %lu, %ld, %ld},", t, mdy.year,
                  static_cast<unsigned long>(mdy.month), static_cast<unsigned long>(mdy.day), dow,
                  static_cast<long>(eom));
    rows.emplace_back(buf);
  }
  std::printf("inline constexpr std::array<DateCase, %zu> kDateCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- y/m/d -> TDate, including every rejection -------------------------
  std::printf("/// One construction attempt and whether upstream accepted it.\n"
              "struct MakeCase {\n"
              "  int year;\n  int month;\n  int day;\n  bool ok;\n  long tdate;\n};\n\n");

  rows.clear();
  // month 13 is omitted deliberately: it reaches an out-of-bounds read in
  // upstream's JpmcdsMDYToDate (UPSTREAM-1 in docs/port-notes.md), so there
  // is no defined answer to record.
  for (int y : {1600, 1601, 1900, 2000, 2024, 2100}) {
    for (int m : {0, 1, 2, 3, 4, 12}) {
      for (int d : {0, 1, 28, 29, 30, 31, 32}) {
        const TDate got = JpmcdsDate(y, m, d);
        const bool ok = got != -1;
        char buf[160];
        std::snprintf(buf, sizeof buf, "    {%d, %d, %d, %s, %ld},", y, m, d, ok ? "true" : "false",
                      ok ? static_cast<long>(got) : -1L);
        rows.emplace_back(buf);
      }
    }
  }
  std::printf("inline constexpr std::array<MakeCase, %zu> kMakeCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- normalisation: month carry plus day clamp -------------------------
  std::printf("/// One normalisation attempt. Upstream clamps an overlong day but\n"
              "/// rejects a day outside 1-31 entirely -- deliberately asymmetric.\n"
              "struct NormalizeCase {\n"
              "  int year;\n  int month;\n  int day;\n  bool ok;\n"
              "  int out_year;\n  unsigned out_month;\n  unsigned out_day;\n};\n\n");

  rows.clear();
  for (int y : {1601, 2000, 2024}) {
    for (int m : {-14, -1, 0, 1, 12, 13, 26}) {
      for (int d : {0, 1, 29, 31, 32}) {
        TMonthDayYear mdy{m, d, y};
        const bool ok = JpmcdsNormalizeMDY(&mdy) == SUCCESS;
        char buf[200];
        std::snprintf(buf, sizeof buf, "    {%d, %d, %d, %s, %ld, %lu, %lu},", y, m, d,
                      ok ? "true" : "false", ok ? mdy.year : 0L,
                      ok ? static_cast<unsigned long>(mdy.month) : 0UL,
                      ok ? static_cast<unsigned long>(mdy.day) : 0UL);
        rows.emplace_back(buf);
      }
    }
  }
  std::printf("inline constexpr std::array<NormalizeCase, %zu> kNormalizeCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
