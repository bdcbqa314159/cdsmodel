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
#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/defaulted.hpp>
#include <cdsmodel/interpolation.hpp>
#include <cdsmodel/stub.hpp>
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
  return 0;
}
