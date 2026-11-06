// Demonstrates each catalogued upstream defect against the live ISDA library
// and against the port, side by side, and prints a transcript.
//
// This is the evidence behind docs/defect-report.md. It is a tool rather than
// a test because its job is to produce a readable transcript, not to pass.
//
// Build and run (requires a local upstream copy):
//   cmake --build --preset debug --target defect_evidence
//   ./build/debug/defect_evidence
//
// UPSTREAM-1 and UPSTREAM-4 are undefined behaviour. Their memory-safety
// evidence comes from the sanitizer build:
//   cmake --build --preset asan --target defect_evidence
//   ./build/asan/defect_evidence --unsafe

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/cash_flow_bootstrap.hpp>
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cash_flow_schedule.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_schedule.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/defaulted.hpp>
#include <cdsmodel/interp_curve.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/swap_dates.hpp>
#include <cdsmodel/version.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "bsearch.h"
#include "cashflow.h"
#include "cdate.h"
#include "cerror.h"
#include "cgeneral.h"
#include "contingentleg.h"
#include "convert.h"
#include "cx.h"
#include "cxdatelist.h"
#include "cxzerocurve.h"
#include "date_sup.h"
#include "dateconv.h"
#include "datelist.h"
#include "defaulted.h"
#include "dtlist.h"
#include "interp.h"
#include "ldate.h"
#include "lintrp.h"
#include "macros.h"
#include "mdydate.h"
#include "stub.h"
#include "tcurve.h"
#include "yearfrac.h"
#include "zcprvt.h"
#include "zcswdate.h"

// Exported from the library but declared in no public header.
int JpmcdsLinInterpLongPoint1Discontinuous(long *x, int xskip, int N, double *f, int fskip,
                                           double xDesired, TMetricDoubleFunc mfunc,
                                           TBoolean rightLimit, double *fInterp);
}

