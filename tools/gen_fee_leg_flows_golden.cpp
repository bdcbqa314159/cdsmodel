// Emits tests/golden/fee_leg_flows_cases.hpp.
//
// Records the non-contingent cash flows of a CDS fee leg -- what the
// protection buyer pays if there is no default. Amounts are
// `yearFraction * couponRate * notional`: two multiplies, nothing
// accumulated. But the file crosses a compiler to get here, so the replay
// compares at the accumulating tier regardless; that reason alone has decided
// it before.
//
// Flows live in one flat pool with an offset and a count per case.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_fee_leg_flows_golden
//   ./build/debug/gen_fee_leg_flows_golden > tests/golden/fee_leg_flows_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cds.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "mdydate.h"
#include "stub.h"
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

std::vector<std::string> g_flows;
std::vector<std::string> g_cases;

struct Span {
  long sy, sm, sd, ey, em, ed;
};

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const Span spans[] = {
      {2024, 3, 20, 2029, 6, 20}, /* on cycle for 3M */
      {2024, 3, 20, 2029, 7, 11}, /* off cycle: a stub */
      {2024, 2, 29, 2025, 2, 28}, /* leap day */
      {2024, 3, 20, 2024, 6, 20}, /* one period */
      {2024, 1, 31, 2026, 7, 31}, /* month ends */
  };
  const long dccs[] = {JPMCDS_ACT_360, JPMCDS_ACT_365F, JPMCDS_B30_360};
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW, JPMCDS_BAD_DAY_MODIFIED};
  const double coupons[] = {0.0, 0.01, 0.05, -0.002};

  for (const Span &s : spans) {
    const TDate start = ymd(s.sy, s.sm, s.sd);
    const TDate end = ymd(s.ey, s.em, s.ed);
    for (const int months : {3, 6, 12}) {
      for (const long dcc : dccs) {
        for (const long conv : convs) {
          for (const double coupon : coupons) {
            TDateInterval iv;
            JpmcdsMakeDateInterval(months, 'M', &iv);
            TStubMethod stub;
            if (JpmcdsStringToStubMethod((char *)"f/s", &stub) != SUCCESS) {
              continue;
            }
            TCashFlowList *cfl = JpmcdsCdsFeeLegFlows(start, end, &iv, &stub, 1.0e7, coupon, dcc,
                                                      conv, (char *)"NONE");
            const std::size_t offset = g_flows.size();
            std::size_t count = 0;
            if (cfl != nullptr) {
              for (int i = 0; i < cfl->fNumItems; ++i) {
                char row[120];
                std::snprintf(row, sizeof row, "    {%ld, %s},", (long)cfl->fArray[i].fDate,
                              num(cfl->fArray[i].fAmount).c_str());
                g_flows.emplace_back(row);
              }
              count = static_cast<std::size_t>(cfl->fNumItems);
            }
            char row[240];
            std::snprintf(row, sizeof row, "    {%ld, %ld, %d, %ld, %ld, %s, %s, %zu, %zu},",
                          (long)start, (long)end, months, dcc, conv, num(coupon).c_str(),
                          cfl != nullptr ? "true" : "false", offset, count);
            g_cases.emplace_back(row);
            JpmcdsFreeCFL(cfl);
          }
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Non-contingent cash flows of a CDS fee leg, from JpmcdsCdsFeeLegFlows --
// what the protection buyer pays if there is no default.
//
// Amounts are `yearFraction * couponRate * notional`: two multiplies with
// nothing accumulated. The replay still compares at the accumulating tier,
// because this file crossed a compiler to get here and that reason alone has
// decided it before.
//
// Flows live in one flat pool; each case carries an offset and a count.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_fee_leg_flows_golden
//   ./build/debug/gen_fee_leg_flows_golden > tests/golden/fee_leg_flows_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Relative. The file crossed a compiler, which settles it on its own.
inline constexpr double kFeeLegFlowsTolerance = 1.0e-12;

struct RecordedFeeFlow {
  TDate date;
  double amount;
};

struct FeeLegFlowsCase {
  TDate start;
  TDate end;
  int months;
  long day_count;
  long conv;
  double coupon_rate;
  bool accepted;
  std::size_t first_flow;
  std::size_t flow_count;
};

)");

  std::printf("inline constexpr double kFeeLegFlowsNotional = 1.0e7;\n\n");
  std::printf("inline constexpr std::array<RecordedFeeFlow, %zu> kFeeLegFlows{{\n", g_flows.size());
  for (const std::string &r : g_flows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");
  std::printf("inline constexpr std::array<FeeLegFlowsCase, %zu> kFeeLegFlowsCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
