// Emits tests/golden/cx_cases.hpp.
//
// Records JpmcdsContingentLegMake over a grid of spans crossed with
// protectStart. Everything recorded is a date, a flag or a count, so the
// replay compares EXACTLY -- there is no tolerance in this header.
//
// The point of recording it at all is the acceptance rule: upstream validates
// `endDate > startDate` AFTER applying the protectStart shift, so the flag
// decides whether a zero-length leg is legal. That is a one-line behaviour
// that a port can get wrong in a way no pricing test would notice, because a
// refused leg and an accepted zero-length leg both produce no cash flows.
//
// JpmcdsFeeLegMake is NOT recorded: upstream exports it and declares it in no
// header, so a recording would be of a function the library itself cannot
// reach. Its behaviour is pinned by a differential test using a hand-written
// prototype instead, which is the only way any caller could reach it.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_cx_golden
//   ./build/debug/gen_cx_golden > tests/golden/cx_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cmemory.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "macros.h"
#include "mdydate.h"
}

namespace {

TDate ymd(long y, long m, long d) {
  TMonthDayYear mdy;
  mdy.year = y;
  mdy.month = m;
  mdy.day = d;
  TDate out = 0;
  JpmcdsMDYToDate(&mdy, &out);
  return out;
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::vector<std::string> g_cases;

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const TDate anchors[] = {ymd(2024, 3, 20), ymd(2024, 2, 29), ymd(2024, 1, 31), ymd(2024, 12, 31)};
  // Offsets in days from the anchor, including the two that make the
  // protectStart asymmetry visible.
  const long offsets[] = {-2, -1, 0, 1, 2, 90, 1827};
  const double notionals[] = {1.0, 1.0e7, -5.0e6};

  for (const TDate start : anchors) {
    for (const long offset : offsets) {
      const TDate end = start + offset;
      for (const int protect : {0, 1}) {
        for (const double notional : notionals) {
          TContingentLeg *p =
              JpmcdsContingentLegMake(start, end, notional, PROT_PAY_DEF, protect ? TRUE : FALSE);
          char row[220];
          std::snprintf(row, sizeof row, "    {%ld, %ld, %d, %s, %s, %ld, %ld, %s},", (long)start,
                        (long)end, protect, num(notional).c_str(), p != nullptr ? "true" : "false",
                        p != nullptr ? (long)p->startDate : 0L,
                        p != nullptr ? (long)p->endDate : 0L,
                        p != nullptr && p->protectStart == TRUE ? "true" : "false");
          g_cases.emplace_back(row);
          FREE(p);
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// JpmcdsContingentLegMake over a grid of spans crossed with protectStart and
// notional. Dates, flags and counts only, so the replay compares EXACTLY.
//
// The behaviour worth pinning is the acceptance rule: upstream validates
// `endDate > startDate` AFTER applying the protectStart shift, so the flag
// decides whether a zero-length leg is legal. A port can get that wrong in a
// way no pricing test would notice, because a refused leg and an accepted
// zero-length leg both produce no cash flows.
//
// JpmcdsFeeLegMake is NOT recorded: upstream exports it and declares it in no
// header, so a recording would be of a function the library cannot reach. It
// is pinned by a differential test through a hand-written prototype instead.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_cx_golden
//   ./build/debug/gen_cx_golden > tests/golden/cx_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

struct ContingentLegMakeCase {
  TDate start;
  TDate end;
  int protect_start;
  double notional;
  bool accepted;
  TDate stored_start;
  TDate stored_end;
  bool stored_protect_start;
};

)");

  std::printf("inline constexpr std::array<ContingentLegMakeCase, %zu> "
              "kContingentLegMakeCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