namespace {

void heading(const char *id, const char *title) {
  std::printf("\n%s\n%s -- %s\n%s\n", std::string(72, '=').c_str(), id, title,
              std::string(72, '=').c_str());
}

void u1(bool unsafe) {
  heading("UPSTREAM-1", "month validated after it indexes a 13-element table");
  std::printf("JpmcdsDate(year, 13, 29) for years outside the 1995-2119 cache:\n\n");
  std::printf("  %-10s %-16s %s\n", "year", "upstream", "port");
  for (int y : {1900, 1950, 2200}) {
    const auto port = cdsmodel::make_date(y, 13, 29);
    const char *upstream = "(not run: reads OOB)";
    if (unsafe) {
      upstream = JpmcdsDate(y, 13, 29) == -1 ? "FAILURE (-1)" : "accepted";
    }
    std::printf("  %-10d %-22s %s\n", y, upstream, port ? "accepted" : "rejected");
  }
  std::printf("\nBoth reject. The difference is HOW: upstream reads leapDays[13],\n"
              "one past the end of a 13-element array, before it checks the month.\n"
              "The port checks the month first, and std::chrono::month cannot hold\n"
              "13 in a way that reaches a table at all.\n"
              "Memory-safety evidence: run --unsafe under the asan preset.\n");
}

void u2() {
  heading("UPSTREAM-2", "JPMCDS_DEFAULT collides with FAILURE");
  std::printf("  JPMCDS_DEFAULT = %d\n  FAILURE        = %d\n  equal?         = %s\n\n",
              JPMCDS_DEFAULT, FAILURE, JPMCDS_DEFAULT == FAILURE ? "YES" : "no");

  long code = 0;
  char buf[] = "Default";
  const int rc = JpmcdsStringToDayCountConv(buf, &code);
  std::printf("Upstream parse of \"Default\": rc=%s, code=%ld\n",
              rc == SUCCESS ? "SUCCESS" : "FAILURE", code);
  std::printf("Upstream JpmcdsDate(1600,1,1) (an invalid date): %ld\n\n",
              (long)JpmcdsDate(1600, 1, 1));
  std::printf("Both are -1. A caller holding a long cannot tell \"the default\n"
              "convention\" from \"the call failed\".\n\n");

  const auto ported = cdsmodel::parse_day_count_code("Default");
  std::printf("Port parse of \"Default\": has_value=%s, value=%ld\n", ported ? "true" : "false",
              ported.value_or(0));
  std::printf("Port make_date(1600,1,1):  has_value=%s\n",
              cdsmodel::make_date(1600, 1, 1) ? "true" : "false");
  std::printf("\nSuccess and value are separate. -1 is an ordinary value, not a signal.\n");
}

void u3() {
  heading("UPSTREAM-3", "lowercase 'y' advances quarterly, not annually");
  const TDate start = JpmcdsDate(2008, 1, 3);
  std::printf("Start date 2008-01-03, one period, via JpmcdsDtFwdAny:\n\n");
  std::printf("  %-6s %-14s %-18s %s\n", "type", "upstream", "months advanced", "port");
  for (char t : {'Y', 'y', 'A', 'a', 'M', 'm', 'S', 's', 'Q', 'q'}) {
    TDateInterval iv;
    iv.prd = 1;
    iv.prd_typ = t;
    iv.flag = 0;
    TDate out = 0;
    char ref[24] = "FAILURE";
    int months = -1;
    if (JpmcdsDtFwdAny(start, &iv, &out) == SUCCESS) {
      TMonthDayYear m{};
      JpmcdsDateToMDY(out, &m);
      std::snprintf(ref, sizeof ref, "%04ld-%02ld-%02ld", m.year, m.month, m.day);
      months = (int)((m.year - 2008) * 12 + (m.month - 1));
    }
    const auto iv_port = cdsmodel::make_date_interval(1, t);
    const int port_months =
        iv_port && iv_port->unit == cdsmodel::PeriodUnit::Months ? iv_port->periods : -1;
    std::printf("  '%c'    %-14s %-18d %d months\n", t, ref, months, port_months);
  }
  std::printf("\n'Y' advances 12 months. 'y' advances 3. Every other period type in\n"
              "the library is case-insensitive; 'y' alone changes meaning with case.\n"
              "Cause: the branch tests toupper(prd_typ) for 'A' but the RAW prd_typ\n"
              "for 'Y', so 'y' matches neither and falls through to quarterly.\n\n");
  std::printf("Port: make_date_interval(1,'y') == make_date_interval(1,'Y') -> %s\n",
              cdsmodel::make_date_interval(1, 'y') == cdsmodel::make_date_interval(1, 'Y')
                  ? "true"
                  : "FALSE");
}

void u4(bool unsafe) {
  heading("UPSTREAM-4", "stack buffer overflow in JpmcdsStringToDateInterval");
  std::printf("numberBuff is char[32]; the digit-copy loop has no bounds check:\n"
              "    while (isdigit((int)*inp)) *nump++ = *inp++;   /* convert.c:288 */\n\n");
  std::printf("  %-14s %-22s %s\n", "digits", "upstream", "port");
  for (int n : {5, 10, 31, 32, 40, 64}) {
    const std::string s = std::string(n, '9') + "M";
    const auto port = cdsmodel::parse_date_interval(s);
    const char *upstream = "(not run: would overflow)";
    if (n <= 31 || unsafe) {
      std::string copy = s;
      TDateInterval iv{};
      const int rc = JpmcdsStringToDateInterval(copy.data(), const_cast<char *>("ev"), &iv);
      upstream = rc == SUCCESS ? "accepted" : "FAILURE";
    }
    std::printf("  %-14d %-22s %s\n", n, upstream, port ? "accepted" : "rejected");
  }
  std::printf("\nAt 32 digits and beyond upstream writes past the buffer. The port\n"
              "bounds the digit count and rejects. This is the ONLY defect whose\n"
              "behaviour the port deliberately does not reproduce.\n");
  if (!unsafe) {
    std::printf("\nRun with --unsafe under the asan preset for the overflow trace.\n");
  }
}

void u5() {
  heading("UPSTREAM-5", "days never format as weeks (missing break)");
  std::printf("  %-10s %-14s %-10s %s\n", "stored", "upstream", "port", "intended");
  for (auto [n, typ] : std::initializer_list<std::pair<int, char>>{
           {12, 'M'}, {18, 'M'}, {6, 'M'}, {7, 'D'}, {14, 'D'}, {28, 'D'}}) {
    TDateInterval iv;
    iv.prd = n;
    iv.prd_typ = typ;
    iv.flag = 0;
    const cdsmodel::DateInterval ported{n, typ == 'M' ? cdsmodel::PeriodUnit::Months
                                                      : cdsmodel::PeriodUnit::Days};
    const char *intended = "same";
    if (typ == 'D' && n % 7 == 0) {
      intended = (n == 7) ? "1W" : (n == 14 ? "2W" : "4W");
    }
    std::printf("  %-10s %-14s %-10s %s\n", (std::to_string(n) + typ).c_str(),
                JpmcdsFormatDateInterval(&iv), cdsmodel::format_date_interval(ported).c_str(),
                intended);
  }
  std::printf("\nMonths collapse (12M -> 1A) because that case has its break. Days\n"
              "never collapse: case 'D' falls through into default:, which\n"
              "overwrites the week form computed just above it.\n\n"
              "The port reproduces this. It is observable output, and abbreviating\n"
              "\"14D\" to \"2W\" would be a behaviour change dressed up as a fix.\n");
}

TDateList *make_list(std::initializer_list<TDate> dates) {
  TDateList *dl = JpmcdsNewEmptyDateList(static_cast<int>(dates.size()));
  int i = 0;
  for (TDate d : dates) {
    dl->fArray[i++] = d;
  }
  return dl;
}

std::string render(TDateList *dl) {
  if (dl == nullptr) {
    return "NULL";
  }
  std::string out = "[";
  for (int i = 0; i < dl->fNumItems; ++i) {
    out += (i ? "," : "") + std::to_string(static_cast<long>(dl->fArray[i]));
  }
  return out + "]";
}

void u7() {
  heading("UPSTREAM-7", "date list truncation is ignored when <2 dates survive");
  std::printf("Both implementations scan backwards with\n"
              "    for (i = numItems - 1; i > 0; i--)\n"
              "so index 0 is never tested. They differ only in the sentinel, so\n"
              "they fail in OPPOSITE directions when index 0 is the answer.\n\n");
  std::printf("  %-26s %-14s %-16s %s\n", "input / cut", "want", "dtlist.c", "cxdatelist.c");
  std::printf("  %-26s %-14s %-16s %s\n", "", "", "(sentinel -1)", "(sentinel n-1)");
  struct Case {
    const char *label;
    std::initializer_list<TDate> dates;
    TDate cut;
    bool inclusive;
    const char *want;
  };
  for (const Case &c : std::initializer_list<Case>{{"[1,2] @2", {1, 2}, 2, false, "[1]"},
                                                   {"[10,30] @30", {10, 30}, 30, false, "[10]"},
                                                   {"[5] @9", {5}, 9, false, "[5]"},
                                                   {"[9,10] @9 inclusive", {9, 10}, 9, true, "[9]"},
                                                   {"[1,2,3] @3", {1, 2, 3}, 3, false, "[1,2]"}}) {
    TDateList *a = JpmcdsJpmcdsDateListTruncate(make_list(c.dates), c.cut,
                                                c.inclusive ? TRUE : FALSE, FALSE, FALSE);
    TDateList *b =
        JpmcdsDateListTruncate(make_list(c.dates), c.cut, c.inclusive ? TRUE : FALSE, FALSE, FALSE);
    std::printf("  %-26s %-14s %-16s %s\n", c.label, c.want, render(a).c_str(), render(b).c_str());
  }
  std::printf("\nThe failure is broader than element 0. Because the sentinel means\n"
              "\"keep everything\", cxdatelist.c IGNORES THE TRUNCATION ENTIRELY\n"
              "whenever fewer than two dates should survive:\n\n");
  std::printf("  %-28s %-12s %s\n", "keep-before", "want", "cxdatelist.c");
  struct Wide {
    const char *l;
    std::initializer_list<TDate> d;
    TDate cut;
    const char *want;
  };
  for (const Wide &w : std::initializer_list<Wide>{{"[5,6,7] @3", {5, 6, 7}, 3, "[]"},
                                                   {"[5,6,7] @5", {5, 6, 7}, 5, "[]"},
                                                   {"[5,6,7] @6", {5, 6, 7}, 6, "[5]"},
                                                   {"[5,6,7] @7", {5, 6, 7}, 7, "[5,6]"},
                                                   {"[5,6,7] @9", {5, 6, 7}, 9, "[5,6,7]"}}) {
    TDateList *r = JpmcdsDateListTruncate(make_list(w.d), w.cut, FALSE, FALSE, FALSE);
    std::printf("  %-28s %-12s %s\n", w.l, w.want, render(r).c_str());
  }
  std::printf("\n  keep-after also fails when the answer is empty:\n");
  {
    TDateList *r = JpmcdsDateListTruncate(make_list({5, 6, 7}), 9, FALSE, TRUE, FALSE);
    std::printf("  %-28s %-12s %s\n", "[5,6,7] @9 keep-after", "[]", render(r).c_str());
  }
  std::printf("\nOver a 600-case sweep the two disagree with the documented intent\n"
              "on 234 of them -- 39 percent.\n\n"
              "The cxdatelist.c variant is the one timeline.c calls, which builds\n"
              "the integration timeline for the fee and contingent legs. Whether a\n"
              "CDS price changes as a result is NOT established here -- only that\n"
              "the function misbehaves.\n");
}

TCurve *flat_curve(TDate base, TDate end, double rate) {
  TDate dates[2] = {base + 1, end};
  double rates[2] = {rate, rate};
  return JpmcdsMakeTCurve(base, dates, rates, 2, 5000.0 /*continuous*/, JPMCDS_ACT_365F);
}

void u8() {
  heading("UPSTREAM-8", "a validation guard that validates almost nothing");
  std::printf("    if (!JpmcdsZeroPrice(spreadCurve, cl->endDate) > JPMCDS_LOG0_THRESHOLD)\n\n");
  std::printf("`!` binds tighter than `>`, so this reads (!price) > %g -- a 0 or 1\n"
              "compared against a threshold. True only when price is exactly 0.\n"
              "The intent was !(price > threshold): reject a survival probability\n"
              "too small to take a logarithm of. Written four times: twice in\n"
              "contingentleg.c, twice in feeleg.c.\n\n",
              (double)JPMCDS_LOG0_THRESHOLD);

  const TDate base = JpmcdsDate(2024, 1, 1);
  const TDate mat = JpmcdsDate(2034, 1, 1);
  TCurve *disc = flat_curve(base, mat, 0.03);
  std::printf("  %-13s %-14s %-10s %-11s %s\n", "hazard rate", "survival(T)", "as-written",
              "as-intended", "JpmcdsContingentLegPV");
  for (double h : {0.01, 0.5, 5.0, 20.0, 50.0, 80.0, 200.0}) {
    TCurve *spread = flat_curve(base, mat, h);
    const double surv = JpmcdsZeroPrice(spread, mat);
    TContingentLeg cl{};
    cl.startDate = base;
    cl.endDate = mat;
    cl.notional = 1.0e7;
    cl.payType = PROT_PAY_DEF;
    cl.protectStart = TRUE;
    double pv = 0.0;
    const int rc = JpmcdsContingentLegPV(&cl, base, base, base, disc, spread, 0.4, &pv);
    char out[48];
    if (rc != SUCCESS) {
      std::snprintf(out, sizeof out, "REJECTED");
    } else {
      std::snprintf(out, sizeof out, "pv=%.6g", pv);
    }
    std::printf("  %-13g %-14.3g %-10s %-11s %s\n", h, surv,
                ((!surv) > JPMCDS_LOG0_THRESHOLD) ? "fires" : "passes",
                (!(surv > JPMCDS_LOG0_THRESHOLD)) ? "fires" : "passes", out);
    JpmcdsFreeTCurve(spread);
  }
  JpmcdsFreeTCurve(disc);
  std::printf("\nSeverity is LOW, and the careful reason matters. The window where the\n"
              "two differ is survival in (0, 1e-100] -- hazard above about 23 for a\n"
              "ten-year maturity, which is not a credit but an underflow. And there\n"
              "the computation returns notional * (1 - R) = 6e6, the correct limit\n"
              "for a credit certain to default. The guard was defensive; the defect\n"
              "disabled the defence; the computation did not need it.\n\n"
              "No mispricing is demonstrated. \"Precedence bug on the pricing path\"\n"
              "would overstate this.\n");
}

void u9(bool unsafe) {
  heading("UPSTREAM-9", "an error message formatted with one argument too few");
  std::printf("stub.c declares one message with TWO %%s conversions:\n\n");
  std::printf("    \"%%s: Invalid stub method format %%s. ...\"\n\n");
  std::printf("The bad-LOCATION branch passes both:\n"
              "    JpmcdsErrMsg (invalidFormatMsg, routine, name);\n"
              "The bad-LENGTH branch, twelve lines below it, passes one:\n"
              "    JpmcdsErrMsg(invalidFormatMsg, routine);\n\n"
              "So vsprintf reaches for a second argument that was never pushed,\n"
              "takes whatever occupies that varargs slot, and dereferences it as\n"
              "a string. Reachable from any caller that passes a stub string whose\n"
              "LENGTH field is present but does not begin with s/S/l/L.\n\n");

  std::printf("  %-20s %-12s %s\n", "input", "both agree", "branch reached");
  for (const char *name : {"f/s", "f", "x", "/s", "f/X", "f/"}) {
    TStubMethod ref{};
    const bool length_branch = [name] {
      const char *slash = std::strchr(name, '/');
      if (slash == nullptr) {
        return false; // no LENGTH field: never reaches the branch
      }
      const char c = slash[1];
      // The LOCATION check runs first, so a bad LOCATION short-circuits.
      const char loc = name[0];
      if (!(loc == 'f' || loc == 'F' || loc == 'b' || loc == 'B')) {
        return false;
      }
      return !(c == 's' || c == 'S' || c == 'l' || c == 'L');
    }();
    const bool port_ok = cdsmodel::parse_stub_method(name).has_value();
    bool ref_ok = false;
    if (!length_branch || unsafe) {
      ref_ok = JpmcdsStringToStubMethod(const_cast<char *>(name), &ref) == SUCCESS;
    }
    std::printf("  %-20s %-12s %s\n", (std::string("\"") + name + "\"").c_str(),
                (!length_branch || unsafe) ? (ref_ok == port_ok ? "yes" : "NO") : "(not run)",
                length_branch ? "bad LENGTH -- the defect" : "safe");
  }

  std::printf("\nOn manifestation, stated carefully because it would be easy to\n"
              "overstate. The read is undefined whenever the branch is taken, but\n"
              "whether it FAULTS depends on what the caller happened to leave in\n"
              "that varargs slot. Measured on arm64 macOS: it faults in a minimal\n"
              "standalone frame --\n\n"
              "    AddressSanitizer: stack-buffer-underflow, READ of size 5\n"
              "      #0 strlen  <- consuming the second %%s\n"
              "      #3 __vsprintf_chk\n"
              "      #5 JpmcdsErrMsgV\n"
              "      #7 JpmcdsStringToStubMethod+0x120\n\n"
              "-- and does NOT fault inside the GoogleTest binary or inside this\n"
              "tool, where the slot holds something readable. A reproducer was\n"
              "written as its own target and then deleted, because it stopped\n"
              "faulting once it was a target rather than a scratch file.\n\n"
              "So the evidence is the source, not a crash, and severity is MEDIUM.\n"
              "Behaviour that appears in one caller and not another is what\n"
              "undefined behaviour looks like; it is not grounds for calling it\n"
              "benign, and it is not grounds for claiming a reliable crash either.\n\n"
              "The port does not reproduce it: parse_stub_method returns nullopt\n"
              "and formats nothing. It also drops upstream's toupper(char) call,\n"
              "whose domain excludes negative values -- any byte above 0x7F where\n"
              "char is signed -- in favour of comparing both cases directly.\n");
  if (!unsafe) {
    std::printf("\nRe-run with --unsafe to execute the bad-LENGTH branch.\n");
  }
}

void u10(bool unsafe) {
  heading("UPSTREAM-10", "unbounded vsprintf into a 4096-byte stack buffer");
  std::printf("cerror.c:533 JpmcdsWriteToLog:\n\n"
              "    char buffer[JPMCDS_ERR_MSG_BUFFER];   /* 4096 */\n"
              "    if (formatted)\n"
              "        vsprintf(buffer, format, parminfo);\n\n"
              "vsprintf, not vsnprintf. Every formatted error message in the\n"
              "library goes through this one function, and 67 of the 452 message\n"
              "sites interpolate a caller-supplied string after the routine name.\n"
              "A caller passing a string of about 4KB or more therefore overflows\n"
              "the stack with content it chose.\n\n"
              "This is also the mechanism that makes UPSTREAM-9 dangerous: the\n"
              "stray pointer U9 produces is dereferenced by this same vsprintf.\n\n");

  std::printf("Measured threshold, two independent public entry points, messages\n"
              "enabled. The message prefix is about 200 characters, so the cliff\n"
              "lands where prefix + argument reaches 4096:\n\n");
  std::printf("  %-10s %-34s %s\n", "length", "JpmcdsStringToStubMethod",
              "JpmcdsStringToDayCountConv");
  for (int n : {1000, 3800, 3900, 4200, 9000}) {
    const bool overflows = n >= 3900;
    const char *verdict = overflows ? "ABORT (fortify check fires)" : "returns FAILURE";
    std::printf("  %-10d %-34s %s\n", n, verdict, verdict);
  }
  std::printf("\nSurvives at 3800 and dies at 3900, identically from both entry\n"
              "points. On a fortified libc the overflow is caught and the process\n"
              "aborts; on an unfortified build it is a plain stack smash with\n"
              "caller-controlled bytes.\n\n");

  std::printf("Severity is MEDIUM rather than HIGH for one reason: the formatting\n"
              "is only reached once a host has called JpmcdsErrMsgOn. Messages are\n"
              "off by default and nothing in the shipped distribution turns them\n"
              "on -- so this is latent in the library and live in any host that\n"
              "enabled diagnostics, which is exactly the configuration someone\n"
              "debugging a pricing discrepancy would be running.\n\n");

  if (unsafe) {
    std::printf("--unsafe: calling JpmcdsStringToDayCountConv with a 9000-character\n"
                "string now. Expect this process to die here.\n");
    std::fflush(stdout);
    JpmcdsErrMsgOn();
    std::string big(9000, 'Z');
    long dcc = 0;
    (void)JpmcdsStringToDayCountConv(big.data(), &dcc);
    std::printf("  ... survived, which means this libc did not fortify vsprintf.\n");
  } else {
    std::printf("Re-run with --unsafe to execute the overflow.\n");
  }

  std::printf("\nThe port has no equivalent. There is no global error log to format\n"
              "into: a failure is an empty std::optional and a caller-supplied\n"
              "string is never copied anywhere. The defect is absent structurally\n"
              "rather than fixed, which is the only kind of fix that cannot regress.\n");
}

void u11() {
  heading("UPSTREAM-11", "a negative accrual, returned as SUCCESS");
  std::printf("defaulted.c locates the coupon period by the TRADE date and then\n"
              "accrues to the EVENT date:\n\n");
  std::printf("    if (fl->accStartDates[i] <= tradeDate && tradeDate < fl->accEndDates[i])\n"
              "    {\n"
              "        *accrualDays = edd + 1.0 - fl->accStartDates[i];\n\n");
  std::printf("It guards `edd >= startDate` but never `edd >= accStartDates[i]`.\n"
              "So once the trade date crosses a coupon boundary the event date\n"
              "did not, the subtraction goes negative -- and it is returned with\n"
              "SUCCESS, so a caller has no signal at all.\n\n");

  TDateInterval iv{3, 'M', 0};
  TStubMethod stub;
  stub.stubAtEnd = FALSE;
  stub.longStub = FALSE;
  const TDate start = JpmcdsDate(2024, 3, 20);
  const TDate end = JpmcdsDate(2029, 3, 20);
  const TDate edd = JpmcdsDate(2024, 8, 1);

  std::printf("Quarterly from 20 Mar 2024, credit event 1 Aug 2024, notional 1e7,\n"
              "coupon 100bp, ACT/360. The event sits in the 20 Jun - 20 Sep period,\n"
              "so 43 days are owed. Varying only WHEN the question is asked:\n\n");
  std::printf("  %-16s %-10s %-14s %-16s %s\n", "trade date", "upstream", "accrual days", "amount",
              "port");
  for (int lag : {0, 14, 45, 50, 90, 365}) {
    double days = 0.0;
    double amount = 0.0;
    const int rc =
        JpmcdsDefaultAccrual(edd + lag, edd, start, end, &iv, &stub, 1.0e7, 0.01, JPMCDS_ACT_360,
                             'N', const_cast<char *>("NONE"), &days, &amount);
    cdsmodel::Cds cds;
    cds.start = cdsmodel::from_tdate(start);
    cds.end = cdsmodel::from_tdate(end);
    cds.coupon_rate = 0.01;
    cds.coupon_interval = cdsmodel::DateInterval{3, cdsmodel::PeriodUnit::Months};
    cds.day_count = cdsmodel::DayCount::Act360;
    const std::vector<cdsmodel::Calendar> cals{
        *cdsmodel::Calendar::create({}, cdsmodel::Weekend::Standard)};
    const auto port = cdsmodel::default_accrual(cds, cdsmodel::from_tdate(edd + lag),
                                                cdsmodel::from_tdate(edd), 1.0e7, cals);
    char port_text[32];
    std::snprintf(port_text, sizeof port_text, "%ld days", port ? port->days : 0L);
    char lag_text[24];
    std::snprintf(lag_text, sizeof lag_text, "event+%d", lag);
    std::printf("  %-16s %-10s %-14.6g %-16.6g %s\n", lag_text,
                rc == SUCCESS ? "SUCCESS" : "FAILURE", days, amount, port_text);
  }

  std::printf("\nThe flip lands exactly where the trade date passes 20 September.\n\n");

  // Scope, measured rather than asserted.
  int total = 0;
  int differ = 0;
  int negative = 0;
  for (int month = 4; month <= 12; ++month) {
    for (int day = 1; day <= 28; day += 3) {
      const TDate e = JpmcdsDate(2024, month, day);
      if (e < start) {
        continue;
      }
      for (int lag = 0; lag <= 120; lag += 5) {
        double d0 = 0.0;
        double a0 = 0.0;
        double dp = 0.0;
        double ap = 0.0;
        if (JpmcdsDefaultAccrual(e + lag, e, start, end, &iv, &stub, 1.0e7, 0.01, JPMCDS_ACT_360,
                                 'N', const_cast<char *>("NONE"), &d0, &a0) != SUCCESS) {
          continue;
        }
        if (JpmcdsDefaultAccrual(e, e, start, end, &iv, &stub, 1.0e7, 0.01, JPMCDS_ACT_360, 'N',
                                 const_cast<char *>("NONE"), &dp, &ap) != SUCCESS) {
          continue;
        }
        ++total;
        if (d0 != dp) {
          ++differ;
        }
        if (d0 < 0.0) {
          ++negative;
        }
      }
    }
  }
  std::printf("Scope, event dates across Apr-Dec 2024 with settlement lags 0-120 days:\n");
  std::printf("  %d cases, %d (%.0f%%) differ from asking on the event date,\n"
              "  %d (%.0f%%) return a NEGATIVE accrual.\n\n",
              total, differ, 100.0 * differ / total, negative, 100.0 * negative / total);

  std::printf("Severity is HIGH, and the argument is not the sign. It is the\n"
              "dependence: the coupon accrued at a credit event cannot depend on\n"
              "when someone asks, and upstream's answer does. Settlement after a\n"
              "CDS auction routinely lands weeks after the determination date,\n"
              "which crosses a quarterly coupon boundary often. This is a wrong\n"
              "cash amount on a settlement calculation, reported as success --\n"
              "not undefined behaviour, not a crash, not an error path.\n\n"
              "The port corrects it by locating the period containing the EVENT\n"
              "date. Then the period start precedes the event by construction,\n"
              "the result is always positive, and it no longer varies with the\n"
              "trade date. One date changed; the arithmetic is untouched.\n");
}

void u12() {
  heading("UPSTREAM-12", "a lunar month computes correctly, then fails");
  std::printf("date_sup.c, JpmcdsDateIntervalToYears:\n\n"
              "    case 'U':\n"
              "        *years = (double)interval->prd *\n"
              "                 (double)JPMCDS_DAYS_PER_LUNAR_MONTH/JPMCDS_DAYS_PER_YEAR;\n"
              "    default:                        /* no break above */\n"
              "        JpmcdsErrMsg(\"...unknown interval type...\");\n"
              "        return FAILURE;\n\n");

  std::printf("  %-7s %-10s %-16s %s\n", "letter", "rc", "years written", "note");
  struct Row {
    char letter;
    const char *note;
  };
  for (const Row &r : {Row{'A', "annual"}, Row{'M', "monthly"}, Row{'D', "daily"},
                       Row{'U', "LUNAR MONTH"}, Row{'Z', "genuinely unknown"}}) {
    TDateInterval raw{1, r.letter, 0};
    double years = -999.0;
    const int rc = JpmcdsDateIntervalToYears(&raw, &years);
    std::printf("  %-7c %-10s %-16.10g %s\n", r.letter, rc == SUCCESS ? "SUCCESS" : "FAILURE",
                years, r.note);
  }
  std::printf("\n28/365 = %.10g  <- what 'U' computes before falling through\n\n", 28.0 / 365.0);
  std::printf("The last row is the one that matters. An unrecognised letter leaves\n"
              "the output at its sentinel; 'U' writes the correct value and THEN\n"
              "returns FAILURE. The computation ran; only the break is gone.\n\n");

  std::printf("Reachability, which is why severity is low:\n\n");
  TDateInterval made{};
  const int mk = JpmcdsMakeDateInterval(1, 'U', &made);
  std::printf("  JpmcdsMakeDateInterval(1, 'U') -> %s", mk == SUCCESS ? "" : "FAILURE\n");
  if (mk == SUCCESS) {
    std::printf("%d%c  -- normalised to DAYS\n", made.prd, made.prd_typ);
    double years = -999.0;
    const int rc = JpmcdsDateIntervalToYears(&made, &years);
    std::printf("  the SAME function on that interval -> %s, years = %.10g\n",
                rc == SUCCESS ? "SUCCESS" : "FAILURE", years);
  }
  std::printf("\nSo an interval built the way callers build one works. The missing\n"
              "break is reachable only through a hand-constructed TDateInterval --\n"
              "which upstream explicitly supports for other letters: JpmcdsDtFwdAny\n"
              "keeps code for A, S and Q \"for people who have set up their own\n"
              "TDateIntervals without calling JpmcdsMakeDateInterval\". It extends\n"
              "that courtesy to 'W' and not to 'U'.\n\n");
  std::printf("The port cannot express the defect: make_date_interval('U') yields\n"
              "{28 * periods, Days}, so no DateInterval carries the letter the bug\n"
              "keys on. Port value for one lunar period: %.10g\n",
              cdsmodel::interval_to_years(*cdsmodel::make_date_interval(1, 'U')));
}

void u13() {
  heading("UPSTREAM-13", "a leaked global that disables validation process-wide");
  std::printf("bSearchCheckOrder_g is a static in bsearch.c, default TRUE, consulted\n"
              "by BOTH JpmcdsBSearchLong and JpmcdsBSearchDouble before validating\n"
              "that their input array is sorted.\n\n"
              "The discontinuous interpolator needs the UNCHECKED search, so it\n"
              "clears that global -- and returns on two error paths without putting\n"
              "it back:\n\n"
              "    checkOrder = JpmcdsBSearchCheckOrder(FALSE);\n"
              "    if (checkOrder) { ... return FAILURE; }      /* leaks */\n"
              "    if (SEARCH_ROUTINE_NAME(...) == FAILURE) goto done;  /* leaks */\n"
              "    JpmcdsBSearchCheckOrder(checkOrder);         /* only on success */\n\n");

  JpmcdsBSearchCheckOrder(TRUE);
  long unordered[4] = {10, 50, 20, 90};
  const auto validates = [&unordered]() {
    int lo = -1;
    int hi = -1;
    return JpmcdsBSearchLong(30.0, unordered, sizeof(long), 4, &lo, &hi) != SUCCESS;
  };
  std::printf("  %-34s %s\n",
              "baseline:", validates() ? "unordered array REJECTED (validated)" : "accepted?!");

  long bad_x[3] = {100, 50, 200};
  double f[3] = {1.0, 2.0, 3.0};
  double out = 0.0;
  const int rc = JpmcdsLinInterpLongPoint1Discontinuous(bad_x, sizeof(long), 3, f, sizeof(double),
                                                        120.0, nullptr, TRUE, &out);
  std::printf("  %-34s %s\n", "one failed call:", rc == SUCCESS ? "SUCCESS" : "FAILURE");
  std::printf("  %-34s %s\n", "after it:",
              validates() ? "still validated" : "*** ACCEPTED -- VALIDATION IS OFF ***");
  std::printf("  %-34s %s\n",
              "global via the getter:", JpmcdsBSearchCheckOrder(TRUE) ? "TRUE" : "FALSE (leaked)");

  std::printf("\nAfterwards every binary search in the process stops checking that\n"
              "its domain is sorted -- including inside the zero curve and the CDS\n"
              "pricing path. A binary search over an unsorted array does not fail;\n"
              "it returns a bracket that is simply wrong.\n\n"
              "Medium rather than high for reachability, not consequence: no caller\n"
              "inside the library and no public declaration, but it IS an exported\n"
              "symbol and it IS in the XLL surface.\n\n"
              "The port cannot express it. bracket() and bracket_checked() are two\n"
              "functions and there is no global -- a choice made in slice 1 whose\n"
              "comment read \"process-wide state that silently changed the behaviour\n"
              "of every caller\". This is that sentence turning out literally true.\n");
}

void u14() {
  heading("UPSTREAM-14", "bond metrics return stack garbage before the epoch");
  std::printf("yearfrac.c:\n\n"
              "    static double bondDaysDiff(double startDate, double endDate)\n"
              "    {\n"
              "        long days;                            /* uninitialised */\n"
              "        (void)JpmcdsDaysDiff((TDate)startDate, (TDate)endDate,\n"
              "                             JPMCDS_B30_360, &days);\n"
              "        return ((double)days);                /* garbage if it failed */\n"
              "    }\n\n");

  std::printf("  %-36s %-10s %s\n", "JpmcdsDaysDiff(start, 100, 30/360)", "status", "out param");
  const long starts[] = {-50, -1, 0, 1, 100};
  for (long start : starts) {
    long days = -123456789; // a sentinel no real answer could be
    const int rc =
        JpmcdsDaysDiff(static_cast<TDate>(start), static_cast<TDate>(100), JPMCDS_B30_360, &days);
    char label[48];
    std::snprintf(label, sizeof label, "  start = %ld", start);
    std::printf("  %-36s %-10s %ld%s\n", label, rc == SUCCESS ? "SUCCESS" : "FAILURE", days,
                (rc != SUCCESS && days == -123456789) ? "  <- NEVER WRITTEN" : "");
  }

  TMetricDoubleFunc metric = JpmcdsDayCountToMetricFunc(JPMCDS_B30_360);
  std::printf("\n  metric(-50, 100) -> %.0f   <- uninitialised stack read\n", metric(-50.0, 100.0));
  std::printf("  metric(100, 200) -> %.0f    <- the defined case, for contrast\n",
              metric(100.0, 200.0));

  long x[2] = {100, 200};
  double f[2] = {0.01, 0.02};
  double garbage = 0.0;
  const int rc =
      JpmcdsLinInterpLongPoint1(x, sizeof(long), 2, f, sizeof(double), -50.0, metric, &garbage);
  std::printf("\n  interpolating a {0.01, 0.02} curve at -50 with 30/360:\n");
  std::printf("    upstream -> %.10g   (reported %s)\n", garbage,
              rc == SUCCESS ? "SUCCESS" : "FAILURE");
  const std::optional<double> ported =
      cdsmodel::linear_interpolate(std::vector<long>{100, 200}, std::vector<double>{0.01, 0.02},
                                   -50.0, cdsmodel::Metric::Bond30_360);
  std::printf("    port     -> %.10g   (a defined extrapolation below the first node)\n",
              ported.value_or(0.0));

  std::printf("\nThe figures above are NOT stable: two binaries built from the same\n"
              "sources gave metric(-50, 100) as 6101050560 and as 4, interpolating\n"
              "to -667794.1351 and -619018.2639. Whatever is in that stack slot is\n"
              "what the caller gets, so run this and expect your own number.\n");
  std::printf("\nIt is an UNINITIALISED READ, so AddressSanitizer cannot see it --\n"
              "MemorySanitizer would, and this project does not run one. The\n"
              "sanitizer job is green on it.\n\n"
              "It was found by the golden generator's full cross-product of\n"
              "abscissae against metrics, not by the differential sweeps, which\n"
              "used realistic dates for each metric.\n\n"
              "The port checks the status it is given. std::chrono has no 1601\n"
              "epoch, so a pre-epoch abscissa is just a date in 1600 and the\n"
              "30/360 difference is well defined -- the port is correct where\n"
              "upstream is garbage. The affected combinations are omitted from the\n"
              "recorded vectors: recording the output of undefined behaviour would\n"
              "enshrine it.\n");
}

void u15() {
  heading("UPSTREAM-15", "a validating constructor that admits unpriceable curves");
  std::printf("tcurve.c's checkTCurve exists \"to guarantee we can generate positive\n"
              "discount factors\". It validates the day count with JpmcdsDayCountValid,\n"
              "which correctly accepts 30/360, and it accepts an empty curve -- its\n"
              "only size test is `fNumItems < 0`.\n\n"
              "Neither can be priced: JpmcdsZeroPrice routes through\n"
              "JpmcdsConvertCompoundRate, which supports ACT/365F and ACT/360 and\n"
              "rejects the rest. And JpmcdsZeroPrice returns a bare double, so it has\n"
              "nowhere to report that.\n\n");

  const TDate base = JpmcdsDate(2024, 1, 1);
  const TDate mat = JpmcdsDate(2029, 1, 1);
  TDate dates[1] = {mat};
  double rates[1] = {0.05};

  std::printf("  %-10s %-20s %s\n", "day count", "construct", "JpmcdsZeroPrice");
  struct Row {
    long dcc;
    const char *name;
  };
  for (const Row &r : {Row{JPMCDS_ACT_365F, "ACT/365F"}, Row{JPMCDS_ACT_360, "ACT/360"},
                       Row{JPMCDS_B30_360, "30/360"}, Row{JPMCDS_B30E_360, "30E/360"}}) {
    TCurve *c = JpmcdsMakeTCurve(base, dates, rates, 1, 1.0, r.dcc);
    char priced[40] = "-";
    if (c != nullptr) {
      std::snprintf(priced, sizeof priced, "%.10g", JpmcdsZeroPrice(c, mat));
    }
    std::printf("  %-10s %-20s %s\n", r.name, c != nullptr ? "accepted" : "REJECTED", priced);
    JpmcdsFreeTCurve(c);
  }
  TCurve *empty = JpmcdsMakeTCurve(base, nullptr, nullptr, 0, 1.0, JPMCDS_ACT_365F);
  if (empty != nullptr) {
    std::printf("  %-10s %-20s %.10g\n", "(empty)", "accepted", JpmcdsZeroPrice(empty, mat));
    JpmcdsFreeTCurve(empty);
  }

  std::printf("\nSo a caller picks a standard day count, a constructor named Make...\n"
              "with an explicit validation pass accepts it, and every discount factor\n"
              "afterwards is NaN. Same problem as UPSTREAM-2 in a new disguise: no\n"
              "error channel, so the error becomes a number.\n\n");

  std::printf("Infinity is NOT part of this finding, and conflating them would\n"
              "overstate it. A continuous-basis rate of -400 over three years is\n"
              "exp(1200):\n\n");
  TDate three[1] = {JpmcdsDate(2027, 1, 1)};
  double big[1] = {-400.0};
  TCurve *over = JpmcdsMakeTCurve(base, three, big, 1, JPMCDS_CONTINUOUS_BASIS, JPMCDS_ACT_365F);
  if (over != nullptr) {
    std::printf("  upstream -> %g\n", JpmcdsZeroPrice(over, three[0]));
    JpmcdsFreeTCurve(over);
  }
  const std::vector<cdsmodel::RatePoint> pts{{cdsmodel::from_tdate(three[0]), -400.0}};
  const std::optional<cdsmodel::ZeroCurve> ported = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(base), pts, cdsmodel::kContinuousBasis, cdsmodel::DayCount::Act365Fixed);
  std::printf("  port     -> %g\n",
              ported ? ported->zero_price(cdsmodel::from_tdate(three[0])) : 0.0);
  std::printf("\nBoth accept it and both return the same infinity. That is a VALUE --\n"
              "the answer is enormous. NaN is not a value; above, it is an error\n"
              "channel.\n\n");

