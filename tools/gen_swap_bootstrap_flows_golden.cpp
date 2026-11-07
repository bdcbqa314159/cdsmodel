// Emits tests/golden/swap_bootstrap_flows_cases.hpp.
//
// Records the cash flows of swap benchmarks as the curve bootstrap builds
// them. Amounts are `rate * yearFraction` -- one multiplication, no
// accumulation -- but the file crossed a compiler to get here, so the replay
// compares at the ACCUMULATING tier anyway. That second reason is sufficient
// on its own and is the one that bit this project before.
//
// Flows live in one flat pool with an offset and a count per case.
//
// The degenerate maturity (value date == maturity) with a NON-ZERO rate is
// deliberately NOT recorded: upstream writes through `fArray[-1]` there, and
// recording its answer would mean running undefined behaviour to produce a
// committed file. It is UPSTREAM-32, covered by a DISABLED reproducer in the
// differential suite. The ZERO-rate degenerate case IS recorded, because
// upstream's special path handles it safely.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_swap_bootstrap_flows_golden
//   ./build/debug/gen_swap_bootstrap_flows_golden > tests/golden/swap_bootstrap_flows_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "datelist.h"
#include "ldate.h"
#include "zcprvt.h"
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
std::vector<std::string> g_dates;
std::vector<std::string> g_date_cases;

} // namespace

int main() {
  JpmcdsErrMsgOff();

  struct Span {
    long vy, vm, vd, my, mm, md;
  };
  const Span spans[] = {
      {2024, 1, 15, 2029, 1, 15}, // on cycle
      {2024, 1, 15, 2029, 4, 15}, // off cycle: a stub
      {2024, 1, 15, 2024, 7, 15}, // exactly one 6M period
      {2024, 2, 29, 2027, 2, 28}, // leap day
      {2024, 1, 31, 2026, 7, 31}, // month ends
      {2024, 3, 15, 2024, 3, 18}, // shorter than any interval
      {2024, 1, 15, 2024, 1, 15}, // degenerate: ZERO RATE ONLY
  };
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW, JPMCDS_BAD_DAY_PREVIOUS,
                        JPMCDS_BAD_DAY_MODIFIED};
  const long dccs[] = {JPMCDS_ACT_360, JPMCDS_ACT_365F, JPMCDS_B30_360};

  for (const Span &s : spans) {
    const TDate value_date = ymd(s.vy, s.vm, s.vd);
    const TDate maturity = ymd(s.my, s.mm, s.md);
    const bool degenerate = value_date == maturity;

    for (const int months : {3, 6, 12}) {
      for (const int stub_at_end : {0, 1}) {
        TDateInterval iv;
        JpmcdsMakeDateInterval(months, 'M', &iv);

        /* ---- coupon dates on their own ---- */
        for (const long conv : convs) {
          TDateList *dl = JpmcdsZCGetSwapCouponDL(value_date, maturity, stub_at_end ? TRUE : FALSE,
                                                  &iv, nullptr, conv, (char *)"NONE");
          const std::size_t offset = g_dates.size();
          std::size_t count = 0;
          if (dl != nullptr) {
            for (int i = 0; i < dl->fNumItems; ++i) {
              char row[40];
              std::snprintf(row, sizeof row, "    %ld,", (long)dl->fArray[i]);
              g_dates.emplace_back(row);
            }
            count = static_cast<std::size_t>(dl->fNumItems);
          }
          char row[200];
          std::snprintf(row, sizeof row, "    {%ld, %ld, %d, %s, %ld, %s, %zu, %zu},",
                        (long)value_date, (long)maturity, months, stub_at_end ? "true" : "false",
                        conv, dl != nullptr ? "true" : "false", offset, count);
          g_date_cases.emplace_back(row);
          JpmcdsFreeDateList(dl);
        }

        /* ---- cash flows ---- */
        for (const long dcc : dccs) {
          for (const long conv : convs) {
            for (const double rate : {0.0, 0.04, -0.01}) {
              // UPSTREAM-32: a non-zero rate on the degenerate span writes
              // through fArray[-1]. Never run, so never recorded.
              if (degenerate && rate != 0.0) {
                continue;
              }
              TCashFlowList *cfl =
                  JpmcdsZCGetSwapCFL(value_date, maturity, stub_at_end ? TRUE : FALSE, rate, &iv,
                                     dcc, nullptr, conv, (char *)"NONE");
              const std::size_t offset = g_flows.size();
              std::size_t count = 0;
              if (cfl != nullptr) {
                for (int i = 0; i < cfl->fNumItems; ++i) {
                  char row[80];
                  std::snprintf(row, sizeof row, "    {%ld, %s},", (long)cfl->fArray[i].fDate,
                                num(cfl->fArray[i].fAmount).c_str());
                  g_flows.emplace_back(row);
                }
                count = static_cast<std::size_t>(cfl->fNumItems);
              }
              char row[240];
              std::snprintf(row, sizeof row, "    {%ld, %ld, %d, %s, %s, %ld, %ld, %s, %zu, %zu},",
                            (long)value_date, (long)maturity, months,
                            stub_at_end ? "true" : "false", num(rate).c_str(), dcc, conv,
                            cfl != nullptr ? "true" : "false", offset, count);
              g_cases.emplace_back(row);
              JpmcdsFreeCFL(cfl);
            }
          }
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Recorded swap-benchmark bootstrap flows, from JpmcdsZCGetSwapCFL, and the
// coupon dates behind them, from JpmcdsZCGetSwapCouponDL.
//
// Amounts are a single `rate * yearFraction` with no accumulation, but this
// file crossed a compiler, so the replay compares at the accumulating tier:
// relative, kSwapBootstrapFlowsTolerance. Dates compare exactly.
//
// All flows and all dates live in one flat pool each; every case carries an
// offset and a count.
//
// The degenerate span (value date == maturity) appears ONLY with a zero rate.
// With a non-zero rate upstream writes through fArray[-1] -- UPSTREAM-32 --
// and recording that would mean running undefined behaviour to produce a
// committed file.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_swap_bootstrap_flows_golden
//   ./build/debug/gen_swap_bootstrap_flows_golden > tests/golden/swap_bootstrap_flows_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Relative. The file crossed a compiler, which is reason enough on its own.
inline constexpr double kSwapBootstrapFlowsTolerance = 1.0e-12;

struct RecordedSwapFlow {
  TDate date;
  double amount;
};

struct SwapCouponDateCase {
  TDate value_date;
  TDate maturity;
  int months;
  bool stub_at_end;
  long conv;
  bool accepted;
  std::size_t first_date;
  std::size_t date_count;
};

struct SwapBootstrapFlowCase {
  TDate value_date;
  TDate maturity;
  int months;
  bool stub_at_end;
  double rate;
  long day_count;
  long conv;
  bool accepted;
  std::size_t first_flow;
  std::size_t flow_count;
};

)");

  std::printf("inline constexpr std::array<TDate, %zu> kSwapCouponDates = {\n", g_dates.size());
  for (const std::string &r : g_dates) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("};\n\n");

  std::printf("inline constexpr std::array<SwapCouponDateCase, %zu> kSwapCouponDateCases{{\n",
              g_date_cases.size());
  for (const std::string &r : g_date_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<RecordedSwapFlow, %zu> kSwapBootstrapFlows{{\n",
              g_flows.size());
  for (const std::string &r : g_flows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<SwapBootstrapFlowCase, %zu> "
              "kSwapBootstrapFlowCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
