// Emits tests/golden/stub_cases.hpp.
//
// Records the three functions whose answers are data: the stub payment, the
// end-stub decision, and the packed split. Each is a single arithmetic step or
// a switch, so the vectors are compared EXACTLY -- the stub payment is one
// multiplication of a year fraction, with no accumulation and no
// transcendental function anywhere.
//
// The acceptance boundary is what these record. All three refuse more inputs
// than they accept, and a replay that only covered the accepted ones would
// verify the easy half.
//
// Licence note: data derived from running the upstream model, exempt under
// section 4(b).
//
//   cmake --build --preset debug --target gen_stub_golden
//   ./build/debug/gen_stub_golden > tests/golden/stub_cases.hpp

#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "bastypes.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "convert.h"
#include "date_sup.h"
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
              "// Recorded stub arithmetic and stub decisions. One multiplication or one\n"
              "// switch each, so these are compared EXACTLY. Data derived from running\n"
              "// the upstream model (licence 4(b)).\n"
              "//\n"
              "// Regenerate with a local upstream copy present:\n"
              "//   cmake --build --preset debug --target gen_stub_golden\n"
              "//   ./build/debug/gen_stub_golden > tests/golden/stub_cases.hpp\n"
              "\n#pragma once\n\n#include <array>\n#include <cstddef>\n\n"
              "#include <cdsmodel/date.hpp>\n\n"
              "namespace cdsmodel::golden {\n\n"
              "/// One stub payment. `accepted` is false where upstream refuses, which is\n"
              "/// most of the sweep -- the acceptance boundary is the point.\n"
              "struct StubPaymentCase {\n"
              "  TDate prev_coupon;\n"
              "  TDate next_coupon;\n"
              "  TDate stub_start;\n"
              "  TDate stub_end;\n"
              "  double rate;\n"
              "  long day_count;\n"
              "  bool accepted;\n"
              "  double payment;\n"
              "};\n\n");

  const TDate prev = JpmcdsDate(2024, 1, 15);
  const TDate next = JpmcdsDate(2024, 7, 15);
  const TDate boundaries[] = {JpmcdsDate(2023, 12, 1), JpmcdsDate(2024, 1, 15),
                              JpmcdsDate(2024, 2, 29), JpmcdsDate(2024, 4, 15),
                              JpmcdsDate(2024, 7, 15), JpmcdsDate(2024, 9, 1)};
  const long dccs[] = {JPMCDS_ACT_365F, JPMCDS_ACT_360, JPMCDS_B30_360, JPMCDS_B30E_360,
                       JPMCDS_ACT_365};
  const double rates[] = {0.0, 0.05, -0.02, 1.5};

  std::vector<std::string> payments;
  for (const TDate stub_start : boundaries) {
    for (const TDate stub_end : boundaries) {
      for (const long dcc : dccs) {
        for (const double rate : rates) {
          double payment = 0.0;
          const bool accepted =
              JpmcdsStubPayment(prev, next, stub_start, stub_end, rate, dcc, &payment) == SUCCESS;
          char row[256];
          std::snprintf(row, sizeof row, "    {%ld, %ld, %ld, %ld, %s, %ld, %s, %s},", (long)prev,
                        (long)next, (long)stub_start, (long)stub_end, num(rate).c_str(), dcc,
                        accepted ? "true" : "false", num(accepted ? payment : 0.0).c_str());
          payments.emplace_back(row);
        }
      }
    }
  }
  std::printf("inline constexpr std::array<StubPaymentCase, %zu> kStubPaymentCases{{\n",
              payments.size());
  for (const std::string &r : payments) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ------------------------------------------------------------------
  std::printf("/// One end-stub decision. `position` is upstream's TStubPos:\n"
              "/// 0 = auto, 1 = front, 2 = back.\n"
              "struct EndStubCase {\n"
              "  TDate start;\n"
              "  TDate maturity;\n"
              "  int periods;\n"
              "  long position;\n"
              "  bool accepted;\n"
              "  bool is_end_stub;\n"
              "};\n\n");

  struct Span {
    int y1, m1, d1, y2, m2, d2;
  };
  const Span spans[] = {{2024, 1, 15, 2027, 1, 15}, {2024, 1, 15, 2024, 2, 20},
                        {2024, 2, 29, 2027, 2, 28}, {2024, 3, 1, 2029, 11, 30},
                        {2024, 1, 15, 2024, 1, 15}, {2024, 6, 30, 2026, 12, 31},
                        {2024, 11, 30, 2031, 5, 31}};
  std::vector<std::string> decisions;
  for (const Span &span : spans) {
    const TDate start = JpmcdsDate(span.y1, span.m1, span.d1);
    const TDate maturity = JpmcdsDate(span.y2, span.m2, span.d2);
    if (start <= 0 || maturity <= 0) {
      continue;
    }
    for (const int periods : {1, 2, 3, 4, 6, 7, 12, 13}) {
      TDateInterval iv;
      if (JpmcdsMakeDateInterval(periods, 'M', &iv) != SUCCESS) {
        continue;
      }
      for (const long position : {0L, 1L, 2L}) {
        TBoolean answer = FALSE;
        const bool accepted =
            JpmcdsIsEndStub(start, maturity, &iv, (TStubPos)position, &answer) == SUCCESS;
        char row[192];
        std::snprintf(row, sizeof row, "    {%ld, %ld, %d, %ld, %s, %s},", (long)start,
                      (long)maturity, periods, position, accepted ? "true" : "false",
                      (accepted && answer == TRUE) ? "true" : "false");
        decisions.emplace_back(row);
      }
    }
  }
  std::printf("inline constexpr std::array<EndStubCase, %zu> kEndStubCases{{\n", decisions.size());
  for (const std::string &r : decisions) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n");

  // ------------------------------------------------------------------
  std::printf("/// One packed split. Upstream validates the stub position and NOT the\n"
              "/// bad day convention, so `accepted` can be true with `conv_valid`\n"
              "/// false -- which is where the port deliberately diverges.\n"
              "struct SplitCase {\n"
              "  long packed;\n"
              "  bool accepted;\n"
              "  long conv;\n"
              "  long position;\n"
              "  bool conv_valid;\n"
              "};\n\n");

  std::vector<std::string> splits;
  // Every stub-position byte from 0 to 3, crossed with valid and invalid
  // convention bytes.
  const long convs[] = {'F', 'P', 'N', 'M', 'A', 'f', 0x00, 0xFF, 0x20, 0x7A};
  for (long position_byte = 0; position_byte <= 3; ++position_byte) {
    for (const long conv : convs) {
      const long packed = (position_byte << 8) | conv;
      long out_conv = -1;
      TStubPos out_position{};
      const bool accepted = JpmcdsBadDayAndStubPosSplit((TBadDayAndStubPos)packed, &out_conv,
                                                        &out_position) == SUCCESS;
      const bool conv_valid = accepted && JpmcdsBadDayConvValid("gen", out_conv) == SUCCESS;
      char row[160];
      std::snprintf(row, sizeof row, "    {%ld, %s, %ld, %ld, %s},", packed,
                    accepted ? "true" : "false", accepted ? out_conv : -1,
                    accepted ? (long)out_position : -1, conv_valid ? "true" : "false");
      splits.emplace_back(row);
    }
  }
  std::printf("inline constexpr std::array<SplitCase, %zu> kSplitCases{{\n", splits.size());
  for (const std::string &r : splits) {
    std::printf("%s\n", r.c_str());
  }
  std::printf("}};\n\n} // namespace cdsmodel::golden\n");
  return 0;
}