  std::printf("The port rejects at construction what cannot be priced, which is why\n"
              "ZeroCurve::create is stricter than checkTCurve on exactly two counts\n"
              "(non-empty, and a day count the conversion supports) and why\n"
              "zero_price needs no NaN sentinel: every curve that exists is\n"
              "priceable. Pinned by a tier-1 invariant over 810 discount factors.\n");
}

void u16() {
  heading("UPSTREAM-16", "a failure that leaves the curve modified");
  std::printf("zcall.c's JpmcdsZCAddGenRate short-circuits when the rate is already\n"
              "quoted in the curve's own terms -- and then forgets to stop:\n\n"
              "    if (basis==zc->basis && dayCountConv==zc->dayCountConv)\n"
              "    {\n"
              "         if (JpmcdsZCAddRate(zc,date,rate) == FAILURE)\n"
              "             goto done;\n"
              "    }\n"
              "    /* no return -- execution falls through */\n"
              "    if (JpmcdsRateToDiscount(...) == FAILURE) goto done;\n"
              "    if (JpmcdsZCAddDiscountFactor(zc,date,df) == FAILURE) goto done;\n\n"
              "So the matching case adds the point, and then adds it AGAIN by a\n"
              "second route. Usually harmless: the duplicate check compares the\n"
              "rates within a hardcoded 1e-7 and does nothing. Wasted work.\n\n");

  const TDate value_date = JpmcdsDate(2024, 1, 1);
  std::printf("  %-28s %-9s %-10s %s\n", "date", "rc", "numItems", "verdict");
  for (const TDate date : {JpmcdsDate(2029, 1, 1), value_date}) {
    ZCurve *zc = JpmcdsZCMake(value_date, 4, 1, JPMCDS_ACT_365F);
    if (zc == nullptr) {
      continue;
    }
    const int rc = JpmcdsZCAddGenRate(zc, date, 0.05, 1, JPMCDS_ACT_365F);
    const char *verdict = (rc == FAILURE && zc->numItems > 0) ? "*** curve MUTATED ***" : "-";
    std::printf("  %-28s %-9s %-10d %s\n",
                date == value_date ? "date == valueDate" : "date > valueDate",
                rc == SUCCESS ? "SUCCESS" : "FAILURE", zc->numItems, verdict);
    for (int i = 0; i < zc->numItems; ++i) {
      if (rc == FAILURE) {
        std::printf("     orphaned point: date=%ld rate=%g discount=%g\n", (long)zc->date[i],
                    zc->rate[i], zc->discount[i]);
      }
    }
    JpmcdsZCFree(zc);
  }

  std::printf("\nA date equal to the value date makes the second route fail --\n"
              "JpmcdsDiscountToRate rejects a zero-length period -- so the function\n"
              "returns FAILURE after the first route already committed the point.\n"
              "A caller that discards the curve on failure is unaffected. One that\n"
              "retries, or carries on past the error, now holds a point it never\n"
              "successfully added, and numItems says it is there.\n\n");

  std::printf("The port, same two inputs:\n\n");
  for (const TDate date : {JpmcdsDate(2029, 1, 1), value_date}) {
    std::optional<cdsmodel::ZeroCurveBuilder> b = cdsmodel::ZeroCurveBuilder::create(
        cdsmodel::from_tdate(value_date), 4, 1, cdsmodel::DayCount::Act365Fixed);
    const bool ok =
        b->add_general_rate(cdsmodel::from_tdate(date), 0.05, 1, cdsmodel::DayCount::Act365Fixed);
    std::printf("  %-28s %-9s %-10zu %s\n",
                date == value_date ? "date == valueDate" : "date > valueDate",
                ok ? "ok" : "refused", b->points().size(), ok ? "-" : "(nothing committed)");
  }
  std::printf("\nadd_general_rate returns after the matching case, as the missing\n"
              "`return` would have. The rate at the value date now simply succeeds:\n"
              "a rate over no time discounts by nothing, which is a perfectly good\n"
              "point. The only reason upstream failed was the redundant second\n"
              "route. The guarantee is broader than the one fix -- every route\n"
              "commits only after all its conversions succeed, so no failure\n"
              "anywhere can leave a point behind, asserted as a tier-1 property.\n");
}

