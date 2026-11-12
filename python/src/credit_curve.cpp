#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cdsmodel/bootstrap.hpp>
#include <cdsmodel/cdsone.hpp>

#include <string>
#include <vector>

#include "blocks.hpp"
#include "date_caster.hpp"
#include "errors.hpp"

namespace nb = nanobind;

void bind_credit_curve(nb::module_ &m) {
  using namespace cdsmodel;
  using cdsmodel::py::Conventions;
  using cdsmodel::py::CurveMarket;
  using cdsmodel::py::FlatMarket;
  using cdsmodel::py::TradeTerms;
  using cdsmodel::py::Valuation;

  nb::class_<Benchmark>(m, "Benchmark", "One quoted CDS: when it matures and at what coupon.")
      .def(
          "__init__",
          [](Benchmark *self, Date end, double coupon_rate, bool include) {
            new (self) Benchmark{end, coupon_rate, include};
          },
          nb::kw_only(), nb::arg("end"), nb::arg("coupon_rate"), nb::arg("include") = true)
      .def_rw("end", &Benchmark::end)
      .def_rw("coupon_rate", &Benchmark::coupon_rate)
      // `include` is upstream's own flag: a benchmark present in the input
      // but excluded from the fit. Kept rather than asking callers to filter,
      // so a sheet and a script can hold the same list.
      .def_rw("include", &Benchmark::include)
      .def("__repr__", [](const Benchmark &b) {
        const std::chrono::year_month_day d{b.end};
        char buf[96];
        std::snprintf(buf, sizeof buf, "Benchmark(end=%04d-%02d-%02d, coupon_rate=%g, include=%s)",
                      int(d.year()), unsigned(d.month()), unsigned(d.day()), b.coupon_rate,
                      b.include ? "True" : "False");
        return std::string{buf};
      });

  nb::class_<CurveMarket>(m, "CurveMarket", "A discount curve and a recovery rate.")
      .def(
          "__init__",
          [](CurveMarket *self, ZeroCurve discount, double recovery) {
            new (self) CurveMarket{std::move(discount), recovery};
          },
          nb::kw_only(), nb::arg("discount"), nb::arg("recovery"))
      .def_rw("discount", &CurveMarket::discount)
      .def_rw("recovery", &CurveMarket::recovery)
      .def("__repr__", [](const CurveMarket &c) {
        return "CurveMarket(recovery=" + std::to_string(c.recovery) + ")";
      });

  nb::class_<UpfrontDates>(m, "UpfrontDates", "The four dates an upfront calculation needs.")
      .def(
          "__init__",
          [](UpfrontDates *self, Date today, Date value, Date benchmark_start, Date stepin) {
            new (self) UpfrontDates{today, value, benchmark_start, stepin};
          },
          nb::kw_only(), nb::arg("today"), nb::arg("value"), nb::arg("benchmark_start"),
          nb::arg("stepin"))
      .def_rw("today", &UpfrontDates::today)
      .def_rw("value", &UpfrontDates::value)
      .def_rw("benchmark_start", &UpfrontDates::benchmark_start)
      .def_rw("stepin", &UpfrontDates::stepin);

  // The missing piece: a credit curve bootstrapped from market quotes. Until
  // this existed a caller had to hand-build a curve of hazard rates with
  // `ZeroCurve.from_points`, which is not how a desk works.
  m.def(
      "clean_spread_curve",
      [](const Valuation &valuation, std::vector<Benchmark> benchmarks,
         const Conventions &conventions, const CurveMarket &market, Date start) {
        const Cds carrier = cdsmodel::py::make_cds(TradeTerms{start, start, 0.0}, conventions);
        return cdsmodel::py::unwrap(cdsmodel::clean_spread_curve(
            valuation.today, market.discount, start, valuation.step_in, valuation.cash_settle,
            benchmarks, market.recovery, carrier, conventions.holidays));
      },
      nb::arg("valuation"), nb::arg("benchmarks"), nb::arg("conventions"), nb::arg("market"),
      nb::kw_only(), nb::arg("start"), nb::call_guard<nb::gil_scoped_release>(),
      "Bootstrap a clean spread curve from quoted CDS benchmarks.");

  // `quote` is the par spread here and the upfront charge in the inverse
  // below -- which is why FlatMarket names the field for its role (D11b).
  m.def(
      "upfront",
      [](const TradeTerms &trade, const UpfrontDates &dates, const Conventions &conventions,
         const FlatMarket &market, bool pay_accrued_at_start) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        return cdsmodel::py::unwrap(cdsone_upfront_charge(cds, dates, conventions.holidays,
                                                          market.discount, market.quote,
                                                          market.recovery, pay_accrued_at_start));
      },
      nb::arg("trade"), nb::arg("dates"), nb::arg("conventions"), nb::arg("market"), nb::kw_only(),
      nb::arg("pay_accrued_at_start") = false, nb::call_guard<nb::gil_scoped_release>(),
      "The upfront charge implied by a quoted flat par spread (market.quote).");

  m.def(
      "par_spread_flat",
      [](const TradeTerms &trade, const UpfrontDates &dates, const Conventions &conventions,
         const FlatMarket &market, bool pay_accrued_at_start) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        return cdsmodel::py::unwrap(cdsone_spread(cds, dates, conventions.holidays, market.discount,
                                                  market.quote, market.recovery,
                                                  pay_accrued_at_start));
      },
      nb::arg("trade"), nb::arg("dates"), nb::arg("conventions"), nb::arg("market"), nb::kw_only(),
      nb::arg("pay_accrued_at_start") = false, nb::call_guard<nb::gil_scoped_release>(),
      "The flat par spread implied by an upfront charge (market.quote). The inverse of upfront.");
}
