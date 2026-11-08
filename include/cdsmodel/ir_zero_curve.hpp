#pragma once

// Port of upstream zerocurve.c -- the top of the interest-rate curve stack.
// One exported function, and the only one in the library a caller reaches
// without knowing anything about ZCurves, stub curves or bootstraps.
//
//   JpmcdsBuildIRZeroCurve -> build_ir_zero_curve
//
// It sorts a benchmark list into money-market and swap instruments, builds a
// cash curve from the first group, and bootstraps the second on top.
//
// ## Three parallel arrays and one count
//
// Upstream takes `char *instrNames`, `TDate *dates`, `double *rates` and a
// single `long nInstr` covering all three -- so a caller can hand it three
// arrays of different lengths and it will read past the end of two of them.
// The same shape the fee leg had, and the same fix: one span of structs, so a
// mismatched list is unrepresentable rather than merely undetected.
//
// ## Deliberate difference: UPSTREAM-35
//
// `instr = toupper(instrNames[i]);` passes a plain `char` to `toupper`. See
// the note on `instrument_kind_from_code`.
//
// ## The parameter whose comment admits it is unused
//
// ```c
// char    fwdLength = '3';   /* not used */
// ```
//
// declared, initialised, passed to `JpmcdsZCSwaps`, and -- as gtozc.c's port
// records -- never read by anything. This is the only place in the library
// where the dead parameter is annotated as dead at the call site. Four files
// in a row now carry the parameters of a feature that was removed; here
// somebody noticed and wrote it down rather than deleting it.
//
// ## An empty instrument list fails as an allocation error
//
// `nInstr == 0` reaches `NEW_ARRAY(TDate, 0)`, which is
// `JpmcdsMallocSafe(0)`, which returns NULL **by design**:
//
//     if (theSize <= 0)
//     {
//         JpmcdsErrMsg("%s: Number of bytes (%lu) must be at least 1.\n", ...);
//         return NULL;
//     }
//
// so the four allocations fail, the NULL check fires, and the caller is told
// about bytes rather than about instruments. Measured. The port also refuses
// an empty list -- there is no curve to build -- but refuses it as what it is.
// Not filed as a defect: the outcome is correct and only the diagnostic is
// wrong, and a diagnostic is not something a `std::optional` port can
// reproduce or improve on.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/curve_instruments.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// Which kind of benchmark a quote is. Upstream's `'M'` and `'S'`.
enum class InstrumentKind { MoneyMarket, Swap };

/// One benchmark quote: what it is, when it matures, what it is quoted at.
///
/// Upstream keeps these in three parallel arrays with one shared count.
struct InstrumentQuote {
  InstrumentKind kind = InstrumentKind::MoneyMarket;
  Date date{};
  double rate = 0.0;

  friend bool operator==(const InstrumentQuote &, const InstrumentQuote &) = default;
};

/// Parse upstream's instrument letter. `'M'`/`'m'` is money market,
/// `'S'`/`'s'` is a swap, anything else is refused.
///
/// DELIBERATE DIFFERENCE, UPSTREAM-35. Upstream writes
///
/// ```c
/// instr = toupper(instrNames[i]);
/// if (instr != JpmcdsMONEYNAME && instr != JpmcdsSWAPNAME)
/// ```
///
/// and `instrNames[i]` is a plain `char`, which is signed on every platform
/// this port builds on. C requires `toupper`'s argument to be representable as
/// `unsigned char` or equal to `EOF`; a byte with the high bit set is neither.
/// Measured: `(char)0xE9` reaches `toupper` as `-23`, with `EOF` being `-1`.
///
/// The instrument check rejects the character immediately afterwards, so no
/// wrong answer follows -- the undefined behaviour happens first, and on a
/// library whose only such table happens to be padded for it, nothing visible
/// occurs. UBSan has no check for this. Like UPSTREAM-31 it is undefined
/// behaviour that the harness cannot see, and unlike UPSTREAM-31 it is one of
/// the best-known traps in C.
///
/// This function compares the four characters directly. No `toupper`, so no
/// argument contract to violate -- and no locale either, which `toupper` does
/// consult: under a locale where `'s'` uppercases to something other than
/// `'S'`, upstream's parse changes. That is a second reason not to use it.
[[nodiscard]] constexpr Expected<InstrumentKind> instrument_kind_from_code(char code) noexcept {
  switch (code) {
  case 'M':
  case 'm':
    return InstrumentKind::MoneyMarket;
  case 'S':
  case 's':
    return InstrumentKind::Swap;
  default:
    return fail(ErrorKind::unsupported, "instrument code must be M or S");
  }
}

/// Replaces `JpmcdsBuildIRZeroCurve`: build an interest-rate zero curve from a
/// list of money-market and swap benchmarks.
///
/// The money-market quotes become the short end, and the swaps are
/// bootstrapped on top of them -- so the order within each group matters and
/// the interleaving between groups does not, exactly as upstream's two passes
/// imply.
///
/// `fwdLength` is not a parameter: upstream's is annotated `/* not used */` at
/// the call site. There is no discount curve parameter either -- upstream
/// hardcodes `NULL`, meaning the floating leg is taken at par.
[[nodiscard]] inline Expected<ZeroCurve>
build_ir_zero_curve(Date value_date, std::span<const InstrumentQuote> instruments,
                    DayCount money_market_day_count, const SwapConventions &conventions,
                    const Calendar &calendar) {
  if (instruments.empty()) {
    // Upstream fails here too, via JpmcdsMallocSafe(0). See the header note.
    return fail(ErrorKind::bad_argument, "no instruments were supplied");
  }

  std::vector<RatePoint> cash;
  std::vector<Date> swap_dates;
  std::vector<double> swap_rates;
  for (const InstrumentQuote &quote : instruments) {
    if (quote.kind == InstrumentKind::MoneyMarket) {
      cash.push_back(RatePoint{quote.date, quote.rate});
    } else {
      swap_dates.push_back(quote.date);
      swap_rates.push_back(quote.rate);
    }
  }

  // Upstream starts from JpmcdsNewTCurve(valueDate, 0, 1.0, ACT/365F) -- a
  // curve with no points -- and passes it through both stages. The port's
  // ZeroCurve cannot be empty, so the first stage that has instruments starts
  // from the value date instead. Same two stages, same order.
  // D7: `optional` for "no stage has run yet", which is a state of the build
  // rather than a failure. A stage that FAILS returns through the error
  // channel immediately, so this never holds a failure.
  std::optional<ZeroCurve> curve;
  if (!cash.empty()) {
    const Expected<ZeroCurve> from_cash = curve_with_cash(value_date, cash, money_market_day_count);
    if (!from_cash) {
      return unexpected(from_cash.error());
    }
    curve = *from_cash;
  }

  if (swap_dates.empty()) {
    // Money market only. `curve` is necessarily set: an empty `cash` with an
    // empty `swap_dates` was refused above.
    if (!curve) {
      return fail(ErrorKind::bad_argument, "no instruments were supplied");
    }
    return *curve;
  }
  if (!curve) {
    // Swaps only: upstream hands JpmcdsZCSwaps the zero-point curve, where
    // lastStubDate falls back to the base date.
    return curve_with_swaps(value_date, std::nullopt, swap_dates, swap_rates, conventions,
                            calendar);
  }
  return curve_with_swaps(*curve, std::nullopt, swap_dates, swap_rates, conventions, calendar);
}

} // namespace cdsmodel