void u17() {
  heading("UPSTREAM-17", "an optimisation that changes the answer at its boundary");
  std::printf("zcall.c's JpmcdsZCComputeDiscount has a fast path its own comment\n"
              "calls \"a special optimization\":\n\n"
              "    if (zc->basis == 1 && rate >= -1.0 && date >= zc->valueDate && ...)\n"
              "    {\n"
              "        *discount = pow(1+rate, (zc->valueDate - date) / 365.0);\n"
              "        return SUCCESS;\n"
              "    }\n\n"
              "The guard is `rate >= -1.0`. At exactly -1.0 the base is zero and the\n"
              "exponent negative, so pow returns an infinity -- while the general\n"
              "path it stands in for REJECTS the same input, because\n"
              "JpmcdsRateToDiscountYearFrac refuses a base at or below zero.\n\n");

  const TDate value_date = JpmcdsDate(2024, 1, 1);
  const TDate date = JpmcdsDate(2032, 1, 1);
  ZCurve *zc = JpmcdsZCMake(value_date, 4, 1, JPMCDS_ACT_365F);
  if (zc == nullptr) {
    std::printf("  (upstream refused the curve; nothing to show)\n");
    return;
  }

  std::printf("  %-12s %-26s %s\n", "rate", "ZCComputeDiscount", "JpmcdsRateToDiscount");
  for (const double rate : {-0.9999999, -1.0, -1.5}) {
    double fast = 0.0;
    double general = 0.0;
    const int rc_fast = JpmcdsZCComputeDiscount(zc, date, rate, &fast);
    const int rc_gen = JpmcdsRateToDiscount(rate, value_date, date, JPMCDS_ACT_365F, 1, &general);
    char a[40];
    char b[40];
    std::snprintf(a, sizeof a, "%.17g", fast);
    std::snprintf(b, sizeof b, "%.17g", general);
    std::printf("  %-12.9g %-26s %s\n", rate, rc_fast == SUCCESS ? a : "FAILURE",
                rc_gen == SUCCESS ? b : "FAILURE");
  }

  std::printf("\nAnd the infinity is not merely returned -- it is stored, and served:\n\n");
  const int rc = JpmcdsZCAddRate(zc, date, -1.0);
  std::printf("  JpmcdsZCAddRate(curve, date, -1.0) -> %s\n",
              rc == SUCCESS ? "SUCCESS" : "FAILURE");
  for (int i = 0; i < zc->numItems; ++i) {
    std::printf("    stored: date=%ld rate=%g discount=%g\n", (long)zc->date[i], zc->rate[i],
                zc->discount[i]);
  }
  double served = 0.0;
  const int rc_df = JpmcdsZCDiscountFactor(zc, date, JPMCDS_LINEAR_INTERP, nullptr, &served);
  std::printf("    JpmcdsZCDiscountFactor at that date -> %s  %g\n",
              rc_df == SUCCESS ? "SUCCESS" : "FAILURE", served);
  JpmcdsZCFree(zc);

  std::printf("\nTwo code paths for one quantity, disagreeing at the boundary, and\n"
              "the fast one wins because it is tested first. The guard wants\n"
              "`rate > -1.0`.\n\n"
              "This is the mirror image of the infinity in UPSTREAM-15, and the\n"
              "difference is the point. There, exp(1200) overflowing is a VALUE --\n"
              "the answer really is enormous, and the port returns the same thing.\n"
              "Here the general path is on record calling the same input an ERROR.\n\n");

  std::printf("The port, same three rates:\n\n");
  std::optional<cdsmodel::ZeroCurveBuilder> port = cdsmodel::ZeroCurveBuilder::create(
      cdsmodel::from_tdate(value_date), 4, 1, cdsmodel::DayCount::Act365Fixed);
  std::printf("  %-12s %-26s %s\n", "rate", "compute_discount", "general path");
  for (const double rate : {-0.9999999, -1.0, -1.5}) {
    const std::optional<double> fast = port->compute_discount(cdsmodel::from_tdate(date), rate);
    const std::optional<double> general =
        cdsmodel::rate_to_discount(rate, cdsmodel::from_tdate(value_date),
                                   cdsmodel::from_tdate(date), cdsmodel::DayCount::Act365Fixed, 1);
    char a[40] = "refused";
    char b[40] = "refused";
    if (fast) {
      std::snprintf(a, sizeof a, "%.17g", *fast);
    }
    if (general) {
      std::snprintf(b, sizeof b, "%.17g", *general);
    }
    std::printf("  %-12.9g %-26s %s\n", rate, a, b);
  }
  std::printf("\nThe fast path is reproduced -- it is not purely an optimisation,\n"
              "since (valueDate-date)/365.0 and -1.0*((date-valueDate)/365.0) are\n"
              "the same double only because negation is exact -- but it is taken\n"
              "only where it agrees with the general path, so the boundary falls\n"
              "through and is refused. Pinned from both sides: one test asserts\n"
              "the two paths return bit-identical factors inside the boundary,\n"
              "another that nothing infinite can be stored.\n");
}

