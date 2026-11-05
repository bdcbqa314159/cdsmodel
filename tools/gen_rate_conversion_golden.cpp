// Emits tests/golden/rate_conversion_cases.hpp.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_rate_conversion_golden
//   ./build/debug/gen_rate_conversion_golden > tests/golden/rate_conversion_cases.hpp

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cerror.h"
#include "cfinanci.h"
#include "cgeneral.h"
}

namespace {

std::string num(double v) {
  if (std::isinf(v)) {
    return v > 0.0 ? "std::numeric_limits<double>::infinity()"
                   : "-std::numeric_limits<double>::infinity()";
  }
  if (std::isnan(v)) {
    return "std::numeric_limits<double>::quiet_NaN()";
  }
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf(
      "// GENERATED FILE -- DO NOT EDIT.\n"
      "//\n"
      "// Recorded rate <-> discount factor conversions and rate-validity\n"
      "// verdicts. Data derived from running the upstream model (licence 4(b)).\n"
      "//\n"
      "// Regenerate with a local upstream copy present:\n"
      "//   cmake --build --preset debug --target gen_rate_conversion_golden\n"
      "//   ./build/debug/gen_rate_conversion_golden > tests/golden/rate_conversion_cases.hpp\n"
      "\n#pragma once\n\n#include <array>\n#include <cstddef>\n#include <limits>\n\n"
      "namespace cdsmodel::golden {\n\n"
      "struct ConversionCase {\n  double value;\n  double year_fraction;\n  double basis;\n"
      "  bool forward_ok;\n  double discount;\n"
      "  bool backward_ok;\n  double rate;\n};\n\n"
      "struct ValidityCase {\n  double rate;\n  double year_fraction;\n  double basis;\n"
      "  bool valid;\n};\n\n");

  const std::vector<double> bases{0.0,  -2.0,  512.0, 5000.0, 1.0,  2.0, 4.0,
                                  12.0, 365.0, 1.5,   4.7,    -1.0, -3.0};
  const std::vector<double> values{0.05, 0.0, -0.05, 0.5, -0.5,   -0.999, -1.0,
                                   -1.5, 2.0, 100.0, 1.0, 0.9999, 1.0e-8};
  const std::vector<double> yfs{0.0, 1.0e-17, 0.25, 1.0, 5.0, 30.0, -1.0};

  std::vector<std::string> rows;
  for (double basis : bases) {
    for (double value : values) {
      for (double yf : yfs) {
        double disc = 0.0;
        const bool fwd = JpmcdsRateToDiscountYearFrac(value, yf, basis, &disc) == SUCCESS;
        double rate = 0.0;
        const bool back = JpmcdsDiscountToRateYearFrac(value, yf, basis, &rate) == SUCCESS;
        rows.push_back("    {" + num(value) + ", " + num(yf) + ", " + num(basis) + ", " +
                       (fwd ? "true" : "false") + ", " + num(fwd ? disc : 0.0) + ", " +
                       (back ? "true" : "false") + ", " + num(back ? rate : 0.0) + "},");
      }
    }
  }
  std::printf("inline constexpr std::array<ConversionCase, %zu> kConversionCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::vector<std::string> valid;
  for (double basis : bases) {
    for (double rate : {0.05, 0.0, -0.5, -1.0, -1.5, -2.0, -3.0, -13.0, 2.0, 1000.0}) {
      for (double yf : {0.0, 0.25, 1.0, 5.0}) {
        const bool ok =
            JpmcdsRateValidYearFrac(const_cast<char *>("t"), rate, yf, basis) == SUCCESS;
        valid.push_back("    {" + num(rate) + ", " + num(yf) + ", " + num(basis) + ", " +
                        (ok ? "true" : "false") + "},");
      }
    }
  }
  std::printf("inline constexpr std::array<ValidityCase, %zu> kValidityCases{{\n", valid.size());
  for (const std::string &r : valid) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
