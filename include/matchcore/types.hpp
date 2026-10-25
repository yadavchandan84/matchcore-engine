#ifndef MATCHCORE_TYPES_HPP
#define MATCHCORE_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Core domain types shared across the parser, order book and matching engine.
//
// These are intentionally small, trivially-copyable value types. The hot path
// never allocates, so everything here is a plain integer alias or a thin
// wrapper that the compiler can keep in registers.
namespace matchcore {

/// Day-unique order reference number (ITCH "Order Reference Number", 8 bytes).
using OrderId = std::uint64_t;

/// Nasdaq-assigned per-day instrument index (ITCH "Stock Locate", starts at 1).
/// Used directly as an array index into the per-symbol book table.
using StockLocate = std::uint16_t;

/// Share quantity. ITCH share fields are 4-byte unsigned integers.
using Quantity = std::uint32_t;

/// Nasdaq day-unique match number (8 bytes).
using MatchNumber = std::uint64_t;

/// Nanoseconds since midnight. ITCH carries this as a 6-byte big-endian value;
/// we widen it to 64 bits once decoded.
using Timestamp = std::uint64_t;

/// Side of an order.
enum class Side : std::uint8_t { kBuy, kSell };

/// Fixed-point price.
///
/// ITCH "Price (4)" fields are 4-byte unsigned integers with 4 implied decimal
/// places (e.g. the raw value 1234500 means a displayed price of 123.4500).
/// We keep the raw integer representation to avoid any floating-point in the
/// hot path, and only convert to a human-readable form at the edges.
class Price {
 public:
  /// Number of implied decimal places in an ITCH Price (4) field.
  static constexpr std::uint32_t kImpliedDecimals = 4;
  static constexpr std::int64_t kScale = 10000;  // 10^kImpliedDecimals

  constexpr Price() noexcept = default;
  explicit constexpr Price(std::uint32_t raw) noexcept : raw_(raw) {}

  /// The raw integer as it appears on the wire (price * 10^4).
  [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return raw_; }

  friend constexpr bool operator==(Price, Price) noexcept = default;
  friend constexpr auto operator<=>(Price, Price) noexcept = default;

 private:
  std::uint32_t raw_{0};
};

/// Fixed-width, space-padded ASCII field (ITCH "Alpha" type).
///
/// ITCH alpha fields are left-justified and padded on the right with spaces.
/// The raw bytes are stored as-is; `view()` trims the padding without copying.
template <std::size_t N>
struct FixedString {
  std::array<char, N> chars{};

  /// The field contents with trailing space padding removed.
  [[nodiscard]] constexpr std::string_view view() const noexcept {
    std::size_t len = N;
    while (len > 0 && chars[len - 1] == ' ') {
      --len;
    }
    return {chars.data(), len};
  }

  friend constexpr bool operator==(const FixedString&, const FixedString&) noexcept = default;
};

/// 8-character stock symbol (ITCH "Stock", Alpha 8).
using Symbol = FixedString<8>;

}  // namespace matchcore

#endif  // MATCHCORE_TYPES_HPP
