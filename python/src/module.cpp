#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cdsmodel/version.hpp>

#include <string>

#include "date_caster.hpp"

void bind_types(nanobind::module_ &);
void bind_curves(nanobind::module_ &);
void bind_calendars(nanobind::module_ &);
void bind_blocks(nanobind::module_ &);
void bind_instruments(nanobind::module_ &);
void bind_curve_builder(nanobind::module_ &);
void bind_results(nanobind::module_ &);
void bind_pricing(nanobind::module_ &);
void bind_credit_curve(nanobind::module_ &);
namespace cdsmodel::py {
void register_errors(nanobind::module_ &);
}

namespace nb = nanobind;

NB_MODULE(_core, m) {
  m.doc() = "The ISDA CDS Standard Model, ported to C++20.";

  // D12: one version, read from CMakeLists through the C++ library, never
  // retyped here. `pyproject.toml` reads the same line for the wheel metadata.
  m.attr("__version__") = std::string{cdsmodel::version()};
  m.attr("__upstream_version__") = std::string{cdsmodel::upstream_version()};

  // Errors first: everything else may raise during binding.
  cdsmodel::py::register_errors(m);
  bind_types(m);
  bind_curves(m);
  bind_calendars(m);
  bind_blocks(m);
  bind_instruments(m);
  // After bind_curves: it attaches a static to the ZeroCurve type.
  bind_curve_builder(m);
  bind_results(m);
  bind_pricing(m);
  bind_credit_curve(m);
}
