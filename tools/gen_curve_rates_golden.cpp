// Emits tests/golden/curve_rates_cases.hpp.
//
// Records the rates a zero curve implies: forwards between two dates, and par
// coupons for an instrument. Both are quotients of accumulated present values,
// so these compare at the accumulating tier -- kCurveRatesTolerance -- not in
// ULPs. The header records which rather than leaving the replay to guess.
//
// Every call resets the order-check global, because everything here routes
// through JpmcdsInterpRateExact, which leaks it on its extrapolation paths
// (UPSTREAM-28). Without the reset the recording would depend on its own
// iteration order.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_curve_rates_golden
//   ./build/debug/gen_curve_rates_golden > tests/golden/curve_rates_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "bsearch.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
#include "zr2coup.h"
#include "zr2fwd.h"
}

namespace {

constexpr std::size_t kMaxPoints = 8;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const TDate base = JpmcdsDate(2024, 1, 1);
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (int i = 1; i <= 8; ++i) {
    dates.push_back(JpmcdsDate(2024 + i, 1, 1));
    rates.push_back(0.02 + 0.003 * static_cast<double>(i));
  }
  std::string dates_literal = "{";
  std::string rates_literal = "{";
  for (std::size_t i = 0; i < kMaxPoints; ++i) {
    const char *sep = i == 0 ? "" : ", ";
    dates_literal += sep;
    rates_literal += sep;
    dates_literal += i < dates.size() ? std::to_string((long)dates[i]) : "0";
    rates_literal += i < rates.size() ? num(rates[i]) : "0";
  }
  dates_literal += "}";
  rates_literal += "}";

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded forward rates and par coupon rates off one zero curve. Both\n"
              "// are quotients of accumulated present values, so compare at\n"
              "// kCurveRatesTolerance rather than in ULPs.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_curve_rates_golden\n"
              "//   ./build/debug/gen_curve_rates_golden > tests/golden/curve_rates_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// Relative. Both quantities are ratios of sums, so the accumulating tier.\n"
              "inline constexpr double kCurveRatesTolerance = 1.0e-12;\n\n"
              "/// The one curve every case below is computed off.\n"
              "inline constexpr TDate kCurveRatesBaseDate = %ld;\n"
              "inline constexpr std::size_t kCurveRatesPoints = %zu;\n"
              "inline constexpr std::array<TDate, %zu> kCurveRatesDates = %s;\n"
              "inline constexpr std::array<double, %zu> kCurveRatesRates = %s;\n\n"
              "struct ForwardRateCase {\n"
              "  double basis;\n"
              "  long curve_day_count;\n"
              "  long interp;\n"
              "  TDate start;\n"
              "  TDate end;\n"
              "  long quote_day_count;\n"
              "  long quote_basis;\n"
              "  bool accepted;\n"
              "  double rate;\n"
              "};\n\n",
              (long)base, dates.size(), kMaxPoints, dates_literal.c_str(), kMaxPoints,
              rates_literal.c_str());

  const double bases[] = {1.0, 2.0, 12.0, 365.0, 5000.0};
  const long dccs[] = {JPMCDS_ACT_365F, JPMCDS_ACT_360};
  const long interps[] = {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS};

  std::vector<std::string> forwards;
  for (const double basis : bases) {
    for (const long curve_dcc : dccs) {
      TCurve *zc =
          JpmcdsMakeTCurve(base, dates.data(), rates.data(), (int)dates.size(), basis, curve_dcc);
      if (zc == nullptr) {
        continue;
      }
      for (const long interp : interps) {
        struct Span {
          TDate from, to;
        };
        const Span spans[] = {{JpmcdsDate(2025, 1, 1), JpmcdsDate(2026, 1, 1)},
                              {JpmcdsDate(2026, 7, 1), JpmcdsDate(2027, 3, 15)},
                              {JpmcdsDate(2027, 1, 1), JpmcdsDate(2025, 1, 1)},
                              {JpmcdsDate(2028, 1, 1), JpmcdsDate(2034, 1, 1)}};
        for (const Span &span : spans) {
          for (const long quote_basis : {0L, 1L, 2L, 4L}) {
            for (const long quote_dcc : dccs) {
              JpmcdsBSearchCheckOrder(TRUE);
              double rate = 0.0;
              const bool accepted =
                  JpmcdsForwardFromZCurve(zc, interp, span.from, span.to, quote_dcc, quote_basis,
                                          &rate) == SUCCESS;
              JpmcdsBSearchCheckOrder(TRUE);
              char row[200];
              std::snprintf(row, sizeof row, "    {%s, %ld, %ld, %ld, %ld, %ld, %ld, %s, %s},",
                            num(basis).c_str(), curve_dcc, interp, (long)span.from, (long)span.to,
                            quote_dcc, quote_basis, accepted ? "true" : "false",
                            num(accepted ? rate : 0.0).c_str());
              forwards.emplace_back(row);
            }
          }
        }
      }
      JpmcdsFreeTCurve(zc);
    }
  }
  std::printf("inline constexpr std::array<ForwardRateCase, %zu> kForwardRateCases{{\n",
              forwards.size());
  for (const std::string &r : forwards) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ------------------------------------------------------------------
  std::printf("/// One par coupon rate. `bad_day` is applied to both the accrual and\n"
              "/// the payment side, as JpmcdsZerosToCouponsPointAdj's caller does.\n"
              "struct ParCouponCase {\n"
              "  double basis;\n"
              "  long interp;\n"
              "  TDate start;\n"
              "  TDate maturity;\n"
              "  int periods;\n"
              "  long coupon_day_count;\n"
              "  bool stub_at_end;\n"
              "  long bad_day;\n"
              "  bool accepted;\n"
              "  double rate;\n"
              "};\n\n");

  std::vector<std::string> coupons;
  struct Span {
    int y1, m1, d1, y2, m2, d2;
  };
  const Span spans[] = {{2024, 1, 1, 2029, 1, 1},
                        {2024, 2, 20, 2030, 5, 10},
                        {2025, 1, 1, 2028, 1, 1},
                        {2024, 1, 6, 2029, 1, 6}};
  const long bad_days[] = {'N', 'F', 'P', 'M'};

  for (const double basis : {1.0, 2.0, 5000.0}) {
    TCurve *zc = JpmcdsMakeTCurve(base, dates.data(), rates.data(), (int)dates.size(), basis,
                                  JPMCDS_ACT_365F);
    if (zc == nullptr) {
      continue;
    }
    for (const long interp : interps) {
      for (const Span &span : spans) {
        const TDate start = JpmcdsDate(span.y1, span.m1, span.d1);
        const TDate maturity = JpmcdsDate(span.y2, span.m2, span.d2);
        for (const int periods : {3, 6, 12}) {
          TDateInterval iv;
          if (JpmcdsMakeDateInterval(periods, 'M', &iv) != SUCCESS) {
            continue;
          }
          for (const long dcc : dccs) {
            for (const int stub_at_end : {0, 1}) {
              for (const long bad_day : bad_days) {
                // Only sweep conventions on the unflagged shape; otherwise
                // this file grows past usefulness.
                if (bad_day != 'N' && (periods != 6 || dcc != JPMCDS_ACT_365F)) {
                  continue;
                }
                JpmcdsBSearchCheckOrder(TRUE);
                double rate = 0.0;
                const bool accepted =
                    JpmcdsZerosToCouponsPointAdj(zc, interp, start, &iv, maturity, dcc,
                                                 stub_at_end ? TRUE : FALSE, bad_day, bad_day,
                                                 (char *)"NONE", &rate) == SUCCESS;
                JpmcdsBSearchCheckOrder(TRUE);
                char row[200];
                std::snprintf(row, sizeof row, "    {%s, %ld, %ld, %ld, %d, %ld, %s, %ld, %s, %s},",
                              num(basis).c_str(), interp, (long)start, (long)maturity, periods, dcc,
                              stub_at_end ? "true" : "false", bad_day, accepted ? "true" : "false",
                              num(accepted ? rate : 0.0).c_str());
                coupons.emplace_back(row);
              }
            }
          }
        }
      }
    }
    JpmcdsFreeTCurve(zc);
  }
  JpmcdsBSearchCheckOrder(TRUE);

  std::printf("inline constexpr std::array<ParCouponCase, %zu> kParCouponCases{{\n",
              coupons.size());
  for (const std::string &r : coupons) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
