// Emits tests/golden/timeline_cases.hpp -- upstream's integration timelines,
// recorded so CI can verify the port without the oracle.
//
// These are plain dates, not transcendental values, so unlike the zero curve
// vectors they ARE bit-reproducible on every platform and the replay test
// compares them exactly.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_timeline_golden
//   ./build/debug/gen_timeline_golden > tests/golden/timeline_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/timeline.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "dtlist.h"
#include "ldate.h"
#include "tcurve.h"
#include "timeline.h"
}

namespace {

constexpr std::size_t kMaxTimeline = 16;

cdsmodel::Date d(int y, int m, int day) { return *cdsmodel::make_date(y, m, day); }

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &points) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : points) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(points.size()), 1.0, JPMCDS_ACT_365F);
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const cdsmodel::Date base = d(2024, 1, 1);
  const std::vector<cdsmodel::RatePoint> disc{{d(2024, 4, 1), 0.031},
                                              {d(2025, 1, 1), 0.035},
                                              {d(2027, 1, 1), 0.040},
                                              {d(2031, 1, 1), 0.044}};
  const std::vector<cdsmodel::RatePoint> spread{
      {d(2024, 6, 20), 0.010}, {d(2025, 6, 20), 0.013}, {d(2029, 6, 20), 0.018}};

  TCurve *rd = build(base, disc);
  TCurve *rs = build(base, spread);
  if (rd == nullptr || rs == nullptr) {
    return 1;
  }

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Integration timelines recorded from the upstream ISDA CDS Standard\n"
              "// Model 1.8.3 (timeline.c). Computed data, exempt from the upstream\n"
              "// licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// These are dates, not transcendental values, so unlike the zero curve\n"
              "// vectors they reproduce exactly on every platform.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_timeline_golden\n"
              "//   ./build/debug/gen_timeline_golden > tests/golden/timeline_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr long kTimelineBase = %ld;\n"
              "inline constexpr std::size_t kTimelineMax = %zu;\n\n",
              cdsmodel::to_tdate(base), kMaxTimeline);

  const auto emit_curve = [](const char *name, const std::vector<cdsmodel::RatePoint> &pts) {
    std::printf("inline constexpr std::array<long, %zu> %s{{", pts.size(), name);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(pts[i].date));
    }
    std::printf("}};\n");
  };
  emit_curve("kTimelineDiscountDates", disc);
  emit_curve("kTimelineSpreadDates", spread);
  std::printf("\ninline constexpr std::array<double, %zu> kTimelineDiscountRates{{"
              "0.031, 0.035, 0.040, 0.044}};\n",
              disc.size());
  std::printf("inline constexpr std::array<double, %zu> kTimelineSpreadRates{{"
              "0.010, 0.013, 0.018}};\n\n",
              spread.size());

  std::printf("struct TimelineCase {\n  long start;\n  long end;\n  std::size_t n;\n"
              "  std::array<long, kTimelineMax> dates;\n};\n\n");

  std::vector<std::string> rows;
  for (int start_off : {0, 1, 60, 91, 92, 170, 365, 366, 900, 1096, 2000, 2800}) {
    for (int span : {1, 2, 5, 30, 180, 365, 1000, 3000}) {
      const cdsmodel::Date start = base + std::chrono::days{start_off};
      const cdsmodel::Date end = start + std::chrono::days{span};
      TDateList *tl =
          JpmcdsRiskyTimeLine(cdsmodel::to_tdate(start), cdsmodel::to_tdate(end), rd, rs);
      if (tl == nullptr) {
        continue;
      }
      std::string row = "    {" + std::to_string(cdsmodel::to_tdate(start)) + ", " +
                        std::to_string(cdsmodel::to_tdate(end)) + ", " +
                        std::to_string(tl->fNumItems) + ", {{";
      for (std::size_t i = 0; i < kMaxTimeline; ++i) {
        row += (i ? ", " : "");
        row += i < static_cast<std::size_t>(tl->fNumItems)
                   ? std::to_string(static_cast<long>(tl->fArray[i]))
                   : "0";
      }
      rows.push_back(row + "}}},");
      JpmcdsFreeDateList(tl);
    }
  }
  std::printf("inline constexpr std::array<TimelineCase, %zu> kTimelineCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  JpmcdsFreeTCurve(rd);
  JpmcdsFreeTCurve(rs);
  return 0;
}
