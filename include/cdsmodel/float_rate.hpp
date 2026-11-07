#pragma once

// Port of upstream fltrate.c -- the definition of a floating rate. The whole
// file is one function, `JpmcdsFloatRateSet`, which fills a `TFloatRate` and
// returns SUCCESS unconditionally.
//
// Ported ahead of `streamcf.c`, which is both the only caller of
// `JpmcdsFloatRateSet` and the only consumer of the struct it fills.
//
// ## That one call site is a dead store
//
// Worth stating before anything else, because it bounds how much any of this
// matters. `streamcf.c:481` is the library's only call to
// `JpmcdsFloatRateSet`:
//
//     TFloatRate    floatRateDef; /* Used for convexity adjustment */
//     ...
//     if (JpmcdsFloatRateSet(&matInterval, &payInterval, rateDayCountConv,
//                         0 /*SpotOffsetDays*/, 0./*Spread*/, 1./*Weight*/,
//                         &floatRateDef) == FAILURE)
//         goto done;
//
// `floatRateDef` is a local, and after that assignment **nothing reads it** --
// the convexity adjustment its comment names does not exist in this version.
// `matInterval` is computed only to fill it, so it is dead too, and the spread
// and weight passed in are hardcoded 0 and 1 rather than the real ones.
//
// So `fltrate.c` is, in the shipped library, entirely dead: a struct whose
// only constructor is called once, into a variable never read. It is ported
// anyway because `TFloatRate` is a public type that `zcswap.c` also builds (by
// hand, at lines 621-625) and that `streamcf.c` genuinely reads fields from
// via `stream->floatingRate` -- just not from the one this function fills.
//
// Not a defect: dead code computes no wrong answer. It is a latent trap,
// though, and a specific one -- `floatRateDef` is a STACK struct and
// `JpmcdsFloatRateSet` leaves `rateType` unassigned, so restoring the convexity
// adjustment would read an indeterminate field. Neither ASan nor UBSan sees an
// uninitialised stack read, and this project has no MSan job, so that is
// exactly the class the harness would miss.
//
// ## The setter disappears
//
// `JpmcdsFloatRateSet` takes six values and a pointer to the struct, assigns
// five fields directly, expands a macro into the sixth, and returns SUCCESS.
// It validates nothing: not the day count convention, though
// `JpmcdsDayCountValid` is in the same library and `yearfrac.h` is already
// included; not the intervals; not even its three pointers. A function that
// cannot fail and whose entire body is assignment is aggregate initialisation,
// so `FloatRate` is an aggregate and there is no setter to call.
//
// ## The field that is documented, never set, and never read
//
// `TFloatRate` has a seventh field, `rateType`, and this is not the ordinary
// dead-field story:
//
//   * `bastypes.h` documents it with real semantics -- "Note that payInterval
//     is ONLY used if rateType = JPMCDS_SIMPLE_BASIS", and "In order to
//     compute compounding rates, set rateType = JPMCDS_ANNUAL_BASIS (1) or 2
//     for semi-annual compounding, etc.";
//   * `JpmcdsFloatRateSet`, the only constructor, never assigns it;
//   * nothing in the library ever reads it. The only `rateType` identifiers
//     elsewhere are unrelated parameters of `cfinanci.h` functions;
//   * and `streamcf.c:264` reads `payInterval` with no test on `rateType` at
//     all, so the documented condition on `payInterval` is simply false.
//
// Both in-library constructions leave it uninitialised: `streamcf.c:481` goes
// through the setter, and `zcswap.c:621-625` assigns the fields by hand and
// skips it too. It is NOT filed as a defect, on the same grounds as
// `TDateInterval.flag` and `TCouponDates.exDividendDate` -- an uninitialised
// field is a defect when something reads it, and nothing does. What is worth
// recording is that the struct's own documentation describes a mechanism the
// code does not have, which is the UPSTREAM-26 shape without the consequence.
//
// Dropped here. A field with no reader is not part of the interface.
//
// ## The spot offset is calendar days, and no in-library caller adjusts it
//
// `spotOffset` is a `TDateAdjIntvl` (`AdjustedOffset` here), and both
// constructions build it through one macro:
//
//     #define JPMCDS_SET_ADJ_INTERVAL_DAYS(adjIntval, days) \
//     adjIntval.interval.prd = days;             \
//     adjIntval.interval.prd_typ = 'D';          \
//     adjIntval.isBusDays = JPMCDS_DATE_ADJ_TYPE_CALENDAR;  \
//     adjIntval.holidayFile = NULL;              \
//     adjIntval.badDayConv = JPMCDS_BAD_DAY_NONE;
//
// Despite the name, that is a CALENDAR offset with bad-day adjustment
// disabled, so a two-day spot offset from a Friday reset is the Sunday. The
// struct's documentation says "the holiday file and bad day convention in the
// TDateAdjIntvl are used to adjust dates used to compute the rate", and every
// in-library caller sets both to the values that mean "do not adjust".
//
// Reproduced exactly -- `calendar_day_offset` is the macro -- because the
// offset is the caller's to choose and `AdjustedOffset` can express all four
// bases. Whether the unadjusted index start date has a consequence is a
// question about `streamcf.c`, which consumes it, and is answered there.

