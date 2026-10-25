#ifndef MATCHCORE_BOOK_BOOK_TYPES_HPP
#define MATCHCORE_BOOK_BOOK_TYPES_HPP

#include <cstdint>

#include "matchcore/types.hpp"

// Value types shared by every book implementation (reference and optimized).
// Keeping the public surface identical is what lets the two be compared
// operation-for-operation in the differential tests.
namespace matchcore {

/// Aggregated view of one price level.
struct LevelInfo {
  Price price;
  std::uint64_t total_shares{0};  // sum over resting orders (can exceed 2^32)
  std::uint32_t order_count{0};

  friend constexpr bool operator==(const LevelInfo&, const LevelInfo&) noexcept = default;
};

/// A single resting order as seen from outside the book.
struct OrderInfo {
  OrderId id{0};
  StockLocate locate{0};
  Side side{Side::kBuy};
  Price price;
  Quantity shares{0};

  friend constexpr bool operator==(const OrderInfo&, const OrderInfo&) noexcept = default;
};

/// Outcome of a book mutation. Every rejected operation leaves the book
/// completely unchanged.
enum class BookResult : std::uint8_t {
  kOk,
  kDuplicateOrderId,  ///< add/replace: the new id is already resting
  kUnknownOrderId,    ///< execute/cancel/delete/replace: id is not resting
  kInvalidQuantity,   ///< zero shares, or a reduction larger than what rests
  kCapacityExceeded,  ///< optimized book only: preallocated pool is full
};

/// Opposite side.
[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::kBuy ? Side::kSell : Side::kBuy;
}

/// True if `a` is a better (more aggressive) price than `b` for `side`.
[[nodiscard]] constexpr bool is_better(Side side, Price a, Price b) noexcept {
  return side == Side::kBuy ? a > b : a < b;
}

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_BOOK_TYPES_HPP
