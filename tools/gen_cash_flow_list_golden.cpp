// Emits tests/golden/cash_flow_list_cases.hpp.
//
// Records what upstream's merge produces, which is the one thing properties
// cannot pin down on their own: the ORDER it emits elements in when dates tie,
// and how it treats a duplicate date inside a single list. Sizes and sums
// follow from the algorithm; the interleaving is a choice.
//
// Nothing here goes near a transcendental function, so these vectors carry no
// floating-point tolerance question -- the amounts are exact sums of exact
// inputs.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_cash_flow_list_golden
//   ./build/debug/gen_cash_flow_list_golden > tests/golden/cash_flow_list_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
}

namespace {

constexpr int kPoolSize = 4;
constexpr std::size_t kMaxItems = 2 * kPoolSize;
const TDate kPool[kPoolSize] = {40000, 40500, 41000, 41500};

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

/// The subset of the pool selected by `mask`, amounts offset by `base` so a
/// misplaced element is identifiable.
void build(unsigned mask, double base, std::vector<TDate> &dates, std::vector<double> &amounts) {
  dates.clear();
  amounts.clear();
  for (int bit = 0; bit < kPoolSize; ++bit) {
    if (mask & (1U << bit)) {
      dates.push_back(kPool[bit]);
      amounts.push_back(base + 0.25 * double(bit));
    }
  }
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded cash-flow-list merges: the element ORDER upstream emits, which\n"
              "// is the part no property pins down. Data derived from running the\n"
              "// upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_cash_flow_list_golden\n"
              "//   ./build/debug/gen_cash_flow_list_golden > "
              "tests/golden/cash_flow_list_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One recorded merge. `a_mask` and `b_mask` select subsets of the date\n"
              "/// pool {40000, 40500, 41000, 41500}; amounts are base + 0.25*bit, with\n"
              "/// base 1.0 for the first list and 100.0 for the second.\n"
              "struct MergeCase {\n"
              "  unsigned a_mask;\n"
              "  unsigned b_mask;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> amounts;\n"
              "};\n\n",
              kMaxItems, kMaxItems);

  std::vector<std::string> rows;
  std::vector<TDate> da;
  std::vector<double> ma;
  std::vector<TDate> db;
  std::vector<double> mb;

  const unsigned masks = 1U << kPoolSize;
  for (unsigned a_mask = 0; a_mask < masks; ++a_mask) {
    for (unsigned b_mask = 0; b_mask < masks; ++b_mask) {
      build(a_mask, 1.0, da, ma);
      build(b_mask, 100.0, db, mb);
      TCashFlowList *a =
          JpmcdsNewCFL(da.empty() ? nullptr : da.data(), ma.empty() ? nullptr : ma.data(),
                       static_cast<int>(da.size()));
      TCashFlowList *b =
          JpmcdsNewCFL(db.empty() ? nullptr : db.data(), mb.empty() ? nullptr : mb.data(),
                       static_cast<int>(db.size()));
      TCashFlowList *merged = (a != nullptr && b != nullptr) ? JpmcdsMergeCFL(a, b) : nullptr;
      if (merged != nullptr) {
        std::string dates_literal = "{";
        std::string amounts_literal = "{";
        for (std::size_t i = 0; i < kMaxItems; ++i) {
          const bool present = i < static_cast<std::size_t>(merged->fNumItems);
          dates_literal += (i == 0 ? "" : ", ");
          amounts_literal += (i == 0 ? "" : ", ");
          dates_literal +=
              present ? std::to_string(static_cast<long>(merged->fArray[i].fDate)) : "0";
          amounts_literal += present ? num(merged->fArray[i].fAmount) : "0";
        }
        dates_literal += "}";
        amounts_literal += "}";
        char head[64];
        std::snprintf(head, sizeof head, "    {%u, %u, %d, ", a_mask, b_mask, merged->fNumItems);
        rows.emplace_back(std::string(head) + dates_literal + ", " + amounts_literal + "},");
      }
      JpmcdsFreeCFL(a);
      JpmcdsFreeCFL(b);
      JpmcdsFreeCFL(merged);
    }
  }

  std::printf("inline constexpr std::array<MergeCase, %zu> kMergeCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
