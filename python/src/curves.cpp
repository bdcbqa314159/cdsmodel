#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cdsmodel/day_count.hpp>
#include <cdsmodel/day_count_string.hpp>
#include <cdsmodel/zero_curve.hpp>

#include <string>
#include <vector>

#include "date_caster.hpp"
#include "errors.hpp"

namespace nb = nanobind;
using cdsmodel::Date;
using cdsmodel::DayCount;
using cdsmodel::RatePoint;
using cdsmodel::ZeroCurve;

void bind_curves(nb::module_ &m) {
  // D17: UPPER_CASE throughout. Two values elsewhere in the library are named
  // `None`, which does not parse as a Python attribute; a blanket rule avoids
  // a per-name special case. P16 checks every emitted name.
  nb::enum_<DayCount>(m, "DayCount", "Day count conventions.")
      .value("ACT_365", DayCount::Act365)
      .value("ACT_365F", DayCount::Act365Fixed)
      .value("ACT_360", DayCount::Act360)
      .value("THIRTY_360", DayCount::Thirty360)
      .value("THIRTY_E_360", DayCount::ThirtyE360)
      .value("EFFECTIVE_RATE", DayCount::EffectiveRate);

  nb::class_<ZeroCurve>(m, "ZeroCurve", "An interpolated zero-rate curve.")
      // D19: two parallel columns, not a list of RatePoint -- a caller
      // building a curve has dates and rates, and it keeps one type off the
      // public surface.
      .def_static(
          "from_points",
          [](Date base_date, std::vector<Date> dates, std::vector<double> rates, double basis,
             DayCount day_count) {
            if (dates.size() != rates.size()) {
              // Refused here rather than downstream: the library takes one
              // span of pairs and could not see the mismatch.
              throw cdsmodel::py::Failure{
                  cdsmodel::ErrorKind::bad_argument,
                  "dates and rates must be the same length: " + std::to_string(dates.size()) +
                      " dates, " + std::to_string(rates.size()) + " rates"};
            }
            std::vector<RatePoint> points;
            points.reserve(dates.size());
            for (std::size_t i = 0; i < dates.size(); ++i) {
              points.push_back(RatePoint{dates[i], rates[i]});
            }
            // D18: a vector crosses the boundary; the span stays inside.
            return cdsmodel::py::unwrap(ZeroCurve::create(base_date, points, basis, day_count));
          },
          // `base_date` is explicit, as it is in the XLL's IRZeroCurveMake:
          // it is where discount factors are 1.0, which is the valuation date
          // and need not be the first curve point. Inferring it from
          // `dates.front()` was both wrong and, on an empty list, undefined.
          // Five parameters, the cap exactly -- the XLL's sixth is `Name`,
          // which Python does not need because a curve is an object, not a
          // handle into a registry.
          nb::kw_only(), nb::arg("base_date"), nb::arg("dates"), nb::arg("rates"),
          nb::arg("basis") = 1.0, nb::arg("day_count") = DayCount::Act365Fixed,
          "Build a curve from a base date and parallel date and rate columns.")
      // Named for the XLL's `DiscountFactor`, which is the user-facing name
      // on the other surface; the library calls it `zero_price`. Both return
      // a plain double with no `Expected`, because `create` is strict enough
      // that there is no NaN sentinel to report.
      .def(
          "discount_factor", [](const ZeroCurve &c, Date date) { return c.zero_price(date); },
          nb::arg("date"), nb::call_guard<nb::gil_scoped_release>())
      .def(
          "forward_discount_factor",
          [](const ZeroCurve &c, Date start, Date maturity) {
            return c.forward_zero_price(start, maturity);
          },
          nb::arg("start"), nb::arg("maturity"), nb::call_guard<nb::gil_scoped_release>())
      .def(
          "zero_rate", [](const ZeroCurve &c, Date date) { return c.zero_rate(date); },
          nb::arg("date"), nb::call_guard<nb::gil_scoped_release>())
      .def_prop_ro("base_date", &ZeroCurve::base_date)
      .def_prop_ro("basis", &ZeroCurve::basis)
      .def_prop_ro("day_count", &ZeroCurve::day_count)
      .def("dates", [](const ZeroCurve &c) { return c.dates(); })
      .def("rates",
           [](const ZeroCurve &c) {
             std::vector<double> out;
             for (const RatePoint &p : c.points()) {
               out.push_back(p.rate);
             }
             return out;
           })
      .def("__len__", [](const ZeroCurve &c) { return c.points().size(); })
      .def("__repr__", [](const ZeroCurve &c) {
        const std::chrono::year_month_day b{c.base_date()};
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "ZeroCurve(base_date=%04d-%02d-%02d, points=%zu, day_count=%s)",
                      int(b.year()), unsigned(b.month()), unsigned(b.day()), c.points().size(),
                      std::string{to_string(c.day_count())}.c_str());
        return std::string{buf};
      });
}
