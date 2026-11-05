// Emits tests/golden/interp_curve_cases.hpp.
//
// Records interpolated rates across basis, day count, interpolation type and
// where the date sits relative to the curve and its base date. Flat forwards
// goes through pow and two rate conversions, so these carry a tolerance -- the
// header records which, rather than leaving the replay to guess.
//
// Includes fractional dates, which only ACT/360 and ACT/365F admit and which
// are the reason JpmcdsInterpRateExact takes a double at all.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_interp_curve_golden
//   ./build/debug/gen_interp_curve_golden > tests/golden/interp_curve_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "bsearch.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "interp.h"
#include "ldate.h"
#include "tcurve.h"
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

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded curve interpolations. Flat forwards goes through pow and two\n"
              "// rate conversions, so compare at kTolerance rather than exactly.\n"
              "// `interp` is 0 for linear and 124 for flat forwards; `at` is a double\n"
              "// because JpmcdsInterpRateExact admits fractional dates.\n"
              "// Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_interp_curve_golden\n"
              "//   ./build/debug/gen_interp_curve_golden > tests/golden/interp_curve_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// Four ULPs relative, the non-accumulating tier: one pow and two\n"
              "/// conversions, no summation.\n"
              "inline constexpr double kInterpCurveUlps = 4.0;\n\n"
              "struct InterpCurveCase {\n"
              "  TDate base_date;\n"
              "  double basis;\n"
              "  long day_count;\n"
              "  long interp;\n"
              "  double at;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> rates;\n"
              "  bool accepted;\n"
              "  double rate;\n"
              "};\n\n",
              kMaxPoints, kMaxPoints);

  std::vector<std::string> rows;
  const double bases[] = {1.0, 2.0, 4.0, 12.0, 365.0, 5000.0};
  const long dccs[] = {JPMCDS_ACT_365F, JPMCDS_ACT_360};
  const long interps[] = {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS};

  // Two curve shapes: one starting after the base date, and one straddling it
  // so the base-date branch is exercised.
  struct Shape {
    int base_year, base_month, base_day;
    int first_year;
    int count;
  };
  const Shape shapes[] = {{2024, 1, 1, 2025, 6}, {2025, 6, 1, 2024, 4}};

  for (const Shape &shape : shapes) {
    const TDate base = JpmcdsDate(shape.base_year, shape.base_month, shape.base_day);
    std::vector<TDate> dates;
    std::vector<double> rates;
    for (int i = 0; i < shape.count; ++i) {
      dates.push_back(JpmcdsDate(shape.first_year + i, 1, 1));
      rates.push_back(0.02 + 0.004 * static_cast<double>(i + 1));
    }
    if (dates.size() > kMaxPoints) {
      continue;
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
        for (const long interp : interps) {
          // Whole dates spanning before, on, inside and past the curve, plus
          // fractional offsets.
          std::vector<double> probes;
          for (const TDate probe :
               {JpmcdsDate(2020, 1, 1), base, dates.front(), JpmcdsDate(2026, 3, 15), dates.back(),
                JpmcdsDate(2040, 1, 1)}) {
            probes.push_back((double)probe);
          }
          const double anchor = (double)JpmcdsDate(2026, 1, 1);
          for (const double offset : {0.25, 0.5, 100.125}) {
            probes.push_back(anchor + offset);
          }
          for (const double at : probes) {
            // Reset the leaked global between calls, or the recording would
            // depend on its own iteration order -- UPSTREAM-28.
            JpmcdsBSearchCheckOrder(TRUE);
            double rate = 0.0;
            const bool accepted = JpmcdsInterpRateExact(at, curve, interp, &rate) == SUCCESS;
            char head[192];
            std::snprintf(head, sizeof head, "    {%ld, %s, %ld, %ld, %s, %zu, ", (long)base,
                          num(basis).c_str(), dcc, interp, num(at).c_str(), dates.size());
            rows.emplace_back(std::string(head) + dates_literal + ", " + rates_literal + ", " +
                              (accepted ? "true" : "false") + ", " + num(accepted ? rate : 0.0) +
                              "},");
          }
        }
        JpmcdsFreeTCurve(curve);
      }
    }
  }
  JpmcdsBSearchCheckOrder(TRUE);

  std::printf("inline constexpr std::array<InterpCurveCase, %zu> kInterpCurveCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
