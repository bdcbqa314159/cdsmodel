// Emits tests/golden/root_find_cases.hpp -- recorded roots AND evaluation
// counts for the root finder.
//
// The call count is recorded deliberately. For every other module a recorded
// answer is sufficient, because the computation is a formula. Here the answer
// is the end of an iteration whose path depends on comparisons, so recording
// only the root would let a divergent path pass whenever the tolerance
// absorbed the difference. Recording the count makes CI check the path.
//
// Licence note: these are data derived from running the upstream model,
// exempt under section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_root_find_golden
//   ./build/debug/gen_root_find_golden > tests/golden/root_find_cases.hpp

#include "root_find_objectives.hpp"

#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "cgeneral.h"
#include "rtbrent.h"
}

namespace {

using cdsmodel::testing::Objective;
using cdsmodel::testing::Which;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

extern "C" int trampoline(double x, void *data, double *out) {
  Objective *obj = static_cast<Objective *>(data);
  const cdsmodel::Expected<double> value = (*obj)(x);
  if (!value) {
    return FAILURE;
  }
  *out = *value;
  return SUCCESS;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded roots and objective-evaluation counts. Data derived from\n"
              "// running the upstream model (licence 4(b)).\n"
              "//\n"
              "// `calls` is part of the contract, not diagnostics: the root finder is\n"
              "// iterative, so a divergent path could reach the same root and a\n"
              "// root-only check would not notice.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_root_find_golden\n"
              "//   ./build/debug/gen_root_find_golden > tests/golden/root_find_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// `objective` indexes cdsmodel::testing::kAllObjectives.\n"
              "/// `solved` false means upstream returned FAILURE; `root` is unused then.\n"
              "struct RootFindCase {\n"
              "  int objective;\n  double bound_lo;\n  double bound_hi;\n"
              "  int max_iterations;\n  double guess;\n  double initial_x_step;\n"
              "  double initial_f_derivative;\n  double x_tolerance;\n  double f_tolerance;\n"
              "  bool solved;\n  double root;\n  int calls;\n};\n\n");

  struct Bounds {
    double lo, hi;
  };
  const std::vector<Bounds> brackets{{0.0, 100.0}, {-10.0, 10.0}, {0.0, 3.0},
                                     {1.0, 2.0},   {-1.0, 5.0},   {0.0, 1.0}};
  const std::vector<double> guesses{0.0, 0.01, 0.5, 1.0, 1.5};
  const std::vector<double> steps{0.0, 0.0001, 0.1, -0.25};
  const std::vector<double> derivs{0.0, 1.0, -2.0};
  const std::vector<double> accs{1.0e-8, 1.0e-12, 0.0};

  std::vector<std::string> rows;
  const auto *first = std::begin(cdsmodel::testing::kAllObjectives);
  const auto *last = std::end(cdsmodel::testing::kAllObjectives);
  for (const auto *it = first; it != last; ++it) {
    const int index = static_cast<int>(it - first);
    for (const Bounds &b : brackets) {
      for (double guess : guesses) {
        if (guess < b.lo || guess > b.hi) {
          continue;
        }
        for (double step : steps) {
          for (double deriv : derivs) {
            for (double acc : accs) {
              Objective obj{*it};
              double solution = 0.0;
              const int rc = JpmcdsRootFindBrent(trampoline, &obj, b.lo, b.hi, 100, guess, step,
                                                 deriv, acc, acc, &solution);
              const bool solved = rc == SUCCESS;
              rows.push_back("    {" + std::to_string(index) + ", " + num(b.lo) + ", " + num(b.hi) +
                             ", 100, " + num(guess) + ", " + num(step) + ", " + num(deriv) + ", " +
                             num(acc) + ", " + num(acc) + ", " + (solved ? "true" : "false") +
                             ", " + num(solved ? solution : 0.0) + ", " +
                             std::to_string(obj.calls) + "},");
            }
          }
        }
      }
    }
  }
  std::printf("inline constexpr std::array<RootFindCase, %zu> kRootFindCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
