// Emits tests/golden/cds_cases.hpp -- recorded reference prices for the
// vanilla CDS.
//
// This is the set that matters most. CI has no oracle, so without these the
// port's prices are verified on one laptop and nowhere else.
//
// The market curves are recorded alongside the prices rather than duplicated
// as literals in the test, because a price is meaningless without the curve
// that produced it and two copies of a curve drift.
//
// Licence note: these are data derived from running the upstream model,
// exempt under section 4(b). No upstream source is reproduced.
//
//   cmake --build --preset debug --target gen_cds_golden
//   ./build/debug/gen_cds_golden > tests/golden/cds_cases.hpp

#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cds.h"
#include "cerror.h"
#include "cx.h"
#include "dateconv.h"
#include "ldate.h"
#include "stub.h"
#include "tcurve.h"
}

namespace {

constexpr std::size_t kCurveMax = 8;

struct Curve {
  std::vector<TDate> dates;
  std::vector<double> rates;
  TCurve *tc = nullptr;
};

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

Curve make_curve(TDate base, const std::vector<std::pair<TDate, double>> &pts) {
  Curve c;
  for (const auto &[date, rate] : pts) {
    c.dates.push_back(date);
    c.rates.push_back(rate);
  }
  c.tc = JpmcdsMakeTCurve(base, c.dates.data(), c.rates.data(), static_cast<int>(pts.size()), 1.0,
                          JPMCDS_ACT_365F);
  return c;
}

std::string render_curve(TDate base, const Curve &c) {
  std::string out = "{" + std::to_string(base) + ", " + std::to_string(c.dates.size()) + ", {{";
  for (std::size_t i = 0; i < kCurveMax; ++i) {
    out += (i ? ", " : "");
    out += i < c.dates.size() ? std::to_string(c.dates[i]) : "0";
  }
  out += "}}, {{";
  for (std::size_t i = 0; i < kCurveMax; ++i) {
    out += (i ? ", " : "");
    out += i < c.rates.size() ? num(c.rates[i]) : "0.0";
  }
  return out + "}}}";
}

TDate ymd(int y, int m, int d) { return JpmcdsDate(y, m, d); }

} // namespace

