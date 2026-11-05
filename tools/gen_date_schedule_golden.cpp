// Emits tests/golden/date_schedule_cases.hpp.
//
// Records the schedules upstream generates. These are pure date arithmetic --
// no transcendental function anywhere -- so the vectors carry no tolerance
// question and the replay compares them exactly.
//
// What the properties cannot pin down, and these can, is where the boundary
// dates land: which end the grid is counted from, which boundary is replaced
// and which is pushed off the list. That is the whole difference between
// `JpmcdsNewDateList` and `JpmcdsNewDateListExtended`, and it only shows at
// the ends.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_date_schedule_golden
//   ./build/debug/gen_date_schedule_golden > tests/golden/date_schedule_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "date_sup.h"
#include "datelist.h"
#include "dtlist.h"
#include "ldate.h"
}

namespace {

constexpr std::size_t kMaxDates = 48;

struct Span {
  int y1, m1, d1;
  int y2, m2, d2;
};

const Span kSpans[] = {
    {2024, 1, 15, 2027, 1, 15}, {2024, 1, 31, 2027, 1, 31},  {2024, 2, 29, 2027, 2, 28},
    {2024, 1, 15, 2024, 4, 15}, {2024, 1, 15, 2024, 2, 20},  {2024, 3, 1, 2029, 11, 30},
    {2024, 1, 15, 2024, 1, 15}, {2024, 6, 30, 2026, 12, 31}, {2024, 11, 30, 2031, 5, 31},
};

struct Iv {
  int periods;
  char unit;
};

const Iv kIntervals[] = {{1, 'M'},  {2, 'M'},  {3, 'M'},  {4, 'M'},  {6, 'M'},  {7, 'M'},
                         {12, 'M'}, {13, 'M'}, {30, 'D'}, {91, 'D'}, {182, 'D'}};

/// A list rendered as a brace-initialiser padded to kMaxDates.
std::string literal(const TDateList *dl) {
  std::string out = "{";
  for (std::size_t i = 0; i < kMaxDates; ++i) {
    out += i == 0 ? "" : ", ";
    const bool present = dl != nullptr && i < static_cast<std::size_t>(dl->fNumItems);
    out += present ? std::to_string(static_cast<long>(dl->fArray[i])) : "0";
  }
  return out + "}";
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded date schedules. Pure date arithmetic, so these are compared\n"
              "// EXACTLY -- there is no tolerance question anywhere in this file.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_date_schedule_golden\n"
              "//   ./build/debug/gen_date_schedule_golden > tests/golden/date_schedule_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One recorded schedule. `which` selects the generator: 0 = NewDateList,\n"
              "/// 1 = NewDateListExtended, 2 = NewPayDates, 3 = NewDateListExtendedRoll\n"
              "/// with `roll` as its roll date (0 meaning upstream's no-roll sentinel).\n"
              "struct DateScheduleCase {\n"
              "  int which;\n"
              "  TDate start;\n"
              "  TDate maturity;\n"
              "  TDate roll;\n"
              "  int periods;\n"
              "  char unit;\n"
              "  bool stub_at_end;\n"
              "  bool accepted;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "};\n\n",
              kMaxDates);

  std::vector<std::string> rows;
  const TDate kRolls[] = {0L, 0L}; // placeholder, filled per span below

  for (const Span &span : kSpans) {
    const TDate start = JpmcdsDate(span.y1, span.m1, span.d1);
    const TDate maturity = JpmcdsDate(span.y2, span.m2, span.d2);
    if (start <= 0 || maturity <= 0) {
      continue;
    }
    for (const Iv &iv : kIntervals) {
      TDateInterval interval;
      if (JpmcdsMakeDateInterval(iv.periods, iv.unit, &interval) != SUCCESS) {
        continue;
      }
      for (const int stub_at_end : {0, 1}) {
        const TBoolean stub = stub_at_end ? TRUE : FALSE;
        struct Variant {
          int which;
          TDate roll;
          TDateList *list;
        };
        // Roll dates: none, the start itself (what cashflow.c passes), and one
        // well before the start.
        const TDate roll_before = JpmcdsDate(span.y1 - 1, span.m1, 15);
        Variant variants[] = {
            {0, 0L, JpmcdsNewDateList(start, maturity, &interval, stub)},
            {1, 0L, JpmcdsNewDateListExtended(start, maturity, &interval, stub)},
            {2, 0L, JpmcdsNewPayDates(start, maturity, &interval, stub)},
            {3, 0L, JpmcdsNewDateListExtendedRoll(start, maturity, 0L, &interval, stub)},
            {3, start, JpmcdsNewDateListExtendedRoll(start, maturity, start, &interval, stub)},
            {3, roll_before,
             JpmcdsNewDateListExtendedRoll(start, maturity, roll_before, &interval, stub)},
        };
        for (const Variant &v : variants) {
          const bool accepted = v.list != nullptr;
          const std::size_t n =
              accepted ? static_cast<std::size_t>(v.list->fNumItems) : static_cast<std::size_t>(0);
          if (n > kMaxDates) {
            JpmcdsFreeDateList(v.list);
            continue; // would not fit the record; the 1M/5y spans are the long ones
          }
          char head[160];
          std::snprintf(head, sizeof head, "    {%d, %ld, %ld, %ld, %d, '%c', %s, %s, %zu, ",
                        v.which, (long)start, (long)maturity, (long)v.roll, iv.periods, iv.unit,
                        stub_at_end ? "true" : "false", accepted ? "true" : "false", n);
          rows.emplace_back(std::string(head) + literal(v.list) + "},");
          JpmcdsFreeDateList(v.list);
        }
      }
    }
  }
  (void)kRolls;

  std::printf("inline constexpr std::array<DateScheduleCase, %zu> kDateScheduleCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
