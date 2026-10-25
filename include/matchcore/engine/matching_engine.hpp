#ifndef MATCHCORE_ENGINE_MATCHING_ENGINE_HPP
#define MATCHCORE_ENGINE_MATCHING_ENGINE_HPP

#include <algorithm>
#include <cstdint>
#include <optional>

#include "matchcore/book/book_types.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/types.hpp"

namespace matchcore {

enum class OrderType : std::uint8_t {
  kLimit,   ///< Executes at the limit price or better.
  kMarket,  ///< Executes at any price; never rests.
};

enum class TimeInForce : std::uint8_t {
  kGtc,  ///< Good-till-cancel: unfilled remainder rests on the book.
  kIoc,  ///< Immediate-or-cancel: fill what is possible, cancel the rest.
  kFok,  ///< Fill-or-kill: fill completely at once, or do nothing.
};

struct NewOrder {
  StockLocate locate{0};
  OrderId id{0};
  Side side{Side::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce tif{TimeInForce::kGtc};
  Price price;  ///< ignored for market orders
  Quantity quantity{0};
};

/// One execution between an incoming (taker) order and a resting (maker) one.
/// Trades print at the maker's price, as in every price-time priority market.
struct Fill {
  MatchNumber match_number{0};
  StockLocate locate{0};
  OrderId taker_id{0};
  OrderId maker_id{0};
  Side taker_side{Side::kBuy};
  Price price;
  Quantity quantity{0};
  Quantity maker_remaining{0};  ///< maker shares left after this fill (0 = gone)

  friend constexpr bool operator==(const Fill&, const Fill&) noexcept = default;
};

enum class OrderStatus : std::uint8_t {
  kResting,    ///< (Partially filled and) now on the book.
  kFilled,     ///< Completely filled.
  kCancelled,  ///< Remainder cancelled: IOC/market leftover, or explicit cancel.
  kRejected,   ///< Not accepted; the book is unchanged.
};

enum class RejectReason : std::uint8_t {
  kNone,
  kDuplicateOrderId,
  kUnknownOrderId,
  kInvalidQuantity,
  kInvalidPrice,
  kInvalidTimeInForce,  ///< market + GTC (a market order cannot rest)
  kFokNotFillable,
  kCapacityExceeded,
};

struct ExecReport {
  OrderId id{0};
  OrderStatus status{OrderStatus::kRejected};
  RejectReason reason{RejectReason::kNone};
  Quantity filled{0};     ///< shares executed by this request
  Quantity remaining{0};  ///< shares left resting after this request

  friend constexpr bool operator==(const ExecReport&, const ExecReport&) noexcept = default;
};

/// Sink that ignores fills.
struct NullFillSink {
  void on_fill(const Fill& /*fill*/) noexcept {}
};

/// Price-time priority matching engine over an OrderBook.
///
/// - Incoming orders match against the opposite side best price first, and
///   within a price level in arrival order (FIFO).
/// - Trades execute at the resting order's price.
/// - Fills are delivered synchronously to a caller-supplied sink, statically
///   dispatched (template parameter), in execution order.
/// - No heap allocation: all state lives in the preallocated OrderBook.
///
/// Modify semantics (the common exchange convention):
///   - quantity decrease at the same price keeps time priority;
///   - any price change or quantity increase loses priority, and the modified
///     order is re-submitted as a limit GTC order, so it can match if it now
///     crosses the spread.
///
/// Out of scope: self-trade prevention, stop orders, auctions, hidden or
/// iceberg quantity.
class MatchingEngine {
 public:
  using Config = OrderBook::Config;

  MatchingEngine() : MatchingEngine(Config{}) {}
  explicit MatchingEngine(const Config& config) : book_(config) {}

  template <class Sink>
  ExecReport submit(const NewOrder& o, Sink& sink) noexcept {
    if (const auto r = validate(o); r != RejectReason::kNone) {
      return reject(o.id, r);
    }
    if (o.tif == TimeInForce::kFok && available(o) < o.quantity) {
      return reject(o.id, RejectReason::kFokNotFillable);
    }

    const Quantity filled = match(o, o.quantity, sink);
    const Quantity left = o.quantity - filled;
    if (left == 0) {
      return {o.id, OrderStatus::kFilled, RejectReason::kNone, filled, 0};
    }
    if (o.tif != TimeInForce::kGtc) {
      return {o.id, OrderStatus::kCancelled, RejectReason::kNone, filled, 0};
    }
    if (book_.add(o.locate, o.id, o.side, o.price, left) != BookResult::kOk) {
      // Only capacity can fail here: id and quantity were validated.
      return {o.id, OrderStatus::kCancelled, RejectReason::kCapacityExceeded, filled, 0};
    }
    return {o.id, OrderStatus::kResting, RejectReason::kNone, filled, left};
  }

