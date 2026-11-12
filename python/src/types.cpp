#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
// Every translation unit needs its own STL caster includes -- they are not
// inherited from the TU that defines NB_MODULE. Omitting this one compiles
// and links, then fails when the function is called (or, if the value is a
// module attribute, at import with an opaque `std::bad_cast`).
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <string>
#include <vector>

#include <cdsmodel/day_count_string.hpp>

#include "blocks.hpp"
#include "date_caster.hpp"

namespace nb = nanobind;

void bind_types(nb::module_ &m) {
  nb::class_<cdsmodel::py::TradeTerms>(m, "TradeTerms",
                                       "What was traded: the dates and the coupon.")
      // Keyword-only (D11): `start` and `end` are adjacent same-typed dates,
      // so a positional swap would be silent.
      .def(
          "__init__",
          [](cdsmodel::py::TradeTerms *self, cdsmodel::Date start, cdsmodel::Date end,
             double coupon_rate) { new (self) cdsmodel::py::TradeTerms{start, end, coupon_rate}; },
          nb::kw_only(), nb::arg("start"), nb::arg("end"), nb::arg("coupon_rate") = 0.0,
          "A trade: when it starts, when it ends, and the coupon it pays.")
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

namespace {

/// `repr` for a date field, without pulling in a formatting library.
std::string iso(cdsmodel::Date d) {
  const std::chrono::year_month_day ymd{d};
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", int(ymd.year()), unsigned(ymd.month()),
                unsigned(ymd.day()));
  return std::string{buf};
}

} // namespace

