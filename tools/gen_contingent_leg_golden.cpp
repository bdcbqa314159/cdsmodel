// Emits tests/golden/contingent_leg_cases.hpp -- upstream's protection leg
// PVs, recorded so CI can verify the port without the oracle.
//
// These are PRICES, derived from exp and log, so they are libm-specific in the
// same way the zero curve vectors are: correct to a few ULPs anywhere, not
// bit-identical everywhere. The replay test uses EXPECT_DOUBLE_EQ. Bit
// equality is the right bar only in the differential suite, where both
// implementations run in one process and share one libm.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_contingent_leg_golden
//   ./build/debug/gen_contingent_leg_golden > tests/golden/contingent_leg_cases.hpp

#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "contingentleg.h"
#include "cx.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::string dbl(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s{buf};
  if (s.find_first_of(".eEni") == std::string::npos) {
    s += ".0";
  }
  return s;
}

const std::vector<cdsmodel::RatePoint> kDisc{{d(2024, 4, 1), 0.0310},
                                             {d(2025, 1, 1), 0.0350},
                                             {d(2027, 1, 1), 0.0400},
                                             {d(2031, 1, 1), 0.0440},
                                             {d(2039, 1, 1), 0.0455}};
const std::vector<cdsmodel::RatePoint> kSpread{{d(2024, 6, 20), 0.0090},
                                               {d(2025, 6, 20), 0.0130},
                                               {d(2029, 6, 20), 0.0185},
                                               {d(2034, 6, 20), 0.0220}};

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &pts) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : pts) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(pts.size()), 1.0, JPMCDS_ACT_365F);
}

void emit_curve(const char *name, const std::vector<cdsmodel::RatePoint> &pts) {
  std::printf("inline constexpr std::array<long, %zu> %sDates{{", pts.size(), name);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(pts[i].date));
  }
  std::printf("}};\ninline constexpr std::array<double, %zu> %sRates{{", pts.size(), name);
  for (std::size_t i = 0; i < pts.size(); ++i) {
    std::printf("%s%s", i ? ", " : "", dbl(pts[i].rate).c_str());
  }
  std::printf("}};\n");
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const cdsmodel::Date base = d(2024, 1, 1);
  TCurve *disc = build(base, kDisc);
  TCurve *spread = build(base, kSpread);
  if (disc == nullptr || spread == nullptr) {
    return 1;
  }

  std::printf(
      "// GENERATED FILE -- DO NOT EDIT.\n"
      "//\n"
      "// Protection leg present values recorded from the upstream ISDA CDS\n"
      "// Standard Model 1.8.3 (contingentleg.c). Computed data, exempt from the\n"
      "// upstream licence's distribution terms under its section 4(b).\n"
      "//\n"
      "// These are PRICES derived from exp and log, so they are libm-specific:\n"
      "// correct to a few ULPs anywhere, not bit-identical everywhere. The replay\n"
      "// test allows 4 ULPs. Bit equality is the right bar only in the\n"
      "// differential suite, where both implementations share one libm.\n"
      "//\n"
      "// Regenerate with a local upstream copy present:\n"
      "//   cmake --build --preset debug --target gen_contingent_leg_golden\n"
      "//   ./build/debug/gen_contingent_leg_golden > tests/golden/contingent_leg_cases.hpp\n"
      "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
      "namespace cdsmodel::golden {\n\n"
      "inline constexpr long kClBase = %ld;\n\n",
      cdsmodel::to_tdate(base));
  emit_curve("kClDisc", kDisc);
  emit_curve("kClSpread", kSpread);

  std::printf("\n/// pay: 0 = at default, 1 = at maturity.\n"
              "struct ContingentLegCase {\n  long start;\n  long end;\n  double recovery;\n"
              "  int pay;\n  bool protect_start;\n  double pv;\n};\n\n");

  std::vector<std::string> rows;
  for (int start_off : {0, 1, 45, 180, 400, 1200}) {
    for (int tenor : {30, 91, 365, 1826, 3653, 5479}) {
      for (double recovery : {0.0, 0.25, 0.40, 0.75, 1.0}) {
        for (int protect_start = 0; protect_start <= 1; ++protect_start) {
          for (int pay = 0; pay <= 1; ++pay) {
            const cdsmodel::Date start = base + std::chrono::days{start_off};
            const cdsmodel::Date end = start + std::chrono::days{tenor};
            TContingentLeg cl{};
            cl.startDate = cdsmodel::to_tdate(start);
            cl.endDate = cdsmodel::to_tdate(end);
            cl.notional = 1.0e7;
            cl.payType = pay == 0 ? PROT_PAY_DEF : PROT_PAY_MAT;
            cl.protectStart = protect_start ? TRUE : FALSE;
            double pv = 0.0;
            if (JpmcdsContingentLegPV(&cl, cdsmodel::to_tdate(base), cdsmodel::to_tdate(base),
                                      cdsmodel::to_tdate(base), disc, spread, recovery,
                                      &pv) != SUCCESS) {
              continue;
            }
            rows.push_back("    {" + std::to_string(cdsmodel::to_tdate(start)) + ", " +
                           std::to_string(cdsmodel::to_tdate(end)) + ", " + dbl(recovery) + ", " +
                           std::to_string(pay) + ", " + (protect_start ? "true" : "false") + ", " +
                           dbl(pv) + "},");
          }
        }
      }
    }
  }
  std::printf("inline constexpr std::array<ContingentLegCase, %zu> kContingentLegCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
  return 0;
}
