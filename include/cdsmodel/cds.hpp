#pragma once

// Port of upstream cds.c -- the vanilla CDS. This is the layer where the two
// legs stop being independent pieces of machinery and become a price.
//
// Upstream's entry points take 11 and 17 positional parameters, several of
// them adjacent dates of the same type. `JpmcdsCdsPrice` alone takes five
// TDates in a row: today, settleDate, stepinDate, startDate, endDate. Nothing
// but argument order stops a caller swapping two of them, and the result of
// swapping them is a plausible-looking number.
//
// So the contract terms -- the things that would appear on a confirmation --
// are gathered into `Cds`, and the valuation inputs stay as arguments. That
// split is the one the model itself makes: `Cds` is what was traded, the
// arguments are when and against what it is being valued.
//
// `JpmcdsCdsFeeLegFlows` is not ported here YET, and the reason this comment
// used to give has expired. It said the function "needs cashflow.c, which is
// layer 7 and not on the pricing path" -- cashflow.c has since been ported in
// full (cash_flow_list.hpp and cash_flow_schedule.hpp), so the dependency is
// available and only the work remains. Flagged by `port_progress.py`'s
// mentioned-but-not-claimed check, which is how a stale deferral gets noticed
// instead of calcifying into a permanent-looking decision.

#include <cdsmodel/bad_day.hpp>
#include <cdsmodel/calendar.hpp>
#include <cdsmodel/contingent_leg.hpp>
#include <cdsmodel/date.hpp>
#include <cdsmodel/date_interval.hpp>
#include <cdsmodel/day_count.hpp>
#include <cdsmodel/fee_leg.hpp>
#include <cdsmodel/schedule.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <span>
#include <vector>

namespace cdsmodel {

/// The terms of a vanilla CDS.
///
/// The coupon interval defaults to three months because upstream does, but it
/// does so by testing `dateInterval == NULL` inside `JpmcdsCdsFeeLegMake` --
/// a default hidden in the callee. Here it is visible at the declaration.
struct Cds {
  Date start;
  Date end;
  double coupon_rate = 0.0;
  DateInterval coupon_interval{3, PeriodUnit::Months};
  StubMethod stub{};
  DayCount day_count = DayCount::Act360;
  BadDayConv bad_day = BadDayConv::None;

  /// `payAccOnDefault`. Whether accrual up to the default date is still owed.
  bool pay_accrual_on_default = true;

