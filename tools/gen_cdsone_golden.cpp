// Emits tests/golden/cdsone_cases.hpp -- recorded upfront charges and flat
// spreads, plus the discount curve the shipped example builds.
//
// Recording that curve is what lets CI replay ISDA's own published example
// without the 5,126-line interest-rate stack, which this port does not
// implement and does not need: the CDS model takes a discount curve as an
// input. The curve is data derived from running the upstream model, exempt
// under licence section 4(b), exactly like every other recorded vector here.
//
//   cmake --build --preset debug --target gen_cdsone_golden
//   ./build/debug/gen_cdsone_golden > tests/golden/cdsone_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "busday.h"
#include "cdate.h"
#include "cdsone.h"
#include "cerror.h"
#include "convert.h"
#include "cx.h"
#include "date_sup.h"
#include "dateconv.h"
#include "ldate.h"
#include "stub.h"
#include "tcurve.h"
#include "zerocurve.h"
}

namespace {

constexpr std::size_t kMaxCurve = 24;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

TCurve *example_ir_curve() {
  const char *types = "MMMMMSSSSSSSSS";
  const char *expiries[14] = {"1M", "2M", "3M", "6M", "9M", "1Y", "2Y",
                              "3Y", "4Y", "5Y", "6Y", "7Y", "8Y", "9Y"};
  double rates[14];
  for (double &r : rates) {
    r = 1e-9;
  }
  const TDate base = JpmcdsDate(2008, 1, 3);
  long mm_dcc = 0;
  long dcc = 0;
  TDateInterval ivl;
  double freq = 0.0;
  if (JpmcdsStringToDayCountConv(const_cast<char *>("Act/360"), &mm_dcc) != SUCCESS ||
      JpmcdsStringToDayCountConv(const_cast<char *>("30/360"), &dcc) != SUCCESS ||
      JpmcdsStringToDateInterval(const_cast<char *>("6M"), const_cast<char *>("t"), &ivl) !=
          SUCCESS ||
      JpmcdsDateIntervalToFreq(&ivl, &freq) != SUCCESS) {
    return nullptr;
  }
  std::vector<TDate> dates(14);
  for (int i = 0; i < 14; ++i) {
    TDateInterval tmp;
    if (JpmcdsStringToDateInterval(const_cast<char *>(expiries[i]), const_cast<char *>("t"),
                                   &tmp) != SUCCESS) {
      return nullptr;
    }
    if (JpmcdsDateFwdThenAdjust(base, &tmp, JPMCDS_BAD_DAY_NONE, const_cast<char *>("None"),
                                &dates[static_cast<std::size_t>(i)]) != SUCCESS) {
      return nullptr;
    }
  }
  return JpmcdsBuildIRZeroCurve(base, const_cast<char *>(types), dates.data(), rates, 14, mm_dcc,
                                static_cast<long>(freq), static_cast<long>(freq), dcc, dcc, 'F',
                                const_cast<char *>("None"));
}

std::string render_curve(const TCurve *tc) {
  std::string out = "{" + std::to_string(tc->fBaseDate) + ", " + std::to_string(tc->fNumItems) +
                    ", " + num(tc->fBasis) + ", " + std::to_string(tc->fDayCountConv) + ", {{";
  for (std::size_t i = 0; i < kMaxCurve; ++i) {
    out += (i ? ", " : "");
    out += i < static_cast<std::size_t>(tc->fNumItems) ? std::to_string(tc->fArray[i].fDate) : "0";
  }
  out += "}}, {{";
  for (std::size_t i = 0; i < kMaxCurve; ++i) {
    out += (i ? ", " : "");
    out += i < static_cast<std::size_t>(tc->fNumItems) ? num(tc->fArray[i].fRate) : "0.0";
  }
  return out + "}}}";
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  TCurve *ir = example_ir_curve();
  if (ir == nullptr) {
    std::fprintf(stderr, "could not build the example IR curve\n");
    return 1;
  }

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded upfront charges and flat spreads, with the discount curve\n"
              "// the shipped example builds. Data derived from running the upstream\n"
              "// model (licence 4(b)).\n"
              "//\n"
              "// The curve is recorded because the example builds it with\n"
              "// JpmcdsBuildIRZeroCurve, the single entry point of the interest-rate\n"
              "// stack this port does not implement. Recording it lets CI replay\n"
              "// ISDA's own published numbers with no oracle present.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_cdsone_golden\n"
              "//   ./build/debug/gen_cdsone_golden > tests/golden/cdsone_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "inline constexpr std::size_t kCdsoneCurveMax = 24;\n\n"
              "struct CdsoneCurveSpec {\n  long base;\n  std::size_t n;\n  double basis;\n"
              "  long day_count;\n  std::array<long, kCdsoneCurveMax> dates;\n"
              "  std::array<double, kCdsoneCurveMax> rates;\n};\n\n"
              "/// `solved` false means upstream returned FAILURE.\n"
              "struct CdsoneChargeCase {\n"
              "  long today;\n  long value;\n  long benchmark_start;\n  long stepin;\n"
              "  long start;\n  long end;\n  double coupon_rate;\n  double one_spread;\n"
              "  double recovery;\n  long dcc;\n  int interval_months;\n  char bad_day;\n"
              "  bool pay_accrual_on_default;\n  bool clean;\n"
              "  bool solved;\n  double charge;\n};\n\n"
              "struct CdsoneSpreadCase {\n"
              "  long end;\n  double coupon_rate;\n  double recovery;\n  double charge;\n"
              "  bool solved;\n  double spread;\n};\n\n");