void bind_blocks(nb::module_ &m) {
  using namespace cdsmodel;

  // D17 again: `BadDayConv::None` is the second value in the library whose
  // name is a Python keyword. The blanket UPPER_CASE rule covers it with no
  // special case, exactly as it covered `Weekend::None`.
  nb::enum_<BadDayConv>(m, "BadDayConv", "What to do when a date lands on a non-business day.")
      .value("FOLLOW", BadDayConv::Follow)
      .value("PREVIOUS", BadDayConv::Previous)
      .value("NONE", BadDayConv::None)
      .value("MODIFIED", BadDayConv::Modified);

  nb::enum_<PeriodUnit>(m, "PeriodUnit", "The unit a DateInterval counts in.")
      .value("DAYS", PeriodUnit::Days)
      .value("MONTHS", PeriodUnit::Months);

  nb::enum_<StubPosition>(m, "StubPosition", "Which end carries the odd period.")
      .value("FRONT", StubPosition::Front)
      .value("BACK", StubPosition::Back);

  nb::enum_<StubLength>(m, "StubLength", "Whether the odd period absorbs its neighbour.")
      .value("SHORT", StubLength::Short)
      .value("LONG", StubLength::Long);

  nb::class_<DateInterval>(m, "DateInterval", "A count of days or months.")
      .def(
          "__init__",
          [](DateInterval *self, int periods, PeriodUnit unit) {
            new (self) DateInterval{periods, unit};
          },
          nb::kw_only(), nb::arg("periods"), nb::arg("unit") = PeriodUnit::Months)
      .def_rw("periods", &DateInterval::periods)
      .def_rw("unit", &DateInterval::unit)
      .def(nb::self == nb::self)
      .def("__repr__", [](const DateInterval &i) {
        return "DateInterval(periods=" + std::to_string(i.periods) + ", unit=PeriodUnit." +
               (i.unit == PeriodUnit::Days ? std::string{"DAYS"} : std::string{"MONTHS"}) + ")";
      });

  // Two fields rather than two booleans: upstream's `TStubMethod` was
  // `stubAtEnd` and `longStub`, which could be swapped at a call site without
  // the compiler noticing. Keeping the distinction in Python preserves what
  // the port fixed.
  nb::class_<StubMethod>(m, "StubMethod", "Where the odd period sits, and how long it is.")
      .def(
          "__init__",
          [](StubMethod *self, StubPosition position, StubLength length) {
            new (self) StubMethod{position, length};
          },
          nb::kw_only(), nb::arg("position") = StubPosition::Front,
          nb::arg("length") = StubLength::Short)
      .def_rw("position", &StubMethod::position)
      .def_rw("length", &StubMethod::length)
      .def(nb::self == nb::self)
      .def("__repr__", [](const StubMethod &s) {
        return std::string{"StubMethod(position=StubPosition."} +
               (s.position == StubPosition::Front ? "FRONT" : "BACK") + ", length=StubLength." +
               (s.length == StubLength::Short ? "SHORT" : "LONG") + ")";
      });

  nb::class_<py::Valuation>(m, "Valuation", "When the valuation happens.")
      .def(
          "__init__",
          [](py::Valuation *self, Date today, Date cash_settle, Date step_in) {
            new (self) py::Valuation{today, cash_settle, step_in};
          },
          nb::kw_only(), nb::arg("today"), nb::arg("cash_settle"), nb::arg("step_in"))
      .def_rw("today", &py::Valuation::today)
      .def_rw("cash_settle", &py::Valuation::cash_settle)
      .def_rw("step_in", &py::Valuation::step_in)
      .def("__repr__", [](const py::Valuation &v) {
        return "Valuation(today=" + iso(v.today) + ", cash_settle=" + iso(v.cash_settle) +
               ", step_in=" + iso(v.step_in) + ")";
      });

  // Six parameters, all defaulted: the single named exemption to the cap.
  nb::class_<py::Conventions>(m, "Conventions", "What a desk sets once.")
      .def(
          "__init__",
          [](py::Conventions *self, bool pay_accrual_on_default, DateInterval coupon_interval,
             StubMethod stub, DayCount day_count, BadDayConv bad_day,
             std::vector<Calendar> holidays) {
            new (self)
                py::Conventions{pay_accrual_on_default, coupon_interval, stub, day_count, bad_day,
                                std::move(holidays)};
          },
          nb::kw_only(), nb::arg("pay_accrual_on_default") = true,
          nb::arg("coupon_interval") = DateInterval{3, PeriodUnit::Months},
          nb::arg("stub") = StubMethod{}, nb::arg("day_count") = DayCount::Act360,
          nb::arg("bad_day") = BadDayConv::None,
          nb::arg("holidays") = std::vector<Calendar>{Calendar::none()})
      .def_rw("pay_accrual_on_default", &py::Conventions::pay_accrual_on_default)
      .def_rw("coupon_interval", &py::Conventions::coupon_interval)
      .def_rw("stub", &py::Conventions::stub)
      .def_rw("day_count", &py::Conventions::day_count)
      .def_rw("bad_day", &py::Conventions::bad_day)
      .def_rw("holidays", &py::Conventions::holidays)
      .def("__repr__", [](const py::Conventions &c) {
        return "Conventions(coupon_interval=" + std::to_string(c.coupon_interval.periods) +
               ", day_count=" + std::string{to_string(c.day_count)} +
               ", holidays=" + std::to_string(c.holidays.size()) + ")";
      });

  nb::class_<py::MarketData>(m, "MarketData", "What the market says today.")
      .def(
          "__init__",
          [](py::MarketData *self, ZeroCurve discount, ZeroCurve spread, double recovery) {
            new (self) py::MarketData{std::move(discount), std::move(spread), recovery};
          },
          nb::kw_only(), nb::arg("discount"), nb::arg("spread"), nb::arg("recovery"))
      .def_rw("discount", &py::MarketData::discount)
      .def_rw("spread", &py::MarketData::spread)
      .def_rw("recovery", &py::MarketData::recovery)
      .def("__repr__", [](const py::MarketData &d) {
        return "MarketData(recovery=" + std::to_string(d.recovery) + ")";
      });

  nb::class_<py::FlatMarket>(m, "FlatMarket", "A discount curve and a flat quote.")
      .def(
          "__init__",
          [](py::FlatMarket *self, ZeroCurve discount, double quote, double recovery) {
            new (self) py::FlatMarket{std::move(discount), quote, recovery};
          },
          nb::kw_only(), nb::arg("discount"), nb::arg("quote"), nb::arg("recovery"))
      .def_rw("discount", &py::FlatMarket::discount)
      .def_rw("quote", &py::FlatMarket::quote)
      .def_rw("recovery", &py::FlatMarket::recovery)
      .def("__repr__", [](const py::FlatMarket &f) {
        return "FlatMarket(quote=" + std::to_string(f.quote) +
               ", recovery=" + std::to_string(f.recovery) + ")";
      });
}
