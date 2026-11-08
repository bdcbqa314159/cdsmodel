// B2 / B5 of specs/02_consumability.md -- the consumer.
//
// This file exists to be compiled OUTSIDE the source tree, against an
// installed package, by a project that knows nothing about cdsmodel beyond
// `find_package`. It therefore includes exactly one header, deliberately:
// if the umbrella is incomplete or the install tree is missing anything, this
// does not compile.
//
// The oracle is self-consistency rather than a pinned number. A CDS whose
// coupon equals its own par spread is worth nothing, by the definition of par
// spread -- so solving for the spread and then pricing at it must return
// (near) zero. That checks the whole stack end to end -- curves, both legs,
// the schedule, the solver -- without this file needing to carry a magic
// constant that could only have come from running it once.

#include <cdsmodel/cdsmodel.hpp>

#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

int main() {
  using namespace cdsmodel;

  const Date today = from_tdate(39000); // 2006-10-09
  const std::vector<Calendar> calendars{Calendar::none()};

  // A flat 4% continuously-compounded discount curve and a flat 1% hazard
  // curve, each pinned at two dates so interpolation has something to do.
  const std::vector<RatePoint> discount_points{{from_tdate(39100), 0.04},
                                               {from_tdate(43000), 0.04}};
  const std::vector<RatePoint> spread_points{{from_tdate(39100), 0.01}, {from_tdate(43000), 0.01}};

  const Expected<ZeroCurve> discount =
      ZeroCurve::create(today, discount_points, kContinuousBasis, DayCount::Act365Fixed);
  const Expected<ZeroCurve> spread =
      ZeroCurve::create(today, spread_points, kContinuousBasis, DayCount::Act365Fixed);
  if (!discount || !spread) {
    std::printf("FAIL: could not build the curves: %s\n",
                (!discount ? discount.error() : spread.error()).message.c_str());
    return 1;
  }

  Cds cds;
  cds.start = today;
  cds.end = from_tdate(40826); // five years out
  cds.coupon_rate = 0.01;
  cds.day_count = DayCount::Act360;

  const double recovery = 0.4;

  const Expected<double> par = cds_par_spread(
      cds, today, today, std::span<const Calendar>{calendars}, *discount, *spread, recovery);
  if (!par) {
    std::printf("FAIL: par spread: %s\n", par.error().message.c_str());
    return 1;
  }

  Cds at_par = cds;
  at_par.coupon_rate = *par;
  const Expected<double> price =
      cds_price(at_par, today, today, today, std::span<const Calendar>{calendars}, *discount,
                *spread, recovery, true);
  if (!price) {
    std::printf("FAIL: price at par: %s\n", price.error().message.c_str());
    return 1;
  }

  std::printf("par spread   %.10f\n", *par);
  std::printf("price at par %.3e\n", *price);

  // The solver's own stopping tolerance bounds how close to zero this can be,
  // so the check is against that and not against an ULP count.
  if (std::fabs(*price) > 1e-6) {
    std::printf("FAIL: a CDS priced at its own par spread should be worth ~0\n");
    return 1;
  }

  // And the error channel must survive the package boundary intact: a refused
  // call has to arrive with its kind and its message, not as a bare failure.
  Cds backwards = cds;
  backwards.end = cds.start - std::chrono::days{1};
  const Expected<double> refused =
      cds_price(backwards, today, today, today, std::span<const Calendar>{calendars}, *discount,
                *spread, recovery, true);
  if (refused) {
    std::printf("FAIL: a CDS ending before it starts was priced\n");
    return 1;
  }
  if (refused.error().message.empty()) {
    std::printf("FAIL: the refusal carried no message across the package boundary\n");
    return 1;
  }
  std::printf("refusal      %s: %s\n", to_string(refused.error().kind).data(),
              refused.error().message.c_str());

  std::printf("CONSUMER OK\n");
  return 0;
}
