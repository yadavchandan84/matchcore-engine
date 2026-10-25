#include "matchcore/book/order_book.hpp"

#include <algorithm>

namespace matchcore {

namespace {

// Ladder ordering is worst -> best. `worse(side, a, b)` is true if price `a`
// sorts before `b`, i.e. `a` is a less aggressive price than `b` for `side`.
constexpr bool worse(Side side, Price a, Price b) noexcept { return is_better(side, b, a); }

// Position of `price` in `ladder`, or ladder.end() if absent. Checks back()
// first because almost all traffic hits the top of the book.
template <class Ladder>
auto find_entry(Ladder& ladder, Side side, Price price) noexcept {
  if (!ladder.empty() && ladder.back().price == price) {
    return ladder.end() - 1;
  }
  const auto it = std::lower_bound(
      ladder.begin(), ladder.end(), price,
      [side](const auto& entry, Price p) { return worse(side, entry.price, p); });
  return (it != ladder.end() && it->price == price) ? it : ladder.end();
}

}  // namespace

OrderBook::OrderBook(const Config& config)
    : orders_(config.max_orders),
      levels_(config.max_levels),
      ids_(config.max_orders),
      symbols_(kSymbolSlots) {
  if (config.levels_per_side != 0) {
    const std::size_t n = std::min(config.symbols_reserved + 1, kSymbolSlots);
    for (std::size_t i = 0; i < n; ++i) {
      for (Ladder& ladder : symbols_[i].sides) {
        ladder.reserve(config.levels_per_side);
      }
    }
  }
}

PoolIndex OrderBook::find_or_create_level(StockLocate locate, Side side, Price price) noexcept {
  Ladder& ladder = ladder_for(locate, side);

  // Fast path: same as, or better than, the current best -> at/after back().
  auto pos = ladder.end();
  if (!ladder.empty()) {
    const Price best = ladder.back().price;
    if (best == price) {
      return ladder.back().level;
    }
    if (worse(side, price, best)) {
      pos = std::lower_bound(
          ladder.begin(), ladder.end(), price,
          [side](const LadderEntry& e, Price p) { return worse(side, e.price, p); });
      if (pos->price == price) {
        return pos->level;
      }
    }
  }

  const PoolIndex idx = levels_.allocate();
  if (idx == kNullIndex) {
    return kNullIndex;
  }
  levels_[idx] = Level{price, 0, 0, kNullIndex, kNullIndex};
  ladder.insert(pos, LadderEntry{price, idx});
  return idx;
}

BookResult OrderBook::add(StockLocate locate, OrderId id, Side side, Price price,
                          Quantity shares) noexcept {
  if (shares == 0) {
    return BookResult::kInvalidQuantity;
  }
  if (ids_.find(id) != kNullIndex) {
    return BookResult::kDuplicateOrderId;
  }
  const PoolIndex idx = orders_.allocate();
  if (idx == kNullIndex) {
    return BookResult::kCapacityExceeded;
  }
  const PoolIndex lvl_idx = find_or_create_level(locate, side, price);
  if (lvl_idx == kNullIndex) {
    orders_.release(idx);
    return BookResult::kCapacityExceeded;
  }
  // Cannot fail: the id is absent and the map holds as many entries as the
  // order pool, which just had a free slot.
  [[maybe_unused]] const bool inserted = ids_.insert(id, idx);

  Level& lvl = levels_[lvl_idx];
  orders_[idx] = Order{id, price, shares, lvl_idx, lvl.tail, kNullIndex, locate, side};
  if (lvl.tail != kNullIndex) {
    orders_[lvl.tail].next = idx;
  } else {
    lvl.head = idx;
  }
  lvl.tail = idx;
  ++lvl.order_count;
  lvl.total_shares += shares;
  return BookResult::kOk;
}

void OrderBook::unlink_order(PoolIndex idx) noexcept {
  const Order& o = orders_[idx];
  Level& lvl = levels_[o.level];

  if (o.prev != kNullIndex) {
    orders_[o.prev].next = o.next;
  } else {
    lvl.head = o.next;
  }
  if (o.next != kNullIndex) {
    orders_[o.next].prev = o.prev;
  } else {
    lvl.tail = o.prev;
  }
  lvl.total_shares -= o.shares;
  --lvl.order_count;

  if (lvl.order_count == 0) {
    Ladder& ladder = ladder_for(o.locate, o.side);
    ladder.erase(find_entry(ladder, o.side, o.price));
    levels_.release(o.level);
  }
  ids_.erase(o.id);
  orders_.release(idx);
}

BookResult OrderBook::reduce(OrderId id, Quantity shares) noexcept {
  const PoolIndex idx = ids_.find(id);
  if (idx == kNullIndex) {
    return BookResult::kUnknownOrderId;
  }
  Order& o = orders_[idx];
  if (shares == 0 || shares > o.shares) {
    return BookResult::kInvalidQuantity;
  }
  if (shares == o.shares) {
    unlink_order(idx);
    return BookResult::kOk;
  }
  o.shares -= shares;
  levels_[o.level].total_shares -= shares;
  return BookResult::kOk;
}

BookResult OrderBook::remove(OrderId id) noexcept {
  const PoolIndex idx = ids_.find(id);
  if (idx == kNullIndex) {
    return BookResult::kUnknownOrderId;
  }
  unlink_order(idx);
  return BookResult::kOk;
}

BookResult OrderBook::replace(OrderId old_id, OrderId new_id, Price price,
                              Quantity shares) noexcept {
  const PoolIndex idx = ids_.find(old_id);
  if (idx == kNullIndex) {
    return BookResult::kUnknownOrderId;
  }
  if (shares == 0) {
    return BookResult::kInvalidQuantity;
  }
  if (new_id != old_id && ids_.find(new_id) != kNullIndex) {
    return BookResult::kDuplicateOrderId;
  }
  const Order old = orders_[idx];

  // Rejected operations must leave the book untouched, so make sure the add
  // below cannot run out of level capacity after the old order is gone.
  if (levels_.in_use() == levels_.capacity()) {
    Ladder& ladder = ladder_for(old.locate, old.side);
    const bool level_exists = find_entry(ladder, old.side, price) != ladder.end();
    const bool frees_level = levels_[old.level].order_count == 1;
    if (!level_exists && !frees_level) {
      return BookResult::kCapacityExceeded;
    }
  }

  // ITCH semantics: the replacement keeps the side and loses time priority.
  unlink_order(idx);
  return add(old.locate, new_id, old.side, price, shares);
}

std::optional<OrderInfo> OrderBook::find(OrderId id) const noexcept {
  const PoolIndex idx = ids_.find(id);
  if (idx == kNullIndex) {
    return std::nullopt;
  }
  return info_of(orders_[idx]);
}

std::optional<LevelInfo> OrderBook::best(StockLocate locate, Side side) const noexcept {
  const Ladder& ladder = ladder_for(locate, side);
  if (ladder.empty()) {
    return std::nullopt;
  }
  const Level& lvl = levels_[ladder.back().level];
  return LevelInfo{lvl.price, lvl.total_shares, lvl.order_count};
}

std::optional<OrderInfo> OrderBook::front(StockLocate locate, Side side) const noexcept {
  const Ladder& ladder = ladder_for(locate, side);
  if (ladder.empty()) {
    return std::nullopt;
  }
  return info_of(orders_[levels_[ladder.back().level].head]);
}

}  // namespace matchcore
