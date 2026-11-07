// Emits tests/golden/stream_cash_flows_cases.hpp.
//
// Records the cash flows of fixed and floating swap legs. Amounts are products
// and quotients of accumulated present values, so the replay compares at the
// ACCUMULATING tier -- a relative tolerance, not ULPs -- because these vectors
// crossed a compiler. Dates are exact.
//
// Flows live in one flat pool with an offset and a count per case, the same
// shape coupon_schedule_cases.hpp uses and for the same reason: a generated
// constexpr table cannot hold a vector.
//
// The float cases are all computed off ONE curve, recorded alongside them, so
// the replay reconstructs it rather than trusting a second recording.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_stream_cash_flows_golden
//   ./build/debug/gen_stream_cash_flows_golden > tests/golden/stream_cash_flows_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "fltrate.h"
#include "interp.h"
#include "ldate.h"
#include "schedule.h"
#include "streamcf.h"
#include "tcurve.h"
}

namespace {

TDate ymd(long y, long m, long d) {
  TMonthDayYear mdy;
  mdy.year = y;
  mdy.month = m;
  mdy.day = d;
  TDate out = 0;
  JpmcdsMDYToDate(&mdy, &out);
  return out;
}

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

std::vector<std::string> g_flows;
std::vector<std::string> g_fixed;
std::vector<std::string> g_float;

std::size_t record_flows(const TCashFlowList *cfl) {
  if (cfl == nullptr) {
    return 0;
  }
  for (int i = 0; i < cfl->fNumItems; ++i) {
    char row[80];
    std::snprintf(row, sizeof row, "    {%ld, %s},", (long)cfl->fArray[i].fDate,
                  num(cfl->fArray[i].fAmount).c_str());
    g_flows.emplace_back(row);
  }
  return static_cast<std::size_t>(cfl->fNumItems);
}

constexpr int kCurvePoints = 8;

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const TDate base = ymd(2024, 1, 15);
  TDate curve_dates[kCurvePoints];
  double curve_rates[kCurvePoints];
  for (int i = 1; i <= kCurvePoints; ++i) {
    curve_dates[i - 1] = ymd(2024 + 5 * i, 1, 15);
    curve_rates[i - 1] = 0.02 + 0.003 * i;
  }
  TCurve *zc = JpmcdsMakeTCurve(base, curve_dates, curve_rates, kCurvePoints, 1.0, JPMCDS_ACT_365F);
  if (zc == nullptr) {
    std::fprintf(stderr, "could not build the curve\n");
    return 1;
  }

  struct Span {
    long y;
    long m;
    long d;
  };
  // Deliberately narrower than the tier-2 sweep: tier 1 exists so CI has
  // coverage with no oracle, not to re-run the live comparison, and the flow
  // pool is what grows. Three maturities (on cycle, off cycle, a leap span),
  // two intervals, two day counts.
  const Span maturities[] = {{2029, 1, 15}, {2029, 4, 15}, {2027, 2, 28}};
  const long dccs[] = {JPMCDS_ACT_360, JPMCDS_B30_360};

  for (const Span &mat : maturities) {
    for (const int months : {3, 6, 12}) {
      for (const int stub_at_end : {0, 1}) {
        TDateInterval iv;
        JpmcdsMakeDateInterval(months, 'M', &iv);
        TCouponDateList *dl = JpmcdsNewCouponDatesSwap(
            base, ymd(mat.y, mat.m, mat.d), &iv, TRUE, FALSE, 0, 0, stub_at_end ? TRUE : FALSE,
            FALSE, 0, 0, 0, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
            (char *)"NONE");
        if (dl == nullptr || dl->fNumItems == 0) {
          JpmcdsFreeCouponDateList(dl);
          continue;
        }

        for (const long dcc : dccs) {
          for (const int subtract_initial : {0, 1}) {
            for (const int add_final : {0, 1}) {
              const long flags = (subtract_initial ? JPMCDS_SUBTRACT_INITIAL : 0) |
                                 (add_final ? JPMCDS_ADD_FINAL : 0);

              /* ---- fixed ---- */
              for (const double fixed_rate : {0.0, 0.04}) {
                TStreamFixed *fs = JpmcdsNewStreamFixed(dl, JPMCDS_SINGLE_REFIX, 1.0e6, dcc, flags,
                                                        0.03, 0.05, fixed_rate);
                if (fs == nullptr) {
                  continue;
                }
                TCashFlowList *cfl = JpmcdsNewStreamFixedCFL(base, fs);
                const std::size_t offset = g_flows.size();
                const std::size_t count = record_flows(cfl);
                char row[260];
                std::snprintf(row, sizeof row,
                              "    {%ld, %ld, %d, %s, %ld, %d, %d, %s, %ld, %s, %s, %zu, %zu},",
                              (long)base, (long)ymd(mat.y, mat.m, mat.d), months,
                              stub_at_end ? "true" : "false", dl->stubLocation, subtract_initial,
                              add_final, num(fixed_rate).c_str(), dcc, num(0.03).c_str(),
                              num(0.05).c_str(), offset, count);
                g_fixed.emplace_back(row);
                JpmcdsFreeCFL(cfl);
                JpmcdsFreeTStreamFixed(fs);
              }

              /* ---- floating ---- */
              for (const int spot : {0, 2}) {
                for (const double spread : {0.0025}) {
                  for (const long interp : {JPMCDS_LINEAR_INTERP, JPMCDS_FLAT_FORWARDS}) {
                    TDateInterval rate_iv;
                    JpmcdsMakeDateInterval(months, 'M', &rate_iv);
                    TFloatRate fr;
                    if (JpmcdsFloatRateSet(&rate_iv, &rate_iv, JPMCDS_ACT_360, spot, spread, 1.0,
                                           &fr) != SUCCESS) {
                      continue;
                    }
                    TStubRates first = {0.0, rate_iv};
                    TStubRates final_ = {0.0, rate_iv};
                    TStreamFloat *st = JpmcdsNewStreamFloat(dl, JPMCDS_SINGLE_REFIX, 1.0e6, dcc,
                                                            flags, &first, &final_, 0.0, 0.0, &fr);
                    if (st == nullptr) {
                      continue;
                    }
                    TCashFlowList *cfl = JpmcdsNewStreamFloatCFLGen(
                        zc, interp, st, JPMCDS_BAD_DAY_NONE, (char *)"NONE");
                    const std::size_t offset = g_flows.size();
                    const std::size_t count = record_flows(cfl);
                    char row[300];
                    std::snprintf(
                        row, sizeof row,
                        "    {%ld, %ld, %d, %s, %ld, %d, %d, %ld, %d, %s, %ld, %s, %zu, %zu},",
                        (long)base, (long)ymd(mat.y, mat.m, mat.d), months,
                        stub_at_end ? "true" : "false", dl->stubLocation, subtract_initial,
                        add_final, dcc, spot, num(spread).c_str(), interp,
                        cfl != nullptr ? "true" : "false", offset, count);
                    g_float.emplace_back(row);
                    JpmcdsFreeCFL(cfl);
                    JpmcdsFreeTStreamFloat(st);
                  }
                }
              }
            }
          }
        }
        JpmcdsFreeCouponDateList(dl);
      }
    }
  }

