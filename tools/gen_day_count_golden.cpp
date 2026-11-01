// Emits tests/golden/day_count_cases.hpp -- the upstream day count layer's
// answers, recorded so CI can verify the port without possessing the oracle.
//
// Licence note: computed data, exempt under section 4(b) of the upstream
// licence. The code that produced it is not committed.
//
//   cmake --build --preset debug --target gen_day_count_golden
//   ./build/debug/gen_day_count_golden > tests/golden/day_count_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <array>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "ldate.h"
}

namespace {

std::string dbl(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v); // round-trips an IEEE double
  std::string s{buf};
  if (s.find_first_of(".eEn") == std::string::npos) {
    s += ".0";
  }
  return s;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const long codes[] = {1, 2, 3, 4, 5, 8};
  std::vector<cdsmodel::Date> dates;
  for (auto ymd : std::initializer_list<std::array<int, 3>>{{1999, 1, 31},
                                                            {1999, 2, 28},
                                                            {1999, 3, 30},
                                                            {1999, 12, 31},
                                                            {2000, 1, 1},
                                                            {2000, 2, 29},
                                                            {2000, 5, 31},
                                                            {2001, 1, 1},
                                                            {2004, 2, 29},
                                                            {2008, 1, 3},
                                                            {2023, 8, 31},
                                                            {2024, 2, 29}}) {
    dates.push_back(*cdsmodel::make_date(ymd[0], ymd[1], ymd[2]));
  }

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 day count layer (ldate.c), so this port can be verified where\n"
              "// the upstream sources are not available (CI). Computed data, exempt\n"
              "// from the upstream licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_day_count_golden\n"
              "//   ./build/debug/gen_day_count_golden > tests/golden/day_count_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n\nnamespace cdsmodel::golden {\n\n"
              "/// One (from, to, convention) query and what upstream answered for both\n"
              "/// the day count fraction and the day count numerator.\n"
              "struct DayCountCase {\n"
              "  long from;\n  long to;\n  long dcc;\n"
              "  bool yf_ok;\n  double yf;\n"
              "  bool days_ok;\n  long days;\n};\n\n");

  std::vector<std::string> rows;
  for (cdsmodel::Date a : dates) {
    for (cdsmodel::Date b : dates) {
      for (long code : codes) {
        const long ta = cdsmodel::to_tdate(a);
        const long tb = cdsmodel::to_tdate(b);

        double yf = 0.0;
        const bool yf_ok = JpmcdsDayCountFraction(ta, tb, code, &yf) == SUCCESS;
        long days = 0;
        const bool days_ok = JpmcdsDaysDiff(ta, tb, code, &days) == SUCCESS;

        char buf[256];
        std::snprintf(buf, sizeof buf, "    {%ld, %ld, %ld, %s, %s, %s, %ld},", ta, tb, code,
                      yf_ok ? "true" : "false", yf_ok ? dbl(yf).c_str() : "0.0",
                      days_ok ? "true" : "false", days_ok ? days : 0L);
        rows.emplace_back(buf);
      }
    }
  }

  std::printf("inline constexpr std::array<DayCountCase, %zu> kDayCountCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
