#include "matchcore/version.hpp"

namespace matchcore {

std::string_view version() noexcept {
  // Kept in sync with the version constants in version.hpp. A single string
  // literal avoids any runtime formatting / allocation.
  return "0.1.0";
}

}  // namespace matchcore
