// Emits tests/golden/bad_day_cases.hpp -- upstream's bad day adjustment
// answers, recorded so CI can verify the port without the oracle.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_bad_day_golden
//   ./build/debug/gen_bad_day_golden > tests/golden/bad_day_cases.hpp
//
// An all-weekend calendar (weekends = 0x7F) is deliberately absent: upstream
// never returns from an adjustment against one, so there is nothing to record.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/date.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "buscache.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
}

namespace {

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

THolidayList *build(const std::vector<cdsmodel::Date> &holidays, long weekends,
                    std::vector<TDate> &storage) {
  storage.clear();
  for (cdsmodel::Date h : holidays) {
    storage.push_back(cdsmodel::to_tdate(h));
  }
  TDateList dl{};
  dl.fNumItems = static_cast<int>(storage.size());
  dl.fArray = storage.empty() ? nullptr : storage.data();
  return JpmcdsHolidayListNewGeneral(storage.empty() ? nullptr : &dl, weekends);
}

const long kWeekendSets[] = {0x00, 0x60, 0x40, 0x7E};
const char kConvs[] = {'F', 'P', 'N', 'M'};

std::vector<cdsmodel::Date> holiday_set() {
  return {d(2023, 12, 25), d(2023, 12, 26), d(2024, 1, 1), d(2024, 3, 29), d(2024, 4, 1),
          d(2024, 5, 6),   d(2024, 5, 27),  d(2024, 7, 4), d(2024, 8, 26), d(2024, 11, 28),
          d(2024, 12, 25), d(2024, 12, 26), d(2025, 1, 1)};
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 bad day conventions (busday.c), so this port can be verified\n"
              "// where the upstream sources are not available (CI). Computed data,\n"
              "// exempt from the upstream licence's distribution terms under its\n"
              "// section 4(b).\n"
              "//\n"
              "// An all-weekend calendar is absent on purpose: upstream never returns\n"
              "// from an adjustment against one (UPSTREAM-6).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_bad_day_golden\n"
              "//   ./build/debug/gen_bad_day_golden > tests/golden/bad_day_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n\nnamespace cdsmodel::golden {\n\n");

  const std::vector<cdsmodel::Date> holidays = holiday_set();
  std::printf("/// The holiday set these cases were recorded against, as TDates.\n");
  std::printf("inline constexpr std::array<long, %zu> kBadDayHolidays{{", holidays.size());
  for (std::size_t i = 0; i < holidays.size(); ++i) {
    std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(holidays[i]));
  }
  std::printf("}};\n\n");

  std::printf("/// One (date, convention, weekend set) query and upstream's answer.\n"
              "struct BadDayCase {\n  long weekends;\n  char conv;\n  long tdate;\n"
              "  bool ok;\n  long adjusted;\n};\n\n");

  std::vector<std::string> rows;
  std::vector<TDate> storage;
  for (long w : kWeekendSets) {
    THolidayList *hl = build(holidays, w, storage);
    if (hl == nullptr) {
      std::fprintf(stderr, "upstream rejected weekends=0x%lx\n", w);
      return 1;
    }
    for (int i = 0; i < 400; i += 2) {
      const cdsmodel::Date day = d(2023, 12, 1) + std::chrono::days{i};
      const TDate t = cdsmodel::to_tdate(day);
      for (char conv : kConvs) {
        TDate out = 0;
        const bool ok =
            JpmcdsHolidayListBusinessDay(t, static_cast<long>(conv), hl, &out) == SUCCESS;
        char buf[128];
        std::snprintf(buf, sizeof buf, "    {0x%02lx, '%c', %ld, %s, %ld},", w, conv, (long)t,
                      ok ? "true" : "false", ok ? (long)out : 0L);
        rows.emplace_back(buf);
      }
    }
    JpmcdsHolidayListDelete(hl);
  }
  std::printf("inline constexpr std::array<BadDayCase, %zu> kBadDayCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
