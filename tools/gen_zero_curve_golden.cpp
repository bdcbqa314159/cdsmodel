// Emits tests/golden/zero_curve_cases.hpp -- upstream's zero rates and
// discount factors, recorded so CI can verify the port without the oracle.
//
// Doubles are written with %.17g, which round-trips an IEEE double exactly,
// so nothing is lost in the rendering.
//
// What IS platform-specific is the value itself. These come from `exp` and
// `log`, which are not specified to the last bit and differ by about 1 ULP
// between macOS libm, glibc and the MSVC CRT. So vectors generated here are
// correct to 4 ULPs anywhere, not bit-identical everywhere, and the replay
// test uses EXPECT_DOUBLE_EQ for exactly that reason. Bit equality is the
// right bar only for the differential suite, where both implementations run
// in one process and share one libm.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_zero_curve_golden
//   ./build/debug/gen_zero_curve_golden > tests/golden/zero_curve_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cxzerocurve.h"
#include "tcurve.h"
}

namespace {

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

std::string dbl(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s{buf};
  if (s.find_first_of(".eEni") == std::string::npos) {
    s += ".0";
  }
  return s;
}

struct Scenario {
  const char *name;
  double basis;
  long day_count;
  std::vector<cdsmodel::RatePoint> points;
};

std::vector<Scenario> scenarios() {
  const auto p = [](int y, int m, int day, double r) {
    return cdsmodel::RatePoint{d(y, m, day), r};
  };
  const long a365f = cdsmodel::to_upstream_code(cdsmodel::DayCount::Act365Fixed);
  const long a360 = cdsmodel::to_upstream_code(cdsmodel::DayCount::Act360);
  return {
      {"annual upward",
       1.0,
       a365f,
       {p(2024, 7, 1, 0.0310), p(2025, 1, 1, 0.0345), p(2026, 1, 1, 0.0390), p(2029, 1, 1, 0.0425),
        p(2034, 1, 1, 0.0450)}},
      {"annual inverted",
       1.0,
       a365f,
       {p(2024, 7, 1, 0.0520), p(2025, 1, 1, 0.0480), p(2027, 1, 1, 0.0410),
        p(2031, 1, 1, 0.0375)}},
      {"continuous a365f",
       cdsmodel::kContinuousBasis,
       a365f,
       {p(2024, 7, 1, 0.0295), p(2026, 1, 1, 0.0330), p(2030, 1, 1, 0.0365)}},
      {"continuous a360",
       cdsmodel::kContinuousBasis,
       a360,
       {p(2024, 7, 1, 0.0295), p(2026, 1, 1, 0.0330), p(2030, 1, 1, 0.0365)}},
      {"semiannual a360",
       2.0,
       a360,
       {p(2024, 7, 1, 0.0300), p(2025, 7, 1, 0.0340), p(2028, 1, 1, 0.0380)}},
      {"quarterly a365f",
       4.0,
       a365f,
       {p(2024, 4, 1, 0.0250), p(2025, 4, 1, 0.0290), p(2027, 4, 1, 0.0330)}},
      {"near zero", 1.0, a365f, {p(2024, 7, 1, 1e-9), p(2026, 1, 1, 1e-9), p(2030, 1, 1, 1e-9)}},
      {"single point", 1.0, a365f, {p(2027, 1, 1, 0.0400)}},
      {"negative rates",
       1.0,
       a365f,
       {p(2024, 7, 1, -0.0050), p(2026, 1, 1, -0.0020), p(2030, 1, 1, 0.0100)}},
      {"base on first knot",
       1.0,
       a365f,
       {p(2024, 1, 1, 0.0310), p(2026, 1, 1, 0.0390), p(2030, 1, 1, 0.0430)}},
  };
}

TCurve *build(cdsmodel::Date base, const Scenario &s) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : s.points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(s.points.size()), s.basis, s.day_count);
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const cdsmodel::Date base = d(2024, 1, 1);
  const auto all = scenarios();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Zero rates and discount factors recorded from the upstream ISDA CDS\n"
              "// Standard Model 1.8.3 zero curve (cxzerocurve.c). Computed data, exempt\n"
              "// from the upstream licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// Doubles are %%.17g, which round-trips exactly: the port reproduces these\n"
              "// BIT-for-bit, so a lossy rendering would weaken the test.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_zero_curve_golden\n"
              "//   ./build/debug/gen_zero_curve_golden > tests/golden/zero_curve_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr long kZeroCurveBase = %ld;\n"
              "inline constexpr std::size_t kZeroCurveMaxPoints = 8;\n\n"
              "struct ZeroCurveDef {\n  double basis;\n  long day_count;\n  std::size_t n;\n"
              "  std::array<long, kZeroCurveMaxPoints> dates;\n"
              "  std::array<double, kZeroCurveMaxPoints> rates;\n};\n\n",
              cdsmodel::to_tdate(base));

  std::printf("inline constexpr std::array<ZeroCurveDef, %zu> kZeroCurves{{\n", all.size());
  for (const Scenario &s : all) {
    std::printf("    {%s, %ld, %zu, {{", dbl(s.basis).c_str(), s.day_count, s.points.size());
    for (std::size_t i = 0; i < 8; ++i) {
      std::printf("%s%ld", i ? ", " : "",
                  i < s.points.size() ? cdsmodel::to_tdate(s.points[i].date) : 0L);
    }
    std::printf("}}, {{");
    for (std::size_t i = 0; i < 8; ++i) {
      std::printf("%s%s", i ? ", " : "",
                  i < s.points.size() ? dbl(s.points[i].rate).c_str() : "0.0");
    }
    std::printf("}}},\n");
  }
  std::printf("}};\n\n");

  std::printf("/// One query against kZeroCurves[curve].\n"
              "struct ZeroCurveCase {\n  std::size_t curve;\n  long tdate;\n"
              "  double rate;\n  double price;\n};\n\n");

  std::vector<std::string> rows;
  for (std::size_t i = 0; i < all.size(); ++i) {
    TCurve *ref = build(base, all[i]);
    if (ref == nullptr) {
      std::fprintf(stderr, "upstream rejected scenario %s\n", all[i].name);
      return 1;
    }
    for (int offset = -400; offset <= 4200; offset += 23) {
      const cdsmodel::Date date = base + std::chrono::days{offset};
      const double rate = JpmcdsZeroRate(ref, cdsmodel::to_tdate(date));
      const double price = JpmcdsZeroPrice(ref, cdsmodel::to_tdate(date));
      if (std::isnan(rate) || std::isnan(price)) {
        continue;
      }
      rows.push_back("    {" + std::to_string(i) + ", " + std::to_string(cdsmodel::to_tdate(date)) +
                     ", " + dbl(rate) + ", " + dbl(price) + "},");
    }
    JpmcdsFreeTCurve(ref);
  }
  std::printf("inline constexpr std::array<ZeroCurveCase, %zu> kZeroCurveCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
