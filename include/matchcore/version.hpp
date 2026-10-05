#ifndef MATCHCORE_VERSION_HPP
#define MATCHCORE_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace matchcore {

inline constexpr std::uint32_t kVersionMajor = 0;
inline constexpr std::uint32_t kVersionMinor = 1;
inline constexpr std::uint32_t kVersionPatch = 0;

/// Returns the semantic version string, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

}  // namespace matchcore

#endif  // MATCHCORE_VERSION_HPP
