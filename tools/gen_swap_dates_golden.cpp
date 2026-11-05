// Emits tests/golden/swap_dates_cases.hpp.
//
// Records all four of upstream's parallel arrays per maturity -- adjusted,
// original, previous, onCycle -- because the failure this module is shaped to
// produce is one of the four disagreeing while the others match. Pure date
// arithmetic and a boolean, so these are compared exactly.
//
// The sweep varies the value date's day-of-month independently of the
// maturity's, which is what the on-cycle heuristic keys on and the only way
// the UPSTREAM-27 cases appear at all.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_swap_dates_golden
//   ./build/debug/gen_swap_dates_golden > tests/golden/swap_dates_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
#include "zcswdate.h"
}

namespace {

constexpr std::size_t kMaxDates = 6;

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded swap date sets: all four of upstream's parallel arrays per\n"
              "// maturity. Pure date arithmetic plus a boolean, compared EXACTLY.\n"
              "// `source` is 0 for JpmcdsSwapDatesNewFromAdjusted and 1 for\n"
              "// ...NewFromOriginal; `bad_day` is 0 when no substitution table was\n"
              "// supplied. Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_swap_dates_golden\n"
              "//   ./build/debug/gen_swap_dates_golden > tests/golden/swap_dates_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "struct SwapDatesCase {\n"
              "  int source;\n"
              "  TDate value_date;\n"
              "  long freq;\n"
              "  long bad_day;\n"
              "  bool accepted;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> input;\n"
              "  std::array<TDate, %zu> adjusted;\n"
              "  std::array<TDate, %zu> original;\n"
              "  std::array<TDate, %zu> previous;\n"
              "  std::array<bool, %zu> on_cycle;\n"
              "};\n\n",
              kMaxDates, kMaxDates, kMaxDates, kMaxDates, kMaxDates);

  std::vector<std::string> rows;
  const long freqs[] = {1, 2, 4, 12};
  const long convs[] = {'N', 'F', 'P', 'M'};

  for (int month : {1, 2, 6, 11}) {
    for (int value_day = 26; value_day <= 31; ++value_day) {
      const TDate value_date = JpmcdsDate(2024, month, value_day);
      if (value_date <= 0) {
        continue;
      }
      for (int maturity_day = 26; maturity_day <= 31; ++maturity_day) {
        std::vector<TDate> input;
        for (int years = 1; years <= 4; ++years) {
          const TDate maturity = JpmcdsDate(2024 + years, month, maturity_day);
          if (maturity > 0) {
            input.push_back(maturity);
          }
        }
        if (input.empty() || input.size() > kMaxDates) {
          continue;
        }
        for (const long freq : freqs) {
          for (int source = 0; source <= 1; ++source) {
            const long conv = source == 0 ? 0 : convs[(value_day + maturity_day) % 4];
            TSwapDates *sd =
                source == 0 ? JpmcdsSwapDatesNewFromAdjusted(value_date, (int)freq, input.data(),
                                                             (int)input.size(), nullptr)
                            : JpmcdsSwapDatesNewFromOriginal(value_date, (int)freq, input.data(),
                                                             (int)input.size(), nullptr, conv,
                                                             (char *)"NONE");
            const bool accepted = sd != nullptr;
            const std::size_t n = accepted ? (std::size_t)sd->numDates : (std::size_t)0;

            const auto dates = [&](const TDate *arr) {
              std::string out = "{";
              for (std::size_t i = 0; i < kMaxDates; ++i) {
                out += i == 0 ? "" : ", ";
                out += (accepted && i < n) ? std::to_string((long)arr[i]) : "0";
              }
              return out + "}";
            };
            std::string input_literal = "{";
            std::string cycle_literal = "{";
            for (std::size_t i = 0; i < kMaxDates; ++i) {
              const char *sep = i == 0 ? "" : ", ";
              input_literal += sep;
              cycle_literal += sep;
              input_literal += i < input.size() ? std::to_string((long)input[i]) : "0";
              cycle_literal += (accepted && i < n && sd->onCycle[i] == TRUE) ? "true" : "false";
            }
            input_literal += "}";
            cycle_literal += "}";

            char head[160];
            std::snprintf(head, sizeof head, "    {%d, %ld, %ld, %ld, %s, %zu, ", source,
                          (long)value_date, freq, conv, accepted ? "true" : "false", n);
            rows.emplace_back(std::string(head) + input_literal + ", " +
                              (accepted ? dates(sd->adjusted) : dates(nullptr)) + ", " +
                              (accepted ? dates(sd->original) : dates(nullptr)) + ", " +
                              (accepted ? dates(sd->previous) : dates(nullptr)) + ", " +
                              cycle_literal + "},");
            JpmcdsSwapDatesFree(sd);
          }
        }
      }
    }
  }

  std::printf("inline constexpr std::array<SwapDatesCase, %zu> kSwapDatesCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