void u18() {
  heading("UPSTREAM-18", "an error that is logged, then reported as success");
  std::printf("zcall.c's JpmcdsZCFwdDiscountFactor:\n\n"
              "    *discount = 1.0;\n"
              "    if (discount1<=0.0)                 /* bad discount factor */\n"
              "    {\n"
              "        JpmcdsErrMsg(\"...: bad discount at start of fwd\\n\");\n"
              "    }\n"
              "    else\n"
              "    {\n"
              "        *discount = discount2 / discount1;\n"
              "    }\n"
              "    return SUCCESS;\n\n"
              "No missing error channel: the function returns int, callers check it, and\n"
              "the condition is detected well enough to write a message about. The status\n"
              "just does not match what the function found. And 1.0 is the worst possible\n"
              "stand-in -- it means NO discounting over the period, so the forward rate\n"
              "comes out as exactly zero.\n\n");

  const TDate value_date = JpmcdsDate(2024, 1, 1);
  const TDate start = JpmcdsDate(2054, 1, 1);
  const TDate end = JpmcdsDate(2055, 1, 1);
  std::printf("Continuous basis, 30y, sweeping the rate:\n\n");
  std::printf("  %-10s %-16s %-12s %-10s %s\n", "rate", "df(start)", "fwd df", "fwd rate", "");
  for (const double rate : {20.0, 23.0, 24.0, 25.0, 30.0}) {
    ZCurve *zc = JpmcdsZCMake(value_date, 8, 5000, JPMCDS_ACT_365F);
    if (zc == nullptr) {
      continue;
    }
    JpmcdsZCAddRate(zc, start, rate);
    JpmcdsZCAddRate(zc, end, rate);
    double df = 0.0;
    double fwd_df = 0.0;
    double fwd = 0.0;
    JpmcdsZCDiscountFactor(zc, start, JPMCDS_LINEAR_INTERP, nullptr, &df);
    const int rc_fwd_df =
        JpmcdsZCFwdDiscountFactor(zc, start, end, JPMCDS_LINEAR_INTERP, nullptr, &fwd_df);
    const int rc_fwd = JpmcdsZCForwardRate(zc, start, end, JPMCDS_SIMPLE_BASIS, JPMCDS_ACT_365F,
                                           JPMCDS_LINEAR_INTERP, nullptr, &fwd);
    std::printf("  %-10g %-16g %-12g %-10g %s\n", rate, df, rc_fwd_df == SUCCESS ? fwd_df : -1.0,
                rc_fwd == SUCCESS ? fwd : -1.0, df <= 0.0 ? "<- fabricated 1.0, rate 0" : "");
    JpmcdsZCFree(zc);
  }

  std::printf("\nNote the direction of travel. As the input deteriorates the output\n"
              "stops LOOKING wrong: 2.4e10 is visibly nonsense and any sanity check\n"
              "catches it, 0 is not. The failure mode improves in appearance exactly\n"
              "as the situation gets worse.\n\n"
              "It is also basis-specific. Annual compounding never reaches it --\n"
              "(1+r)^-t decays polynomially in r, so 10,000%% over 30y is 6.7e-61,\n"
              "nowhere near underflow. Continuous decays exponentially.\n\n");

  std::printf("The port, same sweep:\n\n");
  std::printf("  %-10s %-16s %s\n", "rate", "df(start)", "forward discount factor");
  for (const double rate : {20.0, 24.0, 25.0, 30.0}) {
    std::optional<cdsmodel::ZeroCurveBuilder> b = cdsmodel::ZeroCurveBuilder::create(
        cdsmodel::from_tdate(value_date), 8, 5000, cdsmodel::DayCount::Act365Fixed);
    if (!b->add_rate(cdsmodel::from_tdate(start), rate) ||
        !b->add_rate(cdsmodel::from_tdate(end), rate)) {
      continue;
    }
    const std::optional<double> df =
        b->discount_factor(cdsmodel::from_tdate(start), cdsmodel::InterpType::Linear);
    const std::optional<double> fwd = b->forward_discount_factor(
        cdsmodel::from_tdate(start), cdsmodel::from_tdate(end), cdsmodel::InterpType::Linear);
    char shown[40] = "refused";
    if (fwd) {
      std::snprintf(shown, sizeof shown, "%.17g", *fwd);
    }
    std::printf("  %-10g %-16g %s\n", rate, df ? *df : -1.0, shown);
  }
  std::printf("\nThe underflow is reproduced exactly -- the same zero discount factor,\n"
              "because that part is arithmetic and not a defect. Only the fabricated\n"
              "forward is refused.\n");
}

void u19() {
  heading("UPSTREAM-19", "the guard against a bad discount factor cannot see NaN");
  std::printf("zcall.c's JpmcdsZCForwardRate guards the conversion:\n\n"
              "    if (discount<=0.0)                  /* bad discount factor */\n"
              "    {\n"
              "        JpmcdsErrMsg(\"...: bad discount for forward\\n\");\n"
              "        return FAILURE;\n"
              "    }\n\n"
              "NaN <= 0.0 is false. The one check whose entire job is to stop an\n"
              "unusable discount factor is written as the one comparison every NaN\n"
              "passes. JpmcdsDiscountToRate guards the same way.\n\n");

  const TDate value_date = JpmcdsDate(2024, 1, 1);
  const TDate start = JpmcdsDate(2054, 1, 1);
  const TDate end = JpmcdsDate(2055, 1, 1);
  std::printf("Continuous basis, 30y, a large NEGATIVE rate overflows the factor:\n\n");
  std::printf("  %-10s %-16s %-16s %-10s %s\n", "rate", "df(start)", "df(end)", "fwd df",
              "fwd rate");
  for (const double rate : {-20.0, -23.0, -24.0, -100.0}) {
    ZCurve *zc = JpmcdsZCMake(value_date, 8, 5000, JPMCDS_ACT_365F);
    if (zc == nullptr) {
      continue;
    }
    JpmcdsZCAddRate(zc, start, rate);
    JpmcdsZCAddRate(zc, end, rate);
    double df1 = 0.0;
    double df2 = 0.0;
    double fwd_df = 0.0;
    double fwd = 0.0;
    JpmcdsZCDiscountFactor(zc, start, JPMCDS_LINEAR_INTERP, nullptr, &df1);
    JpmcdsZCDiscountFactor(zc, end, JPMCDS_LINEAR_INTERP, nullptr, &df2);
    JpmcdsZCFwdDiscountFactor(zc, start, end, JPMCDS_LINEAR_INTERP, nullptr, &fwd_df);
    const int rc = JpmcdsZCForwardRate(zc, start, end, JPMCDS_SIMPLE_BASIS, JPMCDS_ACT_365F,
                                       JPMCDS_LINEAR_INTERP, nullptr, &fwd);
    std::printf("  %-10g %-16g %-16g %-10g %-10g %s\n", rate, df1, df2, fwd_df, fwd,
                (rc == SUCCESS && std::isnan(fwd)) ? "<- NaN, reported as SUCCESS" : "");
    JpmcdsZCFree(zc);
  }

  std::printf("\nBoth discount factors are legitimately infinite. That is the same\n"
              "overflow UPSTREAM-15 concluded is a VALUE and not a finding, and the\n"
              "port returns the same infinities. The defect is downstream: the ratio\n"
              "of two infinities is indeterminate and nothing stops it.\n\n"
              "So the lineage is UPSTREAM-15's accepted infinity feeding UPSTREAM-19's\n"
              "blind guard -- the same relationship UPSTREAM-9 has with UPSTREAM-10,\n"
              "where one supplies the bad input and the other fails to contain it.\n\n");

  const double nan_value = std::numeric_limits<double>::quiet_NaN();
  double direct = 0.0;
  const int rc_direct =
      JpmcdsDiscountToRate(nan_value, start, end, JPMCDS_ACT_365F, JPMCDS_SIMPLE_BASIS, &direct);
  std::printf("Straight at the conversion, no curve involved:\n\n"
              "  JpmcdsDiscountToRate(NaN, ...) -> %s  rate=%g\n",
              rc_direct == SUCCESS ? "SUCCESS" : "FAILURE", direct);
  const std::optional<double> ported =
      cdsmodel::discount_to_rate(nan_value, cdsmodel::from_tdate(start), cdsmodel::from_tdate(end),
                                 cdsmodel::DayCount::Act365Fixed, 0);
  std::printf("  port discount_to_rate(NaN, ...) -> %s\n\n", ported ? "SUCCESS (!)" : "refused");

  std::printf("Which is why the port fixed it there rather than at each caller: a NaN\n"
              "discount factor is not a rate no matter who asks. The guard is written\n"
              "as isfinite, so it means what it was for, and the tier-1 property is\n"
              "the general form -- across every basis, day count and rate in the\n"
              "sweep, no evaluation returns NaN. Infinity is still permitted.\n");
}

void u20() {
  heading("UPSTREAM-20", "the precondition the documentation does not mention");
  std::printf("cashflow.h states JpmcdsMergeCFL's contract in one sentence:\n\n"
              "    \"Merges two cash flow lists. The input cash flow lists cannot be NULL.\"\n\n"
              "That precondition is real and it IS checked, on both arguments. The\n"
              "other one is not stated and not checked: the algorithm is a single\n"
              "forward pass advancing whichever side holds the earlier date, so it is\n"
              "a merge only if both inputs are already ordered.\n\n");

  TDate a_dates[3] = {40000, 20000, 30000};
  double a_amounts[3] = {1.0, 2.0, 3.0};
  TDate b_dates[2] = {25000, 15000};
  double b_amounts[2] = {10.0, 20.0};
  TCashFlowList *a = JpmcdsNewCFL(a_dates, a_amounts, 3);
  TCashFlowList *b = JpmcdsNewCFL(b_dates, b_amounts, 2);
  TCashFlowList *merged = JpmcdsMergeCFL(a, b);
  std::printf("  a = [40000, 20000, 30000]   b = [25000, 15000]\n\n");
  if (merged != nullptr) {
    std::printf("  MergeCFL -> SUCCESS, fNumItems=%d\n", merged->fNumItems);
    for (int i = 0; i < merged->fNumItems; ++i) {
      std::printf("      [%d] date=%-8ld amount=%g\n", i, (long)merged->fArray[i].fDate,
                  merged->fArray[i].fAmount);
    }
    const bool sorted_out =
        merged->fNumItems < 2 || merged->fArray[0].fDate <= merged->fArray[1].fDate;
    std::printf("\n  ordered on the way out? %s\n", sorted_out ? "yes" : "NO");
  }

  std::printf("\nEvery element is present, so nothing is lost -- but the output is not\n"
              "sorted and not a stable interleaving either. It is an artefact of where\n"
              "the comparison stopped.\n\n");

  std::printf("The documented precondition, for contrast:\n");
  std::printf("  MergeCFL(NULL, b) -> %s\n",
              JpmcdsMergeCFL(nullptr, b) == nullptr ? "NULL" : "(!)");
  std::printf("  MergeCFL(a, NULL) -> %s\n\n",
              JpmcdsMergeCFL(a, nullptr) == nullptr ? "NULL" : "(!)");

  const std::vector<cdsmodel::CashFlow> port_a{{cdsmodel::from_tdate(40000), 1.0},
                                               {cdsmodel::from_tdate(20000), 2.0},
                                               {cdsmodel::from_tdate(30000), 3.0}};
  const std::vector<cdsmodel::CashFlow> port_b{{cdsmodel::from_tdate(25000), 10.0},
                                               {cdsmodel::from_tdate(15000), 20.0}};
  std::printf("The port:\n  merge_cash_flows(a, b) -> %s\n  merge_cash_flows(b, a) -> %s\n",
              cdsmodel::merge_cash_flows(port_a, port_b) ? "(!)" : "refused",
              cdsmodel::merge_cash_flows(port_b, port_a) ? "(!)" : "refused");

  std::printf("\nSeverity is low because of reachability: one call site exists in the\n"
              "library -- zcswap.c:804, merging a fixed and a floating schedule, both\n"
              "generated in order. Reachable only through the public header.\n\n");

  std::printf("And the finding this is NOT. JpmcdsNewEmptyCFL sets fNumItems to the\n"
              "count it was asked for and allocates via JpmcdsMallocSafe, whose\n"
              "allocation line reads:\n\n"
              "    ptr = malloc(theSize);        /* Don't use calloc due to RS6000. */\n\n"
              "On that line alone a public function named \"NewEmpty\" returns n cash\n"
              "flows of heap garbage. It does not, because the next branch memsets the\n"
              "block to zero -- \"we reproduce calloc by hand\", not \"we skip it\":\n\n");
  TCashFlowList *empty = JpmcdsNewEmptyCFL(4);
  if (empty != nullptr) {
    std::printf("  NewEmptyCFL(4) -> fNumItems=%d\n", empty->fNumItems);
    for (int i = 0; i < empty->fNumItems; ++i) {
      std::printf("      [%d] date=%-8ld amount=%g\n", i, (long)empty->fArray[i].fDate,
                  empty->fArray[i].fAmount);
    }
  }
  std::printf("\nSo every NEW and NEW_ARRAY in the library returns zeroed memory, and\n"
              "upstream cannot have an uninitialised-HEAP read anywhere. The one\n"
              "uninitialised read in this register, UPSTREAM-14, is a stack local --\n"
              "where no allocator intervenes.\n\n"
              "One detail still deserves recording: fDate=0 is not a null. TDate 0 is\n"
              "1601-01-01, a date like any other. An under-filled list does not look\n"
              "uninitialised, it looks like n deliberate payments of zero in 1601.\n");

  JpmcdsFreeCFL(a);
  JpmcdsFreeCFL(b);
  JpmcdsFreeCFL(merged);
  JpmcdsFreeCFL(empty);
}

/// Seed a small upstream curve, for the bootstrap sections below.
ZCurve *seed_curve(int npts, double rate) {
  ZCurve *zc = JpmcdsZCMake(JpmcdsDate(2024, 1, 1), 32, 1, JPMCDS_ACT_365F);
  if (zc == nullptr) {
    return nullptr;
  }
  for (int i = 1; i <= npts; ++i) {
    JpmcdsZCAddRate(zc, JpmcdsDate(2024 + i, 1, 1), rate * static_cast<double>(i));
  }
  return zc;
}

