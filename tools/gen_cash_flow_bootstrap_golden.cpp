// Emits tests/golden/cash_flow_bootstrap_cases.hpp.
//
// Records the whole curve each bootstrap produces -- every date, rate and
// cached discount -- because the solved rate alone would not show whether the
// remaining cash flow dates were placed, and that is exactly what UPSTREAM-23
// changes.
//
// These are SOLVER outputs. The recorded rates are pinned to the root finder's
// f-tolerance of 1e-10 on a price of order 1, not to rounding, so the replay
// compares them at 1e-9 rather than in ULPs. The tolerance is a property of
// the problem and is recorded here rather than guessed at the test.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_cash_flow_bootstrap_golden
//   ./build/debug/gen_cash_flow_bootstrap_golden > tests/golden/cash_flow_bootstrap_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "interp.h"
#include "ldate.h"
#include "zcprvt.h"
}

namespace {

constexpr std::size_t kMaxPoints = 16;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

struct Bond {
  double coupon;
  int first_year;
  int years;
  double price;
  double seed_rate;
  const char *name;
};

const Bond kBonds[] = {
    {0.04, 2027, 5, 0.95, 0.030, "coupon 4%, discount"},
    {0.04, 2027, 5, 1.00, 0.030, "coupon 4%, par"},
    {0.04, 2027, 5, 1.08, 0.030, "coupon 4%, premium"},
    {0.00, 2027, 5, 0.88, 0.030, "zero coupon"},
    {0.00, 2027, 5, 1.04, -0.005, "zero coupon, negative curve"},
    {0.01, 2027, 5, 1.06, -0.005, "coupon 1%, negative curve"},
    {0.02, 2027, 3, 0.99, 0.001, "short, near-zero curve"},
    {0.06, 2028, 8, 1.12, 0.045, "long, high coupon"},
    {0.04, 2027, 5, 0.80, 0.030, "coupon 4%, deep discount"},
    {0.04, 2027, 5, 1.20, 0.030, "coupon 4%, deep premium"},
    {0.00, 2027, 2, 0.96, 0.030, "two payments only"},
    {0.03, 2027, 5, 1.00, 0.000, "flat zero seed curve"},
    {0.08, 2027, 6, 1.05, 0.070, "high coupon, high curve"},
    {0.01, 2030, 4, 0.93, 0.030, "starts well past the curve"},
    {0.02, 2027, 5, 1.02, -0.012, "coupon 2%, deeper negative curve"},
};

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded cash-flow bootstraps: the whole curve each one produces. The\n"
              "// rates are SOLVER outputs, pinned to the root finder's f-tolerance of\n"
              "// 1e-10 on a price of order 1 -- compare at kSolverTolerance, not in ULPs.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_cash_flow_bootstrap_golden\n"
              "//   ./build/debug/gen_cash_flow_bootstrap_golden > "
              "tests/golden/cash_flow_bootstrap_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// What a recorded bootstrapped rate can be compared to. Not a rounding\n"
              "/// tolerance: the root find stops on |f| < 1e-10 of PRICE, and the rate\n"
              "/// that corresponds to is whatever the instrument's sensitivity makes it.\n"
              "inline constexpr double kSolverTolerance = 1.0e-9;\n\n"
              "/// One bootstrap: a two-point seed curve, a bond, and the curve that came\n"
              "/// out. `interp` is 0 for linear and 124 for flat forwards.\n"
              "struct CashFlowBootstrapCase {\n"
              "  double coupon;\n"
              "  int first_year;\n"
              "  int years;\n"
              "  double price;\n"
              "  double seed_rate;\n"
              "  long basis;\n"
              "  long interp;\n"
              "  bool accepted;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> rates;\n"
              "  std::array<double, %zu> discounts;\n"
              "\n"
              "  /// Cash flow dates upstream did NOT place in the curve -- UPSTREAM-23's\n"
              "  /// `if (rate >= 0.0)` filter, recorded as data rather than described\n"
              "  /// in prose. The port places all of them, so a replay compares the\n"
              "  /// curves point-for-point only when this is empty, and otherwise\n"
              "  /// asserts that these dates are the whole of the difference.\n"
              "  std::size_t n_dropped;\n"
              "  std::array<TDate, %zu> dropped;\n"
              "};\n\n",
              kMaxPoints, kMaxPoints, kMaxPoints, kMaxPoints);

  std::vector<std::string> rows;
  const long bases[] = {1, 2};
  const long interps[] = {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS};

  for (const Bond &bond : kBonds) {
    for (const long basis : bases) {
      for (const long interp : interps) {
        ZCurve *zc = JpmcdsZCMake(JpmcdsDate(2024, 1, 1), 32, basis, JPMCDS_ACT_365F);
        if (zc == nullptr) {
          continue;
        }
        JpmcdsZCAddRate(zc, JpmcdsDate(2025, 1, 1), bond.seed_rate);
        JpmcdsZCAddRate(zc, JpmcdsDate(2026, 1, 1), bond.seed_rate * 1.3);

        std::vector<TDate> dates;
        std::vector<double> amounts;
        for (int i = 0; i < bond.years; ++i) {
          dates.push_back(JpmcdsDate(bond.first_year + i, 1, 1));
          amounts.push_back(i == bond.years - 1 ? 1.0 + bond.coupon : bond.coupon);
        }
        TCashFlowList *cfl =
            JpmcdsNewCFL(dates.data(), amounts.data(), static_cast<int>(dates.size()));
        const bool accepted = cfl != nullptr && JpmcdsZCAddCashFlowList(zc, cfl, bond.price, 0L,
                                                                        interp, nullptr) == SUCCESS;

        const std::size_t n =
            accepted ? static_cast<std::size_t>(zc->numItems) : static_cast<std::size_t>(0);
        std::string ds = "{";
        std::string rs = "{";
        std::string fs = "{";
        for (std::size_t i = 0; i < kMaxPoints; ++i) {
          const bool present = i < n;
          const char *sep = i == 0 ? "" : ", ";
          ds += sep;
          rs += sep;
          fs += sep;
          ds += present ? std::to_string(static_cast<long>(zc->date[i])) : "0";
          rs += present ? num(zc->rate[i]) : "0";
          fs += present ? num(zc->discount[i]) : "0";
        }
        ds += "}";
        rs += "}";
        fs += "}";

        // Which of the instrument's own dates are missing from the curve?
        std::vector<TDate> dropped;
        if (accepted) {
          for (const TDate date : dates) {
            if (JpmcdsZCFindDateExact(zc, date) < 0) {
              dropped.push_back(date);
            }
          }
        }
        std::string drop = "{";
        for (std::size_t i = 0; i < kMaxPoints; ++i) {
          drop += i == 0 ? "" : ", ";
          drop += i < dropped.size() ? std::to_string(static_cast<long>(dropped[i])) : "0";
        }
        drop += "}";

        char head[192];
        std::snprintf(head, sizeof head, "    {%s, %d, %d, %s, %s, %ld, %ld, %s, %zu, ",
                      num(bond.coupon).c_str(), bond.first_year, bond.years,
                      num(bond.price).c_str(), num(bond.seed_rate).c_str(), basis, interp,
                      accepted ? "true" : "false", n);
        char tail[32];
        std::snprintf(tail, sizeof tail, ", %zu, ", dropped.size());
        rows.emplace_back(std::string(head) + ds + ", " + rs + ", " + fs + tail + drop + "},");

        JpmcdsFreeCFL(cfl);
        JpmcdsZCFree(zc);
      }
    }
  }

  std::printf("inline constexpr std::array<CashFlowBootstrapCase, %zu> kCashFlowBootstrapCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
