// Emits tests/golden/swap_bootstrap_cases.hpp.
//
// Records zero curves bootstrapped from swap benchmark strips, on both paths:
// floating-at-par (no discount curve) and explicit floating valuation.
//
// The recorded rates are ROOTS. They are bounded by the solver's stopping
// criterion -- |f| <= 1e-11, no x criterion, at most 50 iterations -- and not
// by floating-point rounding, so the replay compares at the SOLVER tier. That
// is looser than every other golden file here and deliberately so: two
// implementations taking different Brent step sequences can land on different
// points of the same flat region, and demanding they agree to 1e-12 would be
// demanding they iterate identically.
//
// The replay also checks the thing that actually matters, which no tolerance
// can express: every recorded benchmark must re-price to par off the curve
// built from it.
//
// Points live in one flat pool with an offset and a count per case.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_swap_bootstrap_golden
//   ./build/debug/gen_swap_bootstrap_golden > tests/golden/swap_bootstrap_cases.hpp

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "badday.h"
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
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

std::vector<std::string> g_points;
std::vector<std::string> g_cases;

constexpr int kStubPoints = 3;
constexpr int kBenchmarks = 4;

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate value_date = ymd(2024, 1, 15);

  const TDate stub_dates[kStubPoints] = {ymd(2024, 4, 15), ymd(2024, 7, 15), ymd(2025, 1, 15)};
  const double stub_rates[kStubPoints] = {0.0230, 0.0245, 0.0260};

  /* Two strips: one on-cycle from the value date, one deliberately off-cycle
   * so every benchmark carries a stub. */
  TDate bench_dates_a[kBenchmarks] = {ymd(2026, 1, 15), ymd(2027, 1, 15), ymd(2029, 1, 15),
                                      ymd(2031, 1, 15)};
  double bench_rates_a[kBenchmarks] = {0.0280, 0.0295, 0.0320, 0.0340};
  TDate bench_dates_b[kBenchmarks] = {ymd(2026, 4, 20), ymd(2027, 7, 20), ymd(2029, 10, 20),
                                      ymd(2032, 2, 20)};
  double bench_rates_b[kBenchmarks] = {0.0275, 0.0300, 0.0325, 0.0345};

  /* A discount curve for the explicit-floating path. */
  TDate disc_dates[2] = {ymd(2030, 1, 15), ymd(2045, 1, 15)};
  double disc_rates[2] = {0.030, 0.035};
  TCurve *disc = JpmcdsMakeTCurve(value_date, disc_dates, disc_rates, 2, 1.0, JPMCDS_ACT_365F);
  if (disc == nullptr) {
    std::fprintf(stderr, "could not build the discount curve\n");
    return 1;
  }

  const long freqs[] = {1, 2, 4};
  const long dccs[] = {JPMCDS_ACT_360, JPMCDS_ACT_365F};
  const long interps[] = {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS};
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW};

  for (const int strip : {0, 1}) {
    TDate *bench_dates = strip == 0 ? bench_dates_a : bench_dates_b;
    double *bench_rates = strip == 0 ? bench_rates_a : bench_rates_b;
    for (const int with_disc : {0, 1}) {
      for (const long freq : freqs) {
        for (const long dcc : dccs) {
          for (const long interp : interps) {
            for (const long conv : convs) {
              ZCurve *zc = JpmcdsZCMake(value_date, 8, 1.0, JPMCDS_ACT_365F);
              if (zc == nullptr) {
                continue;
              }
              bool seeded = true;
              for (int i = 0; i < kStubPoints; ++i) {
                if (JpmcdsZCAddRate(zc, stub_dates[i], stub_rates[i]) != SUCCESS) {
                  seeded = false;
                }
              }
              if (!seeded) {
                JpmcdsZCFree(zc);
                continue;
              }

              TInterpData idata;
              std::memset(&idata, 0, sizeof idata);
              const int rc = JpmcdsZCAddSwaps(
                  zc, with_disc ? disc : nullptr, bench_dates, bench_rates, kBenchmarks,
                  static_cast<int>(freq), static_cast<int>(freq), dcc, dcc, interp, &idata, nullptr,
                  static_cast<TBadDayAndStubPos>(conv), (char *)"NONE");

              const std::size_t offset = g_points.size();
              std::size_t count = 0;
              if (rc == SUCCESS) {
                for (int i = 0; i < zc->numItems; ++i) {
                  char row[120];
                  std::snprintf(row, sizeof row, "    {%ld, %s},", (long)zc->date[i],
                                num(zc->rate[i]).c_str());
                  g_points.emplace_back(row);
                }
                count = static_cast<std::size_t>(zc->numItems);
              }
              char row[200];
              std::snprintf(row, sizeof row, "    {%d, %d, %ld, %ld, %ld, %ld, %s, %zu, %zu},",
                            strip, with_disc, freq, dcc, interp, conv,
                            rc == SUCCESS ? "true" : "false", offset, count);
              g_cases.emplace_back(row);
              JpmcdsZCFree(zc);
            }
          }
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Zero curves bootstrapped from swap benchmark strips by JpmcdsZCAddSwaps, on
// both paths: floating-at-par and explicit floating valuation.
//
// The rates here are ROOTS, bounded by the solver's stopping criterion
// (|f| <= 1e-11, no x criterion, at most 50 iterations) and not by
// floating-point rounding. The replay compares at kSwapBootstrapRateTolerance,
// which is looser than every other golden file in this tree and deliberately
// so: two implementations taking different Brent step sequences can land on
// different points of the same flat region.
//
// The replay also asserts the thing no tolerance can express -- that every
// recorded benchmark re-prices to par off the curve built from it.
//
// Points live in one flat pool; each case carries an offset and a count.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_swap_bootstrap_golden
//   ./build/debug/gen_swap_bootstrap_golden > tests/golden/swap_bootstrap_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// A solved rate, not a rounded one. See the note above.
inline constexpr double kSwapBootstrapRateTolerance = 1.0e-7;

/// How closely a benchmark must re-price to par off its own curve. This is the
/// solver's own f-tolerance, loosened by one decade for the second
/// implementation's step sequence.
inline constexpr double kSwapBootstrapParTolerance = 1.0e-10;

struct RecordedCurvePoint {
  TDate date;
  double rate;
};

struct SwapBootstrapCase {
  int strip;
  int with_discount_curve;
  long frequency;
  long day_count;
  long interp;
  long conv;
  bool accepted;
  std::size_t first_point;
  std::size_t point_count;
};

)");

  std::printf("inline constexpr TDate kSwapBootstrapValueDate = %ld;\n", (long)value_date);
  std::printf("inline constexpr std::size_t kSwapBootstrapStubPoints = %d;\n", kStubPoints);
  std::printf("inline constexpr std::array<TDate, %d> kSwapBootstrapStubDates = {", kStubPoints);
  for (int i = 0; i < kStubPoints; ++i) {
    std::printf("%s%ld", i ? ", " : "", (long)stub_dates[i]);
  }
  std::printf("};\n");
  std::printf("inline constexpr std::array<double, %d> kSwapBootstrapStubRates = {", kStubPoints);
  for (int i = 0; i < kStubPoints; ++i) {
    std::printf("%s%s", i ? ", " : "", num(stub_rates[i]).c_str());
  }
  std::printf("};\n\n");

  std::printf("inline constexpr std::size_t kSwapBootstrapBenchmarks = %d;\n", kBenchmarks);
  for (int s_i = 0; s_i < 2; ++s_i) {
    const TDate *bd = s_i == 0 ? bench_dates_a : bench_dates_b;
    const double *br = s_i == 0 ? bench_rates_a : bench_rates_b;
    std::printf("inline constexpr std::array<TDate, %d> kSwapBenchmarkDates%d = {", kBenchmarks,
                s_i);
    for (int i = 0; i < kBenchmarks; ++i) {
      std::printf("%s%ld", i ? ", " : "", (long)bd[i]);
    }
    std::printf("};\n");
    std::printf("inline constexpr std::array<double, %d> kSwapBenchmarkRates%d = {", kBenchmarks,
                s_i);
    for (int i = 0; i < kBenchmarks; ++i) {
      std::printf("%s%s", i ? ", " : "", num(br[i]).c_str());
    }
    std::printf("};\n");
  }
  std::printf("\n");

  std::printf("inline constexpr std::size_t kSwapBootstrapDiscountPoints = 2;\n");
  std::printf("inline constexpr std::array<TDate, 2> kSwapBootstrapDiscountDates = {%ld, %ld};\n",
              (long)disc_dates[0], (long)disc_dates[1]);
  std::printf("inline constexpr std::array<double, 2> kSwapBootstrapDiscountRates = {%s, %s};\n\n",
              num(disc_rates[0]).c_str(), num(disc_rates[1]).c_str());

  std::printf("inline constexpr std::array<RecordedCurvePoint, %zu> kSwapBootstrapPoints{{\n",
              g_points.size());
  for (const std::string &r : g_points) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<SwapBootstrapCase, %zu> kSwapBootstrapCases{{\n",
              g_cases.size());
  for (const std::string &r : g_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");

  JpmcdsFreeTCurve(disc);
  return 0;
}
