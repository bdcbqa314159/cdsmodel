// The add-in itself -- artifact #3, layer 4 of 4.
//
// Five entry points and nothing else that touches Excel (D3). Every exported
// function does the same three things: convert arguments in, call `invoke` from
// `xl/core`, convert the result out. No logic lives here, which is why C2-C7
// and C9-C10 can be tested with no Excel anywhere.
//
// Registration walks `all_functions()` rather than repeating an 11-argument
// `Excel12f(xlfRegister, ...)` call per function. `cpp_fdm_world` hand-writes
// each one, which is fine for six and copy-paste for twenty-four; upstream's
// `TFuncDef` table had this right and it is the one idea kept from the old XLL.

// `CDSMODEL_XL_TYPECHECK` compiles this on a non-Windows machine against the
// real `xlcall.h` plus the minimal shim in `tools/winshim`. It exists because
// this file is otherwise the only code in the project no compiler has ever
// seen -- see tools/typecheck_xl.sh. Defining `_WIN32` instead would make the
// standard library believe it is on Windows, which breaks libc++ immediately.
#if defined(_WIN32) || defined(CDSMODEL_XL_TYPECHECK)

#include <cdsmodel/version.hpp>
#include <cdsmodel/xl/functions.hpp>
#include <cdsmodel/xl/handles.hpp>
#include <cdsmodel/xl/oper.hpp>

#include <windows.h>

#include "framework.h"
#include "xlcall.h"

#include <array>
#include <string>
#include <vector>

namespace {

/// The one piece of add-in-wide state, and it is an object rather than a
/// process-wide `static` list keyed by `stricmp` (which is what upstream had).
/// `xlAutoClose` empties it, so C7 holds: no handle survives a reload.
cdsmodel::xl::CurveRegistry &curve_registry() {
  static cdsmodel::xl::CurveRegistry instance;
  return instance;
}

cdsmodel::xl::CalendarRegistry &calendar_registry() {
  static cdsmodel::xl::CalendarRegistry instance;
  return instance;
}

/// Gathers the arguments Excel passed, calls the function, renders the result.
/// `count` is what the registration table declared, so a mismatch between the
/// two is impossible by construction here -- C4 checks the table against
/// itself, and this uses the same number.
LPXLOPER12 run(std::string_view name, std::initializer_list<LPXLOPER12> opers) {
  // The SDK's temp memory is freed on entry, not on exit: Excel reads our
  // result after we return, so it has to outlive the call.
  FreeAllTempMemory();

  const cdsmodel::xl::FunctionSpec *spec = cdsmodel::xl::find_function(name);
  if (spec == nullptr) {
    return cdsmodel::xl::temp_string("this add-in has no function by that name");
  }

  std::vector<cdsmodel::xl::XlValue> args;
  args.reserve(opers.size());
  for (const LPXLOPER12 oper : opers) {
    args.push_back(cdsmodel::xl::from_oper(oper));
  }

  cdsmodel::xl::CallContext context{curve_registry(), calendar_registry()};
  return cdsmodel::xl::to_oper(cdsmodel::xl::invoke(*spec, args, context));
}

/// Registers one table entry. Eleven arguments is what `xlfRegister` takes;
/// building them from the table means the names, the type codes, the help and
/// the category cannot drift apart the way they do when each call is written
/// by hand.
void register_one(LPXLOPER12 dll_name, const cdsmodel::xl::FunctionSpec &spec) {
  std::string argument_names;
  for (const auto &p : spec.params) {
    if (!argument_names.empty()) {
      argument_names += ", ";
    }
    argument_names += std::string(p.name);
  }

  // The exported C symbol is the function's name with '.' removed: Excel
  // accepts a dot in the *worksheet* name but not in a DLL export.
  std::string entry_point;
  for (const char c : spec.name) {
    if (c != '.') {
      entry_point.push_back(c);
    }
  }

  std::array<LPXLOPER12, 11> argv{
      dll_name,
      cdsmodel::xl::temp_string(entry_point),
      cdsmodel::xl::temp_string(spec.type_codes),
      cdsmodel::xl::temp_string(spec.name),
      cdsmodel::xl::temp_string(argument_names),
      cdsmodel::xl::temp_string("1"), // 1 = a worksheet function
      cdsmodel::xl::temp_string(spec.category),
      cdsmodel::xl::temp_string(""),
      cdsmodel::xl::temp_string(""),
      cdsmodel::xl::temp_string(spec.help),
      cdsmodel::xl::temp_string(""),
  };
  Excel12v(xlfRegister, nullptr, static_cast<int>(argv.size()), argv.data());
}

} // namespace

