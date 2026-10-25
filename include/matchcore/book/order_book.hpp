#ifndef MATCHCORE_BOOK_ORDER_BOOK_HPP
#define MATCHCORE_BOOK_ORDER_BOOK_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "matchcore/book/book_types.hpp"
#include "matchcore/book/index_pool.hpp"
#include "matchcore/book/order_id_map.hpp"
#include "matchcore/types.hpp"

namespace matchcore {

/// Optimized multi-symbol limit order book.
///
/// Layout:
///   - Orders live in a preallocated IndexPool. Each order node carries 32-bit
///     prev/next links forming an intrusive FIFO per price level, and the index
///     of its level, so execute/cancel/delete never search.
///   - Price levels live in a second IndexPool (head/tail/total/count).
///   - Each symbol side keeps a flat, sorted std::vector of {price, level}
///     ordered worst -> best, so the best price is always back(). Real order
///     flow clusters at the top of the book, so inserting or removing a level
///     near back() shifts only a few 8-byte entries; the whole ladder is
///     contiguous and cache-friendly, unlike a node-per-level std::map.
///   - Symbols are indexed directly by Stock Locate: no hashing.
///   - OrderId lookup is the open-addressing OrderIdMap.
///
/// Complexity: add O(log L) to find/create the level (L = levels on that
/// side), execute/cancel/delete O(1) unless the level empties (then O(log L)),
/// best/front O(1).
///
/// Allocation: the order pool, level pool and id map are sized once from
/// Config. The per-side level vectors reserve Config::levels_per_side; they
/// only grow if a side exceeds that depth, and never shrink, so after warm-up
/// the steady state performs no heap allocation (verified by alloc_test).
class OrderBook {
 public:
  struct Config {
    std::size_t max_orders = std::size_t{1} << 20U;  // concurrently resting orders
    std::size_t max_levels = std::size_t{1} << 18U;  // concurrently live levels (all symbols)
    std::size_t levels_per_side = 0;  // initial reserve per symbol side (0 = grow lazily)
    std::size_t symbols_reserved = 0;  // locates 1..N get the reserve up front
  };

  OrderBook() : OrderBook(Config{}) {}
  explicit OrderBook(const Config& config);

  BookResult add(StockLocate locate, OrderId id, Side side, Price price, Quantity shares) noexcept;
  BookResult execute(OrderId id, Quantity shares) noexcept { return reduce(id, shares); }
  BookResult cancel(OrderId id, Quantity shares) noexcept { return reduce(id, shares); }
  BookResult remove(OrderId id) noexcept;
  BookResult replace(OrderId old_id, OrderId new_id, Price price, Quantity shares) noexcept;

  [[nodiscard]] std::optional<OrderInfo> find(OrderId id) const noexcept;
  [[nodiscard]] std::optional<LevelInfo> best(StockLocate locate, Side side) const noexcept;
  [[nodiscard]] std::optional<OrderInfo> front(StockLocate locate, Side side) const noexcept;
  [[nodiscard]] std::size_t order_count() const noexcept { return orders_.in_use(); }
  [[nodiscard]] std::size_t level_count() const noexcept { return levels_.in_use(); }

  /// Visit levels best-first. Stop early by returning false from `fn`.
  template <class Fn>
  void for_each_level(StockLocate locate, Side side, Fn&& fn) const {
    const Ladder& ladder = ladder_for(locate, side);
    for (auto it = ladder.rbegin(); it != ladder.rend(); ++it) {
      const Level& lvl = levels_[it->level];
      if (!fn(LevelInfo{lvl.price, lvl.total_shares, lvl.order_count})) {
        return;
      }
    }
  }

  /// Visit the orders resting at one price level in time priority.
  template <class Fn>
  void for_each_order_at(StockLocate locate, Side side, Price price, Fn&& fn) const {
    const Ladder& ladder = ladder_for(locate, side);
    for (const LadderEntry& e : ladder) {
      if (e.price == price) {
        for (PoolIndex o = levels_[e.level].head; o != kNullIndex; o = orders_[o].next) {
          fn(info_of(orders_[o]));
        }
        return;
      }
    }
  }

 private:
  struct Order {
    OrderId id;
    Price price;
    Quantity shares;
    PoolIndex level;
    PoolIndex prev;
    PoolIndex next;
    StockLocate locate;
    Side side;
  };

  struct Level {
    Price price;
    std::uint32_t order_count;
    std::uint64_t total_shares;
    PoolIndex head;
    PoolIndex tail;
  };

  struct LadderEntry {
    Price price;
    PoolIndex level;
  };
  using Ladder = std::vector<LadderEntry>;  // worst -> best; best is back()

  struct SymbolBook {
    Ladder sides[2];  // NOLINT(*-avoid-c-arrays): indexed by Side
  };

  static constexpr std::size_t kSymbolSlots = std::size_t{1} << 16U;  // every StockLocate

  [[nodiscard]] static std::size_t side_index(Side s) noexcept { return s == Side::kBuy ? 0 : 1; }
  [[nodiscard]] Ladder& ladder_for(StockLocate locate, Side side) noexcept {
    return symbols_[locate].sides[side_index(side)];
  }
  [[nodiscard]] const Ladder& ladder_for(StockLocate locate, Side side) const noexcept {
    return symbols_[locate].sides[side_index(side)];
  }
  [[nodiscard]] static OrderInfo info_of(const Order& o) noexcept {
    return {o.id, o.locate, o.side, o.price, o.shares};
  }

  BookResult reduce(OrderId id, Quantity shares) noexcept;
  PoolIndex find_or_create_level(StockLocate locate, Side side, Price price) noexcept;
  void unlink_order(PoolIndex idx) noexcept;

  IndexPool<Order> orders_;
  IndexPool<Level> levels_;
  OrderIdMap ids_;
  std::vector<SymbolBook> symbols_;
};

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_ORDER_BOOK_HPP
