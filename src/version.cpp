#include <cdsmodel/version.hpp>

namespace cdsmodel {

// CDSMODEL_VERSION comes from `project(cdsmodel VERSION ...)` via CMake, so
// the library, the Python extension and the wheel metadata all read one
// number. It was hardcoded until 0.1.1, which meant two sources that agreed
// only by coincidence -- the first version bump made them disagree.
#ifndef CDSMODEL_VERSION
#error "CDSMODEL_VERSION is not defined; the build must pass the project version in"
#endif

std::string_view version() noexcept { return CDSMODEL_VERSION; }

std::string_view upstream_version() noexcept { return "1.8.3"; }

std::string_view attribution_notice() noexcept {
  return "This application is based on the ISDA CDS Standard Model "
         "(version 1.8.3), developed and supported in collaboration with "
         "Markit Group Ltd.";
}

} // namespace cdsmodel
