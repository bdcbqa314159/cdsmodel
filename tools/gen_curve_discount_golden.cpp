// Emits tests/golden/curve_discount_cases.hpp.
//
// Records discount factors and cash flow present values off a finished curve,
// across basis, day count, interpolation type and where the date sits.
//
// The PV cases use amounts spanning eighteen orders of magnitude on purpose:
// a left-to-right sum of those is bit-identical only if the accumulation order
// matches, so the recording pins the order and not merely the total.
//
// Every call resets the order-check global first. JpmcdsInterpRateExact leaks
// it on its extrapolation paths (UPSTREAM-28), and without the reset this
// recording would depend on its own iteration order.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_curve_discount_golden
//   ./build/debug/gen_curve_discount_golden > tests/golden/curve_discount_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "bsearch.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "cxzerocurve.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
}

namespace {

constexpr std::size_t kMaxPoints = 6;
constexpr std::size_t kMaxFlows = 5;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded discount factors and cash flow present values off a finished\n"
              "// curve. The PV amounts span eighteen orders of magnitude, so the recorded\n"
              "// total pins the ACCUMULATION ORDER and not just the sum.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_curve_discount_golden\n"
              "//   ./build/debug/gen_curve_discount_golden > "
              "tests/golden/curve_discount_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One discount factor. `interp` is 0 for linear, 124 for flat forwards.\n"
              "struct DiscountDateCase {\n"
              "  TDate base_date;\n"
              "  double basis;\n"
              "  long day_count;\n"
              "  long interp;\n"
              "  TDate at;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> rates;\n"
              "  bool accepted;\n"
              "  double discount;\n"
              "  double zero_price;   ///< the other route to the same quantity\n"
              "  TDate first_date;    ///< JpmcdsCurveFirstDate, min(base, dates[0])\n"
              "};\n\n",
              kMaxPoints, kMaxPoints);

  struct Shape {
    int by, bm, bd;
    int first_year;
    int count;
  };
  const Shape shapes[] = {{2024, 1, 1, 2025, 5}, {2025, 6, 1, 2024, 4}};
  const double bases[] = {1.0, 2.0, 4.0, 12.0, 365.0, 5000.0};
  const long dccs[] = {JPMCDS_ACT_365F, JPMCDS_ACT_360};
  const long interps[] = {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS};

  std::vector<std::string> discounts;
  std::vector<std::string> pvs;

  for (const Shape &shape : shapes) {
    const TDate base = JpmcdsDate(shape.by, shape.bm, shape.bd);
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (int i = 0; i < shape.count; ++i) {
      dates.push_back(JpmcdsDate(shape.first_year + i, 1, 1));
      rates.push_back(0.02 + 0.004 * static_cast<double>(i + 1));
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

    for (const double basis : bases) {
      for (const long dcc : dccs) {
        TCurve *curve =
            JpmcdsMakeTCurve(base, dates.data(), rates.data(), (int)dates.size(), basis, dcc);
        if (curve == nullptr) {
          continue;
        }
        TDate first = 0;
        JpmcdsCurveFirstDate(curve, &first);

        for (const long interp : interps) {
          for (const TDate at : {JpmcdsDate(2020, 1, 1), base, dates.front(),
                                 JpmcdsDate(2026, 7, 4), dates.back(), JpmcdsDate(2035, 1, 1)}) {
            JpmcdsBSearchCheckOrder(TRUE);
            double discount = 0.0;
            const bool accepted = JpmcdsDiscountDate(at, curve, interp, &discount) == SUCCESS;
            const double zero_price = JpmcdsZeroPrice(curve, at);
            JpmcdsBSearchCheckOrder(TRUE);
            char head[200];
            std::snprintf(head, sizeof head, "    {%ld, %s, %ld, %ld, %ld, %zu, ", (long)base,
                          num(basis).c_str(), dcc, interp, (long)at, dates.size());
            discounts.emplace_back(std::string(head) + dates_literal + ", " + rates_literal + ", " +
                                   (accepted ? "true" : "false") + ", " +
                                   num(accepted ? discount : 0.0) + ", " + num(zero_price) + ", " +
                                   std::to_string((long)first) + "},");
          }
        }
        JpmcdsFreeTCurve(curve);
      }
    }
  }

  std::printf("inline constexpr std::array<DiscountDateCase, %zu> kDiscountDateCases{{\n",
              discounts.size());
  for (const std::string &r : discounts) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ------------------------------------------------------------------
  std::printf("/// One cash flow present value. The amounts span 1e-6 to 1e12 so the\n"
              "/// recorded total pins the accumulation order.\n"
              "struct CashFlowPvCase {\n"
              "  TDate base_date;\n"
              "  double basis;\n"
              "  long day_count;\n"
              "  long interp;\n"
              "  std::size_t n_points;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> rates;\n"
              "  std::size_t n_flows;\n"
              "  std::array<TDate, %zu> flow_dates;\n"
              "  std::array<double, %zu> flow_amounts;\n"
              "  bool accepted;\n"
              "  double pv;\n"
              "};\n\n",
              kMaxPoints, kMaxPoints, kMaxFlows, kMaxFlows);

  const TDate base = JpmcdsDate(2024, 1, 1);
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (int i = 0; i < 5; ++i) {
    dates.push_back(JpmcdsDate(2025 + i, 1, 1));
    rates.push_back(0.02 + 0.004 * static_cast<double>(i + 1));
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

  struct FlowSet {
    TDate dates[kMaxFlows];
    double amounts[kMaxFlows];
    std::size_t n;
  };
  const FlowSet flow_sets[] = {
      {{JpmcdsDate(2025, 1, 1), JpmcdsDate(2026, 1, 1), JpmcdsDate(2027, 1, 1),
        JpmcdsDate(2028, 1, 1), JpmcdsDate(2029, 1, 1)},
       {0.05, 0.05, 0.05, 0.05, 1.05},
       5},
      {{JpmcdsDate(2025, 1, 1), JpmcdsDate(2025, 7, 1), JpmcdsDate(2026, 1, 1),
        JpmcdsDate(2027, 1, 1), JpmcdsDate(2029, 1, 1)},
       {1.0e12, 0.05, 1.0e-6, 1.0e12, 1.05},
       5},
      {{JpmcdsDate(2026, 7, 4), 0, 0, 0, 0}, {1.0, 0, 0, 0, 0}, 1},
  };

  for (const double basis : {1.0, 2.0, 5000.0}) {
    for (const long dcc : dccs) {
      TCurve *curve =
          JpmcdsMakeTCurve(base, dates.data(), rates.data(), (int)dates.size(), basis, dcc);
      if (curve == nullptr) {
        continue;
      }
      for (const long interp : interps) {
        for (const FlowSet &set : flow_sets) {
          std::vector<TDate> flow_dates(set.dates, set.dates + set.n);
          std::vector<double> flow_amounts(set.amounts, set.amounts + set.n);
          TCashFlowList *cfl = JpmcdsNewCFL(flow_dates.data(), flow_amounts.data(), (int)set.n);
          JpmcdsBSearchCheckOrder(TRUE);
          double pv = 0.0;
          const bool accepted =
              cfl != nullptr && JpmcdsCashFlowPV(cfl, curve, interp, &pv) == SUCCESS;
          JpmcdsBSearchCheckOrder(TRUE);
          std::string fd = "{";
          std::string fa = "{";
          for (std::size_t i = 0; i < kMaxFlows; ++i) {
            const char *sep = i == 0 ? "" : ", ";
            fd += sep;
            fa += sep;
            fd += i < set.n ? std::to_string((long)set.dates[i]) : "0";
            fa += i < set.n ? num(set.amounts[i]) : "0";
          }
          fd += "}";
          fa += "}";
          char head[200];
          std::snprintf(head, sizeof head, "    {%ld, %s, %ld, %ld, %zu, ", (long)base,
                        num(basis).c_str(), dcc, interp, dates.size());
          pvs.emplace_back(std::string(head) + dates_literal + ", " + rates_literal + ", " +
                           std::to_string(set.n) + ", " + fd + ", " + fa + ", " +
                           (accepted ? "true" : "false") + ", " + num(accepted ? pv : 0.0) + "},");
          JpmcdsFreeCFL(cfl);
        }
      }
      JpmcdsFreeTCurve(curve);
    }
  }
  JpmcdsBSearchCheckOrder(TRUE);

  std::printf("inline constexpr std::array<CashFlowPvCase, %zu> kCashFlowPvCases{{\n", pvs.size());
  for (const std::string &r : pvs) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
