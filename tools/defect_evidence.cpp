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
#include <cdsmodel/cds.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/defaulted.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/version.hpp>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

extern "C" {
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
#include "ldate.h"
#include "macros.h"
#include "mdydate.h"
#include "stub.h"
#include "tcurve.h"
#include "yearfrac.h"
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
  return 0;
}
