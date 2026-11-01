// Emits tests/golden/bsearch_cases.hpp: the upstream oracle's answers for a
// swept input domain, recorded so CI can verify the port without possessing
// the oracle.
//
// Licence note: section 4(b) of the ISDA CDS Standard Model Public License
// exempts "data derived from calculations that have been attained by using
// the ISDA CDS Standard Model" from its distribution terms. The numbers this
// emits are such data. The code that produced them is not, and is never
// committed.
//
// Build and run (requires a local upstream copy):
//   cmake --build --preset debug --target gen_bsearch_golden
//   ./build/debug/gen_bsearch_golden > tests/golden/bsearch_cases.hpp

#include <cstdio>
#include <span>
#include <string>
#include <vector>

extern "C" {
#include "bsearch.h"
#include "cerror.h"
}

namespace {

std::vector<double> ramp(int n, double step, double base) {
  std::vector<double> v;
  v.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    v.push_back(base + step * static_cast<double>(i));
  }
  return v;
}

template <typename T> std::string literal(T v);
template <> std::string literal(double v) {
  char buf[64];
  // %.17g round-trips an IEEE double exactly.
  std::snprintf(buf, sizeof buf, "%.17g", v);
  std::string s{buf};
  if (s.find_first_of(".eEn") == std::string::npos) {
    s += ".0";
  }
  return s;
}
template <> std::string literal(long v) { return std::to_string(v) + "L"; }

template <typename T> const char *type_name();
template <> const char *type_name<double>() { return "double"; }
template <> const char *type_name<long>() { return "long"; }

int call_reference(std::span<const double> xs, double desired, int *lo, int *hi) {
  return JpmcdsBSearchDoubleFast(desired, const_cast<double *>(xs.data()), sizeof(double),
                                 static_cast<int>(xs.size()), lo, hi);
}
int call_reference(std::span<const long> xs, double desired, int *lo, int *hi) {
  return JpmcdsBSearchLongFast(desired, const_cast<long *>(xs.data()), sizeof(long),
                               static_cast<int>(xs.size()), lo, hi);
}

template <typename T> struct Domain {
  const char *name;
  std::vector<T> xs;
  double from, to, step;
  bool probe_knots;
};

template <typename T> void emit(const std::vector<Domain<T>> &domains, const char *tag) {
  const char *ty = type_name<T>();

  for (std::size_t d = 0; d < domains.size(); ++d) {
    std::printf("inline constexpr std::array<%s, %zu> %s{{", ty, domains[d].xs.size(),
                domains[d].name);
    for (std::size_t i = 0; i < domains[d].xs.size(); ++i) {
      std::printf("%s%s", i ? ", " : "", literal(domains[d].xs[i]).c_str());
    }
    std::printf("}};\n");
  }
  std::printf("\ninline constexpr std::array<std::span<const %s>, %zu> k%sDomains{{", ty,
              domains.size(), tag);
  for (std::size_t d = 0; d < domains.size(); ++d) {
    std::printf("%s%s", d ? ", " : "", domains[d].name);
  }
  std::printf("}};\n\n");

  struct Row {
    std::size_t domain;
    double desired;
    bool ok;
    int lo, hi;
  };
  std::vector<Row> rows;
  for (std::size_t d = 0; d < domains.size(); ++d) {
    const auto &dom = domains[d];
    std::vector<double> probes;
    for (double x = dom.from; x <= dom.to; x += dom.step) {
      probes.push_back(x);
    }
    if (dom.probe_knots) {
      for (T knot : dom.xs) {
        probes.push_back(static_cast<double>(knot));
      }
    }
    for (double p : probes) {
      int lo = -1, hi = -1;
      const bool ok = call_reference(std::span<const T>{dom.xs}, p, &lo, &hi) == SUCCESS;
      rows.push_back({d, p, ok, lo, hi});
    }
  }

  std::printf("inline constexpr std::array<Case, %zu> k%sCases{{\n", rows.size(), tag);
  for (const Row &r : rows) {
    std::printf("    {%zu, %s, %s, %d, %d},\n", r.domain, literal(r.desired).c_str(),
                r.ok ? "true" : "false", r.lo, r.hi);
  }
  std::printf("}};\n");
}

} // namespace

int main() {
  JpmcdsErrMsgOff(); // upstream logs failures to a process-global file handle

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Reference answers recorded from the upstream ISDA CDS Standard Model\n"
              "// 1.8.3 binary search, so this port can be verified where the upstream\n"
              "// sources are not available (CI). These are computed data, exempt from\n"
              "// the upstream licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_bsearch_golden\n"
              "//   ./build/debug/gen_bsearch_golden > tests/golden/bsearch_cases.hpp\n"
              "\n#pragma once\n\n"
              "#include <array>\n#include <cstddef>\n#include <span>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One recorded query: which domain, what was asked, what upstream said.\n"
              "struct Case {\n"
              "  std::size_t domain;\n"
              "  double desired;\n"
              "  bool ok;\n"
              "  int lo;\n"
              "  int hi;\n"
              "};\n\n");

  emit<double>({{"kDoubleKnots", {0.0, 1.0, 2.5, 4.0, 10.0, 11.0, 30.0}, -5.0, 35.0, 0.25, true},
                {"kDoubleSingle", {7.0}, -1.0, 9.0, 4.0, true},
                {"kDoublePair", {1.0, 2.0}, 0.0, 3.0, 0.5, true},
                // Equal neighbours: upstream's Fast variant accepts these and
                // walks past the run. This is the one place the port bounds a
                // loop upstream left unbounded, so it must be pinned.
                {"kDoubleWithRun", {1.0, 2.0, 2.0, 2.0, 3.0, 4.0}, 0.0, 5.0, 0.25, true},
                {"kDoubleRamp", ramp(50, 1.7, 0.3), -10.0, 95.0, 2.9, false}},
               "Double");

  std::printf("\n");
  emit<long>(
      {{"kLongDates", {19000L, 19365L, 19730L, 20095L, 21000L}, 18000.0, 22000.0, 37.0, true}},
      "Long");

  std::printf("\n} // namespace cdsmodel::golden\n");
  return 0;
}
