#include <nanobind/nanobind.h>
#include <nanobind/operators.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cdsmodel/calendar.hpp>
#include <cdsmodel/ir_zero_curve.hpp>
#include <cdsmodel/stub.hpp>
#include <cdsmodel/swap_bootstrap.hpp>
#include <cdsmodel/zero_curve_builder.hpp>

#include <string>
#include <vector>

#include "date_caster.hpp"
#include "errors.hpp"

namespace nb = nanobind;

void bind_instruments(nb::module_ &m) {
  using namespace cdsmodel;

  nb::enum_<InstrumentKind>(m, "InstrumentKind", "What a curve benchmark is.")
      .value("MONEY_MARKET", InstrumentKind::MoneyMarket)
      .value("SWAP", InstrumentKind::Swap);

  nb::enum_<InterpType>(m, "InterpType", "How the curve interpolates between points.")
      .value("LINEAR", InterpType::Linear)
      .value("FLAT_FORWARDS", InterpType::FlatForwards);

  nb::enum_<StubPosDefault>(m, "StubPosDefault", "Where a swap's odd period goes.")
      .value("AUTO", StubPosDefault::Auto)
      .value("FRONT", StubPosDefault::Front)
      .value("BACK", StubPosDefault::Back);

  // Upstream keeps these in three parallel arrays with one shared count --
  // the shape that lets a caller pass mismatched lengths. One object per
  // quote makes that unrepresentable.
  nb::class_<InstrumentQuote>(m, "InstrumentQuote",
                              "One benchmark: what it is, when it matures, its quote.")
      .def(
          "__init__",
          [](InstrumentQuote *self, InstrumentKind kind, Date date, double rate) {
            new (self) InstrumentQuote{kind, date, rate};
          },
          nb::kw_only(), nb::arg("kind"), nb::arg("date"), nb::arg("rate"))
      .def_rw("kind", &InstrumentQuote::kind)
      .def_rw("date", &InstrumentQuote::date)
      .def_rw("rate", &InstrumentQuote::rate)
      .def(nb::self == nb::self)
      .def("__repr__", [](const InstrumentQuote &q) {
        const std::chrono::year_month_day d{q.date};
        char buf[96];
        std::snprintf(buf, sizeof buf,
                      "InstrumentQuote(kind=InstrumentKind.%s, date=%04d-%02d-%02d, rate=%g)",
                      q.kind == InstrumentKind::Swap ? "SWAP" : "MONEY_MARKET", int(d.year()),
                      unsigned(d.month()), unsigned(d.day()), q.rate);
        return std::string{buf};
      });

  // Seven fields, every one defaulted -- the second named exemption to the
  // cap, on the same grounds as `Conventions`: a caller writes
  // `SwapConventions()` and overrides what differs, so zero are required.
  // P15 enforces that an exempt callable has NO required parameters, which
  // is what stops the exemption list becoming a way to smuggle a wide
  // signature past the cap.
  nb::class_<SwapConventions>(m, "SwapConventions", "How the swap leg of a curve is quoted.")
      .def(
          "__init__",
          [](SwapConventions *self, long fixed_frequency, long float_frequency,
             DayCount fixed_day_count, DayCount float_day_count, InterpType interp,
             BadDayConv bad_day, StubPosDefault stub_position) {
            new (self)
                SwapConventions{fixed_frequency, float_frequency, fixed_day_count, float_day_count,
                                interp,          bad_day,         stub_position};
          },
          nb::kw_only(), nb::arg("fixed_frequency") = 2L, nb::arg("float_frequency") = 2L,
          nb::arg("fixed_day_count") = DayCount::Act360,
          nb::arg("float_day_count") = DayCount::Act360,
          nb::arg("interp") = InterpType::FlatForwards, nb::arg("bad_day") = BadDayConv::None,
          nb::arg("stub_position") = StubPosDefault::Auto)
      .def_rw("fixed_frequency", &SwapConventions::fixed_frequency)
      .def_rw("float_frequency", &SwapConventions::float_frequency)
      .def_rw("fixed_day_count", &SwapConventions::fixed_day_count)
      .def_rw("float_day_count", &SwapConventions::float_day_count)
      .def_rw("interp", &SwapConventions::interp)
      .def_rw("bad_day", &SwapConventions::bad_day)
      .def_rw("stub_position", &SwapConventions::stub_position)
      .def("__repr__", [](const SwapConventions &c) {
        return "SwapConventions(fixed_frequency=" + std::to_string(c.fixed_frequency) +
               ", float_frequency=" + std::to_string(c.float_frequency) + ")";
      });
}

void bind_curve_builder(nb::module_ &m) {
  using namespace cdsmodel;
  // Five parameters, the cap exactly. Bootstraps the money-market quotes into
  // the short end and the swaps on top, which is where the solver lives --
  // and so where `no_root` and `no_convergence` become reachable.
  m.attr("ZeroCurve").attr("build") = nb::cpp_function(
      [](Date value_date, std::vector<InstrumentQuote> instruments, DayCount money_market_day_count,
         const SwapConventions &conventions, const Calendar &calendar) {
        return cdsmodel::py::unwrap(build_ir_zero_curve(
            value_date, instruments, money_market_day_count, conventions, calendar));
      },
      nb::kw_only(), nb::arg("value_date"), nb::arg("instruments"),
      nb::arg("money_market_day_count") = DayCount::Act360,
      nb::arg("conventions") = SwapConventions{}, nb::arg("calendar") = Calendar::none(),
      nb::call_guard<nb::gil_scoped_release>(),
      "Bootstrap a zero curve from money-market and swap quotes.");
}
