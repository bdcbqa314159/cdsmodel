// Emits tests/golden/business_days_cases.hpp -- upstream's business day
// counts, recorded so CI can verify the port without the oracle.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_business_days_golden
//   ./build/debug/gen_business_days_golden > tests/golden/business_days_cases.hpp

#include <cdsmodel/business_days.hpp>
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

const long kWeekendSets[] = {0x00, 0x60, 0x40, 0x30, 0x7E};

std::vector<cdsmodel::Date> holiday_set() {
  return {d(2023, 12, 25), d(2023, 12, 26), d(2024, 1, 1),   d(2024, 3, 29),
          d(2024, 4, 1),   d(2024, 5, 6),   d(2024, 5, 27),  d(2024, 7, 4),
          d(2024, 8, 26),  d(2024, 11, 28), d(2024, 12, 25), d(2024, 12, 26)};
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 business day counting (busday.c), so this port can be verified\n"
              "// where the upstream sources are not available (CI). Computed data,\n"
              "// exempt from the upstream licence's distribution terms under its\n"
              "// section 4(b).\n"
              "//\n"
              "// Both directions are recorded for every pair, because the forward and\n"
              "// backward lookup tables upstream uses are NOT negatives of each other.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_business_days_golden\n"
              "//   ./build/debug/gen_business_days_golden > tests/golden/business_days_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n\nnamespace cdsmodel::golden {\n\n");

  const std::vector<cdsmodel::Date> holidays = holiday_set();
  std::printf("/// The holiday set these cases were recorded against, as TDates.\n");
  std::printf("inline constexpr std::array<long, %zu> kBusDayHolidays{{", holidays.size());
  for (std::size_t i = 0; i < holidays.size(); ++i) {
    std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(holidays[i]));
  }
  std::printf("}};\n\n");

  std::printf("struct BusDayCase {\n  long weekends;\n  long from;\n  long to;\n"
              "  bool ok;\n  long days;\n};\n\n");

  std::vector<std::string> rows;
  std::vector<TDate> storage;
  const int offsets[] = {-200, -91, -35, -14, -8, -7, -6, -3, -1, 0,
                         1,    3,   6,   7,   8,  14, 35, 91, 200};
  for (long w : kWeekendSets) {
    THolidayList *hl = build(holidays, w, storage);
    if (hl == nullptr) {
      std::fprintf(stderr, "upstream rejected weekends=0x%lx\n", w);
      return 1;
    }
    for (int anchor : {0, 17, 31, 60, 120, 200, 300}) {
      const cdsmodel::Date a = d(2023, 12, 1) + std::chrono::days{anchor};
      for (int offset : offsets) {
        const cdsmodel::Date b = a + std::chrono::days{offset};
        long days = 0;
        const bool ok = JpmcdsHolidayListBusinessDaysDiff(
                            cdsmodel::to_tdate(a), cdsmodel::to_tdate(b), hl, &days) == SUCCESS;
        char buf[160];
        std::snprintf(buf, sizeof buf, "    {0x%02lx, %ld, %ld, %s, %ld},", w,
                      cdsmodel::to_tdate(a), cdsmodel::to_tdate(b), ok ? "true" : "false",
                      ok ? days : 0L);
        rows.emplace_back(buf);
      }
    }
    JpmcdsHolidayListDelete(hl);
  }
  std::printf("inline constexpr std::array<BusDayCase, %zu> kBusDayCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