extern "C" {

// ------------------------------------------------------------- the functions
//
// One wrapper per registered form. These are the only functions Excel calls
// directly, and each is three lines because everything else is above.

__declspec(dllexport) LPXLOPER12
CdsPrice(LPXLOPER12 today, LPXLOPER12 cash_settle, LPXLOPER12 step_in, LPXLOPER12 start,
         LPXLOPER12 end, LPXLOPER12 coupon, LPXLOPER12 pay_accrual, LPXLOPER12 interval,
         LPXLOPER12 stub, LPXLOPER12 day_count, LPXLOPER12 bad_day, LPXLOPER12 holidays,
         LPXLOPER12 discount, LPXLOPER12 spread, LPXLOPER12 recovery, LPXLOPER12 clean) {
  return run("CdsPrice", {today, cash_settle, step_in, start, end, coupon, pay_accrual, interval,
                          stub, day_count, bad_day, holidays, discount, spread, recovery, clean});
}

__declspec(dllexport) LPXLOPER12 CDSPrice(LPXLOPER12 valuation, LPXLOPER12 trade,
                                          LPXLOPER12 conventions, LPXLOPER12 market,
                                          LPXLOPER12 clean) {
  return run("CDS.Price", {valuation, trade, conventions, market, clean});
}

// One wrapper per registered function. Each is three lines because every
// decision lives in `xl/core`; these only name the arguments Excel passes.
// Generated by hand rather than by macro so a reader can see the arity.

__declspec(dllexport) LPXLOPER12 ParSpreads(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                            LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                            LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9,
                                            LPXLOPER12 a10, LPXLOPER12 a11, LPXLOPER12 a12,
                                            LPXLOPER12 a13) {
  return run("ParSpreads", {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13});
}

__declspec(dllexport) LPXLOPER12 UpfrontFlat(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                             LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                             LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9,
                                             LPXLOPER12 a10, LPXLOPER12 a11, LPXLOPER12 a12,
                                             LPXLOPER12 a13, LPXLOPER12 a14, LPXLOPER12 a15,
                                             LPXLOPER12 a16, LPXLOPER12 a17) {
  return run("UpfrontFlat",
             {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17});
}

__declspec(dllexport) LPXLOPER12 ParSpreadFlat(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                               LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                               LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9,
                                               LPXLOPER12 a10, LPXLOPER12 a11, LPXLOPER12 a12,
                                               LPXLOPER12 a13, LPXLOPER12 a14, LPXLOPER12 a15,
                                               LPXLOPER12 a16, LPXLOPER12 a17) {
  return run("ParSpreadFlat",
             {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17});
}

__declspec(dllexport) LPXLOPER12 DefaultAccrual(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                                LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                                LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9,
                                                LPXLOPER12 a10, LPXLOPER12 a11) {
  return run("DefaultAccrual", {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11});
}

__declspec(dllexport) LPXLOPER12 FeeLegFlows(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                             LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                             LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9) {
  return run("FeeLegFlows", {a1, a2, a3, a4, a5, a6, a7, a8, a9});
}

__declspec(dllexport) LPXLOPER12 IRZeroCurveMake(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                                 LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6) {
  return run("IRZeroCurveMake", {a1, a2, a3, a4, a5, a6});
}

__declspec(dllexport) LPXLOPER12 IRZeroCurveBuild(LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3,
                                                  LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
                                                  LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9,
                                                  LPXLOPER12 a10, LPXLOPER12 a11, LPXLOPER12 a12) {
  return run("IRZeroCurveBuild", {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12});
}

__declspec(dllexport) LPXLOPER12 CleanSpreadCurveBuild(
    LPXLOPER12 a1, LPXLOPER12 a2, LPXLOPER12 a3, LPXLOPER12 a4, LPXLOPER12 a5, LPXLOPER12 a6,
    LPXLOPER12 a7, LPXLOPER12 a8, LPXLOPER12 a9, LPXLOPER12 a10, LPXLOPER12 a11, LPXLOPER12 a12,
    LPXLOPER12 a13, LPXLOPER12 a14, LPXLOPER12 a15, LPXLOPER12 a16) {
  return run("CleanSpreadCurveBuild",
             {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16});
}

__declspec(dllexport) LPXLOPER12 DiscountFactor(LPXLOPER12 curve, LPXLOPER12 date) {
  return run("DiscountFactor", {curve, date});
}

__declspec(dllexport) LPXLOPER12 DatesAndRates(LPXLOPER12 curve) {
  return run("DatesAndRates", {curve});
}

__declspec(dllexport) LPXLOPER12 LoadHolidays(LPXLOPER12 path, LPXLOPER12 name) {
  return run("LoadHolidays", {path, name});
}

__declspec(dllexport) LPXLOPER12 CDSCalendar(LPXLOPER12 dates, LPXLOPER12 weekend,
                                             LPXLOPER12 name) {
  return run("CDS.Calendar", {dates, weekend, name});
}

__declspec(dllexport) LPXLOPER12 CDSCalendarFromFile(LPXLOPER12 path, LPXLOPER12 name) {
  return run("CDS.CalendarFromFile", {path, name});
}

__declspec(dllexport) LPXLOPER12 CDSCalendarDates(LPXLOPER12 handle) {
  return run("CDS.CalendarDates", {handle});
}

__declspec(dllexport) LPXLOPER12 CdsModelVersion(void) {
  FreeAllTempMemory();
  return cdsmodel::xl::temp_string(std::string(cdsmodel::version()) + " (port of ISDA " +
                                   std::string(cdsmodel::upstream_version()) + ")");
}

// ----------------------------------------------------------- the entry points

__declspec(dllexport) short xlAutoOpen(void) {
  XLOPER12 dll_name;
  Excel12(xlGetName, &dll_name, 0);
  for (const cdsmodel::xl::FunctionSpec &spec : cdsmodel::xl::all_functions()) {
    register_one(&dll_name, spec);
  }
  // Registered by hand: it takes no arguments, so it is not in the table.
  std::array<LPXLOPER12, 11> version_argv{
      &dll_name,
      cdsmodel::xl::temp_string("CdsModelVersion"),
      cdsmodel::xl::temp_string("U"),
      cdsmodel::xl::temp_string("CdsModelVersion"),
      cdsmodel::xl::temp_string(""),
      cdsmodel::xl::temp_string("1"),
      cdsmodel::xl::temp_string("CDS Model"),
      cdsmodel::xl::temp_string(""),
      cdsmodel::xl::temp_string(""),
      cdsmodel::xl::temp_string("This add-in's version, and the ISDA release it ports."),
      cdsmodel::xl::temp_string(""),
  };
  Excel12v(xlfRegister, nullptr, static_cast<int>(version_argv.size()), version_argv.data());

  Excel12(xlFree, nullptr, 1, &dll_name);
  return 1;
}

__declspec(dllexport) short xlAutoClose(void) {
  // C7: every handle stops resolving. Upstream's cache persisted for the life
  // of the process, so a reload left stale names resolvable.
  curve_registry().clear();
  calendar_registry().clear();
  return 1;
}

/// Nothing to do: every result this add-in returns lives in the SDK's temp
/// memory, which `FreeAllTempMemory()` releases at the start of the next call.
/// Returning memory Excel must free would mean setting `xlbitDLLFree`, and
/// nothing here does.
__declspec(dllexport) void xlAutoFree12(LPXLOPER12) {}

__declspec(dllexport) short xlAutoAdd(void) { return 1; }

__declspec(dllexport) short xlAutoRemove(void) { return 1; }

} // extern "C"

#endif // _WIN32 || CDSMODEL_XL_TYPECHECK