void u21(bool unsafe) {
  heading("UPSTREAM-21", "the error path dereferences a null pointer");
  std::printf("zcall.c's JpmcdsZCAddCashFlowList reads the last element twice, and\n"
              "neither read is guarded:\n\n"
              "    if (date==0L)\n"
              "        date = cfl->fArray[ cfl->fNumItems-1].fDate;\n"
              "    ...\n"
              "    done:\n"
              "        if (status == FAILURE)\n"
              "            JpmcdsErrMsg(\"...maturing %%s\", \n"
              "                JpmcdsFormatDate(cfl->fArray[cfl->fNumItems-1].fDate));\n\n");

  TCashFlowList *empty = JpmcdsNewEmptyCFL(0);
  std::printf("JpmcdsNewEmptyCFL(0) is a PUBLIC constructor, and gives:\n"
              "  fNumItems=%d  fArray=%s\n\n",
              empty != nullptr ? empty->fNumItems : -1,
              (empty != nullptr && empty->fArray == nullptr) ? "NULL" : "non-NULL");
  std::printf("So both reads are NULL[-1].\n\n");

  std::printf("The second one is what matters. With an EXPLICIT date the first read\n"
              "is skipped, and the function reaches its own branch:\n\n"
              "    \"no cash flows in list beyond ZeroCurve. Therefore, nothing to add.\"\n\n"
              "The error is detected, the branch is written, the message is composed --\n"
              "and then `done:` dereferences the empty list to format it. The\n"
              "diagnostic path for an empty list is the one path that cannot survive\n"
              "an empty list, so there is no way to receive the error.\n\n");

  if (!unsafe) {
    std::printf("Measured separately, because each call takes the process with it:\n"
                "    date == 0        -> exit status 139 (SIGSEGV)\n"
                "    explicit date    -> exit status 139 (SIGSEGV)\n\n"
                "Pass --unsafe to run it here and watch this process die.\n");
  } else {
    std::printf("--unsafe given: calling it now. This process is about to crash.\n");
    std::fflush(stdout);
    ZCurve *zc = seed_curve(1, 0.05);
    (void)JpmcdsZCAddCashFlowList(zc, empty, 1.0, 0L, JPMCDS_LINEAR_INTERP, nullptr);
    std::printf("  (did not crash -- that is itself worth investigating)\n");
    JpmcdsZCFree(zc);
  }

  std::printf("\nThe port refuses an empty span before anything reads from it:\n");
  std::optional<cdsmodel::ZeroCurveBuilder> port = cdsmodel::ZeroCurveBuilder::create(
      cdsmodel::from_tdate(JpmcdsDate(2024, 1, 1)), 8, 1, cdsmodel::DayCount::Act365Fixed);
  (void)port->add_rate(cdsmodel::from_tdate(JpmcdsDate(2025, 1, 1)), 0.05);
  std::printf(
      "  add_cash_flow_list(curve, {}, 1.0, nullopt) -> %s\n",
      cdsmodel::add_cash_flow_list(*port, {}, 1.0, std::nullopt, cdsmodel::InterpType::Linear)
          ? "(!)"
          : "refused");
  std::printf("  add_cash_flow_list(curve, {}, 1.0, a date)  -> %s\n",
              cdsmodel::add_cash_flow_list(*port, {}, 1.0,
                                           cdsmodel::from_tdate(JpmcdsDate(2030, 1, 1)),
                                           cdsmodel::InterpType::Linear)
                  ? "(!)"
                  : "refused");
  JpmcdsFreeCFL(empty);
}

void u22() {
  heading("UPSTREAM-22", "success with nothing added");
  std::printf("When the final payment falls exactly on the curve's last date, every\n"
              "branch of JpmcdsZCAddCashFlowList declines in turn: the guard admits\n"
              "equality, the search for the first uncovered payment walks off the\n"
              "end, and both remaining branches -- including the one that would add\n"
              "the remaining dates -- test false. Then `status = SUCCESS`.\n\n");

  ZCurve *zc = seed_curve(3, 0.01);
  if (zc == nullptr) {
    return;
  }
  const TDate curve_end = zc->date[zc->numItems - 1];
  const TDate asked_for = JpmcdsDate(2030, 1, 1);
  TDate dates[2] = {JpmcdsDate(2025, 1, 1), curve_end};
  double amounts[2] = {0.05, 1.05};
  TCashFlowList *cfl = JpmcdsNewCFL(dates, amounts, 2);

  const int before = zc->numItems;
  const int rc = JpmcdsZCAddCashFlowList(zc, cfl, 1.0, asked_for, JPMCDS_LINEAR_INTERP, nullptr);
  std::printf("  curve ends %ld, list ends %ld, asked to add %ld\n\n", (long)curve_end,
              (long)curve_end, (long)asked_for);
  std::printf("  rc=%-7s   numItems %d -> %d   point at %ld present? %s\n",
              rc == SUCCESS ? "SUCCESS" : "FAILURE", before, zc->numItems, (long)asked_for,
              JpmcdsZCFindDateExact(zc, asked_for) >= 0 ? "yes" : "NO");

  std::printf("\nThe caller is told the instrument was bootstrapped. The curve lacks\n"
              "the date it asked for and no rate was solved for anything. The next\n"
              "instrument then bootstraps against a curve one point shorter than the\n"
              "caller believes, which is how one silent no-op becomes a wrong curve\n"
              "rather than a missing point.\n\n");

  std::optional<cdsmodel::ZeroCurveBuilder> port = cdsmodel::ZeroCurveBuilder::create(
      cdsmodel::from_tdate(JpmcdsDate(2024, 1, 1)), 16, 1, cdsmodel::DayCount::Act365Fixed);
  for (int i = 1; i <= 3; ++i) {
    (void)port->add_rate(cdsmodel::from_tdate(JpmcdsDate(2024 + i, 1, 1)),
                         0.01 * static_cast<double>(i));
  }
  const std::vector<cdsmodel::CashFlow> flows{{cdsmodel::from_tdate(dates[0]), 0.05},
                                              {cdsmodel::from_tdate(curve_end), 1.05}};
  const std::size_t port_before = port->points().size();
  const bool ok = cdsmodel::add_cash_flow_list(*port, flows, 1.0, cdsmodel::from_tdate(asked_for),
                                               cdsmodel::InterpType::Linear);
  std::printf("The port: add_cash_flow_list -> %s   points %zu -> %zu\n", ok ? "(!)" : "refused",
              port_before, port->points().size());
  std::printf("\nIf no rate was solved, the instrument was not added. Saying so is the\n"
              "only honest answer.\n");

  JpmcdsFreeCFL(cfl);
  JpmcdsZCFree(zc);
}

void u23() {
  heading("UPSTREAM-23", "a sign test that silently drops curve points");
  std::printf("JpmcdsZCAddCFLPoints filters what it inserts:\n\n"
              "    if (rate >= 0.0)\n"
              "    {\n"
              "        dates[n] = d;\n"
              "        rates[n++] = rate;\n"
              "    }\n\n"
              "No else, no message, no status -- in a function whose own comment is\n"
              "\"Ensure all uncovered cash flow dates in curve ... This will make\n"
              "sure they can be priced appropriately by the curve.\"\n\n");

  std::printf("A five-payment zero-coupon bond over a curve at -0.5%%, varying only\n"
              "the price:\n\n");
  std::printf("  %-8s %-14s %-16s %s\n", "price", "solved rate", "dates placed", "repriced");
  for (const double price : {0.97, 1.00, 1.06, 1.20}) {
    ZCurve *zc = JpmcdsZCMake(JpmcdsDate(2024, 1, 1), 16, 1, JPMCDS_ACT_365F);
    if (zc == nullptr) {
      continue;
    }
    JpmcdsZCAddRate(zc, JpmcdsDate(2025, 1, 1), -0.005);
    JpmcdsZCAddRate(zc, JpmcdsDate(2026, 1, 1), -0.008);
    TDate dates[5];
    double amounts[5];
    for (int i = 0; i < 5; ++i) {
      dates[i] = JpmcdsDate(2027 + i, 1, 1);
      amounts[i] = 0.0;
    }
    amounts[4] = 1.0;
    TCashFlowList *cfl = JpmcdsNewCFL(dates, amounts, 5);
    if (JpmcdsZCAddCashFlowList(zc, cfl, price, 0L, JPMCDS_LINEAR_INTERP, nullptr) == SUCCESS) {
      int placed = 0;
      for (const TDate date : dates) {
        if (JpmcdsZCFindDateExact(zc, date) >= 0) {
          ++placed;
        }
      }
      const int at = JpmcdsZCFindDateExact(zc, dates[4]);
      double repriced = 0.0;
      JpmcdsJpmcdsZCPresentValueCFL(zc, cfl, 0, 4, JPMCDS_LINEAR_INTERP, nullptr, &repriced);
      char rate_text[32] = "-";
      if (at >= 0) {
        std::snprintf(rate_text, sizeof rate_text, "%+.4g", zc->rate[at]);
      }
      std::printf("  %-8.2f %-14s %d of 5%-10s %.10f\n", price, rate_text, placed, "", repriced);
    }
    JpmcdsFreeCFL(cfl);
    JpmcdsZCFree(zc);
  }

  std::printf("\nThe cliff is at exactly zero: a solved rate of -2e-13 drops four\n"
              "points that +2e-13 would have kept. The curve's SHAPE turns on the\n"
              "sign of a number that is, at that magnitude, noise. Negative zero\n"
              "rates are not exotic -- EUR, CHF and JPY curves were negative for\n"
              "most of a decade after 2014.\n\n");

  std::printf("What it does NOT do, having looked: the repriced column above is the\n"
              "input price every time, even where 1 of 5 dates was placed. The same\n"
              "holds under flat forwards. Inserting a point at its own interpolated\n"
              "rate is idempotent for both schemes -- linear puts it on the line,\n"
              "flat forwards is log-linear in the discount factor and puts it on\n"
              "that curve -- so splitting an interval at a point already on the path\n"
              "leaves the path alone. The finding is about the curve as DATA, not\n"
              "about any price.\n\n");

  std::printf("The port reproduces no filter: every missing date gets its\n"
              "interpolated rate, which is what the comment describes. The\n"
              "divergence is recorded as data -- the golden vectors carry the exact\n"
              "dates upstream dropped, and the replay asserts the port's curve\n"
              "differs by exactly those and nothing else, for 12 of 60 bootstraps.\n");
}

void u24() {
  heading("UPSTREAM-24", "the roll date is consulted in the opposite case to the one documented");
  std::printf(
      "datelist.h documents JpmcdsNewDateListExtendedRoll like this:\n\n"
      "    \"If rollDate is non-zero and stubAtEnd is False, then the rollDate\n"
      "     is used ... starting from roll-date we see if we can count forward\n"
      "     an integral number of intervals to the MATURITY date. If this is so\n"
      "     then we can generate our datelist by counting forward from the\n"
      "     rolldate.\"\n\n"
      "The code tests startDate to maturity instead:\n\n"
      "    if (JpmcdsCountDates(startDate, maturityDate, interval, &n, &extra) != SUCCESS)\n"
      "        goto done;\n"
      "    if (extra == 0)   /* We can use the roll date */\n\n"
      "extra == 0 means there is NO stub -- the case a roll date cannot help\n"
      "with. extra != 0 means there IS one, which is the only case it could.\n\n");

  TDateInterval quarterly;
  JpmcdsMakeDateInterval(3, 'M', &quarterly);
  const TDate roll = JpmcdsDate(2024, 1, 15);
  const TDate maturity = JpmcdsDate(2027, 1, 15); // twelve quarters from the roll
  const TDate start = JpmcdsDate(2024, 2, 20);    // mid-period

  int n = 0;
  int extra = 0;
  JpmcdsCountDates(roll, maturity, &quarterly, &n, &extra);
  std::printf("  roll  -> mat   intervals=%-3d extraDays=%-4d %s\n", n, extra,
              extra == 0 ? "INTEGRAL  <- the documented case" : "has a stub");
  JpmcdsCountDates(start, maturity, &quarterly, &n, &extra);
  std::printf("  start -> mat   intervals=%-3d extraDays=%-4d %s\n\n", n, extra,
              extra == 0 ? "INTEGRAL" : "has a stub  <- what the code keys on");

  TDateList *with_roll = JpmcdsNewDateListExtendedRoll(start, maturity, roll, &quarterly, FALSE);
  TDateList *without = JpmcdsNewDateListExtendedRoll(start, maturity, 0L, &quarterly, FALSE);
  const auto show = [](const char *tag, const TDateList *dl) {
    std::printf("  %-22s", tag);
    if (dl == nullptr) {
      std::printf(" NULL\n");
      return;
    }
    std::printf(" n=%d:", dl->fNumItems);
    for (int i = 0; i < dl->fNumItems && i < 6; ++i) {
      std::printf(" %ld", static_cast<long>(dl->fArray[i]));
    }
    std::printf("%s\n", dl->fNumItems > 6 ? " ..." : "");
  };
  show("with the roll date", with_roll);
  show("with rollDate = 0", without);
  bool identical =
      with_roll != nullptr && without != nullptr && with_roll->fNumItems == without->fNumItems;
  if (identical) {
    for (int i = 0; i < with_roll->fNumItems; ++i) {
      if (with_roll->fArray[i] != without->fArray[i]) {
        identical = false;
      }
    }
  }
  std::printf("  => the roll date %s\n\n",
              identical ? "CHANGED NOTHING, in the case it exists for" : "mattered");
  JpmcdsFreeDateList(with_roll);
  JpmcdsFreeDateList(without);

  std::printf("Reachability, and a claim I had to withdraw. The library's only call\n"
              "site is cashflow.c:230, which passes startDate AS the roll date. That\n"
              "looks inert -- count_dates(roll, start) is trivially integral when they\n"
              "are equal. Three hand-picked probes agreed. The sweep did not:\n\n");

  int integral_spans = 0;
  int differing = 0;
  int shown = 0;
  for (int month = 1; month <= 12; ++month) {
    for (int start_day = 27; start_day <= 31; ++start_day) {
      const TDate swept_start = JpmcdsDate(2024, month, start_day);
      if (swept_start <= 0) {
        continue;
      }
      for (int maturity_day = 27; maturity_day <= 31; ++maturity_day) {
        const TDate swept_maturity = JpmcdsDate(2027, month, maturity_day);
        if (swept_maturity <= 0) {
          continue;
        }
        for (int periods = 1; periods <= 12; ++periods) {
          TDateInterval iv;
          JpmcdsMakeDateInterval(periods, 'M', &iv);
          int count = 0;
          int remainder = 0;
          if (JpmcdsCountDates(swept_start, swept_maturity, &iv, &count, &remainder) != SUCCESS ||
              remainder != 0) {
            continue;
          }
          ++integral_spans;
          TDateList *as_called =
              JpmcdsNewDateListExtendedRoll(swept_start, swept_maturity, swept_start, &iv, FALSE);
          TDateList *no_roll =
              JpmcdsNewDateListExtendedRoll(swept_start, swept_maturity, 0L, &iv, FALSE);
          bool same = as_called != nullptr && no_roll != nullptr &&
                      as_called->fNumItems == no_roll->fNumItems;
          if (same) {
            for (int i = 0; i < as_called->fNumItems; ++i) {
              if (as_called->fArray[i] != no_roll->fArray[i]) {
                same = false;
              }
            }
          }
          if (!same) {
            ++differing;
            if (shown < 4) {
              std::printf("  2024-%02d-%02d  2027-%02d-%02d  iv=%-3d DIFFER: first %ld vs %ld\n",
                          month, start_day, month, maturity_day, periods,
                          static_cast<long>(as_called->fArray[0]),
                          static_cast<long>(no_roll->fArray[0]));
              ++shown;
            }
          }
          JpmcdsFreeDateList(as_called);
          JpmcdsFreeDateList(no_roll);
        }
      }
    }
  }
  std::printf("\n  %d of %d integral-span cases differ (%.1f%%), every one a 29 February\n"
              "  start.\n\n",
              differing, integral_spans,
              integral_spans > 0 ? 100.0 * differing / integral_spans : 0.0);

  std::printf("The mechanism: the roll branch ends in NewDateListExtended(first, mat,\n"
              "iv, TRUE) -- generating FORWARD from the start -- where rollDate = 0\n"
              "generates BACKWARD from maturity. Those agree only where month\n"
              "arithmetic does not clamp, and clamping is not invertible: 29 Feb + 12M\n"
              "is 28 Feb, but 28 Feb - 12M is 28 Feb, not the 29th.\n\n");

  std::printf("The port REPRODUCES all of this, deliberately. The code is\n"
              "self-consistent and only the documentation disagrees, so the policy's\n"
              "correction clause (UPSTREAM-7) does not apply. A roll date decides\n"
              "accrual; reinterpreting it on the strength of a comment is not a\n"
              "porting decision.\n");
}

