// Emits tests/golden/interpolation_cases.hpp.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
// The discontinuous cases are recorded through upstream, and upstream leaks
// the order-checking global on its error paths (UPSTREAM-13) -- so the global
// is restored after every call here. A generator that corrupted its own later
// cases would record garbage.
//
//   cmake --build --preset debug --target gen_interpolation_golden
//   ./build/debug/gen_interpolation_golden > tests/golden/interpolation_cases.hpp

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bsearch.h"
#include "cerror.h"
#include "cgeneral.h"
#include "ldate.h"
#include "lintrp.h"
#include "yearfrac.h"
int JpmcdsLinInterpLongPoint1Discontinuous(long *x, int xskip, int N, double *f, int fskip,
                                           double xDesired, TMetricDoubleFunc mfunc,
                                           TBoolean rightLimit, double *fInterp);
}

namespace {

constexpr std::size_t kMaxPoints = 8;

/// `%.17g` renders an infinity as "inf", which is not a C++ literal -- and
/// this is the first generator in the project whose results can be infinite,
/// because a zero-width bracket divides by zero. Emit something constexpr
/// instead.
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

template <class T> std::string render(const std::vector<T> &v) {
  std::string out = "{{";
  for (std::size_t i = 0; i < kMaxPoints; ++i) {
    out += (i ? ", " : "");
    if (i >= v.size()) {
      out += std::is_floating_point_v<T> ? "0.0" : "0";
    } else if constexpr (std::is_floating_point_v<T>) {
      out += num(static_cast<double>(v[i]));
    } else {
      out += std::to_string(v[i]);
    }
  }
  return out + "}}";
}

struct Curve {
  std::vector<long> xs;
  std::vector<double> fs;
};

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded linear interpolation results. Data derived from running the\n"
              "// upstream model (licence 4(b)).\n"
              "//\n"
              "// `metric`: 0 = upstream's NULL (direct), 1 = actual days, 2 = 30/360,\n"
              "// 3 = 30E/360. Note 0 and 1 differ on a zero-width bracket -- see\n"
              "// interpolation.hpp.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_interpolation_golden\n"
              "//   ./build/debug/gen_interpolation_golden > tests/golden/interpolation_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n#include <limits>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kInterpMax = 8;\n\n"
              "struct InterpCase {\n  std::size_t n;\n"
              "  std::array<long, kInterpMax> xs;\n  std::array<double, kInterpMax> fs;\n"
              "  double desired;\n  int metric;\n  bool solved;\n  double value;\n};\n\n"
              "/// right_limit: which side of a repeated abscissa to take.\n"
              "struct DiscontinuousCase {\n  std::size_t n;\n"
              "  std::array<long, kInterpMax> xs;\n  std::array<double, kInterpMax> fs;\n"
              "  double desired;\n  int metric;\n  bool right_limit;\n"
              "  bool solved;\n  double value;\n};\n\n");

  const std::vector<Curve> curves{
      {{100}, {0.05}},
      {{100, 200}, {0.01, 0.02}},
      {{100, 200, 400, 800, 1600}, {0.01, 0.02, 0.025, 0.03, 0.028}},
      {{0, 1, 2, 3}, {-1.0, 0.0, 1.0, 2.0}},
      {{154497, 154862, 155227, 155592}, {0.028, 0.031, 0.034, 0.037}},
  };
  const std::vector<double> desireds{-50.0, 0.0,    99.5,   100.0,    150.0,    199.9,    200.0,
                                     333.3, 1000.0, 2000.0, 154500.0, 154700.0, 155000.0, 155592.0};
  struct Met {
    int code;
    long dcc;
    bool use_null;
  };
  const std::vector<Met> metrics{{0, 0, true},
                                 {1, JPMCDS_ACT_365F, false},
                                 {2, JPMCDS_B30_360, false},
                                 {3, JPMCDS_B30E_360, false}};

  // UPSTREAM-14: the bond metrics discard JpmcdsDaysDiff's status over an
  // uninitialised local, so a pre-epoch abscissa (TDate < 0) makes them
  // return stack garbage. The output of undefined behaviour is not recorded
  // -- the same rule that kept UPSTREAM-4's buffer overflow out of the
  // vectors. The divergence is asserted in the differential suite instead.
  const auto upstream_metric_is_defined = [](const Met &m, double desired) {
    return m.use_null || m.dcc == JPMCDS_ACT_365F || desired >= 0.0;
  };

  std::vector<std::string> rows;
  for (const Curve &c : curves) {
    std::vector<long> x = c.xs;
    std::vector<double> f = c.fs;
    for (const Met &m : metrics) {
      for (double desired : desireds) {
        if (!upstream_metric_is_defined(m, desired)) {
          continue;
        }
        double out = 0.0;
        const bool ok =
            JpmcdsLinInterpLongPoint1(
                x.data(), sizeof(long), static_cast<int>(x.size()), f.data(), sizeof(double),
                desired, m.use_null ? nullptr : JpmcdsDayCountToMetricFunc(m.dcc), &out) == SUCCESS;
        JpmcdsBSearchCheckOrder(TRUE);
        rows.push_back("    {" + std::to_string(x.size()) + ", " + render(x) + ", " + render(f) +
                       ", " + num(desired) + ", " + std::to_string(m.code) + ", " +
                       (ok ? "true" : "false") + ", " + num(ok ? out : 0.0) + "},");
      }
    }
  }
  std::printf("inline constexpr std::array<InterpCase, %zu> kInterpCases{{\n", rows.size());
  for (const std::string &r : rows)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n");

  const std::vector<Curve> discs{
      {{100, 200, 200, 300}, {1.0, 2.0, 5.0, 6.0}},
      {{100, 200, 200, 200, 300}, {1.0, 2.0, 3.0, 5.0, 6.0}},
      {{100, 100, 200}, {1.0, 7.0, 2.0}},
      {{100, 200, 300, 300}, {1.0, 2.0, 3.0, 9.0}},
      {{100, 200, 300}, {1.0, 2.0, 3.0}},
  };
  std::vector<std::string> drows;
  for (const Curve &c : discs) {
    std::vector<long> x = c.xs;
    std::vector<double> f = c.fs;
    for (const Met &m : metrics) {
      for (double desired : {50.0, 100.0, 150.0, 200.0, 250.0, 300.0, 400.0}) {
        for (bool right : {false, true}) {
          if (!upstream_metric_is_defined(m, desired)) {
            continue;
          }
          double out = 0.0;
          const bool ok =
              JpmcdsLinInterpLongPoint1Discontinuous(
                  x.data(), sizeof(long), static_cast<int>(x.size()), f.data(), sizeof(double),
                  desired, m.use_null ? nullptr : JpmcdsDayCountToMetricFunc(m.dcc),
                  right ? TRUE : FALSE, &out) == SUCCESS;
          // UPSTREAM-13: restore what the call may have leaked.
          JpmcdsBSearchCheckOrder(TRUE);
          drows.push_back("    {" + std::to_string(x.size()) + ", " + render(x) + ", " + render(f) +
                          ", " + num(desired) + ", " + std::to_string(m.code) + ", " +
                          (right ? "true" : "false") + ", " + (ok ? "true" : "false") + ", " +
                          num(ok ? out : 0.0) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<DiscontinuousCase, %zu> kDiscontinuousCases{{\n",
              drows.size());
  for (const std::string &r : drows)
    std::printf("%s\n", r.c_str());
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
