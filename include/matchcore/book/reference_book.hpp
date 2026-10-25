#ifndef MATCHCORE_BOOK_REFERENCE_BOOK_HPP
#define MATCHCORE_BOOK_REFERENCE_BOOK_HPP

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>

#include "matchcore/book/book_types.hpp"
#include "matchcore/types.hpp"

namespace matchcore {

/// Deliberately simple, obviously-correct multi-symbol limit order book.
///
/// Built only from standard containers with no cleverness: std::map for price
/// levels, std::list for FIFO queues, std::unordered_map for order lookup. It
/// allocates freely and is slow; its only job is to be the oracle that the
/// optimized OrderBook is checked against in the differential tests.
///
/// The public API is identical to OrderBook.
class ReferenceBook {
 public:
  BookResult add(StockLocate locate, OrderId id, Side side, Price price, Quantity shares);
  BookResult execute(OrderId id, Quantity shares);
  BookResult cancel(OrderId id, Quantity shares);
  BookResult remove(OrderId id);
  BookResult replace(OrderId old_id, OrderId new_id, Price price, Quantity shares);

  [[nodiscard]] std::optional<OrderInfo> find(OrderId id) const;
  [[nodiscard]] std::optional<LevelInfo> best(StockLocate locate, Side side) const;
  [[nodiscard]] std::optional<OrderInfo> front(StockLocate locate, Side side) const;
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }

  /// Visit levels best-first. Stop early by returning false from `fn`.
  template <class Fn>
  void for_each_level(StockLocate locate, Side side, Fn&& fn) const {
    const auto it = symbols_.find(locate);
    if (it == symbols_.end()) {
      return;
    }
    if (side == Side::kBuy) {
      visit(it->second.bids, fn);
    } else {
      visit(it->second.asks, fn);
    }
  }

 private:
  using Queue = std::list<OrderId>;
  struct Level {
    std::uint64_t total_shares{0};
    Queue queue;
  };
  using Bids = std::map<Price, Level, std::greater<>>;  // best (highest) first
  using Asks = std::map<Price, Level, std::less<>>;     // best (lowest) first

  struct Symbol {
    Bids bids;
    Asks asks;
  };

  struct Order {
    OrderInfo info;
    Queue::iterator pos;
  };

  template <class Levels, class Fn>
  static void visit(const Levels& levels, Fn& fn) {
    for (const auto& [price, level] : levels) {
      const LevelInfo li{price, level.total_shares, static_cast<std::uint32_t>(level.queue.size())};
      if (!fn(li)) {
        return;
      }
    }
  }

  BookResult reduce(OrderId id, Quantity shares);
  void erase_order(std::unordered_map<OrderId, Order>::iterator it);

  std::unordered_map<StockLocate, Symbol> symbols_;
  std::unordered_map<OrderId, Order> orders_;
};

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_REFERENCE_BOOK_HPP