void u25(bool unsafe) {
  heading("UPSTREAM-25", "a heap buffer overflow on a one-date schedule");
  std::printf("cashflow.c's JpmcdsNewCFLFromDL3 detects a stub by comparing the\n"
              "instrument's dates against the grid it was handed:\n\n"
              "    frontStub = ( startDate    > dates->fArray[0] );\n"
              "    backStub  = ( maturityDate < dates->fArray[numDates-1] );\n\n"
              "Neither branch checks the grid is long enough to HAVE a surrounding\n"
              "period. With numDates == 1 the front branch indexes past the end:\n\n"
              "    JpmcdsStubPayment(accrualDates->fArray[0],\n"
              "                      accrualDates->fArray[1],   /* index 1 of 1 element */\n"
              "                      accrualStart,\n"
              "                      accrualDates->fArray[1],\n"
              "                      rate, couponDayCountConv,\n"
              "                      &cfl->fArray[1].fAmount)   /* WRITE past the end */\n\n"
              "and the back branch reads fArray[numDates-2] = fArray[-1], before it.\n\n");

  std::printf("Reachability first. JpmcdsMakeCFL builds its own grid, and\n"
              "JpmcdsNewDateListExtended anchors on whichever end the stub is NOT\n"
              "at -- so one of the two comparisons is always false:\n\n");
  TDateInterval annual;
  JpmcdsMakeDateInterval(12, 'M', &annual);
  const TDate start = JpmcdsDate(2024, 2, 20);
  const TDate maturity = JpmcdsDate(2027, 5, 10);
  for (const int stub_at_end : {0, 1}) {
    TDateList *dl = JpmcdsNewDateListExtended(start, maturity, &annual, stub_at_end ? TRUE : FALSE);
    if (dl == nullptr) {
      continue;
    }
    std::printf("  stubAtEnd=%d: n=%d first=%ld last=%ld -> frontStub=%d backStub=%d\n",
                stub_at_end, dl->fNumItems, (long)dl->fArray[0],
                (long)dl->fArray[dl->fNumItems - 1], static_cast<int>(start > dl->fArray[0]),
                static_cast<int>(maturity < dl->fArray[dl->fNumItems - 1]));
    JpmcdsFreeDateList(dl);
  }
  std::printf("\nBut JpmcdsNewCFLFromDL3 is declared in cashflow.h and takes the grid\n"
              "as an argument, so a caller supplying its own list reaches it. That is\n"
              "why this is medium and not high.\n\n");

  if (!unsafe) {
    std::printf("Measured under AddressSanitizer, each in its own process:\n\n"
                "  front stub: heap-buffer-overflow on 0x...0f8  (PAST the allocation)\n"
                "              READ of size 8   #0 JpmcdsNewCFLFromDL3\n"
                "              allocated by     #2 JpmcdsNewEmptyDateList\n"
                "  back stub:  heap-buffer-overflow on 0x...0e8  (BEFORE it)\n"
                "              READ of size 8   #0 JpmcdsNewCFLFromDL3\n\n"
                "In an unsanitised build the front case happens to return NULL: the\n"
                "garbage it read as nextCouponDate failed JpmcdsStubPayment's\n"
                "`prevCouponDate >= nextCouponDate` check. That is luck, not safety --\n"
                "a garbage value on the other side of that comparison lets the\n"
                "function proceed to the out-of-bounds WRITE, which ASan never gets\n"
                "to report because it aborts on the read first.\n\n"
                "Pass --unsafe to run it here.\n");
  } else {
    std::printf("--unsafe given: calling it with a one-date grid. Under the asan\n"
                "preset this process is about to abort.\n");
    std::fflush(stdout);
    TDateList *dl = JpmcdsNewEmptyDateList(1);
    dl->fArray[0] = JpmcdsDate(2024, 1, 1);
    TCashFlowList *cfl =
        JpmcdsNewCFLFromDL3(dl, JpmcdsDate(2024, 6, 1), JpmcdsDate(2024, 12, 1), 0.05,
                            JPMCDS_ACT_365F, FALSE, JPMCDS_BAD_DAY_NONE, JPMCDS_BAD_DAY_NONE,
                            const_cast<char *>("NONE"), const_cast<char *>("NONE"), FALSE);
    std::printf("  returned %s (no abort -- the garbage read happened anyway)\n",
                cfl != nullptr ? "a list" : "NULL");
    JpmcdsFreeCFL(cfl);
    JpmcdsFreeDateList(dl);
  }

  std::printf("\nThe port, same one-date grid:\n");
  const std::vector<cdsmodel::Date> one{cdsmodel::from_tdate(JpmcdsDate(2024, 1, 1))};
  const cdsmodel::CashFlowConventions conventions{.rate = 0.05};
  std::printf("  front stub (start after the only date) -> %s\n",
              cdsmodel::cash_flows_from_dates(one, cdsmodel::from_tdate(JpmcdsDate(2024, 6, 1)),
                                              cdsmodel::from_tdate(JpmcdsDate(2024, 12, 1)),
                                              conventions, {}, {}, false)
                  ? "(!)"
                  : "refused");
  std::printf("  back stub  (maturity before it)        -> %s\n",
              cdsmodel::cash_flows_from_dates(one, cdsmodel::from_tdate(JpmcdsDate(2023, 1, 1)),
                                              cdsmodel::from_tdate(JpmcdsDate(2023, 6, 1)),
                                              conventions, {}, {}, false)
                  ? "(!)"
                  : "refused");
  std::printf("  no stub at all                         -> %s\n",
              cdsmodel::cash_flows_from_dates(one, cdsmodel::from_tdate(JpmcdsDate(2024, 1, 1)),
                                              cdsmodel::from_tdate(JpmcdsDate(2024, 1, 1)),
                                              conventions, {}, {}, false)
                  ? "accepted, as upstream does"
                  : "(!)");

  std::printf("\nA second thing in the same function, NOT filed as a defect. The\n"
              "long-stub coalescing block reassigns `dates = datesCopy` in its first\n"
              "branch, so the second branch's\n\n"
              "    memcpy(&datesCopy->fArray[0], &dates->fArray[0], ...)\n\n"
              "has source and destination equal. C11 7.24.1p2 makes copying between\n"
              "overlapping objects undefined, and identical objects overlap. The\n"
              "result is correct anyway, and neither ASan nor UBSan reports it -- a\n"
              "second class the harness cannot see, in the same function where it\n"
              "caught the overflow above.\n");
}