  std::printf(R"(// GENERATED FILE -- DO NOT EDIT.
//
// Recorded cash flows for fixed and floating swap legs, from
// JpmcdsNewStreamFixedCFL and JpmcdsNewStreamFloatCFLGen.
//
// Amounts are products and quotients of accumulated present values AND these
// vectors crossed a compiler, so the replay compares at the accumulating tier:
// relative, kStreamCashFlowsTolerance. Dates compare exactly.
//
// All flows live in one flat pool; each case carries an offset and a count.
// Every floating case is computed off the one curve recorded below.
//
// Data derived from running the upstream model (licence 4(b)).
//
// Regenerate with a local upstream copy present:
//   cmake --build --preset debug --target gen_stream_cash_flows_golden
//   ./build/debug/gen_stream_cash_flows_golden > tests/golden/stream_cash_flows_cases.hpp

#pragma once

#include <array>
#include <cstddef>

#include <cdsmodel/date.hpp>

namespace cdsmodel::golden {

/// Relative. Amounts are sums of discounted quantities, and the file crossed a
/// compiler to get here -- both reasons point at the accumulating tier.
inline constexpr double kStreamCashFlowsTolerance = 1.0e-12;

struct RecordedFlow {
  TDate date;
  double amount;
};

struct FixedLegCase {
  TDate start;
  TDate maturity;
  int months;
  bool stub_at_end;
  long stub_location;
  int subtract_initial;
  int add_final;
  double fixed_rate;
  long day_count;
  double first_coupon_rate;
  double final_coupon_rate;
  std::size_t first_flow;
  std::size_t flow_count;
};

struct FloatLegCase {
  TDate start;
  TDate maturity;
  int months;
  bool stub_at_end;
  long stub_location;
  int subtract_initial;
  int add_final;
  long day_count;
  int spot_offset_days;
  double spread;
  long interp;
  bool accepted;
  std::size_t first_flow;
  std::size_t flow_count;
};

/// The one curve every floating case is computed off.
inline constexpr TDate kStreamCurveBaseDate = )");
  std::printf("%ld;\n", (long)base);
  std::printf("inline constexpr std::size_t kStreamCurvePoints = %d;\n", kCurvePoints);
  std::printf("inline constexpr std::array<TDate, %d> kStreamCurveDates = {", kCurvePoints);
  for (int i = 0; i < kCurvePoints; ++i) {
    std::printf("%s%ld", i ? ", " : "", (long)curve_dates[i]);
  }
  std::printf("};\n");
  std::printf("inline constexpr std::array<double, %d> kStreamCurveRates = {", kCurvePoints);
  for (int i = 0; i < kCurvePoints; ++i) {
    std::printf("%s%s", i ? ", " : "", num(curve_rates[i]).c_str());
  }
  std::printf("};\n\n");

  std::printf("inline constexpr std::array<RecordedFlow, %zu> kStreamFlows{{\n", g_flows.size());
  for (const std::string &r : g_flows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<FixedLegCase, %zu> kFixedLegCases{{\n", g_fixed.size());
  for (const std::string &r : g_fixed) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::printf("inline constexpr std::array<FloatLegCase, %zu> kFloatLegCases{{\n", g_float.size());
  for (const std::string &r : g_float) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");

  JpmcdsFreeTCurve(zc);
  return 0;
}
