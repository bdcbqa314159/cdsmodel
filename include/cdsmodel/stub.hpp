#pragma once

// Port of upstream stub.c -- where the stub goes when a schedule does not
// divide evenly into whole periods, and how its string form is parsed.
//
// Upstream's TStubMethod is two independent TBooleans, `stubAtEnd` and
// `longStub`, and every caller has to remember which boolean means what. The
// port splits them into two enums so that `position` and `length` cannot be
// swapped at a call site: StubMethod{StubLength::Long} does not compile.
//
// The parser carries one defect and one documentation divergence; both are
// recorded below.
//
// A later slice completed the file: `JpmcdsStubPayment`, `JpmcdsIsEndStub`,
// `JpmcdsStubPosMake` and `JpmcdsBadDayAndStubPosSplit`. They are what
// `cashflow.c`'s schedule constructors and four sites in `zcswap.c` were
// waiting on, and they are below the parser.
//
// No new defect came out of that half, which is worth stating rather than
// leaving as an absence. Two things looked like one and measurement said
// otherwise; both are recorded as observations at their sites.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/date_sup.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/expected.hpp>

#include <optional>
#include <string_view>
#include <utility>

namespace cdsmodel {

/// Which end of the schedule the odd period sits at.
enum class StubPosition { Front, Back };

/// Whether the odd period absorbs an adjacent regular one.
///
/// Short: the stub is the leftover, shorter than a regular period.
/// Long: the leftover is merged into its neighbour, giving one period longer
/// than regular instead of two periods one of which is short.
enum class StubLength { Short, Long };

/// Replaces upstream's `TStubMethod`.
struct StubMethod {
  StubPosition position = StubPosition::Front;
  StubLength length = StubLength::Short;

