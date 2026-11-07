// Emits tests/golden/match_range_cases.hpp.
//
// Records JpmcdsBinarySearchLong over an EXHAUSTIVE domain: every
// non-decreasing array of length 1 to 5 over the alphabet 0..3, crossed with
// every sought value from -1 to 4. Everything recorded is a small integer, so
// the replay compares EXACTLY -- there is no tolerance in this header.
//
// The sweep is exhaustive rather than sampled because it can be: 125 arrays,
// 750 cases, and the whole file is a few hundred lines. Where a domain is
// small enough to enumerate, enumerating it is strictly better than choosing
// representatives, and the cost here is nil.
//
// Upstream's `exact` is recorded too, even though the port does not return it,
// because it is NOT necessarily the first match -- so the replay can assert
// the weaker property upstream actually promises (that it indexes SOME equal
// element) rather than the stronger one it does not.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_match_range_golden
//   ./build/debug/gen_match_range_golden > tests/golden/match_range_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "cgeneral.h"
#include "cxbsearch.h"
}

namespace {

std::vector<std::string> g_values;
std::vector<std::string> g_cases;

constexpr long kAlphabet = 4;
constexpr std::size_t kMaxLength = 5;

void record(std::vector<long> &xs) {
  const std::size_t offset = g_values.size();
  for (const long value : xs) {
    char row[32];
    std::snprintf(row, sizeof row, "    %ld,", value);
    g_values.emplace_back(row);
  }
  for (long desired = -1; desired <= kAlphabet; ++desired) {
    long exact = -99;
    long lo = -99;
    long hi = -99;
    const bool accepted =
        JpmcdsBinarySearchLong(desired, xs.data(), sizeof(long), static_cast<long>(xs.size()),
                               &exact, &lo, &hi) == SUCCESS;
    char row[160];
    std::snprintf(row, sizeof row, "    {%zu, %zu, %ld, %s, %ld, %ld, %ld},", offset, xs.size(),
                  desired, accepted ? "true" : "false", exact, lo, hi);
    g_cases.emplace_back(row);
  }
}

void enumerate(std::size_t length) {
  std::vector<long> xs(length, 0);
  const auto recurse = [&](auto &&self, std::size_t index, long lowest) -> void {
    if (index == length) {
      record(xs);
      return;
    }
    for (long value = lowest; value < kAlphabet; ++value) {
      xs[index] = value;
      self(self, index + 1, value);
    }
  };
  recurse(recurse, 0, 0);
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  for (std::size_t length = 1; length <= kMaxLength; ++length) {
    enumerate(length);
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// JpmcdsBinarySearchLong over an EXHAUSTIVE domain: every non-decreasing array
// of length 1 to 5 over the alphabet 0..3, crossed with every sought value
// from -1 to 4. 125 arrays, 750 cases.
//
// Everything here is a small integer, so the replay compares EXACTLY. There is
// no tolerance in this header and there should not be one.
//
// `exact` is recorded even though the port does not return it, because it is
// NOT necessarily the first match -- so the replay asserts the weaker property
// upstream actually promises (that it indexes SOME equal element) rather than
// the stronger one it does not.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_match_range_golden
//   ./build/debug/gen_match_range_golden > tests/golden/match_range_cases.hpp

#pragma once

#include <array>
#include <cstddef>

namespace cdsmodel::golden {

/// One search: the array (as a slice of the pool), the sought value, and
/// upstream's three outputs verbatim.
struct MatchRangeCase {
  std::size_t first_value;
  std::size_t length;
  long desired;
  bool accepted;
  long exact;
  long lo_bound;
  long hi_bound;
};

)");

  std::printf("inline constexpr std::array<long, %zu> kMatchRangeValues = {\n", g_values.size());
  for (const std::string &r : g_values) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("};\n\n");

  std::printf("inline constexpr std::array<MatchRangeCase, %zu> kMatchRangeCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