int main() {
  JpmcdsErrMsgOff();

  const TDate today = ymd(2024, 6, 20);
  const Curve discount = make_curve(today, {{ymd(2024, 6, 20), 0.028},
                                            {ymd(2025, 6, 20), 0.031},
                                            {ymd(2027, 6, 20), 0.034},
                                            {ymd(2031, 6, 20), 0.037},
                                            {ymd(2036, 6, 20), 0.039}});

  const std::vector<double> levels{0.002, 0.012, 0.05, 0.2};
  std::vector<Curve> hazards;
  for (double level : levels) {
    hazards.push_back(make_curve(today, {{ymd(2025, 6, 20), level},
                                         {ymd(2027, 6, 20), level * 1.15},
                                         {ymd(2031, 6, 20), level * 1.3},
                                         {ymd(2036, 6, 20), level * 1.4}}));
  }

  std::printf(
      "// GENERATED FILE -- DO NOT EDIT.\n"
      "//\n"
      "// Recorded reference prices for the vanilla CDS, with the market curves\n"
      "// that produced them. Data derived from running the upstream model\n"
      "// (licence 4(b)). These are what verifies the price in CI, where no\n"
      "// oracle exists.\n"
      "//\n"
      "// Regenerate with a local upstream copy present:\n"
      "//   cmake --build --preset debug --target gen_cds_golden\n"
      "//   ./build/debug/gen_cds_golden > tests/golden/cds_cases.hpp\n"
      "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
      "namespace cdsmodel::golden {\n\n"
      "inline constexpr std::size_t kCdsCurveMax = 8;\n\n"
      "struct CurveSpec {\n  long base;\n  std::size_t n;\n"
      "  std::array<long, kCdsCurveMax> dates;\n"
      "  std::array<double, kCdsCurveMax> rates;\n};\n\n"
      "/// stub_pos: 0 = front, 1 = back. stub_len: 0 = short, 1 = long.\n"
      "/// weekend: 0 = no weekend at all, 0x60 = Saturday and Sunday.\n"
      "struct CdsPriceCase {\n"
      "  int hazard;\n  long start;\n  long end;\n  double coupon;\n"
      "  int interval_months;\n  int stub_pos;\n  int stub_len;\n  long dcc;\n"
      "  char bad_day;\n  long weekend;\n  bool pay_accrual_on_default;\n  bool clean;\n"
      "  double recovery;\n  int stepin_offset;\n  int settle_offset;\n  double price;\n};\n\n"
      "struct CdsParSpreadCase {\n"
      "  int hazard;\n  long start;\n  long end;\n  long dcc;\n"
      "  bool pay_accrual_on_default;\n  double spread;\n};\n\n");

  std::printf("inline constexpr long kCdsToday = %ld;\n\n", static_cast<long>(today));
  std::printf("inline constexpr CurveSpec kCdsDiscountCurve = %s;\n\n",
              render_curve(today, discount).c_str());
  std::printf("inline constexpr std::array<CurveSpec, %zu> kCdsHazardCurves{{\n", hazards.size());
  for (const Curve &h : hazards) {
    std::printf("    %s,\n", render_curve(today, h).c_str());
  }
  std::printf("}};\n\n");

  const TDate start = ymd(2024, 3, 20);
  struct Combo {
    int interval_months;
    int stub_pos;
    int stub_len;
    long dcc;
    char bad_day;
    long weekend;
    bool accr;
    bool clean;
    double recovery;
    int stepin_offset;
    int settle_offset;
  };
  const std::vector<Combo> combos{
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, false, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, false, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_365F, 'N', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_B30_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {6, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {12, 1, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {3, 1, 1, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 1, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'F', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'P', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'M', 0x60, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'F', 0x00, true, true, 0.4, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.0, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.8, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 1.0, 0, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 17, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, false, 0.4, 17, 0},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 40, 3},
      {3, 0, 0, JPMCDS_ACT_360, 'N', 0x60, true, true, 0.4, 95, 30},
  };
  const std::vector<TDate> ends{ymd(2025, 6, 20), ymd(2029, 6, 20), ymd(2031, 12, 20),
                                ymd(2027, 8, 7)};
  const std::vector<double> coupons{0.0, 0.005, 0.01, 0.02, 0.05};

  std::vector<std::string> rows;
  for (std::size_t h = 0; h < hazards.size(); ++h) {
    for (const Combo &k : combos) {
      for (TDate end : ends) {
        for (double coupon : coupons) {
          TDateInterval iv{k.interval_months, 'M', 0};
          TStubMethod stub;
          stub.stubAtEnd = k.stub_pos == 1 ? TRUE : FALSE;
          stub.longStub = k.stub_len == 1 ? TRUE : FALSE;
          const char *calendar = k.weekend == 0 ? "NO_WEEKENDS" : "NONE";
          double price = 0.0;
          if (JpmcdsCdsPrice(today, today + k.settle_offset, today + k.stepin_offset, start, end,
                             coupon, k.accr ? TRUE : FALSE, &iv, &stub, k.dcc,
                             static_cast<long>(k.bad_day), const_cast<char *>(calendar),
                             discount.tc, hazards[h].tc, k.recovery, k.clean ? TRUE : FALSE,
                             &price) != SUCCESS) {
            continue; // not recorded: upstream refused these inputs
          }
          rows.push_back("    {" + std::to_string(h) + ", " + std::to_string(start) + ", " +
                         std::to_string(end) + ", " + num(coupon) + ", " +
                         std::to_string(k.interval_months) + ", " + std::to_string(k.stub_pos) +
                         ", " + std::to_string(k.stub_len) + ", " + std::to_string(k.dcc) + ", '" +
                         k.bad_day + "', " + std::to_string(k.weekend) + ", " +
                         (k.accr ? "true" : "false") + ", " + (k.clean ? "true" : "false") + ", " +
                         num(k.recovery) + ", " + std::to_string(k.stepin_offset) + ", " +
                         std::to_string(k.settle_offset) + ", " + num(price) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<CdsPriceCase, %zu> kCdsPriceCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  std::vector<std::string> par;
  for (std::size_t h = 0; h < hazards.size(); ++h) {
    for (TDate end : ends) {
      for (long dcc : {JPMCDS_ACT_360, JPMCDS_ACT_365F}) {
        for (bool accr : {true, false}) {
          TDateInterval iv{3, 'M', 0};
          TStubMethod stub;
          stub.stubAtEnd = FALSE;
          stub.longStub = FALSE;
          TDate end_t = end;
          double spread = 0.0;
          if (JpmcdsCdsParSpreads(today, today, start, 1, &end_t, accr ? TRUE : FALSE, &iv, &stub,
                                  dcc, 'N', const_cast<char *>("NONE"), discount.tc, hazards[h].tc,
                                  0.4, &spread) != SUCCESS) {
            continue;
          }
          par.push_back("    {" + std::to_string(h) + ", " + std::to_string(start) + ", " +
                        std::to_string(end) + ", " + std::to_string(dcc) + ", " +
                        (accr ? "true" : "false") + ", " + num(spread) + "},");
        }
      }
    }
  }
  std::printf("inline constexpr std::array<CdsParSpreadCase, %zu> kCdsParSpreadCases{{\n",
              par.size());
  for (const std::string &r : par) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");

  JpmcdsFreeTCurve(discount.tc);
  for (Curve &h : hazards) {
    JpmcdsFreeTCurve(h.tc);
  }
  return 0;
}
