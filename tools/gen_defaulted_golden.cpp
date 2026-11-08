// Emits tests/golden/defaulted_cases.hpp.
//
// Shaped like the date list generator rather than the others, because this
// module CORRECTS upstream (UPSTREAM-11) and recording upstream's answers
// would enshrine a negative accrual.
//
// What is recorded is the DOCUMENTED INTENT -- computed by the port -- with a
// flag saying whether upstream agreed at generation time. That flag is the
// useful part: a regeneration where upstream suddenly agrees everywhere means
// it was fixed upstream and the correction should be revisited.
//
// Licence note: the intended answers are this project's own; the agreement
// flags are data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_defaulted_golden
//   ./build/debug/gen_defaulted_golden > tests/golden/defaulted_cases.hpp

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/defaulted.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "cdate.h"
#include "cerror.h"
#include "cx.h"
#include "dateconv.h"
#include "defaulted.h"
#include "ldate.h"
#include "stub.h"
}

namespace {

std::string num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  return buf;
}

} // namespace

int main() {
  JpmcdsErrMsgOff();

  std::printf("// GENERATED FILE -- DO NOT EDIT.\n"
              "//\n"
              "// Unlike most recorded vector sets, these are the DOCUMENTED INTENT\n"
              "// rather than upstream's answers: this module corrects upstream\n"
              "// (UPSTREAM-11), which returns a NEGATIVE accrual with SUCCESS once the\n"
              "// trade date crosses a coupon date the credit event did not.\n"
              "//\n"
              "// `upstream_agreed` records whether the upstream implementation matched\n"
              "// the intent at generation time. If a regeneration turns those all\n"
              "// true, upstream fixed it and the correction should be revisited.\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_defaulted_golden\n"
              "//   ./build/debug/gen_defaulted_golden > tests/golden/defaulted_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// solved == false means the intent is \"no answer\": a credit event\n"
              "/// before the contract start, or outside every coupon period.\n"
              "struct DefaultedCase {\n"
              "  long start;\n  long end;\n  long trade;\n  long event;\n"
              "  double coupon_rate;\n  double notional;\n  long dcc;\n"
              "  int interval_months;\n  char bad_day;\n"
              "  bool solved;\n  long days;\n  double amount;\n"
              "  bool upstream_agreed;\n};\n\n");

  const std::vector<cdsmodel::Calendar> cals{
      *cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};

  struct Conv {
    long dcc;
    int months;
    char bad_day;
  };
  const std::vector<Conv> convs{{JPMCDS_ACT_360, 3, 'N'}, {JPMCDS_ACT_365F, 3, 'N'},
                                {JPMCDS_B30_360, 3, 'N'}, {JPMCDS_ACT_360, 6, 'N'},
                                {JPMCDS_ACT_360, 3, 'F'}, {JPMCDS_ACT_360, 12, 'N'}};

  std::vector<std::string> rows;
  for (const Conv &conv : convs) {
    cdsmodel::Cds cds;
    cds.start = *cdsmodel::make_date(2024, 3, 20);
    cds.end = *cdsmodel::make_date(2029, 3, 20);
    cds.coupon_rate = 0.01;
    cds.coupon_interval = cdsmodel::DateInterval{conv.months, cdsmodel::PeriodUnit::Months};
    cds.day_count = *cdsmodel::from_upstream_code(conv.dcc);
    cds.bad_day = *cdsmodel::bad_day_conv_from_upstream(static_cast<long>(conv.bad_day));

    for (int event_offset = -30; event_offset <= 1100; event_offset += 37) {
      const cdsmodel::Date event = cds.start + std::chrono::days{event_offset};
      for (int lag : {-10, 0, 1, 20, 50, 100, 400}) {
        const cdsmodel::Date trade = event + std::chrono::days{lag};
        const cdsmodel::Expected<cdsmodel::DefaultAccrual> intent =
            cdsmodel::default_accrual(cds, trade, event, 1.0e7, cals);

        TDateInterval iv{conv.months, 'M', 0};
        TStubMethod stub;
        stub.stubAtEnd = FALSE;
        stub.longStub = FALSE;
        double ref_days = 0.0;
        double ref_amount = 0.0;
        const bool ref_ok =
            JpmcdsDefaultAccrual(cdsmodel::to_tdate(trade), cdsmodel::to_tdate(event),
                                 cdsmodel::to_tdate(cds.start), cdsmodel::to_tdate(cds.end), &iv,
                                 &stub, 1.0e7, cds.coupon_rate, conv.dcc,
                                 static_cast<long>(conv.bad_day), const_cast<char *>("NONE"),
                                 &ref_days, &ref_amount) == SUCCESS;

        const bool agreed = intent.has_value() == ref_ok &&
                            (!intent || (static_cast<double>(intent->days) == ref_days &&
                                         intent->amount == ref_amount));

        rows.push_back(
            "    {" + std::to_string(cdsmodel::to_tdate(cds.start)) + ", " +
            std::to_string(cdsmodel::to_tdate(cds.end)) + ", " +
            std::to_string(cdsmodel::to_tdate(trade)) + ", " +
            std::to_string(cdsmodel::to_tdate(event)) + ", " + num(cds.coupon_rate) + ", " +
            num(1.0e7) + ", " + std::to_string(conv.dcc) + ", " + std::to_string(conv.months) +
            ", '" + conv.bad_day + "', " + (intent ? "true" : "false") + ", " +
            std::to_string(intent ? intent->days : 0L) + ", " + num(intent ? intent->amount : 0.0) +
            ", " + (agreed ? "true" : "false") + "},");
      }
    }
  }
  std::printf("inline constexpr std::array<DefaultedCase, %zu> kDefaultedCases{{\n", rows.size());
  for (const std::string &r : rows) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
