// Emits tests/golden/curve_instruments_cases.hpp.
//
// Records curves produced by JpmcdsZCCash and JpmcdsZCSwaps -- the two entry
// points a caller actually drives.
//
// Cash rates are a direct conversion and compare tightly. Swap rates are
// ROOTS, bounded by the solver's stopping criterion rather than by rounding,
// so they compare at the solver tier. The two are recorded in SEPARATE case
// arrays precisely so each gets the tolerance it deserves -- merging them
// would force the loose one on both.
//
// Points live in one flat pool per kind, with an offset and a count per case.
//
// The NULL-curve inputs are not recorded: with a positive instrument count
// both validators dereference the NULL they just reported (UPSTREAM-34), and
// recording that would mean running undefined behaviour to produce a committed
// file. The zero-count shortcut, which is safe, IS recorded.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_curve_instruments_golden
//   ./build/debug/gen_curve_instruments_golden > tests/golden/curve_instruments_cases.hpp

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
#include "gtozc.h"
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

std::vector<std::string> g_cash_points;
std::vector<std::string> g_cash_cases;
std::vector<std::string> g_swap_points;
std::vector<std::string> g_swap_cases;

std::size_t record(const TCurve *tc, std::vector<std::string> &pool) {
  if (tc == nullptr) {
    return 0;
  }
  for (int i = 0; i < tc->fNumItems; ++i) {
    char row[120];
    std::snprintf(row, sizeof row, "    {%ld, %s},", (long)tc->fArray[i].fDate,
                  num(tc->fArray[i].fRate).c_str());
    pool.emplace_back(row);
  }
  return static_cast<std::size_t>(tc->fNumItems);
}

