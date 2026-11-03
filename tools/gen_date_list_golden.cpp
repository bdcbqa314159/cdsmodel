// Emits tests/golden/date_list_cases.hpp.
//
// This generator is shaped differently from the others. The date list module
// CORRECTS upstream (UPSTREAM-7), so recording upstream's answers would
// enshrine the defect. What is recorded instead is the DOCUMENTED INTENT --
// computed by the port -- together with a flag saying whether upstream agreed
// at generation time.
//
// That flag is the useful part. A future regeneration where upstream suddenly
// agrees everywhere means the defect was fixed upstream and the correction
// should be revisited. Where it disagrees, the recorded intent is what CI
// verifies, since there is no correct oracle to compare against.
//
// Licence note: the intended answers are this project's own; the agreement
// flags are data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_date_list_golden
//   ./build/debug/gen_date_list_golden > tests/golden/date_list_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/date_list.hpp>

#include <chrono>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cxdatelist.h"
#include "dtlist.h"
}

namespace {

constexpr std::size_t kMaxDates = 16;

TDateList *make_list(std::span<const cdsmodel::Date> dates) {
  TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(dates.size()));
  for (std::size_t i = 0; i < dates.size(); ++i) {
    dl->fArray[i] = cdsmodel::to_tdate(dates[i]);
  }
  return dl;
}

std::vector<cdsmodel::Date> from_list(TDateList *dl) {
  std::vector<cdsmodel::Date> out;
  if (dl == nullptr) {
    return out;
  }
  for (int i = 0; i < dl->fNumItems; ++i) {
    out.push_back(cdsmodel::from_tdate(dl->fArray[i]));
  }
  return out;
}

std::string render(std::span<const cdsmodel::Date> dates) {
  std::string out = "{{";
  for (std::size_t i = 0; i < kMaxDates; ++i) {
    out += (i ? ", " : "");
    out += i < dates.size() ? std::to_string(cdsmodel::to_tdate(dates[i])) : "0";
  }
  return out + "}}";
}

std::vector<cdsmodel::Date> offsets(std::initializer_list<int> days) {
  const cdsmodel::Date base = *cdsmodel::make_date(2024, 1, 1);
  std::vector<cdsmodel::Date> out;
  for (int d : days) {
    out.push_back(base + std::chrono::days{d});
  }
  return out;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Unlike the other recorded vector sets, these are the DOCUMENTED INTENT\n"
              "// rather than upstream's answers: the date list module corrects upstream\n"
              "// (UPSTREAM-7), so recording upstream would enshrine the defect.\n"
              "//\n"
              "// `upstream_agreed` records whether the upstream implementation matched\n"
              "// the intent at generation time. If a regeneration turns those all true,\n"
              "// upstream fixed the off-by-one and the correction should be revisited.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_date_list_golden\n"
              "//   ./build/debug/gen_date_list_golden > tests/golden/date_list_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kDateListMax = 16;\n\n"
              "/// side: 0 = keep before the cut, 1 = keep after.\n"
              "struct TruncateCase {\n"
              "  std::size_t n;\n  std::array<long, kDateListMax> dates;\n  long cut;\n"
              "  int side;\n  bool inclusive;\n"
              "  std::size_t out_n;\n  std::array<long, kDateListMax> out;\n"
              "  bool upstream_agreed;\n};\n\n");

  const std::vector<std::vector<cdsmodel::Date>> lists{
      offsets({0, 1, 2, 3, 4, 5, 6, 10, 20}),
      offsets({1, 2}),
      offsets({10, 30}),
      offsets({5}),
      offsets({9, 10}),
      offsets({0, 7, 14, 21}),
  };

  std::vector<std::string> rows;
  for (const auto &list : lists) {
    for (int cut_offset = -2; cut_offset <= 22; cut_offset += 1) {
      const cdsmodel::Date cut = *cdsmodel::make_date(2024, 1, 1) + std::chrono::days{cut_offset};
      for (int side = 0; side <= 1; ++side) {
        for (int inclusive = 0; inclusive <= 1; ++inclusive) {
          const auto intent = cdsmodel::truncate_date_list(
              list, cut, side == 0 ? cdsmodel::Keep::Before : cdsmodel::Keep::After,
              inclusive != 0);
          if (!intent) {
            continue;
          }
          TDateList *ref =
              JpmcdsDateListTruncate(make_list(list), cdsmodel::to_tdate(cut),
                                     inclusive ? TRUE : FALSE, side == 1 ? TRUE : FALSE, FALSE);
          const bool agreed = from_list(ref) == *intent;
          rows.push_back("    {" + std::to_string(list.size()) + ", " + render(list) + ", " +
                         std::to_string(cdsmodel::to_tdate(cut)) + ", " + std::to_string(side) +
                         ", " + (inclusive ? "true" : "false") + ", " +
                         std::to_string(intent->size()) + ", " + render(*intent) + ", " +
                         (agreed ? "true" : "false") + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<TruncateCase, %zu> kTruncateCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