  std::printf("inline constexpr CdsoneCurveSpec kCdsoneExampleDiscountCurve = %s;\n\n",
              render_curve(ir).c_str());

  const TDate today = JpmcdsDate(2008, 2, 1);
  const TDate value = JpmcdsDate(2008, 2, 1);
  const TDate bench = JpmcdsDate(2008, 2, 2);
  const TDate stepin = JpmcdsDate(2008, 2, 9);
  const TDate start = JpmcdsDate(2008, 2, 8);

  struct Combo {
    TDate end;
    double coupon_bps;
    double spread_bps;
    double recovery;
    long dcc;
    int months;
    char bad_day;
    bool accrual;
    bool clean;
  };
  std::vector<Combo> combos;
  // The example's own three cases first, verbatim.
  for (double cpn : {0.0, 3600.0, 7200.0}) {
    combos.push_back(
        {JpmcdsDate(2008, 2, 12), cpn, 3600.0, 0.4, JPMCDS_ACT_360, 6, 'F', true, false});
  }
  // Then a sweep on a contract long enough to have a real schedule.
  for (double cpn : {0.0, 100.0, 500.0, 3600.0, 7200.0}) {
    for (double spread : {25.0, 100.0, 500.0, 3600.0, 7200.0}) {
      for (double recovery : {0.0, 0.2, 0.4, 0.8}) {
        for (bool clean : {true, false}) {
          for (long dcc : {JPMCDS_ACT_360, JPMCDS_ACT_365F}) {
            combos.push_back(
                {JpmcdsDate(2013, 2, 12), cpn, spread, recovery, dcc, 6, 'F', true, clean});
          }
        }
      }
    }
  }

  std::vector<std::string> rows;
  for (const Combo &k : combos) {
    TDateInterval iv{k.months, 'M', 0};
    TStubMethod stub;
    stub.stubAtEnd = FALSE;
    stub.longStub = FALSE;
    double charge = 0.0;
    const int rc = JpmcdsCdsoneUpfrontCharge(
        today, value, bench, stepin, start, k.end, k.coupon_bps / 10000.0, k.accrual ? TRUE : FALSE,
        &iv, &stub, k.dcc, static_cast<long>(k.bad_day), const_cast<char *>("None"), ir,
        k.spread_bps / 10000.0, k.recovery, k.clean ? TRUE : FALSE, &charge);
    const bool solved = rc == SUCCESS;
    rows.push_back("    {" + std::to_string(today) + ", " + std::to_string(value) + ", " +
                   std::to_string(bench) + ", " + std::to_string(stepin) + ", " +
                   std::to_string(start) + ", " + std::to_string(k.end) + ", " +
                   num(k.coupon_bps / 10000.0) + ", " + num(k.spread_bps / 10000.0) + ", " +
                   num(k.recovery) + ", " + std::to_string(k.dcc) + ", " +
                   std::to_string(k.months) + ", '" + k.bad_day + "', " +
                   (k.accrual ? "true" : "false") + ", " + (k.clean ? "true" : "false") + ", " +
                   (solved ? "true" : "false") + ", " + num(solved ? charge : 0.0) + "},");
  }
  std::printf("inline constexpr std::array<CdsoneChargeCase, %zu> kCdsoneChargeCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // The inverse: charges generated from real spreads, then solved back.
  std::vector<std::string> spreads;
  for (double cpn : {0.0, 500.0, 3600.0}) {
    for (double recovery : {0.0, 0.4, 0.8}) {
      for (double seed : {100.0, 500.0, 3600.0}) {
        TDateInterval iv{6, 'M', 0};
        TStubMethod stub;
        stub.stubAtEnd = FALSE;
        stub.longStub = FALSE;
        const TDate end = JpmcdsDate(2013, 2, 12);
        double charge = 0.0;
        if (JpmcdsCdsoneUpfrontCharge(today, value, bench, stepin, start, end, cpn / 10000.0, TRUE,
                                      &iv, &stub, JPMCDS_ACT_360, 'F', const_cast<char *>("None"),
                                      ir, seed / 10000.0, recovery, FALSE, &charge) != SUCCESS) {
          continue;
        }
        double spread = 0.0;
        const bool solved =
            JpmcdsCdsoneSpread(today, value, bench, stepin, start, end, cpn / 10000.0, TRUE, &iv,
                               &stub, JPMCDS_ACT_360, 'F', const_cast<char *>("None"), ir, charge,
                               recovery, FALSE, &spread) == SUCCESS;
        spreads.push_back("    {" + std::to_string(end) + ", " + num(cpn / 10000.0) + ", " +
                          num(recovery) + ", " + num(charge) + ", " + (solved ? "true" : "false") +
                          ", " + num(solved ? spread : 0.0) + "},");
      }
    }
  }
  std::printf("inline constexpr std::array<CdsoneSpreadCase, %zu> kCdsoneSpreadCases{{\n",
              spreads.size());
  for (const std::string &r : spreads) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");

  JpmcdsFreeTCurve(ir);
  return 0;
}
