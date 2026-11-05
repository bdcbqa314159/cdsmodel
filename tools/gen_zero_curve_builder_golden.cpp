// Emits tests/golden/zero_curve_builder_cases.hpp.
//
// Records the builder's accumulated points -- date, rate AND cached discount
// factor -- because the discount cache is the thing this type adds over a
// finished curve, and a recording that omitted it would verify half the type.
//
// Sequences that reach UPSTREAM-16 are deliberately NOT recorded: upstream
// leaves the curve mutated and reports failure there, so the "result" is a
// state no caller asked for. The divergence is asserted in the differential
// suite instead.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_zero_curve_builder_golden
//   ./build/debug/gen_zero_curve_builder_golden > tests/golden/zero_curve_builder_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "dateconv.h"
#include "ldate.h"
#include "zcprvt.h"
}

namespace {

constexpr std::size_t kMaxPoints = 12;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

TDate ymd(int y, int m, int d) { return JpmcdsDate(y, m, d); }

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded curve-builder states: the accumulated points with their\n"
              "// CACHED discount factors. Data derived from running the upstream model\n"
              "// (licence 4(b)).\n"
              "//\n"
              "// Sequences reaching UPSTREAM-16 are omitted -- upstream leaves the\n"
              "// curve mutated and reports failure, so there is no result to record.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_zero_curve_builder_golden\n"
              "//   ./build/debug/gen_zero_curve_builder_golden > "
              "tests/golden/zero_curve_builder_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kBuilderMax = 12;\n\n"
              "/// `mode`: 0 = add_rate, 1 = add_discount_factor, 2 = add_general_rate.\n"
              "struct BuilderCase {\n"
              "  long value_date;\n  long basis;\n  long day_count;\n  int mode;\n"
              "  long entry_basis;\n  long entry_day_count;\n"
              "  std::size_t nb;\n  std::array<long, kBuilderMax> dates;\n"
              "  std::array<double, kBuilderMax> inputs;\n"
              "  bool all_added;\n"
              "  std::size_t n;\n  std::array<long, kBuilderMax> out_dates;\n"
              "  std::array<double, kBuilderMax> out_rates;\n"
              "  std::array<double, kBuilderMax> out_discounts;\n};\n\n"
              "struct BuilderCreateCase {\n  int expected_points;\n  long basis;\n"
              "  long day_count;\n  bool accepted;\n};\n\n");

  // Construction: which (n, basis, day count) triples are admitted.
  std::vector<std::string> creates;
  for (long basis : {-2L, -1L, 0L, 1L, 2L, 4L, 12L, 365L, 5000L, 5001L, 5002L, 10000L}) {
    for (int n : {-1, 0, 1, 8, 64}) {
      for (long dcc : {JPMCDS_ACT_365F, JPMCDS_ACT_360, JPMCDS_B30_360}) {
        ZCurve *zc = JpmcdsZCMake(ymd(2024, 1, 1), n, basis, dcc);
        creates.push_back("    {" + std::to_string(n) + ", " + std::to_string(basis) + ", " +
                          std::to_string(dcc) + ", " + (zc ? "true" : "false") + "},");
        JpmcdsZCFree(zc);
      }
    }
  }
  std::printf("inline constexpr std::array<BuilderCreateCase, %zu> kBuilderCreateCases{{\n",
              creates.size());
  for (const std::string &r : creates) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  const TDate vd = ymd(2024, 1, 1);
  // Deliberately unsorted, with one exact duplicate, to exercise the insert.
  const std::vector<std::pair<TDate, double>> entries{
      {ymd(2029, 1, 1), 0.05},  {ymd(2025, 1, 1), 0.03}, {ymd(2034, 1, 1), 0.055},
      {ymd(2026, 7, 1), 0.04},  {ymd(2025, 1, 1), 0.03}, {ymd(2027, 1, 1), -0.01},
      {ymd(2031, 3, 15), 0.052}};

  std::vector<std::string> rows;
  for (long basis : {1L, 2L, 4L, 12L, 5000L}) {
    for (long dcc : {JPMCDS_ACT_365F, JPMCDS_ACT_360}) {
      for (int mode = 0; mode <= 2; ++mode) {
        for (long entry_basis : {1L, 2L, 5000L}) {
          if (mode != 2 && entry_basis != 1L) {
            continue; // entry terms only matter for add_general_rate
          }
          ZCurve *zc = JpmcdsZCMake(vd, 16, basis, dcc);
          if (zc == nullptr) {
            continue;
          }
          std::vector<TDate> in_dates;
          std::vector<double> inputs;
          bool all = true;
          bool skip = false;
          for (const auto &[date, rate] : entries) {
            double value = rate;
            int rc = FAILURE;
            if (mode == 0) {
              rc = JpmcdsZCAddRate(zc, date, value);
            } else if (mode == 1) {
              // Feed a plausible discount factor rather than a rate.
              value = 1.0 / (1.0 + rate * 5.0);
              rc = JpmcdsZCAddDiscountFactor(zc, date, value);
            } else {
              rc = JpmcdsZCAddGenRate(zc, date, value, entry_basis, dcc);
              // UPSTREAM-16 territory: a failure here may have mutated the
              // curve, so the state is not recordable.
              if (rc != SUCCESS) {
                skip = true;
              }
            }
            in_dates.push_back(date);
            inputs.push_back(value);
            all = all && rc == SUCCESS;
          }
          if (skip) {
            JpmcdsZCFree(zc);
            continue;
          }
          const auto arr_long = [](const std::vector<TDate> &v) {
            std::string out = "{{";
            for (std::size_t i = 0; i < kMaxPoints; ++i) {
              out += (i ? ", " : "");
              out += i < v.size() ? std::to_string(v[i]) : "0";
            }
            return out + "}}";
          };
          const auto arr_dbl = [](const std::vector<double> &v) {
            std::string out = "{{";
            for (std::size_t i = 0; i < kMaxPoints; ++i) {
              out += (i ? ", " : "");
              out += i < v.size() ? num(v[i]) : "0.0";
            }
            return out + "}}";
          };
          std::vector<TDate> od;
          std::vector<double> orr;
          std::vector<double> odd;
          for (int i = 0; i < zc->numItems; ++i) {
            od.push_back(zc->date[i]);
            orr.push_back(zc->rate[i]);
            odd.push_back(zc->discount[i]);
          }
          rows.push_back("    {" + std::to_string(vd) + ", " + std::to_string(basis) + ", " +
                         std::to_string(dcc) + ", " + std::to_string(mode) + ", " +
                         std::to_string(entry_basis) + ", " + std::to_string(dcc) + ", " +
                         std::to_string(in_dates.size()) + ", " + arr_long(in_dates) + ", " +
                         arr_dbl(inputs) + ", " + (all ? "true" : "false") + ", " +
                         std::to_string(od.size()) + ", " + arr_long(od) + ", " + arr_dbl(orr) +
                         ", " + arr_dbl(odd) + "},");
          JpmcdsZCFree(zc);
        }
      }
    }
  }
  std::printf("inline constexpr std::array<BuilderCase, %zu> kBuilderCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
