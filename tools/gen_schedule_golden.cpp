// Emits tests/golden/schedule_cases.hpp -- recorded reference vectors for
// JpmcdsDateListMakeRegular and JpmcdsStringToStubMethod.
//
// Licence note: these are data derived from running the upstream model,
// exempt under section 4(b). No upstream source is reproduced.
//
// The stub-parser sweep deliberately omits inputs with a malformed LENGTH
// field. Those reach UPSTREAM-9, where upstream formats a two-%s message with
// one argument and reads out of bounds; recording its answer would mean
// running undefined behaviour to produce a committed file. The behaviour is
// covered by a DISABLED reproducer in the differential suite instead.
//
//   cmake --build --preset debug --target gen_schedule_golden
//   ./build/debug/gen_schedule_golden > tests/golden/schedule_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/stub.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cxdatelist.h"
#include "dateconv.h"
#include "dtlist.h"
#include "stub.h"
}

namespace {

constexpr std::size_t kMaxDates = 64;

struct Span {
  int y0, m0, d0;
  int y1, m1, d1;
};

std::string render(const TDateList *dl) {
  std::string out = "{{";
  for (std::size_t i = 0; i < kMaxDates; ++i) {
    out += (i ? ", " : "");
    out += (dl != nullptr && i < static_cast<std::size_t>(dl->fNumItems))
               ? std::to_string(dl->fArray[i])
               : "0";
  }
  return out + "}}";
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded reference vectors for the schedule generator and the stub\n"
              "// parser. Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_schedule_golden\n"
              "//   ./build/debug/gen_schedule_golden > tests/golden/schedule_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kScheduleMax = 64;\n\n"
              "/// position: 0 = front stub, 1 = back stub. length: 0 = short, 1 = long.\n"
              "/// n == 0 means upstream returned NULL for these inputs.\n"
              "struct ScheduleCase {\n"
              "  long start;\n  long end;\n  int periods;\n  char unit;\n"
              "  int position;\n  int length;\n"
              "  std::size_t n;\n  std::array<long, kScheduleMax> dates;\n};\n\n"
              "/// ok == false means upstream returned FAILURE; position/length unused then.\n"
              "struct StubParseCase {\n"
              "  const char *name;\n  bool ok;\n  int position;\n  int length;\n};\n\n");

  // Month-end starts and ends are the interesting part: anchored month
  // arithmetic clamps, so a 31st anchor visits the 28th/29th/30th and comes
  // back. Short spans exercise the totalDates == 1 branch.
  const std::vector<Span> spans{
      {2024, 1, 31, 2029, 1, 31}, {2024, 1, 31, 2026, 6, 30},  {2024, 2, 29, 2027, 2, 28},
      {2023, 3, 15, 2028, 3, 20}, {2024, 6, 20, 2024, 9, 20},  {2024, 6, 20, 2024, 7, 15},
      {2024, 6, 20, 2024, 6, 21}, {2024, 12, 31, 2030, 3, 20}, {2024, 5, 30, 2029, 11, 30},
      {2020, 2, 29, 2024, 2, 29}, {2024, 8, 15, 2025, 2, 10},  {2024, 1, 1, 2024, 3, 1},
  };
  const std::vector<TDateInterval> intervals{
      {1, 'M', 0}, {3, 'M', 0},  {6, 'M', 0},  {12, 'M', 0},
      {5, 'M', 0}, {91, 'D', 0}, {30, 'D', 0}, {7, 'D', 0},
  };

  std::vector<std::string> rows;
  for (const Span &s : spans) {
    const long start = JpmcdsDate(s.y0, s.m0, s.d0);
    const long end = JpmcdsDate(s.y1, s.m1, s.d1);
    for (TDateInterval iv : intervals) {
      for (int position = 0; position <= 1; ++position) {
        for (int length = 0; length <= 1; ++length) {
          TStubMethod stub;
          stub.stubAtEnd = position == 1 ? TRUE : FALSE;
          stub.longStub = length == 1 ? TRUE : FALSE;
          TDateList *dl = JpmcdsDateListMakeRegular(start, end, &iv, &stub);
          if (dl != nullptr && static_cast<std::size_t>(dl->fNumItems) > kMaxDates) {
            JpmcdsFreeDateList(dl);
            continue; // keep the recorded file to a readable width
          }
          rows.push_back("    {" + std::to_string(start) + ", " + std::to_string(end) + ", " +
                         std::to_string(iv.prd) + ", '" + iv.prd_typ + "', " +
                         std::to_string(position) + ", " + std::to_string(length) + ", " +
                         std::to_string(dl == nullptr ? 0 : dl->fNumItems) + ", " + render(dl) +
                         "},");
          JpmcdsFreeDateList(dl);
        }
      }
    }
  }
  std::printf("inline constexpr std::array<ScheduleCase, %zu> kScheduleCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // LENGTH is either absent or well-formed in every one of these -- see the
  // note at the top of the file.
  const std::vector<const char *> names{
      "f/s",   "f/l",   "F/S", "F/L",   "b/s",  "b/l",  "B/S",  "B/L",         "f",
      "b",     "F",     "B",   "front", "back", "f/sh", "b/lo", "front/short", "back/long",
      "f/s/l", "b/l/x", "x",   "x/s",   "",     "/s",   "fb",   "bf",          "f/short/ignored",
  };
  std::printf("inline constexpr std::array<StubParseCase, %zu> kStubParseCases{{\n", names.size());
  for (const char *name : names) {
    TStubMethod stub{};
    const bool ok = JpmcdsStringToStubMethod(const_cast<char *>(name), &stub) == SUCCESS;
    std::printf("    {\"%s\", %s, %d, %d},\n", name, ok ? "true" : "false",
                ok && stub.stubAtEnd ? 1 : 0, ok && stub.longStub ? 1 : 0);
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
