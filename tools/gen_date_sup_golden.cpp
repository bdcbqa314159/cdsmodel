// Emits tests/golden/date_sup_golden.hpp -- recorded interval conversions,
// offsets and date counts.
//
// Intervals are built through JpmcdsMakeDateInterval, which is how callers
// build them and which normalises the exotic unit letters exactly as the
// port's make_date_interval does. Raw hand-built structs are deliberately NOT
// recorded: they reach UPSTREAM-12, and recording a wrong status would
// enshrine it.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_date_sup_golden
//   ./build/debug/gen_date_sup_golden > tests/golden/date_sup_cases.hpp

#include <cdsmodel/date.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "convert.h"
#include "date_sup.h"
#include "dateadj.h"
#include "dateconv.h"
#include "ldate.h"
}

namespace {

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::optional<TDateInterval> built(int periods, char letter) {
  TDateInterval iv{};
  if (JpmcdsMakeDateInterval(periods, letter, &iv) != SUCCESS) {
    return std::nullopt;
  }
  return iv;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded interval conversions, multiplied offsets and date counts.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Intervals are built through JpmcdsMakeDateInterval, as callers build\n"
              "// them. Hand-built raw structs are not recorded: they reach UPSTREAM-12\n"
              "// and recording a wrong status would enshrine it.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_date_sup_golden\n"
              "//   ./build/debug/gen_date_sup_golden > tests/golden/date_sup_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "struct IntervalYearsCase {\n  int periods;\n  char letter;\n"
              "  double years;\n  bool has_frequency;\n  double frequency;\n};\n\n"
              "struct OffsetCase {\n  long start;\n  int periods;\n  char letter;\n"
              "  int index;\n  bool solved;\n  long result;\n};\n\n"
              "struct CountCase {\n  long from;\n  long to;\n  int periods;\n  char letter;\n"
              "  bool solved;\n  int intervals;\n  long extra_days;\n};\n\n"
              "struct FwdAdjustCase {\n  long start;\n  int periods;\n  char letter;\n"
              "  char bad_day;\n  int basis;\n  bool solved;\n  long result;\n};\n\n");

  const char *letters = "AYSQMWDU";

  std::vector<std::string> years;
  for (const char *p = letters; *p; ++p) {
    for (int periods : {1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 24, 37, 100, 365, 1000}) {
      const std::optional<TDateInterval> iv = built(periods, *p);
      if (!iv)
        continue;
      TDateInterval raw = *iv;
      double y = 0.0;
      if (JpmcdsDateIntervalToYears(&raw, &y) != SUCCESS)
        continue;
      double f = 0.0;
      const bool has_f = JpmcdsDateIntervalToFreq(&raw, &f) == SUCCESS;
      years.push_back("    {" + std::to_string(periods) + ", '" + *p + "', " + num(y) + ", " +
                      (has_f ? "true" : "false") + ", " + num(has_f ? f : 0.0) + "},");
    }
  }
  std::printf("inline constexpr std::array<IntervalYearsCase, %zu> kIntervalYearsCases{{\n",
              years.size());
  for (const std::string &r : years)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n");

  const TDate starts[] = {JpmcdsDate(2024, 1, 31), JpmcdsDate(2024, 2, 29), JpmcdsDate(2024, 6, 15),
                          JpmcdsDate(1999, 12, 31), JpmcdsDate(2030, 3, 1)};
  std::vector<std::string> offsets;
  for (const char *p = letters; *p; ++p) {
    for (int periods : {1, 3, 6}) {
      const std::optional<TDateInterval> iv = built(periods, *p);
      if (!iv)
        continue;
      TDateInterval raw = *iv;
      for (TDate s : starts) {
        for (int index : {-40, -7, -1, 0, 1, 5, 40, 200}) {
          TDate out = 0;
          const bool ok = JpmcdsDateFromDateAndOffset(s, &raw, index, &out) == SUCCESS;
          offsets.push_back("    {" + std::to_string(s) + ", " + std::to_string(periods) + ", '" +
                            *p + "', " + std::to_string(index) + ", " + (ok ? "true" : "false") +
                            ", " + std::to_string(ok ? out : 0) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<OffsetCase, %zu> kOffsetCases{{\n", offsets.size());
  for (const std::string &r : offsets)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n");

  const TDate tos[] = {JpmcdsDate(2024, 1, 31), JpmcdsDate(2024, 7, 15), JpmcdsDate(2029, 6, 20),
                       JpmcdsDate(2019, 3, 1), JpmcdsDate(2044, 1, 1)};
  std::vector<std::string> counts;
  for (const char *p = letters; *p; ++p) {
    for (int periods : {1, 2, 3, 6, -1, -3}) {
      const std::optional<TDateInterval> iv = built(periods, *p);
      if (!iv)
        continue;
      TDateInterval raw = *iv;
      for (TDate f : {starts[0], starts[1], starts[2]}) {
        for (TDate t : tos) {
          int n = 0;
          int extra = 0;
          const bool ok = JpmcdsCountDates(f, t, &raw, &n, &extra) == SUCCESS;
          counts.push_back("    {" + std::to_string(f) + ", " + std::to_string(t) + ", " +
                           std::to_string(periods) + ", '" + *p + "', " + (ok ? "true" : "false") +
                           ", " + std::to_string(ok ? n : 0) + ", " +
                           std::to_string(ok ? extra : 0) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<CountCase, %zu> kCountCases{{\n", counts.size());
  for (const std::string &r : counts)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n");

  std::vector<std::string> adj;
  struct Basis {
    int upstream;
    char letter;
  };
  const Basis bases[] = {{JPMCDS_DATE_ADJ_TYPE_BUSINESS, 'D'},
                         {JPMCDS_DATE_ADJ_TYPE_BUSINESS, 'M'},
                         {JPMCDS_DATE_ADJ_TYPE_CALENDAR, 'M'},
                         {JPMCDS_DATE_ADJ_TYPE_CALENDAR, 'D'},
                         {JPMCDS_DATE_ADJ_TYPE_WEEKDAY, 'D'}};
  for (const Basis &b : bases) {
    for (int periods : {1, 2, 5, -2}) {
      for (char conv : {'N', 'F', 'P', 'M'}) {
        for (TDate s : starts) {
          const std::optional<TDateInterval> iv = built(periods, b.letter);
          if (!iv)
            continue;
          TDateAdjIntvl raw{};
          raw.interval = *iv;
          raw.isBusDays = b.upstream;
          raw.holidayFile = const_cast<char *>("NONE");
          raw.badDayConv = static_cast<long>(conv);
          TDate out = 0;
          const bool ok = JpmcdsDtFwdAdj(s, &raw, &out) == SUCCESS;
          adj.push_back("    {" + std::to_string(s) + ", " + std::to_string(periods) + ", '" +
                        b.letter + "', '" + conv + "', " + std::to_string(b.upstream) + ", " +
                        (ok ? "true" : "false") + ", " + std::to_string(ok ? out : 0) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<FwdAdjustCase, %zu> kFwdAdjustCases{{\n", adj.size());
  for (const std::string &r : adj)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
