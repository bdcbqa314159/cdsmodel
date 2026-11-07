// Emits tests/golden/curve_linear_interp_cases.hpp.
//
// Records curves re-expressed on new dates by JpmcdsLinInterpCurve. The rates
// are linear interpolations -- one division and one multiply-add, nothing
// accumulated -- but the file crossed a compiler, so the replay compares at
// the accumulating tier.
//
// Decreasing desired-date lists are NOT recorded. Upstream happily builds a
// curve whose dates decrease; the port's `ZeroCurve` cannot hold one, so there
// is no answer to replay. That divergence is covered by a differential test
// instead.
//
// Zero-point input curves are not recorded either: the clamping loop reads
// through a NULL array (UPSTREAM-36), and recording it would mean running
// undefined behaviour to produce a committed file.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_curve_linear_interp_golden
//   ./build/debug/gen_curve_linear_interp_golden > tests/golden/curve_linear_interp_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "dtlist.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
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
std::vector<std::string> g_decls;

struct Curve {
  int id;
  int count;
  TDate dates[4];
  double rates[4];
};

struct Wanted {
  int id;
  int count;
  TDate dates[3];
};

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate base = ymd(2024, 1, 15);

  Curve curves[4];
  curves[0] = {0, 2, {ymd(2024, 7, 15), ymd(2027, 1, 15), 0, 0}, {0.0300, 0.0350, 0, 0}};
  curves[1] = {1,
               4,
               {ymd(2024, 4, 15), ymd(2025, 1, 15), ymd(2027, 1, 15), ymd(2031, 1, 15)},
               {0.0230, 0.0260, 0.0310, 0.0340}};
  curves[2] = {2, 1, {ymd(2025, 1, 15), 0, 0, 0}, {0.0400, 0, 0, 0}};
  curves[3] = {3, 2, {ymd(2024, 6, 15), ymd(2024, 6, 16), 0, 0}, {0.0300, 0.0301, 0, 0}};

  Wanted wanted[5];
  wanted[0] = {0, 2, {ymd(2025, 1, 15), ymd(2026, 1, 15)}};                /* inside */
  wanted[1] = {1, 2, {ymd(2020, 1, 15), ymd(2023, 1, 15)}};                /* before */
  wanted[2] = {2, 2, {ymd(2035, 1, 15), ymd(2040, 1, 15)}};                /* after */
  wanted[3] = {3, 2, {ymd(2024, 7, 15), ymd(2027, 1, 15)}};                /* on knots */
  wanted[4] = {4, 3, {ymd(2020, 1, 1), ymd(2025, 6, 1), ymd(2045, 1, 1)}}; /* straddling */

  const long dccs[] = {JPMCDS_ACT_365F, JPMCDS_ACT_360, JPMCDS_B30_360};
  const double bases[] = {1.0, 2.0, 12.0};

  for (Curve &curve : curves) {
    for (const long dcc : dccs) {
      for (const double basis : bases) {
        TCurve *old = JpmcdsMakeTCurve(base, curve.dates, curve.rates, curve.count, basis, dcc);
        if (old == nullptr) {
          continue;
        }
        for (Wanted &want : wanted) {
          TDateList *dl = JpmcdsNewDateListFromDates(want.dates, want.count);
          if (dl == nullptr) {
            continue;
          }
          TCurve *out = JpmcdsLinInterpCurve(dl, old, nullptr);
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
          char row[200];
          std::snprintf(row, sizeof row, "    {%d, %d, %ld, %s, %s, %zu, %zu},", curve.id, want.id,
                        dcc, num(basis).c_str(), out != nullptr ? "true" : "false", offset, count);
          g_cases.emplace_back(row);
          JpmcdsFreeTCurve(out);
          JpmcdsFreeDateList(dl);
        }
        JpmcdsFreeTCurve(old);
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Curves re-expressed on new dates by JpmcdsLinInterpCurve.
//
// The rates are linear interpolations with nothing accumulated, but this file
// crossed a compiler, so the replay compares at kCurveLinInterpTolerance.
// Dates compare exactly.
//
// NOT recorded: decreasing desired-date lists, which upstream turns into a
// curve whose dates decrease and the port's ZeroCurve cannot hold; and
// zero-point input curves, where the clamping loop reads through a NULL array
// (UPSTREAM-36). Both divergences are covered by differential tests.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_curve_linear_interp_golden
//   ./build/debug/gen_curve_linear_interp_golden > tests/golden/curve_linear_interp_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Relative. The file crossed a compiler, which decides it on its own.
inline constexpr double kCurveLinInterpTolerance = 1.0e-12;

struct RecordedInterpPoint {
  TDate date;
  double rate;
};

struct CurveLinInterpCase {
  int curve;
  int wanted;
  long day_count;
  double basis;
  bool accepted;
  std::size_t first_point;
  std::size_t point_count;
};

)");

  std::printf("inline constexpr TDate kLinInterpBaseDate = %ld;\n\n", (long)base);

  for (Curve &curve : curves) {
    std::printf("inline constexpr std::size_t kLinInterpCurve%dCount = %d;\n", curve.id,
                curve.count);
    std::printf("inline constexpr std::array<TDate, %d> kLinInterpCurve%dDates = {", curve.count,
                curve.id);
    for (int i = 0; i < curve.count; ++i) {
      std::printf("%s%ld", i ? ", " : "", (long)curve.dates[i]);
    }
    std::printf("};\n");
    std::printf("inline constexpr std::array<double, %d> kLinInterpCurve%dRates = {", curve.count,
                curve.id);
    for (int i = 0; i < curve.count; ++i) {
      std::printf("%s%s", i ? ", " : "", num(curve.rates[i]).c_str());
    }
    std::printf("};\n");
  }
  std::printf("\n");
  for (Wanted &want : wanted) {
    std::printf("inline constexpr std::size_t kLinInterpWanted%dCount = %d;\n", want.id,
                want.count);
    std::printf("inline constexpr std::array<TDate, %d> kLinInterpWanted%dDates = {", want.count,
                want.id);
    for (int i = 0; i < want.count; ++i) {
      std::printf("%s%ld", i ? ", " : "", (long)want.dates[i]);
    }
    std::printf("};\n");
  }
  std::printf("\n");

  std::printf("inline constexpr std::array<RecordedInterpPoint, %zu> kLinInterpPoints{{\n",
              g_points.size());
  for (const std::string &r : g_points) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");
  std::printf("inline constexpr std::array<CurveLinInterpCase, %zu> kLinInterpCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
