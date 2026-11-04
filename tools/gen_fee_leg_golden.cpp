// Emits tests/golden/fee_leg_cases.hpp -- upstream's premium leg PVs.
//
// Accumulating quantities derived from exp and log, so cross-platform replay
// uses a RELATIVE tolerance, not a ULP count: a sum over coupon periods, each
// containing its own integral, amplifies libm differences well past four
// ULPs. See the header of contingent_leg_test.cpp for the measurement.
//
// Licence note: computed data, exempt under section 4(b).
//
//   cmake --build --preset debug --target gen_fee_leg_golden
//   ./build/debug/gen_fee_leg_golden > tests/golden/fee_leg_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cx.h"
#include "feeleg.h"
#include "ldate.h"
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

const std::vector<cdsmodel::RatePoint> kDisc{{d(2024, 4, 1), 0.0310},
                                             {d(2025, 1, 1), 0.0350},
                                             {d(2027, 1, 1), 0.0400},
                                             {d(2031, 1, 1), 0.0440},
                                             {d(2039, 1, 1), 0.0455}};
const std::vector<cdsmodel::RatePoint> kSpread{{d(2024, 6, 20), 0.0090},
                                               {d(2025, 6, 20), 0.0130},
                                               {d(2029, 6, 20), 0.0185},
                                               {d(2034, 6, 20), 0.0220}};

TCurve *build(cdsmodel::Date base, const std::vector<cdsmodel::RatePoint> &pts) {
  std::vector<TDate> dates;
  std::vector<double> rates;
  for (const cdsmodel::RatePoint &p : pts) {
    dates.push_back(cdsmodel::to_tdate(p.date));
    rates.push_back(p.rate);
  }
  return JpmcdsMakeTCurve(cdsmodel::to_tdate(base), dates.data(), rates.data(),
                          static_cast<int>(pts.size()), 1.0, JPMCDS_ACT_365F);
}

std::vector<cdsmodel::FeePayment> quarterly(cdsmodel::Date start, int periods) {
  std::vector<cdsmodel::FeePayment> out;
  cdsmodel::Date from = start;
  const std::chrono::year_month_day ymd = cdsmodel::to_ymd(start);
  int year = static_cast<int>(ymd.year());
  unsigned month = static_cast<unsigned>(ymd.month());
  for (int i = 0; i < periods; ++i) {
    month += 3;
    if (month > 12) {
      month -= 12;
      ++year;
    }
    const cdsmodel::Date to = *cdsmodel::make_date(year, static_cast<int>(month), 20);
    out.push_back({from, to, to});
    from = to;
  }
  return out;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();
  const cdsmodel::Date base = d(2024, 1, 1);
  TCurve *disc = build(base, kDisc);
  TCurve *spread = build(base, kSpread);
  if (disc == nullptr || spread == nullptr) {
    return 1;
  }

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Premium leg present values recorded from the upstream ISDA CDS\n"
              "// Standard Model 1.8.3 (feeleg.c). Computed data, exempt from the\n"
              "// upstream licence's distribution terms under its section 4(b).\n"
              "//\n"
              "// Accumulating values: cross-platform replay uses a relative tolerance,\n"
              "// not a ULP count. A sum over coupon periods, each with its own\n"
              "// integral, amplifies libm differences well past four ULPs.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_fee_leg_golden\n"
              "//   ./build/debug/gen_fee_leg_golden > tests/golden/fee_leg_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr long kFlBase = %ld;\n"
              "inline constexpr long kFlScheduleStart = %ld;\n\n",
              cdsmodel::to_tdate(base), cdsmodel::to_tdate(d(2024, 3, 20)));

  const auto emit = [](const char *name, const std::vector<cdsmodel::RatePoint> &pts) {
    std::printf("inline constexpr std::array<long, %zu> %sDates{{", pts.size(), name);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      std::printf("%s%ld", i ? ", " : "", cdsmodel::to_tdate(pts[i].date));
    }
    std::printf("}};\ninline constexpr std::array<double, %zu> %sRates{{", pts.size(), name);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      std::printf("%s%s", i ? ", " : "", dbl(pts[i].rate).c_str());
    }
    std::printf("}};\n");
  };
  emit("kFlDisc", kDisc);
  emit("kFlSpread", kSpread);

  std::printf("\n/// accrual: 0 = none, 1 = all. dcc is upstream's convention code.\n"
              "struct FeeLegCase {\n  int periods;\n  double coupon;\n  long dcc;\n"
              "  int accrual;\n  bool obs_start_of_day;\n  bool clean;\n  long stepin;\n"
              "  double pv;\n};\n\n");

  std::vector<std::string> rows;
  const long dccs[] = {cdsmodel::to_upstream_code(cdsmodel::DayCount::Act360),
                       cdsmodel::to_upstream_code(cdsmodel::DayCount::Act365Fixed),
                       cdsmodel::to_upstream_code(cdsmodel::DayCount::Thirty360)};
  for (int periods : {1, 2, 4, 20, 40}) {
    for (double coupon : {0.0, 0.0025, 0.0100, 0.0500}) {
      for (long dcc : dccs) {
        for (int accrual = 0; accrual <= 1; ++accrual) {
          for (int obs = 0; obs <= 1; ++obs) {
            for (int clean = 0; clean <= 1; ++clean) {
              for (int stepin_off : {0, 45, 400}) {
                const auto schedule = quarterly(d(2024, 3, 20), periods);
                std::vector<TDate> acc_start;
                std::vector<TDate> acc_end;
                std::vector<TDate> pay;
                for (const auto &p : schedule) {
                  acc_start.push_back(cdsmodel::to_tdate(p.accrual_start));
                  acc_end.push_back(cdsmodel::to_tdate(p.accrual_end));
                  pay.push_back(cdsmodel::to_tdate(p.pay));
                }
                TFeeLeg fl{};
                fl.nbDates = static_cast<int>(schedule.size());
                fl.accStartDates = acc_start.data();
                fl.accEndDates = acc_end.data();
                fl.payDates = pay.data();
                fl.notional = 1.0e7;
                fl.couponRate = coupon;
                fl.dcc = dcc;
                fl.accrualPayConv = accrual ? ACCRUAL_PAY_ALL : ACCRUAL_PAY_NONE;
                fl.obsStartOfDay = obs ? TRUE : FALSE;

                const cdsmodel::Date today = d(2024, 3, 20);
                const cdsmodel::Date stepin = today + std::chrono::days{stepin_off};
                double pv = 0.0;
                if (JpmcdsFeeLegPV(&fl, cdsmodel::to_tdate(today), cdsmodel::to_tdate(stepin),
                                   cdsmodel::to_tdate(today), disc, spread, clean ? TRUE : FALSE,
                                   &pv) != SUCCESS) {
                  continue;
                }
                rows.push_back("    {" + std::to_string(periods) + ", " + dbl(coupon) + ", " +
                               std::to_string(dcc) + ", " + std::to_string(accrual) + ", " +
                               (obs ? "true" : "false") + ", " + (clean ? "true" : "false") + ", " +
                               std::to_string(cdsmodel::to_tdate(stepin)) + ", " + dbl(pv) + "},");
              }
            }
          }
        }
      }
    }
  }
  std::printf("inline constexpr std::array<FeeLegCase, %zu> kFeeLegCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  JpmcdsFreeTCurve(disc);
  JpmcdsFreeTCurve(spread);
  return 0;
}