  ExecReport submit(const NewOrder& o) noexcept {
    NullFillSink sink;
    return submit(o, sink);
  }

  /// Cancel a resting order entirely.
  ExecReport cancel(OrderId id) noexcept {
    if (book_.remove(id) != BookResult::kOk) {
      return reject(id, RejectReason::kUnknownOrderId);
    }
    return {id, OrderStatus::kCancelled, RejectReason::kNone, 0, 0};
  }

  /// Change price and/or quantity of a resting order (see class comment).
  template <class Sink>
  ExecReport modify(OrderId id, Price new_price, Quantity new_quantity, Sink& sink) noexcept {
    const auto cur = book_.find(id);
    if (!cur) {
      return reject(id, RejectReason::kUnknownOrderId);
    }
    if (new_quantity == 0) {
      return reject(id, RejectReason::kInvalidQuantity);
    }
    if (new_price.raw() == 0) {
      return reject(id, RejectReason::kInvalidPrice);
    }
    if (new_price == cur->price && new_quantity <= cur->shares) {
      if (new_quantity < cur->shares) {
        (void)book_.cancel(id, cur->shares - new_quantity);  // keeps priority
      }
      return {id, OrderStatus::kResting, RejectReason::kNone, 0, new_quantity};
    }
    (void)book_.remove(id);
    return submit(NewOrder{cur->locate, id, cur->side, OrderType::kLimit, TimeInForce::kGtc,
                           new_price, new_quantity},
                  sink);
  }

  ExecReport modify(OrderId id, Price new_price, Quantity new_quantity) noexcept {
    NullFillSink sink;
    return modify(id, new_price, new_quantity, sink);
  }

  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] MatchNumber last_match_number() const noexcept { return next_match_ - 1; }

 private:
  [[nodiscard]] static ExecReport reject(OrderId id, RejectReason r) noexcept {
    return {id, OrderStatus::kRejected, r, 0, 0};
  }

  [[nodiscard]] RejectReason validate(const NewOrder& o) const noexcept {
    if (o.quantity == 0) {
      return RejectReason::kInvalidQuantity;
    }
    if (o.type == OrderType::kLimit && o.price.raw() == 0) {
      return RejectReason::kInvalidPrice;
    }
    if (o.type == OrderType::kMarket && o.tif == TimeInForce::kGtc) {
      return RejectReason::kInvalidTimeInForce;
    }
    if (book_.find(o.id)) {
      return RejectReason::kDuplicateOrderId;
    }
    return RejectReason::kNone;
  }

  /// True if an incoming order with this limit may trade at `level`.
  [[nodiscard]] static bool crosses(const NewOrder& o, Price level) noexcept {
    if (o.type == OrderType::kMarket) {
      return true;
    }
    return o.side == Side::kBuy ? level <= o.price : level >= o.price;
  }

  /// Shares on the opposite side that `o` could trade against, capped at
  /// o.quantity (the scan stops as soon as the answer is known).
  [[nodiscard]] Quantity available(const NewOrder& o) const noexcept {
    std::uint64_t total = 0;
    book_.for_each_level(o.locate, opposite(o.side), [&](const LevelInfo& lvl) {
      if (!crosses(o, lvl.price)) {
        return false;
      }
      total += lvl.total_shares;
      return total < o.quantity;
    });
    return static_cast<Quantity>(std::min<std::uint64_t>(total, o.quantity));
  }

  template <class Sink>
  Quantity match(const NewOrder& o, Quantity quantity, Sink& sink) noexcept {
    const Side maker_side = opposite(o.side);
    Quantity left = quantity;
    while (left > 0) {
      const auto best = book_.best(o.locate, maker_side);
      if (!best || !crosses(o, best->price)) {
        break;
      }
      const auto maker = *book_.front(o.locate, maker_side);
      const Quantity q = std::min(left, maker.shares);
      (void)book_.execute(maker.id, q);
      left -= q;
      sink.on_fill(Fill{next_match_++, o.locate, o.id, maker.id, o.side, maker.price, q,
                        maker.shares - q});
    }
    return quantity - left;
  }

  OrderBook book_;
  MatchNumber next_match_{1};
};

}  // namespace matchcore

#endif  // MATCHCORE_ENGINE_MATCHING_ENGINE_HPP
