// Emits tests/golden/cash_flow_schedule_cases.hpp.
//
// Records what JpmcdsMakeCFL produces across flags, stub position and length,
// and spans that do and do not divide evenly. Each amount is rate times a year
// fraction -- one multiplication, no accumulation, no transcendental function
// -- so the vectors are compared EXACTLY.
//
// The flag combinations include the illegal ones. A replay that only covered
// the accepted inputs would verify half the contract, and the refusals are the
// half upstream states in its error messages.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_cash_flow_schedule_golden
//   ./build/debug/gen_cash_flow_schedule_golden > tests/golden/cash_flow_schedule_cases.hpp

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
#include "ldate.h"
#include "stub.h"
}

namespace {

constexpr std::size_t kMaxFlows = 32;

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

struct Span {
  int y1, m1, d1, y2, m2, d2;
};

const Span kSpans[] = {
    {2024, 1, 15, 2027, 1, 15}, {2024, 2, 20, 2027, 5, 10}, {2024, 1, 31, 2026, 7, 31},
    {2024, 2, 29, 2027, 2, 28}, {2024, 1, 15, 2024, 4, 15}, {2024, 6, 30, 2029, 12, 31},
};

const long kFlagSets[] = {
    0,
    JPMCDS_ADD_FINAL,
    JPMCDS_SUBTRACT_INITIAL,
    JPMCDS_PRESTART_ZERO_PAYMENT,
    JPMCDS_POST_MATURITY_ZERO_PAYMENT,
    JPMCDS_KEEP_START_DATE,
    JPMCDS_SUBTRACT_INITIAL | JPMCDS_ADD_FINAL,
    JPMCDS_ADD_FINAL | JPMCDS_KEEP_START_DATE,
    JPMCDS_SUBTRACT_INITIAL | JPMCDS_PRESTART_ZERO_PAYMENT,
    JPMCDS_PRESTART_ZERO_PAYMENT | JPMCDS_KEEP_START_DATE,
    JPMCDS_ADD_FINAL | JPMCDS_POST_MATURITY_ZERO_PAYMENT,
};

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Recorded coupon streams from JpmcdsMakeCFL. Each amount is rate times a\n"
              "// year fraction -- one multiplication -- so these are compared EXACTLY.\n"
              "// The flag sets include the illegal combinations, whose `accepted` is\n"
              "// false; a replay covering only the accepted ones would verify half the\n"
              "// contract. Data derived from running the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_cash_flow_schedule_golden\n"
              "//   ./build/debug/gen_cash_flow_schedule_golden > "
              "tests/golden/cash_flow_schedule_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One recorded coupon stream. `stub_placement` is upstream's packed\n"
              "/// position-and-length (bit 0 = back, bit 1 = long); `flags` is its flag\n"
              "/// word; `bad_day` is the convention applied to both the accrual and the\n"
              "/// payment side, as JpmcdsMakeCFL applies one file to both.\n"
              "struct CashFlowScheduleCase {\n"
              "  TDate start;\n"
              "  TDate maturity;\n"
              "  int periods;\n"
              "  long stub_placement;\n"
              "  long flags;\n"
              "  long bad_day;\n"
              "  double rate;\n"
              "  bool accepted;\n"
              "  std::size_t n;\n"
              "  std::array<TDate, %zu> dates;\n"
              "  std::array<double, %zu> amounts;\n"
              "};\n\n",
              kMaxFlows, kMaxFlows);

  std::vector<std::string> rows;
  const long bad_days[] = {JPMCDS_BAD_DAY_NONE, 'F', 'P', 'M'};

  for (const Span &span : kSpans) {
    const TDate start = JpmcdsDate(span.y1, span.m1, span.d1);
    const TDate maturity = JpmcdsDate(span.y2, span.m2, span.d2);
    if (start <= 0 || maturity <= 0) {
      continue;
    }
    for (const int periods : {3, 6, 12}) {
      TDateInterval interval;
      if (JpmcdsMakeDateInterval(periods, 'M', &interval) != SUCCESS) {
        continue;
      }
      for (const long stub_placement : {0L, 1L, 2L, 3L}) {
        for (const long flags : kFlagSets) {
          for (const long bad_day : bad_days) {
            // The full cross product with four conventions is large; only the
            // unflagged stream is swept over conventions, which is where the
            // adjustment shows.
            if (bad_day != JPMCDS_BAD_DAY_NONE && flags != JPMCDS_ADD_FINAL) {
              continue;
            }
            TCashFlowList *cfl =
                JpmcdsMakeCFL(0.05, start, &interval, maturity, JPMCDS_ACT_365F, stub_placement,
                              flags, bad_day, bad_day, (char *)"NONE");
            const bool accepted = cfl != nullptr;
            const std::size_t n = accepted ? (std::size_t)cfl->fNumItems : (std::size_t)0;
            if (n > kMaxFlows) {
              JpmcdsFreeCFL(cfl);
              continue;
            }
            std::string dates_literal = "{";
            std::string amounts_literal = "{";
            for (std::size_t i = 0; i < kMaxFlows; ++i) {
              const char *sep = i == 0 ? "" : ", ";
              dates_literal += sep;
              amounts_literal += sep;
              const bool present = i < n;
              dates_literal += present ? std::to_string((long)cfl->fArray[i].fDate) : "0";
              amounts_literal += present ? num(cfl->fArray[i].fAmount) : "0";
            }
            dates_literal += "}";
            amounts_literal += "}";

            char head[192];
            std::snprintf(head, sizeof head, "    {%ld, %ld, %d, %ld, %ld, %ld, %s, %s, %zu, ",
                          (long)start, (long)maturity, periods, stub_placement, flags, bad_day,
                          num(0.05).c_str(), accepted ? "true" : "false", n);
            rows.emplace_back(std::string(head) + dates_literal + ", " + amounts_literal + "},");
            JpmcdsFreeCFL(cfl);
          }
        }
      }
    }
  }

  std::printf("inline constexpr std::array<CashFlowScheduleCase, %zu> kCashFlowScheduleCases{{\n",
              rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
