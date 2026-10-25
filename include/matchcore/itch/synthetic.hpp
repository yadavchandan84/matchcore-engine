#ifndef MATCHCORE_ITCH_SYNTHETIC_HPP
#define MATCHCORE_ITCH_SYNTHETIC_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace matchcore::itch {

/// Parameters for a synthetic ITCH 5.0 session.
///
/// The mix defaults approximate the shape of a real NASDAQ day, where adds and
/// deletes dominate and most activity sits near the inside of the book. Every
/// generated execute/cancel/delete/replace refers to a live order with a valid
/// quantity, so a correct book applies 100% of the events.
struct SyntheticConfig {
  std::uint64_t seed = 1;
  std::size_t messages = 1'000'000;  ///< order-flow messages (excludes S/R preamble)
  std::uint16_t symbols = 100;       ///< locates 1..symbols
  std::uint32_t base_price = 1'000'000;  ///< 100.0000
  std::uint32_t tick = 100;              ///< 0.0100
  std::uint32_t price_band_ticks = 50;   ///< adds land within +/- band of mid
  std::size_t target_resting = 20'000;   ///< steer adds/removes toward this depth

  // Relative weights of each operation once the book is near target depth.
  unsigned w_add = 45;
  unsigned w_delete = 30;
  unsigned w_execute = 8;
  unsigned w_cancel = 7;
  unsigned w_replace = 10;
};

/// Generate a complete length-prefixed session buffer: System Event 'O',
/// one Stock Directory per symbol, the order flow, then System Event 'C'.
[[nodiscard]] std::vector<std::byte> generate_session(const SyntheticConfig& config);

}  // namespace matchcore::itch

#endif  // MATCHCORE_ITCH_SYNTHETIC_HPP