  /// Whether protection covers `start` itself as well as `end`.
  ///
  /// `JpmcdsCdsPrice` and `JpmcdsCdsParSpreads` both hardcode this to TRUE and
  /// never expose it; only the leg builders take it. It is settable here
  /// because the leg builders are part of the public surface, but the default
  /// is what the pricing entry points use.
  bool protect_start = true;
};

/// The protection leg of a vanilla CDS. Replaces `JpmcdsCdsContingentLegMake`.
///
/// Upstream delegates to `JpmcdsContingentLegMake` in cx.c, which is ported as
/// `make_contingent_leg` in contingent_leg.hpp -- and this function now
/// delegates to it in the same way, rather than repeating the one-day shift.
/// The earlier revision repeated it AND omitted the accompanying
/// `REQUIRE (endDate > startDate)`, so a zero-length leg with
/// `protect_start == false` was accepted here and refused by upstream.
/// `verify.py claims` found the duplication; the missing check came with it.
///
/// The one-day shift matters and is easy to miss, because it lives in a
/// different file from the PV routine that compensates for it.
/// `ContingentLeg::start` means "protection begins at the END of this day", so
/// covering the start date itself requires backing the stored date up by one:
///
///     if (protectStart) { p->startDate = startDate-1; }
///
/// `contingent_leg_pv` then separately applies the same one-day offset to the
/// step-in and trade dates. BOTH happen; neither substitutes for the other.
/// Omitting this shift costs exactly one day of protection, which on a
/// five-year contract is a relative price error around 2e-5 -- small enough to
/// look like rounding and far too large to be rounding.
[[nodiscard]] inline std::optional<ContingentLeg>
cds_contingent_leg(Date start, Date end, double notional, bool protect_start) {
  return make_contingent_leg(start, end, notional, ProtectionPayment::AtDefault, protect_start);
}

/// PV of the protection leg. Replaces `JpmcdsCdsContingentLegPV`.
///
/// Note what upstream does with `startDate`: it passes it as the leg's start
/// AND as the step-in date. Those are separate parameters of
/// `JpmcdsContingentLegPV` and upstream deliberately ties them together, so
/// the port ties them together too rather than exposing a step-in argument
/// that upstream never lets a caller set independently.
[[nodiscard]] inline std::optional<double>
cds_contingent_leg_pv(Date today, Date value_date, Date start, Date end, double notional,
                      const ZeroCurve &discount, const ZeroCurve &spread, double recovery,
                      bool protect_start) {
  const std::optional<ContingentLeg> leg = cds_contingent_leg(start, end, notional, protect_start);
  if (!leg) {
    return std::nullopt; // upstream's JpmcdsCdsContingentLegMake returns NULL
  }
  return contingent_leg_pv(*leg, today, value_date, /*stepin_date=*/start, discount, spread,
                           recovery);
}

/// The premium leg of a vanilla CDS. Replaces `JpmcdsCdsFeeLegMake`.
///
/// The schedule comes from `make_regular_date_list`, which includes both
/// endpoints, so N+1 dates give N coupon periods.
///
/// Three details are transcribed rather than tidied, because each is
/// observable in a price:
///
///  * The FIRST date is not bad-day adjusted. Every later one is.
///
///  * The LAST accrual end is the UNADJUSTED final date, plus one day when
///    `protect_start` -- while the last PAY date stays adjusted. So the final
///    period can accrue past the day it is paid on. That asymmetry is the
///    standard CDS convention (protection runs to the end of the maturity
///    date, cash settles on a business day) and overwriting the adjusted
///    value is how upstream expresses it.
///
///  * `protect_start` with `end == start` is legal and yields ONE period
///    accruing a single day. `make_regular_date_list` requires `end > start`,
///    so upstream special-cases it with a hand-built two-element date list;
///    the special case exists precisely to bypass that requirement.
///
/// Returns nullopt where upstream fails: `end` before `start` (or equal to it
/// without `protect_start`), an unbuildable schedule, or a bad-day adjustment
/// that cannot land on a business day.
[[nodiscard]] inline std::optional<FeeLeg> cds_fee_leg(const Cds &cds, double notional,
                                                       std::span<const Calendar> calendars) {
  if (cds.protect_start ? cds.end < cds.start : cds.end <= cds.start) {
    return std::nullopt;
  }

  std::vector<Date> dates;
  if (cds.protect_start && cds.end == cds.start) {
    dates = {cds.start, cds.end};
  } else {
    std::optional<std::vector<Date>> schedule =
        make_regular_date_list(cds.start, cds.end, cds.coupon_interval, cds.stub);
    if (!schedule) {
      return std::nullopt;
    }
    dates = std::move(*schedule);
  }

  FeeLeg leg;
  leg.notional = notional;
  leg.coupon_rate = cds.coupon_rate;
  leg.day_count = cds.day_count;
  // Upstream's comment on the None branch says "we will assume that it
  // observes at end of the period", but nothing in the code does that:
  // obsStartOfDay is set from protectStart a few lines later, regardless.
  // The comment describes an intention, not the behaviour.
  leg.accrual_pay = cds.pay_accrual_on_default ? AccrualPayment::All : AccrualPayment::None;
  leg.observe_start_of_day = cds.protect_start;
  leg.payments.reserve(dates.size() - 1);

  Date previous_adjusted = dates.front(); // the first date is never adjusted
  for (std::size_t i = 1; i < dates.size(); ++i) {
    const std::optional<Date> adjusted = adjust(dates[i], cds.bad_day, calendars);
    if (!adjusted) {
      return std::nullopt;
    }
    leg.payments.push_back(FeePayment{previous_adjusted, *adjusted, *adjusted});
    previous_adjusted = *adjusted;
  }

  // The final accrual end comes from the UNADJUSTED date, overwriting what
  // the loop just stored. The pay date keeps its adjustment.
  leg.payments.back().accrual_end =
      cds.protect_start ? dates.back() + std::chrono::days{1} : dates.back();
  return leg;
}

/// PV of the premium leg. Replaces `JpmcdsCdsFeeLegPV`.
[[nodiscard]] inline std::optional<double>
cds_fee_leg_pv(const Cds &cds, Date today, Date value_date, Date stepin, double notional,
               std::span<const Calendar> calendars, const ZeroCurve &discount,
               const ZeroCurve &spread, bool clean) {
  const std::optional<FeeLeg> leg = cds_fee_leg(cds, notional, calendars);
  if (!leg) {
    return std::nullopt;
  }
  return fee_leg_pv(*leg, today, stepin, value_date, discount, spread, clean);
}

/// Price of a vanilla CDS, per unit notional. Replaces `JpmcdsCdsPrice`.
///
/// This is protection bought minus premium paid, so it is positive when the
/// market spread exceeds the contract coupon.
///
/// Three asymmetries between the legs, all upstream's and all deliberate:
///
///  * The premium leg accrues from `cds.start`; protection runs from
///    `max(stepin, cds.start)`. Accrual can predate step-in, protection
///    cannot.
///  * `value_date` is the settlement date for both legs -- `JpmcdsCdsPrice`
///    assigns `valueDate = settleDate` and uses it throughout. Compare
///    `cds_par_spread`, which uses the step-in date instead.
///  * If `max(stepin, cds.start) > cds.end` the protection leg is skipped
///    entirely and contributes zero, rather than being computed as an empty
///    leg. A trade stepping in after maturity therefore prices as the
///    negative of its premium leg.
///
/// `clean` selects a clean price, excluding accrued interest.
///
/// `protect_start` is forced true, as upstream forces it, regardless of what
/// `cds` carries -- the pricing entry point never exposed it.
[[nodiscard]] inline std::optional<double>
cds_price(const Cds &cds, Date today, Date settle, Date stepin, std::span<const Calendar> calendars,
          const ZeroCurve &discount, const ZeroCurve &spread, double recovery, bool clean) {
  if (stepin < today) {
    return std::nullopt; // upstream: REQUIRE(stepinDate >= today)
  }
  // Everything else is left to the routines called, which is what upstream's
  // own comment says it relies on.
  Cds terms = cds;
  terms.protect_start = true;

  const Date value_date = settle;
  const std::optional<double> fee =
      cds_fee_leg_pv(terms, today, value_date, stepin, 1.0, calendars, discount, spread, clean);
  if (!fee) {
    return std::nullopt;
  }

  double contingent = 0.0;
  const Date protection_start = std::max(stepin, terms.start);
  if (protection_start <= terms.end) {
    const std::optional<double> pv =
        cds_contingent_leg_pv(today, value_date, protection_start, terms.end, 1.0, discount, spread,
                              recovery, terms.protect_start);
    if (!pv) {
      return std::nullopt;
    }
    contingent = *pv;
  }
  return contingent - *fee;
}

/// The coupon that makes the CDS worth zero. Replaces `JpmcdsCdsParSpreads`.
///
/// Upstream's version takes an array of end dates and fills an array of
/// spreads. The loop hoists nothing and shares no state between iterations --
/// it rebuilds the schedule from scratch each time -- so it is a plain map,
/// and a caller wanting many maturities loops over this.
///
/// Two differences from `cds_price` that are easy to miss, both upstream's:
///
///  * The value date is the STEP-IN date, not a settlement date. There is no
///    settlement parameter at all.
///  * `cds.coupon_rate` is IGNORED. The premium leg is priced at a coupon of
///    1.0 so that its PV is the annuity, and the par spread is the ratio
///    `protection / annuity`. Passing a coupon here changes nothing, which is
///    worth knowing before trusting a `Cds` round-tripped through this.
///
/// The zero-annuity guard is DEFENSIVE, and the distinction was measured
/// rather than assumed. Upstream writes `parSpread[i] = contingentLegPV /
/// feeLegPV` with no check, so a zero annuity would yield an infinity or a
/// NaN propagating into whatever calibration called it. But no reachable
/// input producing one was found: hazard rates to 1000 leave the annuity
/// positive (survival is damped to 9.8e-16, not zero); a contract already
/// matured is rejected before the division; and the shortest legal contract,
/// `end == start` with `protect_start`, still accrues one day. So this is a
/// guard against a shape that cannot currently occur, not a fix for a
/// demonstrated defect -- and it is not counted as one.
[[nodiscard]] inline std::optional<double>
cds_par_spread(const Cds &cds, Date today, Date stepin, std::span<const Calendar> calendars,
               const ZeroCurve &discount, const ZeroCurve &spread, double recovery) {
  if (stepin < today) {
    return std::nullopt; // upstream: REQUIRE(stepinDate >= today)
  }
  Cds annuity = cds;
  annuity.protect_start = true;
  annuity.coupon_rate = 1.0;

  const std::optional<double> fee = cds_fee_leg_pv(annuity, today, /*value_date=*/stepin, stepin,
                                                   1.0, calendars, discount, spread, true);
  if (!fee) {
    return std::nullopt;
  }
  const std::optional<double> protection =
      cds_contingent_leg_pv(today, /*value_date=*/stepin, std::max(stepin, annuity.start),
                            annuity.end, 1.0, discount, spread, recovery, true);
  if (!protection) {
    return std::nullopt;
  }
  if (*fee == 0.0) {
    return std::nullopt; // upstream divides anyway and returns inf or NaN
  }
  return *protection / *fee;
}

} // namespace cdsmodel
