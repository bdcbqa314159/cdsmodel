#include <cdsmodel/version.hpp>

namespace cdsmodel {

std::string_view version() noexcept { return "0.1.0"; }

std::string_view upstream_version() noexcept { return "1.8.3"; }

std::string_view attribution_notice() noexcept {
  return "This application is based on the ISDA CDS Standard Model "
         "(version 1.8.3), developed and supported in collaboration with "
         "Markit Group Ltd.";
}

} // namespace cdsmodel
