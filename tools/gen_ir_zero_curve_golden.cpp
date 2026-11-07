// Emits tests/golden/ir_zero_curve_cases.hpp.
//
// Records curves from JpmcdsBuildIRZeroCurve -- the top-level entry point, and
// so the vectors that come closest to being an end-to-end check of everything
// underneath it: money market, cash merge, swap schedules, stream cash flows,
// the Brent solve, and the curve container.
//
// Swap rates are ROOTS and compare at the solver tier. Money-market-only
// curves involve no solve, so those cases are recorded with a flag and the
// replay compares them tightly -- the same split as
// curve_instruments_cases.hpp, here expressed per case rather than per array
// because one entry point produces both kinds.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_ir_zero_curve_golden
//   ./build/debug/gen_ir_zero_curve_golden > tests/golden/ir_zero_curve_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "ldate.h"
#include "tcurve.h"
#include "zerocurve.h"
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

std::vector<std::string> g_points;
std::vector<std::string> g_cases;

constexpr int kInstruments = 7;

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate value_date = ymd(2024, 1, 15);

  TDate dates[kInstruments] = {ymd(2024, 4, 15), ymd(2024, 7, 15), ymd(2025, 1, 15),
                               ymd(2026, 1, 15), ymd(2027, 1, 15), ymd(2029, 1, 15),
                               ymd(2031, 1, 15)};
  double rates[kInstruments] = {0.0230, 0.0245, 0.0260, 0.0280, 0.0295, 0.0320, 0.0340};

  const long mm_dccs[] = {JPMCDS_ACT_360, JPMCDS_ACT_365F};
  const long freqs[] = {1, 2, 4};
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW};

  // Every split into a money-market prefix and a swap suffix, which is the
  // only arrangement these dates permit, plus both degenerate ends.
  for (int cash_count = 0; cash_count <= kInstruments; ++cash_count) {
    std::string names(kInstruments, 'S');
    for (int i = 0; i < cash_count; ++i) {
      names[static_cast<std::size_t>(i)] = 'M';
    }
    for (const long mm_dcc : mm_dccs) {
      for (const long freq : freqs) {
        for (const long conv : convs) {
          std::string local_names = names;
          TDate local_dates[kInstruments];
          double local_rates[kInstruments];
          for (int i = 0; i < kInstruments; ++i) {
            local_dates[i] = dates[i];
            local_rates[i] = rates[i];
          }
          TCurve *out = JpmcdsBuildIRZeroCurve(
              value_date, local_names.data(), local_dates, local_rates, kInstruments, mm_dcc, freq,
              freq, JPMCDS_ACT_360, JPMCDS_ACT_360, conv, (char *)"NONE");

          const std::size_t offset = g_points.size();
          std::size_t count = 0;
          if (out != nullptr) {
            for (int i = 0; i < out->fNumItems; ++i) {
              char row[120];
              std::snprintf(row, sizeof row, "    {%ld, %s},", (long)out->fArray[i].fDate,
                            num(out->fArray[i].fRate).c_str());
              g_points.emplace_back(row);
            }
            count = static_cast<std::size_t>(out->fNumItems);
          }
          char row[220];
          std::snprintf(row, sizeof row, "    {%d, %ld, %ld, %ld, %s, %s, %zu, %zu},", cash_count,
                        mm_dcc, freq, conv, cash_count == kInstruments ? "true" : "false",
                        out != nullptr ? "true" : "false", offset, count);
          g_cases.emplace_back(row);
          JpmcdsFreeTCurve(out);
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Curves from JpmcdsBuildIRZeroCurve, the top-level entry point -- so these
// vectors come closest to an end-to-end check of everything beneath it: money
// market, the cash merge, swap schedules, stream cash flows, the Brent solve,
// and the curve container.
//
// Swap rates are ROOTS and compare at kIrCurveSolvedTolerance. A case with
// `money_market_only` set involves no solve at all, and the replay compares
// those at kIrCurveExactTolerance instead -- the split is per case rather than
// per array because one entry point produces both kinds.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_ir_zero_curve_golden
//   ./build/debug/gen_ir_zero_curve_golden > tests/golden/ir_zero_curve_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Solved rates, bounded by the solver's stopping criterion.
inline constexpr double kIrCurveSolvedTolerance = 1.0e-7;

/// Converted rates. The file crossed a compiler, so the accumulating tier.
inline constexpr double kIrCurveExactTolerance = 1.0e-12;

struct RecordedIrCurvePoint {
  TDate date;
  double rate;
};

struct IrCurveCase {
  int cash_count;
  long money_market_day_count;
  long frequency;
  long conv;
  bool money_market_only;
  bool accepted;
  std::size_t first_point;
  std::size_t point_count;
};

)");

  std::printf("inline constexpr TDate kIrCurveValueDate = %ld;\n", (long)value_date);
  std::printf("inline constexpr std::size_t kIrCurveInstruments = %d;\n", kInstruments);
  std::printf("inline constexpr std::array<TDate, %d> kIrCurveDates = {", kInstruments);
  for (int i = 0; i < kInstruments; ++i) {
    std::printf("%s%ld", i ? ", " : "", (long)dates[i]);
  }
  std::printf("};\n");
  std::printf("inline constexpr std::array<double, %d> kIrCurveRates = {", kInstruments);
  for (int i = 0; i < kInstruments; ++i) {
    std::printf("%s%s", i ? ", " : "", num(rates[i]).c_str());
  }
  std::printf("};\n\n");

  std::printf("inline constexpr std::array<RecordedIrCurvePoint, %zu> kIrCurvePoints{{\n",
              g_points.size());
  for (const std::string &r : g_points) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");
  std::printf("inline constexpr std::array<IrCurveCase, %zu> kIrCurveCases{{\n", g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