  friend constexpr bool operator==(const StubMethod &, const StubMethod &) = default;
};

/// Parse a stub method. Replaces `JpmcdsStringToStubMethod`.
///
/// Accepted form is LOCATION[/LENGTH], where only the first character of each
/// field is examined: LOCATION is F or B, LENGTH is S or L. So "f/s", "F/S",
/// "front/short" and "f/sausage" all parse identically, and so do "b" and
/// "back". That leniency is upstream's and is PRESERVED -- the one caller in
/// the shipped distribution passes the lowercase "f/s".
///
/// Two behaviours worth naming, both preserved:
///
///  * "f" succeeds (absent LENGTH defaults to Short) but "f/" FAILS. Upstream
///    distinguishes a missing field, where the parser hands back a null
///    pointer, from an empty one, where it hands back "" and the switch on
///    its first character falls through to the error branch. The asymmetry is
///    observable at a public API, so it stays.
///
///  * Upstream's own error message documents the format as
///    "LOCATION/TYPE/[LENGTH]" -- three fields. The parser splits on a single
///    '/' and reads field two as LENGTH, so there is no TYPE field and any
///    third field is silently ignored: "f/n/l" is read as LENGTH='n' and
///    rejected, never reaching the 'l'. The code is the specification here;
///    the message describes an interface that was never implemented.
///
/// UPSTREAM-9 is NOT reproduced. The malformed-LENGTH branch formats that
/// same two-%s message with a single argument, so vsprintf consumes an
/// indeterminate varargs slot and dereferences it as a string -- an
/// out-of-bounds read on the only input class that reaches the branch. The
/// port returns nullopt and formats nothing, so the defect cannot occur.
[[nodiscard]] constexpr Expected<StubMethod> parse_stub_method(std::string_view name) noexcept {
  // Upstream uses toupper() on a char, which is undefined for negative values
  // -- any byte above 0x7F on a platform with signed char. Comparing both
  // cases directly is locale-independent and has no such domain.
  const auto match = [](char c, char lower, char upper) { return c == lower || c == upper; };

  const std::size_t slash = name.find('/');
  const std::string_view location = name.substr(0, slash);
  if (location.empty()) {
    return fail(ErrorKind::bad_argument, "the stub name has no location field");
  }

  StubMethod stub;
  if (match(location.front(), 'f', 'F')) {
    stub.position = StubPosition::Front;
  } else if (match(location.front(), 'b', 'B')) {
    stub.position = StubPosition::Back;
  } else {
    return fail(ErrorKind::unsupported, "stub location must begin with F or B");
  }

  if (slash == std::string_view::npos) {
    return stub; // no LENGTH field at all -- upstream defaults to short
  }
  const std::string_view length = name.substr(slash + 1);
  if (length.empty()) {
    return fail(ErrorKind::bad_argument, "the stub name has a slash but no length field");
  }
  if (match(length.front(), 's', 'S')) {
    stub.length = StubLength::Short;
  } else if (match(length.front(), 'l', 'L')) {
    stub.length = StubLength::Long;
  } else {
    return fail(ErrorKind::unsupported, "stub length must begin with S or L");
  }
  return stub;
}

/// Upstream's `TStubPos`: the DEFAULT policy for where a stub goes, which is
/// three-valued because it includes "decide from the schedule".
///
/// Deliberately not the same type as `StubPosition`. That one answers "which
/// end is the stub at?" and has two values because a stub is at one end or the
/// other. This one answers "what should I do if nobody said?", and `Auto` is
/// not a position. Upstream encodes both as plain integers and relies on the
/// parameter name.
enum class StubPosDefault { Auto = 0, Front = 1, Back = 2 };

/// Replaces `JpmcdsStubPosMake`, which is a switch over the three values that
/// rejects anything else.
[[nodiscard]] constexpr Expected<StubPosDefault> stub_pos_from_upstream(long code) noexcept {
  switch (code) {
  case 0:
    return StubPosDefault::Auto;
  case 1:
    return StubPosDefault::Front;
  case 2:
    return StubPosDefault::Back;
  default:
    return fail(ErrorKind::unsupported, "unrecognised stub position code");
  }
}

[[nodiscard]] constexpr long to_upstream_code(StubPosDefault pos) noexcept {
  return static_cast<long>(pos);
}

/// Should the stub go at the end? Replaces `JpmcdsIsEndStub`.
///
/// `Front` and `Back` answer directly. `Auto` looks at the schedule: if the
/// span divides evenly there is no stub and the answer is `true`; otherwise
/// there is one and it goes at the front.
///
/// The `Auto` arm is the only one that can fail, and only because counting
/// intervals can. Note what the answer means when there is no stub: nothing.
/// `true` is returned because some value must be, not because an end stub
/// exists -- there is no stub to place. Upstream's comment describes the same
/// thing from the other side ("will return TRUE, unless there is a stub"), and
/// here code and comment agree, which is not true of every function in this
/// port.
[[nodiscard]] inline Expected<bool> is_end_stub(Date start, Date maturity, DateInterval interval,
                                                StubPosDefault position) {
  switch (position) {
  case StubPosDefault::Back:
    return true;
  case StubPosDefault::Front:
    return false;
  case StubPosDefault::Auto: {
    const Expected<DateCount> span = count_dates(start, maturity, interval);
    if (!span) {
      return unexpected(span.error());
    }
    return span->extra_days <= 0;
  }
  }
  return fail(ErrorKind::unsupported, "unrecognised enumerator reached a switch default");
}

/// Split a packed bad-day-convention-and-stub-position into its two parts.
/// Replaces `JpmcdsBadDayAndStubPosSplit`.
///
/// Upstream packs them into one `long`: the convention in the low byte, the
/// stub position in the next one.
///
/// ```c
/// long bdc = value & JPMCDS_BDC_MASK;                              /* 0x000000FF */
/// long sp  = (value & JPMCDS_STUB_POS_MASK) >> JPMCDS_STUB_POS_SHIFT;  /* 0x0000FF00 */
/// if (JpmcdsStubPosMake(sp, stubPos) != SUCCESS) goto done;
/// *badDayConv = bdc;
/// ```
///
/// ## An observation, not a defect
///
/// One output is validated and the other is not. `sp` goes through
/// `JpmcdsStubPosMake`, which rejects anything outside {0,1,2}. `bdc` is
/// masked out and assigned, with no check, although
/// `JpmcdsBadDayConvValid` is right there in the same library. Measured:
///
/// ```
/// input    rc        badDayConv  valid?  stubPos
/// 0x0046   SUCCESS   70 ('F')    yes     0
/// 0x0346   FAILURE   -           -       -        <- stub position rejected
/// 0x0041   SUCCESS   65 ('A')    NO      0        <- convention is not
/// 0x0000   SUCCESS   0           NO      0
/// 0x00FF   SUCCESS   255         NO      0
/// ```
///
/// My first reading was that this lets a meaningless convention travel until
/// some date happens to need adjusting -- making detection depend on the data
/// rather than on the input. It does not. `JpmcdsBusinessDay` validates the
/// convention **before** asking whether the date is a business day, so the
/// same bogus value fails on a Monday and on a Saturday alike:
///
/// ```
/// date                        rc
/// Monday (business day)       FAILURE
/// Saturday (needs adjusting)  FAILURE
/// ```
///
/// So every caller does receive an error. The only consequence is that it is
/// reported from the adjustment site rather than from the split that produced
/// the value, which is a worse diagnostic and not a wrong answer. Recorded
/// here, not in the defect register.
///
/// ## A deliberate difference that follows from the type
///
/// The port cannot reproduce the pass-through, because `BadDayConv` is an enum
/// with four values and an arbitrary byte is not one of them. So the port
/// validates at the split, where upstream validates at first use. Both reject
/// the same inputs overall; the port rejects them earlier. That is recorded in
/// the "Deliberate differences" table, because it IS observable: upstream
/// returns SUCCESS here and fails later, so a caller that splits a packed
/// value and never adjusts a date would see success from upstream and failure
/// from the port.
[[nodiscard]] constexpr Expected<std::pair<BadDayConv, StubPosDefault>>
split_bad_day_and_stub_pos(long packed) noexcept {
  constexpr long kBadDayMask = 0x000000FF;
  constexpr long kStubPosMask = 0x0000FF00;
  constexpr int kStubPosShift = 8;

  const Expected<StubPosDefault> position =
      stub_pos_from_upstream((packed & kStubPosMask) >> kStubPosShift);
  if (!position) {
    return unexpected(position.error());
  }
  const Expected<BadDayConv> conv = bad_day_conv_from_upstream(packed & kBadDayMask);
  if (!conv) {
    return unexpected(conv.error()); // upstream assigns it unchecked; see above
  }
  return std::pair{*conv, *position};
}

/// The payment accrued over a stub period. Replaces `JpmcdsStubPayment`.
///
/// ## Two values computed and both discarded
///
/// Upstream's body, after the validation:
///
/// ```c
/// JpmcdsDayCountFraction(prevCouponDate, nextCouponDate, dcc, &couponYearFrac);
/// coupon = rate * couponYearFrac;              /* never read */
///
/// JpmcdsDayCountFraction(stubStart, stubEnd, dcc, &stubYears);
/// stubFrac = stubYears / couponYearFrac;       /* never read */
///
/// *stubPayment = rate * stubYears;
/// ```
///
/// `coupon` and `stubFrac` are dead. The obvious reading is that the intended
/// answer was `coupon * stubFrac` -- the coupon scaled by the stub's share of
/// its period -- and that is exactly what the two dead lines compute between
/// them. It is also **algebraically the same number**:
///
///     coupon * stubFrac = (rate * couponYearFrac) * (stubYears / couponYearFrac)
///                       = rate * stubYears
///
/// So this is not a defect. It is a simplification that was applied and whose
/// scaffolding was left standing, and the surviving form is the better one:
/// one multiplication instead of a multiply, a divide and a multiply, so one
/// rounding instead of three. The port keeps the single multiplication, which
/// is what upstream actually evaluates.
///
/// ## The validation outlives its purpose
///
/// The answer depends only on `stub_start`, `stub_end`, `rate` and the day
/// count. The coupon period affects nothing in it -- measured, by moving
/// `next_coupon` and watching the result not change:
///
/// ```
/// stub 154511..154602, coupon period ends 154693 -> 0.012465753424657534
/// stub 154511..154602, coupon period ends 154877 -> 0.012465753424657534
/// ```
///
/// But it still decides **acceptance**. Four checks reject inputs on the
/// strength of a period the answer ignores, and the fourth is the interesting
/// one: a stub must touch at least one boundary, so a window strictly inside
/// the period is refused even though `rate * stubYears` is perfectly
/// computable for it:
///
/// ```
/// starts at prevCoupon          SUCCESS
/// ends at nextCoupon            SUCCESS
/// the whole period              SUCCESS
/// strictly inside               refused
/// starts before prevCoupon      refused
/// ```
///
/// All four checks are reproduced. They are the function's contract whatever
/// their original motivation was, and a caller relying on the rejection is
/// relying on observable behaviour -- the same reasoning that kept the
/// `expected_points` check in `ZeroCurveBuilder::create` after it stopped
/// controlling an allocation.
///
/// Returns nullopt where upstream fails: a non-positive coupon period, a stub
/// boundary outside it, a stub touching neither end, or a day count the
/// fraction cannot compute.
[[nodiscard]] inline Expected<double> stub_payment(Date prev_coupon, Date next_coupon,
                                                   Date stub_start, Date stub_end, double rate,
                                                   DayCount day_count) {
  if (prev_coupon >= next_coupon) {
    return fail(ErrorKind::bad_argument, "the previous coupon must come before the next");
  }
  if (stub_start < prev_coupon || stub_start > next_coupon) {
    return fail(ErrorKind::bad_argument, "the stub start lies outside the coupon period");
  }
  if (stub_end > next_coupon || stub_end < prev_coupon) {
    return fail(ErrorKind::bad_argument, "the stub end lies outside the coupon period");
  }
  if (stub_start != prev_coupon && stub_end != next_coupon) {
    return fail(ErrorKind::bad_argument,
                "a stub must begin at the previous coupon or end at the next");
  }
  // Computed and discarded by upstream, but it can FAIL, and that failure is
  // observable -- so the call stays even though the value does not.
  if (!year_fraction(prev_coupon, next_coupon, day_count)) {
    return fail(ErrorKind::bad_argument,
                "the coupon period has no year fraction under this day count");
  }
  const Expected<double> stub_years = year_fraction(stub_start, stub_end, day_count);
  if (!stub_years) {
    return unexpected(stub_years.error());
  }
  return rate * *stub_years;
}

} // namespace cdsmodel
