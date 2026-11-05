// Emits tests/golden/bad_day_map_cases.hpp.
//
// Records which entry each lookup picks, for every map shape including the
// malformed ones. There is no arithmetic in this module, so the vectors are
// dates and nothing else, compared exactly -- and the only thing worth
// recording is the CHOICE: which of several matching entries wins, and what
// happens when none does.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_bad_day_map_golden
//   ./build/debug/gen_bad_day_map_golden > tests/golden/bad_day_map_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
}

namespace {

constexpr std::size_t kMaxEntries = 4;

struct Shape {
  const char *name;
  int n;
  int bad[kMaxEntries]; ///< offsets from the base date
  int good[kMaxEntries];
};

// Offsets from 2024-01-05, so the literals stay readable.
const Shape kShapes[] = {
    {"empty", 0, {0, 0, 0, 0}, {0, 0, 0, 0}},
    {"single", 1, {1, 0, 0, 0}, {3, 0, 0, 0}},
    {"two disjoint", 2, {1, 2, 0, 0}, {3, 4, 0, 0}},
    {"duplicate bad", 2, {1, 1, 0, 0}, {3, 4, 0, 0}},
    {"duplicate good", 2, {1, 2, 0, 0}, {3, 3, 0, 0}},
    {"chained", 2, {1, 2, 0, 0}, {2, 3, 0, 0}},
    {"chained backwards", 2, {2, 1, 0, 0}, {3, 2, 0, 0}},
    {"two-cycle", 2, {1, 2, 0, 0}, {2, 1, 0, 0}},
    {"self mapping", 1, {1, 0, 0, 0}, {1, 0, 0, 0}},
    {"three chained", 3, {1, 2, 3, 0}, {2, 3, 4, 0}},
};

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate base = JpmcdsDate(2024, 1, 5);

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded bad-day substitutions. No arithmetic in this module, so these\n"
              "// are compared EXACTLY -- what is recorded is which entry a lookup picks\n"
              "// when several match, and what it returns when none does. The malformed\n"
              "// shapes (duplicated columns, chains, cycles) are included on purpose:\n"
              "// upstream handles them deterministically, so the port must agree.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_bad_day_map_golden\n"
              "//   ./build/debug/gen_bad_day_map_golden > tests/golden/bad_day_map_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One recorded lookup. `direction` is 0 for bad-to-good and 1 for\n"
              "/// good-to-bad; `shape` names the map for diagnostics only.\n"
              "struct BadDayMapCase {\n"
              "  const char *shape;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> bad;\n"
              "  std::array<TDate, %zu> good;\n"
              "  int direction;\n"
              "  TDate probe;\n"
              "  TDate result;\n"
              "};\n\n",
              kMaxEntries, kMaxEntries);

  std::vector<std::string> rows;
  for (const Shape &shape : kShapes) {
    std::vector<TDate> bad;
    std::vector<TDate> good;
    for (int i = 0; i < shape.n; ++i) {
      bad.push_back(base + shape.bad[i]);
      good.push_back(base + shape.good[i]);
    }
    TBadDayList list{};
    list.count = shape.n;
    list.maxAllowed = shape.n;
    list.badDay = bad.empty() ? nullptr : bad.data();
    list.goodDay = good.empty() ? nullptr : good.data();

    std::string bad_literal = "{";
    std::string good_literal = "{";
    for (std::size_t i = 0; i < kMaxEntries; ++i) {
      const char *sep = i == 0 ? "" : ", ";
      bad_literal += sep;
      good_literal += sep;
      const bool present = i < bad.size();
      bad_literal += present ? std::to_string((long)bad[i]) : "0";
      good_literal += present ? std::to_string((long)good[i]) : "0";
    }
    bad_literal += "}";
    good_literal += "}";

    for (int offset = -1; offset <= 6; ++offset) {
      const TDate probe = base + offset;
      for (int direction = 0; direction <= 1; ++direction) {
        const TDate result = direction == 0 ? JpmcdsBad2GoodBadDayList(&list, probe)
                                            : JpmcdsGood2BadBadDayList(&list, probe);
        char row[320];
        std::snprintf(row, sizeof row, "    {\"%s\", %d, %s, %s, %d, %ld, %ld},", shape.name,
                      shape.n, bad_literal.c_str(), good_literal.c_str(), direction, (long)probe,
                      (long)result);
        rows.emplace_back(row);
      }
    }
  }

  std::printf("inline constexpr std::array<BadDayMapCase, %zu> kBadDayMapCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
