#pragma once

#include <string_view>

namespace cdsmodel {

/// Version of this port.
[[nodiscard]] std::string_view version() noexcept;

/// Upstream ISDA CDS Standard Model version this port is derived from.
[[nodiscard]] std::string_view upstream_version() noexcept;

/// Attribution required by section 4(b) of the ISDA CDS Standard Model Public
/// License. Any application built on this library must display it.
[[nodiscard]] std::string_view attribution_notice() noexcept;

} // namespace cdsmodel