struct Stub {
  int id;
  int count;
  TDate dates[3];
  double rates[3];
};

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate base = ymd(2024, 1, 15);

  Stub stubs[4];
  stubs[0] = {0, 1, {ymd(2024, 2, 15), 0, 0}, {0.0220, 0, 0}};
  stubs[1] = {1, 2, {ymd(2024, 2, 15), ymd(2025, 7, 15), 0}, {0.0220, 0.0270, 0}};
  stubs[2] = {2, 2, {ymd(2024, 2, 15), ymd(2027, 1, 15), 0}, {0.0220, 0.0310, 0}};
  stubs[3] = {
      3, 3, {ymd(2024, 2, 15), ymd(2024, 8, 15), ymd(2026, 2, 15)}, {0.0220, 0.0250, 0.0290}};

  const long dccs[] = {JPMCDS_ACT_360, JPMCDS_ACT_365F, JPMCDS_B30_360};
  const long convs[] = {JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_FOLLOW};
  const long freqs[] = {1, 2, 4};

  /* ---- cash ---- */
  TDate cash_a[1] = {ymd(2024, 4, 15)};
  double cash_ar[1] = {0.0230};
  TDate cash_b[3] = {ymd(2024, 4, 15), ymd(2024, 7, 15), ymd(2025, 1, 15)};
  double cash_br[3] = {0.0230, 0.0245, 0.0260};
  TDate cash_c[2] = {ymd(2024, 2, 20), ymd(2024, 3, 20)};
  double cash_cr[2] = {0.0215, 0.0225};
  TDate cash_d[2] = {ymd(2026, 6, 15), ymd(2026, 12, 15)}; /* past every stub */
  double cash_dr[2] = {0.0305, 0.0315};
  TDate cash_e[3] = {ymd(2024, 2, 29), ymd(2024, 8, 31), ymd(2025, 2, 28)}; /* month ends */
  double cash_er[3] = {0.0225, 0.0255, 0.0275};

  for (Stub &stub : stubs) {
    for (int qs = 0; qs < 5; ++qs) {
      TDate *qd_all[5] = {cash_a, cash_b, cash_c, cash_d, cash_e};
      double *qr_all[5] = {cash_ar, cash_br, cash_cr, cash_dr, cash_er};
      const int qn_all[5] = {1, 3, 2, 2, 3};
      TDate *qd = qd_all[qs];
      double *qr = qr_all[qs];
      const int qn = qn_all[qs];
      for (const long dcc : dccs) {
        TCurve *in =
            JpmcdsMakeTCurve(base, stub.dates, stub.rates, stub.count, 1.0, JPMCDS_ACT_365F);
        if (in == nullptr) {
          continue;
        }
        TCurve *out = JpmcdsZCCash(in, qd, qr, qn, dcc);
        const std::size_t offset = g_cash_points.size();
        const std::size_t count = record(out, g_cash_points);
        char row[200];
        std::snprintf(row, sizeof row, "    {%d, %d, %ld, %s, %zu, %zu},", stub.id, qs, dcc,
                      out != nullptr ? "true" : "false", offset, count);
        g_cash_cases.emplace_back(row);
        JpmcdsFreeTCurve(out);
        JpmcdsFreeTCurve(in);
      }
    }
  }

  /* ---- swaps ---- */
  TDate strip_a[4] = {ymd(2026, 1, 15), ymd(2027, 1, 15), ymd(2029, 1, 15), ymd(2031, 1, 15)};
  TDate strip_b[3] = {ymd(2026, 4, 20), ymd(2027, 7, 20), ymd(2029, 10, 20)};
  TDate strip_c[2] = {ymd(2032, 1, 15), ymd(2034, 1, 15)}; /* far past every stub */
  double strip_rates[4] = {0.0280, 0.0295, 0.0320, 0.0340};

  for (Stub &stub : stubs) {
    for (int sp = 0; sp < 3; ++sp) {
      TDate *sd_all[3] = {strip_a, strip_b, strip_c};
      const int sn_all[3] = {4, 3, 2};
      TDate *sd = sd_all[sp];
      const int sn = sn_all[sp];
      for (const long freq : freqs) {
        for (const long conv : convs) {
          TCurve *in =
              JpmcdsMakeTCurve(base, stub.dates, stub.rates, stub.count, 1.0, JPMCDS_ACT_365F);
          if (in == nullptr) {
            continue;
          }
          TCurve *out = JpmcdsZCSwaps(in, nullptr, sd, strip_rates, sn, static_cast<int>(freq),
                                      static_cast<int>(freq), JPMCDS_ACT_360, JPMCDS_ACT_360, 'M',
                                      static_cast<TBadDayAndStubPos>(conv), (char *)"NONE");
          const std::size_t offset = g_swap_points.size();
          const std::size_t count = record(out, g_swap_points);
          char row[200];
          std::snprintf(row, sizeof row, "    {%d, %d, %ld, %ld, %s, %zu, %zu},", stub.id, sp, freq,
                        conv, out != nullptr ? "true" : "false", offset, count);
          g_swap_cases.emplace_back(row);
          JpmcdsFreeTCurve(out);
          JpmcdsFreeTCurve(in);
        }
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Curves produced by JpmcdsZCCash and JpmcdsZCSwaps.
//
// The two kinds are recorded SEPARATELY so each gets the tolerance it
// deserves. Cash rates are a direct conversion and compare at
// kCashCurveTolerance; swap rates are roots bounded by the solver's stopping
// criterion and compare at kSwapCurveTolerance. Merging them would force the
// loose tolerance on both.
//
// NULL-curve inputs with a positive instrument count are NOT recorded: both
// validators dereference the NULL they just reported (UPSTREAM-34), and
// recording that would mean running undefined behaviour to produce a committed
// file.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_curve_instruments_golden
//   ./build/debug/gen_curve_instruments_golden > tests/golden/curve_instruments_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Cash rates are converted, not solved. The file crossed a compiler, so this
/// is the accumulating tier rather than ULPs.
inline constexpr double kCashCurveTolerance = 1.0e-12;

/// Swap rates are roots. See the note above.
inline constexpr double kSwapCurveTolerance = 1.0e-7;

struct RecordedInstrumentPoint {
  TDate date;
  double rate;
};

struct CashCurveCase {
  int stub;
  int quotes;
  long day_count;
  bool accepted;
  std::size_t first_point;
  std::size_t point_count;
};

struct SwapCurveCase {
  int stub;
  int strip;
  long frequency;
  long conv;
  bool accepted;
  std::size_t first_point;
  std::size_t point_count;
};

)");

  std::printf("inline constexpr TDate kInstrumentsBaseDate = %ld;\n\n", (long)base);

  const auto emit_array = [](const char *name, const TDate *dates, const double *rates, int n) {
    std::printf("inline constexpr std::size_t %sCount = %d;\n", name, n);
    std::printf("inline constexpr std::array<TDate, %d> %sDates = {", n, name);
    for (int i = 0; i < n; ++i) {
      std::printf("%s%ld", i ? ", " : "", (long)dates[i]);
    }
    std::printf("};\n");
    std::printf("inline constexpr std::array<double, %d> %sRates = {", n, name);
    for (int i = 0; i < n; ++i) {
      std::printf("%s%s", i ? ", " : "", num(rates[i]).c_str());
    }
    std::printf("};\n");
  };

  for (Stub &stub : stubs) {
    char name[32];
    std::snprintf(name, sizeof name, "kStub%d", stub.id);
    emit_array(name, stub.dates, stub.rates, stub.count);
  }
  std::printf("\n");
  emit_array("kCashQuotes0", cash_a, cash_ar, 1);
  emit_array("kCashQuotes1", cash_b, cash_br, 3);
  emit_array("kCashQuotes2", cash_c, cash_cr, 2);
  emit_array("kCashQuotes3", cash_d, cash_dr, 2);
  emit_array("kCashQuotes4", cash_e, cash_er, 3);
  std::printf("\n");
  emit_array("kSwapStrip0", strip_a, strip_rates, 4);
  emit_array("kSwapStrip1", strip_b, strip_rates, 3);
  emit_array("kSwapStrip2", strip_c, strip_rates, 2);
  std::printf("\n");

  std::printf("inline constexpr std::array<RecordedInstrumentPoint, %zu> kCashCurvePoints{{\n",
              g_cash_points.size());
  for (const std::string &r : g_cash_points) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");
  std::printf("inline constexpr std::array<CashCurveCase, %zu> kCashCurveCases{{\n",
              g_cash_cases.size());
  for (const std::string &r : g_cash_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<RecordedInstrumentPoint, %zu> kSwapCurvePoints{{\n",
              g_swap_points.size());
  for (const std::string &r : g_swap_points) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");
  std::printf("inline constexpr std::array<SwapCurveCase, %zu> kSwapCurveCases{{\n",
              g_swap_cases.size());
  for (const std::string &r : g_swap_cases) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
