// Emits tests/golden/bootstrap_cases.hpp -- recorded clean spread curves.
//
// Each case carries the quotes that produced it, so a recorded rate is never
// checked against inputs that drifted. Infeasible combinations are recorded
// too, with n == 0: agreeing to REJECT is as much a part of the contract as
// agreeing on a number, and roughly a tenth of the realistic quote space is
// not bootstrappable at high recovery.
//
// Licence note: these are data derived from running the upstream model,
// exempt under section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_bootstrap_golden
//   ./build/debug/gen_bootstrap_golden > tests/golden/bootstrap_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cds.h"
#include "cerror.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "stub.h"
#include "tcurve.h"
}

namespace {

constexpr std::size_t kMaxBenchmarks = 8;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

TDate ymd(int y, int m, int d) { return JpmcdsDate(y, m, d); }

template <class T> std::string render(const std::vector<T> &v, std::size_t n) {
  std::string out = "{{";
  for (std::size_t i = 0; i < kMaxBenchmarks; ++i) {
    out += (i ? ", " : "");
    if (i >= n) {
      out += std::is_floating_point_v<T> ? "0.0" : "0";
    } else if constexpr (std::is_floating_point_v<T>) {
      out += num(static_cast<double>(v[i]));
    } else {
      out += std::to_string(v[i]);
    }
  }
  return out + "}}";
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const TDate today = ymd(2024, 6, 20);
  std::vector<TDate> disc_dates{ymd(2024, 6, 20), ymd(2025, 6, 20), ymd(2027, 6, 20),
                                ymd(2031, 6, 20), ymd(2036, 6, 20)};
  std::vector<double> disc_rates{0.028, 0.031, 0.034, 0.037, 0.039};
  TCurve *discount = JpmcdsMakeTCurve(today, disc_dates.data(), disc_rates.data(),
                                      static_cast<int>(disc_dates.size()), 1.0, JPMCDS_ACT_365F);

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded clean spread curves, with the quotes and discount curve that\n"
              "// produced them. Data derived from running the upstream model\n"
              "// (licence 4(b)).\n"
              "//\n"
              "// n == 0 records a combination upstream refused to bootstrap. Those are\n"
              "// part of the contract: the feasible region has a boundary and both\n"
              "// implementations must put it in the same place.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_bootstrap_golden\n"
              "//   ./build/debug/gen_bootstrap_golden > tests/golden/bootstrap_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kBootstrapMax = 8;\n\n"
              "struct BootstrapCurveSpec {\n  long base;\n  std::size_t n;\n"
              "  std::array<long, kBootstrapMax> dates;\n"
              "  std::array<double, kBootstrapMax> rates;\n};\n\n"
              "/// `n` is the number of benchmarks AND of solved rates; 0 means upstream\n"
              "/// refused. `rates` are ANNUALLY compounded ACT/365F, as published.\n"
              "struct BootstrapCase {\n"
              "  long today;\n  long start;\n  std::size_t nb;\n"
              "  std::array<long, kBootstrapMax> ends;\n"
              "  std::array<double, kBootstrapMax> coupons;\n"
              "  double recovery;\n  long dcc;\n  int interval_months;\n"
              "  bool pay_accrual_on_default;\n"
              "  std::size_t n;\n  std::array<double, kBootstrapMax> rates;\n};\n\n");

  std::printf("inline constexpr long kBootstrapToday = %ld;\n\n", static_cast<long>(today));
  std::printf("inline constexpr BootstrapCurveSpec kBootstrapDiscountCurve = "
              "{%ld, %zu, %s, %s};\n\n",
              static_cast<long>(today), disc_dates.size(), render(disc_dates, 5).c_str(),
              render(disc_rates, 5).c_str());

  const std::vector<TDate> maturities{ymd(2025, 6, 20), ymd(2026, 6, 20), ymd(2027, 6, 20),
                                      ymd(2029, 6, 20), ymd(2031, 6, 20), ymd(2034, 6, 20)};
  const std::vector<std::vector<double>> ladders{
      {0.0025, 0.0030, 0.0035, 0.0045, 0.0055, 0.0065}, // tight, upward
      {0.01, 0.012, 0.014, 0.017, 0.019, 0.021},        // normal, upward
      {0.05, 0.048, 0.046, 0.044, 0.042, 0.040},        // inverted
      {0.05, 0.055, 0.06, 0.07, 0.08, 0.09},            // distressed
      {0.08, 0.09, 0.10, 0.12, 0.14, 0.16},             // very distressed
      {0.01, 0.01, 0.01, 0.01, 0.01, 0.01},             // flat
      {0.002},                                          // single
      {0.01, 0.02},                                     // two
      {0.003, 0.004, 0.005},                            // three
  };

  std::vector<std::string> rows;
  for (const std::vector<double> &quotes : ladders) {
    const std::size_t nb = quotes.size();
    std::vector<TDate> ends(maturities.begin(),
                            maturities.begin() + static_cast<std::ptrdiff_t>(nb));
    for (double recovery : {0.0, 0.2, 0.4, 0.6, 0.8}) {
      for (long dcc : {JPMCDS_ACT_360, JPMCDS_ACT_365F}) {
        for (int months : {3, 6}) {
          for (bool accrual : {true, false}) {
            std::vector<double> coupons = quotes;
            TDateInterval iv{months, 'M', 0};
            TStubMethod stub;
            stub.stubAtEnd = FALSE;
            stub.longStub = FALSE;
            TCurve *cc = JpmcdsCleanSpreadCurve(today, discount, today, today, today,
                                                static_cast<long>(nb), ends.data(), coupons.data(),
                                                nullptr, recovery, accrual ? TRUE : FALSE, &iv, dcc,
                                                &stub, 'N', const_cast<char *>("NONE"));
            std::vector<double> rates;
            std::size_t n = 0;
            if (cc != nullptr) {
              n = static_cast<std::size_t>(cc->fNumItems);
              for (int i = 0; i < cc->fNumItems; ++i) {
                rates.push_back(cc->fArray[i].fRate);
              }
              JpmcdsFreeTCurve(cc);
            }
            rows.push_back("    {" + std::to_string(today) + ", " + std::to_string(today) + ", " +
                           std::to_string(nb) + ", " + render(ends, nb) + ", " +
                           render(coupons, nb) + ", " + num(recovery) + ", " + std::to_string(dcc) +
                           ", " + std::to_string(months) + ", " + (accrual ? "true" : "false") +
                           ", " + std::to_string(n) + ", " + render(rates, n) + "},");
          }
        }
      }
    }
  }
  std::printf("inline constexpr std::array<BootstrapCase, %zu> kBootstrapCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");

  JpmcdsFreeTCurve(discount);
  return 0;
}