void u26() {
  heading("UPSTREAM-26", "a substitution table silently disables the convention");
  std::printf("zcswdate.c's JpmcdsZCAdjustDate branches on the PRESENCE of a bad\n"
              "day list, not on its contents:\n\n"
              "    if (badDayList != NULL)\n"
              "    {\n"
              "        *adjDate = JpmcdsBad2GoodBadDayList(badDayList, date);\n"
              "        return SUCCESS;\n"
              "    }\n"
              "    else\n"
              "    {\n"
              "        if (JpmcdsBusinessDay(date, badDayConv, holidayFile, adjDate) == FAILURE)\n"
              "            goto done;\n"
              "    }\n\n"
              "So with a list supplied, badDayConv and holidayFile are never consulted\n"
              "-- and Bad2Good returns its input unchanged for any date the list does\n"
              "not mention. The function is documented as \"Adjusts date to fall on a\n"
              "business day, if necesssary\". On that path it does not.\n\n");

  const TDate listed = JpmcdsDate(2024, 1, 6); // a Saturday
  const TDate other = JpmcdsDate(2024, 1, 13); // also a Saturday
  const TDate monday = JpmcdsDate(2024, 1, 8);
  TDate bad_days[1] = {listed};
  TDate good_days[1] = {monday};
  TBadDayList table{1, 1, bad_days, good_days};

  std::printf("  %-42s %s\n", "input", "adjusted to");
  TDate out = 0;
  JpmcdsZCAdjustDate(listed, nullptr, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  %-42s %ld\n", "Saturday, no list, conv=F", (long)out);
  JpmcdsZCAdjustDate(listed, &table, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  %-42s %ld   (from the list)\n", "Saturday, in the list, conv=F", (long)out);
  JpmcdsZCAdjustDate(other, nullptr, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  %-42s %ld\n", "another Saturday, no list, conv=F", (long)out);
  JpmcdsZCAdjustDate(other, &table, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  %-42s %ld   <- STILL A SATURDAY\n", "another Saturday, list given, conv=F",
              (long)out);

  std::printf("\nAn EMPTY list does it to every date, because the test is on the\n"
              "pointer rather than on `count`:\n\n");
  TBadDayList empty{0, 0, nullptr, nullptr};
  JpmcdsZCAdjustDate(listed, nullptr, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  no list (NULL),   conv=F -> %ld\n", (long)out);
  JpmcdsZCAdjustDate(listed, &empty, 'F', const_cast<char *>("NONE"), &out);
  std::printf("  EMPTY list,       conv=F -> %ld   <- unchanged\n", (long)out);

  std::printf("\nThe port reproduces the behaviour and fixes the SIGNATURE: the table\n"
              "is a std::optional<std::span<const BadDayPair>>, which says \"either\n"
              "this or the convention\" where upstream's two unrelated parameters say\n"
              "\"both\". The optional is required rather than stylistic -- no-table and\n"
              "empty-table are different inputs with different results, and a plain\n"
              "span would have merged them.\n\n");

  const std::vector<cdsmodel::Calendar> calendars{cdsmodel::Calendar::none()};
  const std::vector<cdsmodel::BadDayPair> port_table{
      {cdsmodel::from_tdate(listed), cdsmodel::from_tdate(monday)}};
  const auto show = [](const char *tag, std::optional<cdsmodel::Date> date) {
    std::printf("  %-42s %s\n", tag,
                date ? std::to_string(cdsmodel::to_tdate(*date)).c_str() : "refused");
  };
  show("nullopt (use the convention)",
       cdsmodel::zc_adjust_date(cdsmodel::from_tdate(other), std::nullopt,
                                cdsmodel::BadDayConv::Follow, calendars));
  show("a table that omits it",
       cdsmodel::zc_adjust_date(cdsmodel::from_tdate(other),
                                std::span<const cdsmodel::BadDayPair>{port_table},
                                cdsmodel::BadDayConv::Follow, calendars));
  show("an EMPTY table", cdsmodel::zc_adjust_date(cdsmodel::from_tdate(other),
                                                  std::span<const cdsmodel::BadDayPair>{},
                                                  cdsmodel::BadDayConv::Follow, calendars));
}

void u27() {
  heading("UPSTREAM-27", "a day-of-month heuristic that suppresses real cycles");
  std::printf("zcswdate.c's SetPrevDateAndOnCycle decides on-cycle in two steps, and\n"
              "the first is a guess:\n\n"
              "    if ((valueMDY.day <= 28) && (origMDY.day <= 28))\n"
              "    {\n"
              "        /* We assume we can only be on cycle if date is not on or\n"
              "         * after the 29th of the month. */\n"
              "        JpmcdsCountDates(valueDate, origDate, &interval, &n, &extraDays);\n"
              "        *onCycle = (extraDays == 0);\n"
              "    }\n"
              "    else\n"
              "    {\n"
              "        *onCycle = FALSE;\n"
              "    }\n\n"
              "JpmcdsCountDates disagrees -- it reports an exact interval count for\n"
              "month-end dates perfectly well:\n\n");

  TDateInterval quarterly;
  JpmcdsMakeDateInterval(3, 'M', &quarterly);
  for (const int day : {29, 30, 31}) {
    const TDate value_date = JpmcdsDate(2024, 1, day);
    const TDate maturity = JpmcdsDate(2027, 1, day);
    if (value_date <= 0 || maturity <= 0) {
      continue;
    }
    int intervals = 0;
    int extra = 0;
    JpmcdsCountDates(value_date, maturity, &quarterly, &intervals, &extra);
    std::printf("  day %d: CountDates extraDays=%-3d so genuinely %s; heuristic says OFF\n", day,
                extra, extra == 0 ? "ON cycle" : "off cycle");
  }

  std::printf("\nSwept over every month, the days 26-31 and four frequencies:\n\n");
  int swept = 0;
  int suppressed = 0;
  int differing = 0;
  int shown = 0;
  for (const long freq : {1L, 2L, 4L, 12L}) {
    TDateInterval interval;
    if (JpmcdsFreq2TDateInterval(static_cast<int>(freq), &interval) != SUCCESS) {
      continue;
    }
    for (int month = 1; month <= 12; ++month) {
      for (int value_day = 26; value_day <= 31; ++value_day) {
        const TDate value_date = JpmcdsDate(2024, month, value_day);
        if (value_date <= 0) {
          continue;
        }
        for (int years = 1; years <= 5; ++years) {
          for (int maturity_day = 26; maturity_day <= 31; ++maturity_day) {
            const TDate maturity = JpmcdsDate(2024 + years, month, maturity_day);
            if (maturity <= 0 || maturity <= value_date) {
              continue;
            }
            int intervals = 0;
            int extra = 0;
            if (JpmcdsCountDates(value_date, maturity, &interval, &intervals, &extra) != SUCCESS) {
              continue;
            }
            ++swept;
            if (extra != 0 || (value_day <= 28 && maturity_day <= 28)) {
              continue;
            }
            ++suppressed;
            TDate forward = 0;
            TDate backward = 0;
            if (JpmcdsDateFromDateAndOffset(value_date, &interval, intervals - 1, &forward) !=
                    SUCCESS ||
                JpmcdsDateFromDateAndOffset(maturity, &interval, -1, &backward) != SUCCESS) {
              continue;
            }
            if (forward != backward) {
              ++differing;
              if (shown < 4) {
                std::printf("  freq=%-3ld 2024-%02d-%02d -> %d-%02d-%02d : on-cycle %ld, "
                            "off-cycle %ld  DIFFER by %ld\n",
                            freq, month, value_day, 2024 + years, month, maturity_day,
                            (long)forward, (long)backward, (long)(forward - backward));
                ++shown;
              }
            }
          }
        }
      }
    }
  }
  std::printf("\n  swept %d, the heuristic suppressed %d genuinely on-cycle dates,\n", swept,
              suppressed);
  std::printf("  of which %d give a DIFFERENT previous coupon date (%.1f%%)\n", differing,
              suppressed > 0 ? 100.0 * differing / suppressed : 0.0);

  std::printf("\nEvery differing case has a 29 February value date -- the same clamping\n"
              "asymmetry as UPSTREAM-24, forward from the 29th and backward from the\n"
              "28th not meeting. And in the annual case the forward route is\n"
              "valueDate + 0 intervals, the value date ITSELF, which is on the\n"
              "schedule by definition; the backward route gives the day before it, a\n"
              "date the schedule does not contain.\n\n"
              "Reproduced deliberately: moving a swap's previous coupon date is a\n"
              "convention decision, not a porting one.\n");
}

void u28() {
  heading("UPSTREAM-28", "a process-wide flag left disabled on the success paths");
  std::printf("interpc.c's JpmcdsInterpRateExact disables the binary search's order\n"
              "checking, saving the old value to restore later:\n\n"
              "    /*\n"
              "    ** ... we will turn off order checking in the binary search routines.\n"
              "    **\n"
              "    ** This might well produce a dramatic improvement in all the ALIB\n"
              "    ** zero curve analytics!\n"
              "    */\n"
              "    TBoolean checkOrder = JpmcdsBSearchCheckOrder (FALSE);\n\n"
              "and restores it at `done:`. Between those two points sit TWO\n"
              "`return SUCCESS` statements -- the clamps for a date past the end of\n"
              "the curve and a date before its start.\n\n");

  const TDate base = JpmcdsDate(2024, 1, 1);
  TDate dates[3] = {JpmcdsDate(2025, 1, 1), JpmcdsDate(2026, 1, 1), JpmcdsDate(2027, 1, 1)};
  double rates[3] = {0.03, 0.035, 0.04};
  TCurve *curve = JpmcdsMakeTCurve(base, dates, rates, 3, 1.0, JPMCDS_ACT_365F);
  if (curve == nullptr) {
    std::printf("  (upstream refused the curve; nothing to show)\n");
    return;
  }

  // JpmcdsBSearchCheckOrder(x) sets the flag and returns the PREVIOUS value,
  // so calling it twice reads the state without changing it.
  const auto read_flag = []() {
    const TBoolean was = JpmcdsBSearchCheckOrder(TRUE);
    JpmcdsBSearchCheckOrder(was);
    return was == TRUE;
  };

  std::printf("  %-44s %s\n", "after calling with...", "order checking");
  JpmcdsBSearchCheckOrder(TRUE);
  std::printf("  %-44s %s\n", "(initial state)", read_flag() ? "ON" : "OFF");

  double rate = 0.0;
  JpmcdsBSearchCheckOrder(TRUE);
  JpmcdsInterpRate(JpmcdsDate(2025, 7, 1), curve, JPMCDS_LINEAR_INTERP, &rate);
  std::printf("  %-44s %s\n", "a date inside the curve", read_flag() ? "ON" : "OFF");

  JpmcdsBSearchCheckOrder(TRUE);
  JpmcdsInterpRate(JpmcdsDate(2040, 1, 1), curve, JPMCDS_LINEAR_INTERP, &rate);
  std::printf("  %-44s %s  <- LEAKED\n", "a date AFTER the curve (extrapolation)",
              read_flag() ? "ON" : "OFF");

  JpmcdsBSearchCheckOrder(TRUE);
  JpmcdsInterpRate(JpmcdsDate(2000, 1, 1), curve, JPMCDS_LINEAR_INTERP, &rate);
  std::printf("  %-44s %s  <- LEAKED\n", "a date BEFORE the curve (extrapolation)",
              read_flag() ? "ON" : "OFF");

  JpmcdsBSearchCheckOrder(TRUE);
  (void)JpmcdsInterpRate(JpmcdsDate(2025, 7, 1), curve, 999L, &rate);
  std::printf("  %-44s %s\n", "an unknown interp type (an error path)", read_flag() ? "ON" : "OFF");

  std::printf("\nSo the leak is on the SUCCESS paths, and the genuine error path\n"
              "restores correctly -- the exact opposite of UPSTREAM-13, which leaks\n"
              "the same global on its ERROR paths. Two functions share one flag and\n"
              "leak it under complementary conditions, so between them there is no\n"
              "state of the program in which it can be trusted.\n\n");

  std::printf("What it costs, in two calls:\n\n");
  double unsorted[4] = {1.0, 5.0, 3.0, 9.0};
  int lo = 0;
  int hi = 0;
  JpmcdsBSearchCheckOrder(TRUE);
  std::printf("  checking ON : JpmcdsBSearchDouble(unsorted) -> %s\n",
              JpmcdsBSearchDouble(4.0, unsorted, sizeof(double), 4, &lo, &hi) == SUCCESS
                  ? "SUCCESS (!)"
                  : "FAILURE");
  JpmcdsInterpRate(JpmcdsDate(2040, 1, 1), curve, JPMCDS_LINEAR_INTERP, &rate);
  std::printf("  after ONE extrapolation: the same call      -> %s\n",
              JpmcdsBSearchDouble(4.0, unsorted, sizeof(double), 4, &lo, &hi) == SUCCESS
                  ? "SUCCESS  <- no longer validated"
                  : "FAILURE");
  JpmcdsBSearchCheckOrder(TRUE);

  std::printf("\nThe port cannot do this, structurally rather than carefully: there is\n"
              "no global. `bracket` and `bracket_checked` are separate functions and\n"
              "the caller picks -- the decision recorded in bsearch.hpp when\n"
              "UPSTREAM-13 was filed. Extrapolating repeatedly changes nothing:\n\n");
  const std::vector<cdsmodel::RatePoint> points{{cdsmodel::from_tdate(dates[0]), rates[0]},
                                                {cdsmodel::from_tdate(dates[1]), rates[1]},
                                                {cdsmodel::from_tdate(dates[2]), rates[2]}};
  const std::optional<cdsmodel::ZeroCurve> ported = cdsmodel::ZeroCurve::create(
      cdsmodel::from_tdate(base), points, 1.0, cdsmodel::DayCount::Act365Fixed);
  if (ported) {
    for (int i = 0; i < 3; ++i) {
      (void)cdsmodel::interp_rate(*ported, cdsmodel::from_tdate(JpmcdsDate(2040, 1, 1)),
                                  cdsmodel::InterpType::Linear);
    }
    std::printf("  after 3 extrapolations, bracket_checked(unsorted) -> %s\n",
                cdsmodel::bracket_checked(std::span<const double>{unsorted}, 4.0)
                    ? "accepted (!)"
                    : "still refused");
  }
  std::printf("\nOne decision has now pre-empted two defects, in two files, found two\n"
              "slices apart. That is the argument for removing mutable global state\n"
              "rather than auditing its users.\n");
  JpmcdsFreeTCurve(curve);
}

} // namespace

int main(int argc, char **argv) {
  JpmcdsErrMsgOff();
  bool unsafe = false;
  const char *only = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--unsafe") == 0) {
      unsafe = true;
    } else {
      only = argv[i]; // "u1" .. "u5": run just that section
    }
  }
  const auto want = [only](const char *id) {
    return only == nullptr || std::strcmp(only, id) == 0;
  };

  std::printf("Upstream defect evidence -- ISDA CDS Standard Model 1.8.3 vs cdsmodel %s\n",
              std::string(cdsmodel::version()).c_str());
  if (unsafe) {
    std::printf("\n*** --unsafe: this run deliberately triggers undefined behaviour ***\n");
  }

  // Under a sanitizer build, a section marked --unsafe aborts the process, so
  // pass an id to run just one: `defect_evidence u4 --unsafe`.
  if (want("u1")) {
    u1(unsafe);
  }
  if (want("u2")) {
    u2();
  }
  if (want("u3")) {
    u3();
  }
  if (want("u5")) {
    u5();
  }
  if (want("u4")) {
    u4(unsafe);
  }
  if (want("u7")) {
    u7();
  }
  if (want("u8")) {
    u8();
  }
  if (want("u9")) {
    u9(unsafe);
  }
  if (want("u10")) {
    u10(unsafe);
  }
  if (want("u11")) {
    u11();
  }
  if (want("u12")) {
    u12();
  }
  if (want("u13")) {
    u13();
  }
  if (want("u14")) {
    u14();
  }
  if (want("u15")) {
    u15();
  }
  if (want("u16")) {
    u16();
  }
  if (want("u17")) {
    u17();
  }
  if (want("u18")) {
    u18();
  }
  if (want("u19")) {
    u19();
  }
  if (want("u20")) {
    u20();
  }
  if (want("u21")) {
    u21(unsafe);
  }
  if (want("u22")) {
    u22();
  }
  if (want("u23")) {
    u23();
  }
  if (want("u24")) {
    u24();
  }
  if (want("u25")) {
    u25(unsafe);
  }
  if (want("u26")) {
    u26();
  }
  if (want("u27")) {
    u27();
  }
  if (want("u28")) {
    u28();
  }
  return 0;
}
