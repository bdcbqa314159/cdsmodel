#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <cdsmodel/version.hpp>

#include <string>

#include "date_caster.hpp"

namespace nb = nanobind;

NB_MODULE(_core, m) {
  m.doc() = "The ISDA CDS Standard Model, ported to C++20.";

  // D12: one version, read from CMakeLists through the C++ library, never
  // retyped here. `pyproject.toml` reads the same line for the wheel metadata.
  m.attr("__version__") = std::string{cdsmodel::version()};
  m.attr("__upstream_version__") = std::string{cdsmodel::upstream_version()};
}
