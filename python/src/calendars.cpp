#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/holiday_file.hpp>

#include <string>
#include <vector>

#include "date_caster.hpp"
#include "errors.hpp"

namespace nb = nanobind;
using cdsmodel::Calendar;
using cdsmodel::Date;
using cdsmodel::Weekend;

void bind_calendars(nb::module_ &m) {
  // D17 in the flesh: `Weekend::None` would be `Weekend.None`, a syntax
  // error. UPPER_CASE throughout, so there is no per-name special case.
  //
  // is_flag: these are bits, and upstream's STANDARD is SATURDAY|SUNDAY. A
  // plain enum would make `SATURDAY | SUNDAY` a TypeError in Python while the
  // same expression is idiomatic in C++.
  // is_flag without is_arithmetic: `is_arithmetic` additionally permits
  // implicit int conversion, and that collapses the repr to a bare number --
  // `96` instead of `Weekend.STANDARD`, which is the same unreadable-object
  // problem P17 exists to prevent.
  nb::enum_<Weekend>(m, "Weekend", nb::is_flag(), "Which weekdays are weekends. Combine with `|`.")
      .value("NONE", Weekend::None)
      .value("MONDAY", Weekend::Monday)
      .value("TUESDAY", Weekend::Tuesday)
      .value("WEDNESDAY", Weekend::Wednesday)
      .value("THURSDAY", Weekend::Thursday)
      .value("FRIDAY", Weekend::Friday)
      .value("SATURDAY", Weekend::Saturday)
      .value("SUNDAY", Weekend::Sunday)
      .value("STANDARD", Weekend::Standard);

  nb::class_<Calendar>(m, "Calendar", "Holidays and weekends, for business-day arithmetic.")
      .def_static(
          "from_dates",
          [](std::vector<Date> holidays, Weekend weekends) {
            return cdsmodel::py::unwrap(Calendar::create(holidays, weekends));
          },
          nb::kw_only(), nb::arg("holidays"), nb::arg("weekends") = Weekend::Standard,
          "A calendar from explicit holiday dates.")
      // Pure, as artifact #4's D4 requires: no IO here. `read_holiday_file`
      // in __init__.py does the reading, because Python's own file errors are
      // what a Python caller expects -- and because `ifstream` opens a
      // directory and reads zero bytes, silently yielding a weekends-only
      // calendar, while `Path.read_text()` raises.
      .def_static(
          "parse",
          [](std::string_view contents) {
            return cdsmodel::py::unwrap(cdsmodel::parse_holiday_file(contents));
          },
          nb::arg("contents"), "Parse holiday-file contents. Does no IO.")
      .def_static("none", &Calendar::none, "Weekends only, no holidays.")
      .def_static("no_weekends", &Calendar::no_weekends, "Every day is a business day.")
      .def("is_weekend", &Calendar::is_weekend, nb::arg("date"))
      .def("is_holiday", &Calendar::is_holiday, nb::arg("date"))
      .def("is_business_day", &Calendar::is_business_day, nb::arg("date"))
      .def_prop_ro("weekends", &Calendar::weekends)
      .def_prop_ro("allows_business_days", &Calendar::allows_business_days)
      .def("holidays",
           [](const Calendar &c) {
             return std::vector<Date>{c.holidays().begin(), c.holidays().end()};
           })
      .def("__len__", [](const Calendar &c) { return c.holidays().size(); })
      .def("__repr__", [](const Calendar &c) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Calendar(holidays=%zu, weekends=%u)", c.holidays().size(),
                      unsigned(c.weekends()));
        return std::string{buf};
      });
}