#include <cdsmodel/date_adjust.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>

namespace cdsmodel {

/// Reproduces `JPMCDS_SET_ADJ_INTERVAL_DAYS`: a count of CALENDAR days with
/// bad-day adjustment disabled. The only way upstream builds a spot offset.
[[nodiscard]] constexpr AdjustedOffset calendar_day_offset(int days) noexcept {
  return AdjustedOffset{.interval = DateInterval{days, PeriodUnit::Days},
                        .basis = OffsetBasis::CalendarPeriod,
                        .bad_day = BadDayConv::None};
}

/// A floating rate: a 5-year semi-annual swap rate, LIBOR, or a compounding
/// rate. Replaces `TFloatRate` and `JpmcdsFloatRateSet`.
///
/// The defaults are the port's choice, not upstream's -- upstream leaves every
/// field uninitialised until the setter runs, and the setter leaves one
/// uninitialised for good. A default that is wrong for a caller is better than
/// a read of indeterminate memory.
struct FloatRate {
  /// Time to maturity of the rate.
  DateInterval maturity_interval{};

  /// Time between the rate's own payments.
  ///
  /// Upstream's documentation is worth keeping: there is no difference between
  /// a pay interval EQUAL to the maturity interval, one GREATER than it, and
  /// one of zero, because the instrument always pays at maturity and
  /// otherwise at pay-interval steps on or before it. Simple LIBOR -- a zero
  /// coupon rate -- is a pay interval equal to the maturity interval.
  DateInterval pay_interval{};

  DayCount day_count = DayCount::Act360;

  /// From the reset date to the date the rate takes effect. See the header
  /// note: upstream's only constructions make this calendar days, unadjusted.
  AdjustedOffset spot_offset{};

  /// Added to the rate AFTER it is multiplied by `weight`, which is upstream's
  /// documented order and the reason these are two fields rather than one.
  double spread = 0.0;

  /// Documented as "Multiplied by rate" -- and NOT read anywhere in the
  /// library. `JpmcdsFloatRateSet` stores it, `zcswap.c:625` sets it to 1.0,
  /// and no line reads it back; `streamcf.c:286` passes the stream's `spread`
  /// to the cash-flow generator and does not pass the weight. So a floating
  /// cash flow is `notional * yearFrac * (rate + spread)` with the weight
  /// absent, whatever it was set to.
  ///
  /// Kept rather than dropped, unlike `rateType`: this one IS part of the
  /// interface a caller fills in, and the port's own consumer may apply it.
  /// Dropping a field two callers set would change their code; dropping one
  /// nobody sets or reads does not.
  double weight = 1.0;

  friend bool operator==(const FloatRate &, const FloatRate &) = default;
};

} // namespace cdsmodel
