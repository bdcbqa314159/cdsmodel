// Emits tests/golden/calendar_cases.hpp -- upstream's holiday calendar
// answers, recorded so CI can verify the port without the oracle.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_calendar_golden
//   ./build/debug/gen_calendar_golden > tests/golden/calendar_cases.hpp

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

THolidayList *reference_calendar(const std::vector<cdsmodel::Date> &holidays, long weekends) {
  static std::vector<TDate> tdates; // outlives the call; upstream copies anyway
  tdates.clear();
  for (cdsmodel::Date h : holidays) {
    tdates.push_back(cdsmodel::to_tdate(h));
  }
  TDateList dl{};
  dl.fNumItems = static_cast<int>(tdates.size());
  dl.fArray = tdates.empty() ? nullptr : tdates.data();
  return JpmcdsHolidayListNewGeneral(tdates.empty() ? nullptr : &dl, weekends);
}

const long kWeekendSets[] = {0x00, 0x60, 0x40, 0x30, 0x01, 0x20};

std::vector<cdsmodel::Date> holiday_set() {
  return {d(2024, 1, 1),   d(2024, 1, 15),  d(2024, 2, 19), d(2024, 3, 29),  d(2024, 5, 27),
          d(2024, 6, 19),  d(2024, 7, 4),   d(2024, 9, 2),  d(2024, 11, 28), d(2024, 11, 29),
          d(2024, 12, 24), d(2024, 12, 25), d(2024, 12, 31)};
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 holiday calendar (buscache.c, busday.c), so this port can be\n"
              "// verified where the upstream sources are not available (CI). Computed\n"
              "// data, exempt from the upstream licence's distribution terms under its\n"
              "// section 4(b).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_calendar_golden\n"
              "//   ./build/debug/gen_calendar_golden > tests/golden/calendar_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n\nnamespace cdsmodel::golden {\n\n"
              "/// The holiday set these cases were recorded against, as TDates.\n");

  const std::vector<cdsmodel::Date> holidays = holiday_set();
  std::printf("inline constexpr std::array<long, %zu> kCalendarHolidays{{", holidays.size());
  for (std::size_t i = 0; i < holidays.size(); ++i) {
    std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(holidays[i]));
  }
  std::printf("}};\n\n");

  std::printf("/// The weekend flag values swept, in upstream's bit layout.\n");
  std::printf("inline constexpr std::array<long, %zu> kWeekendSets{{",
              sizeof kWeekendSets / sizeof kWeekendSets[0]);
  for (std::size_t i = 0; i < sizeof kWeekendSets / sizeof kWeekendSets[0]; ++i) {
    std::printf("%s0x%02lx", i ? ", " : "", kWeekendSets[i]);
  }
  std::printf("}};\n\n");

  // ---- per-day predicates -----------------------------------------------
  std::printf("/// One day under one weekend set, and what upstream reported.\n"
              "struct CalendarDayCase {\n  long weekends;\n  long tdate;\n"
              "  bool is_weekend;\n  bool is_holiday;\n  bool is_business_day;\n};\n\n");

  std::vector<std::string> rows;
  for (long w : kWeekendSets) {
    THolidayList *hl = reference_calendar(holidays, w);
    if (hl == nullptr) {
      std::fprintf(stderr, "upstream rejected the holiday set for weekends=0x%lx\n", w);
      return 1;
    }
    for (int i = 0; i < 400; ++i) {
      const cdsmodel::Date day = d(2023, 12, 1) + std::chrono::days{i};
      const TDate t = cdsmodel::to_tdate(day);
      TBoolean hol = FALSE;
      TBoolean bus = FALSE;
      JpmcdsHolidayListIsHoliday(t, hl, &hol);
      JpmcdsHolidayListIsBusinessDay(t, hl, &bus);
      const bool wknd = JPMCDS_IS_WEEKEND(t, hl->weekends) != 0;
      char buf[128];
      std::snprintf(buf, sizeof buf, "    {0x%02lx, %ld, %s, %s, %s},", w, (long)t,
                    wknd ? "true" : "false", hol != FALSE ? "true" : "false",
                    bus != FALSE ? "true" : "false");
      rows.emplace_back(buf);
    }
    JpmcdsHolidayListDelete(hl);
  }
  std::printf("inline constexpr std::array<CalendarDayCase, %zu> kCalendarDayCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ---- which holidays survive construction ------------------------------
  std::printf("/// After construction, which of kCalendarHolidays upstream kept\n"
              "/// (weekend dates are stripped by verifyHolidayList).\n"
              "struct CalendarStoredCase {\n  long weekends;\n  int count;\n"
              "  std::array<long, 16> kept;\n};\n\n");
  rows.clear();
  for (long w : kWeekendSets) {
    THolidayList *hl = reference_calendar(holidays, w);
    if (hl == nullptr) {
      return 1;
    }
    std::string row = "    {0x" + std::string(1, "0123456789abcdef"[(w >> 4) & 0xF]) +
                      std::string(1, "0123456789abcdef"[w & 0xF]) + ", " +
                      std::to_string(hl->dateList->fNumItems) + ", {{";
    for (int i = 0; i < hl->dateList->fNumItems; ++i) {
      row += (i ? ", " : "") + std::to_string((long)hl->dateList->fArray[i]);
    }
    row += "}}},";
    rows.push_back(row);
    JpmcdsHolidayListDelete(hl);
  }
  std::printf("inline constexpr std::array<CalendarStoredCase, %zu> kCalendarStoredCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
