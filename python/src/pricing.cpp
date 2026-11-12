#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cdsmodel/cash_flow_list.hpp>
#include <cdsmodel/cds.hpp>
#include <cdsmodel/defaulted.hpp>

#include <string>
#include <vector>

#include "blocks.hpp"
#include "date_caster.hpp"
#include "errors.hpp"

namespace nb = nanobind;

namespace {

using cdsmodel::py::Conventions;
using cdsmodel::py::MarketData;
using cdsmodel::py::TradeTerms;
using cdsmodel::py::Valuation;

} // namespace

void bind_results(nb::module_ &m) {
  using namespace cdsmodel;

  // D15: readable objects, not tuples. `flow.date` survives a field being
  // added; `flow[0]` does not.
  nb::class_<CashFlow>(m, "CashFlow", "One payment: when, and how much.")
      .def_ro("date", &CashFlow::date)
      .def_ro("amount", &CashFlow::amount)
      .def("__repr__", [](const CashFlow &f) {
        const std::chrono::year_month_day d{f.date};
        char buf[72];
        std::snprintf(buf, sizeof buf, "CashFlow(date=%04d-%02d-%02d, amount=%.10g)", int(d.year()),
                      unsigned(d.month()), unsigned(d.day()), f.amount);
        return std::string{buf};
      });

  nb::class_<DefaultAccrual>(m, "DefaultAccrual", "Accrued interest owed on a defaulted CDS.")
      .def_ro("days", &DefaultAccrual::days)
      .def_ro("amount", &DefaultAccrual::amount)
      .def("__repr__", [](const DefaultAccrual &a) {
        return "DefaultAccrual(days=" + std::to_string(a.days) +
               ", amount=" + std::to_string(a.amount) + ")";
      });
}

void bind_pricing(nb::module_ &m) {
  using namespace cdsmodel;

  // Five parameters, the cap exactly -- and the same five `CDS.Price` takes
  // in Excel. The library's `cds_price` takes nine; the blocks are what
  // close that gap without losing anything.
  m.def(
      "price",
      [](const TradeTerms &trade, const Valuation &valuation, const Conventions &conventions,
         const MarketData &market, bool clean) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        return cdsmodel::py::unwrap(cds_price(
            cds, valuation.today, valuation.cash_settle, valuation.step_in, conventions.holidays,
            market.discount, market.spread, market.recovery, clean));
      },
      nb::arg("trade"), nb::arg("valuation"), nb::arg("conventions"), nb::arg("market"),
      nb::kw_only(), nb::arg("clean") = true, nb::call_guard<nb::gil_scoped_release>(),
      "The present value of a CDS.");

  m.def(
      "par_spread",
      [](const TradeTerms &trade, const Valuation &valuation, const Conventions &conventions,
         const MarketData &market) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        // `cds_par_spread` takes today and step-in, not the settle date.
        return cdsmodel::py::unwrap(cds_par_spread(cds, valuation.today, valuation.step_in,
                                                   conventions.holidays, market.discount,
                                                   market.spread, market.recovery));
      },
      nb::arg("trade"), nb::arg("valuation"), nb::arg("conventions"), nb::arg("market"),
      nb::call_guard<nb::gil_scoped_release>(), "The coupon at which the CDS would price to zero.");

  m.def(
      "fee_leg_flows",
      [](const TradeTerms &trade, const Conventions &conventions, double notional) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        return cdsmodel::py::unwrap(cds_fee_leg_flows(cds, notional, conventions.holidays));
      },
      nb::arg("trade"), nb::arg("conventions"), nb::kw_only(), nb::arg("notional") = 1.0,
      nb::call_guard<nb::gil_scoped_release>(), "The fee leg's cash flows, one per coupon.");

  m.def(
      "default_accrual",
      [](const TradeTerms &trade, const Conventions &conventions, Date trade_date,
         Date event_determination_date, double notional) {
        const Cds cds = cdsmodel::py::make_cds(trade, conventions);
        return cdsmodel::py::unwrap(cdsmodel::default_accrual(
            cds, trade_date, event_determination_date, notional, conventions.holidays));
      },
      nb::arg("trade"), nb::arg("conventions"), nb::kw_only(), nb::arg("trade_date"),
      nb::arg("event_determination_date"), nb::arg("notional") = 1.0,
      nb::call_guard<nb::gil_scoped_release>(),
      "Accrued interest owed on a CDS that has defaulted.");
}
