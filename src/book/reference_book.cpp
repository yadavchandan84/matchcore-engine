#include "matchcore/book/reference_book.hpp"

namespace matchcore {

namespace {

// Look up / create the level for `price` in either side's map.
template <class Levels>
auto& level_for(Levels& levels, Price price) {
  return levels[price];
}

}  // namespace

BookResult ReferenceBook::add(StockLocate locate, OrderId id, Side side, Price price,
                              Quantity shares) {
  if (shares == 0) {
    return BookResult::kInvalidQuantity;
  }
  if (orders_.contains(id)) {
    return BookResult::kDuplicateOrderId;
  }
  Symbol& sym = symbols_[locate];
  auto& level = side == Side::kBuy ? level_for(sym.bids, price) : level_for(sym.asks, price);
  level.queue.push_back(id);
  level.total_shares += shares;
  orders_.emplace(id, Order{OrderInfo{id, locate, side, price, shares}, std::prev(level.queue.end())});
  return BookResult::kOk;
}

BookResult ReferenceBook::execute(OrderId id, Quantity shares) { return reduce(id, shares); }

BookResult ReferenceBook::cancel(OrderId id, Quantity shares) { return reduce(id, shares); }

BookResult ReferenceBook::reduce(OrderId id, Quantity shares) {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return BookResult::kUnknownOrderId;
  }
  OrderInfo& info = it->second.info;
  if (shares == 0 || shares > info.shares) {
    return BookResult::kInvalidQuantity;
  }
  if (shares == info.shares) {
    erase_order(it);
    return BookResult::kOk;
  }
  info.shares -= shares;
  Symbol& sym = symbols_[info.locate];
  auto& level =
      info.side == Side::kBuy ? sym.bids.at(info.price) : sym.asks.at(info.price);
  level.total_shares -= shares;
  return BookResult::kOk;
}

BookResult ReferenceBook::remove(OrderId id) {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return BookResult::kUnknownOrderId;
  }
  erase_order(it);
  return BookResult::kOk;
}

BookResult ReferenceBook::replace(OrderId old_id, OrderId new_id, Price price, Quantity shares) {
  const auto it = orders_.find(old_id);
  if (it == orders_.end()) {
    return BookResult::kUnknownOrderId;
  }
  if (shares == 0) {
    return BookResult::kInvalidQuantity;
  }
  if (new_id != old_id && orders_.contains(new_id)) {
    return BookResult::kDuplicateOrderId;
  }
  // ITCH semantics: the replacement loses time priority and keeps the side.
  const OrderInfo old = it->second.info;
  erase_order(it);
  return add(old.locate, new_id, old.side, price, shares);
}

void ReferenceBook::erase_order(std::unordered_map<OrderId, Order>::iterator it) {
  const OrderInfo& info = it->second.info;
  Symbol& sym = symbols_[info.locate];
  auto erase_from = [&](auto& levels) {
    const auto lvl = levels.find(info.price);
    lvl->second.queue.erase(it->second.pos);
    lvl->second.total_shares -= info.shares;
    if (lvl->second.queue.empty()) {
      levels.erase(lvl);
    }
  };
  if (info.side == Side::kBuy) {
    erase_from(sym.bids);
  } else {
    erase_from(sym.asks);
  }
  orders_.erase(it);
}

std::optional<OrderInfo> ReferenceBook::find(OrderId id) const {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return std::nullopt;
  }
  return it->second.info;
}

std::optional<LevelInfo> ReferenceBook::best(StockLocate locate, Side side) const {
  std::optional<LevelInfo> out;
  for_each_level(locate, side, [&](const LevelInfo& li) {
    out = li;
    return false;
  });
  return out;
}

std::optional<OrderInfo> ReferenceBook::front(StockLocate locate, Side side) const {
  const auto sym = symbols_.find(locate);
  if (sym == symbols_.end()) {
    return std::nullopt;
  }
  auto front_of = [&](const auto& levels) -> std::optional<OrderInfo> {
    if (levels.empty()) {
      return std::nullopt;
    }
    return orders_.at(levels.begin()->second.queue.front()).info;
  };
  return side == Side::kBuy ? front_of(sym->second.bids) : front_of(sym->second.asks);
}

}  // namespace matchcore
