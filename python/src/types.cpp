#include <nanobind/nanobind.h>
// Every translation unit needs its own STL caster includes -- they are not
// inherited from the TU that defines NB_MODULE. Omitting this one compiles
// and links, then fails when the function is called (or, if the value is a
// module attribute, at import with an opaque `std::bad_cast`).
#include <nanobind/stl/string.h>

#include <string>

#include "blocks.hpp"
#include "date_caster.hpp"

namespace nb = nanobind;

void bind_types(nb::module_ &m) {
  nb::class_<cdsmodel::py::TradeTerms>(m, "TradeTerms",
                                       "What was traded: the dates and the coupon.")
      .def(nb::init<>())
      // Keyword-only (D11): `start` and `end` are adjacent same-typed dates,
      // so a positional swap would be silent.
      .def(
          "__init__",
          [](cdsmodel::py::TradeTerms *self, cdsmodel::Date start, cdsmodel::Date end,
             double coupon_rate) { new (self) cdsmodel::py::TradeTerms{start, end, coupon_rate}; },
          nb::kw_only(), nb::arg("start"), nb::arg("end"), nb::arg("coupon_rate") = 0.0)
      .def_rw("start", &cdsmodel::py::TradeTerms::start)
      .def_rw("end", &cdsmodel::py::TradeTerms::end)
      .def_rw("coupon_rate", &cdsmodel::py::TradeTerms::coupon_rate)
      // P17: a Jupyter user must see the fields, not an address.
      .def("__repr__", [](const cdsmodel::py::TradeTerms &t) {
        const std::chrono::year_month_day s{t.start}, e{t.end};
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "TradeTerms(start=%04d-%02d-%02d, end=%04d-%02d-%02d, coupon_rate=%g)",
                      int(s.year()), unsigned(s.month()), unsigned(s.day()), int(e.year()),
                      unsigned(e.month()), unsigned(e.day()), t.coupon_rate);
        return std::string{buf};
      });
}
