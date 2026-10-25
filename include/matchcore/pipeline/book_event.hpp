#ifndef MATCHCORE_PIPELINE_BOOK_EVENT_HPP
#define MATCHCORE_PIPELINE_BOOK_EVENT_HPP

#include <cstdint>

#include "matchcore/book/book_types.hpp"
#include "matchcore/itch/messages.hpp"
#include "matchcore/types.hpp"

namespace matchcore {

/// Compact, fixed-size book mutation passed from the parser thread to the
/// engine thread. Only order-flow messages that change the book are
/// forwarded; everything else is filtered out on the parser side so the
/// ring carries no dead weight.
struct BookEvent {
  enum class Kind : std::uint8_t { kAdd, kExecute, kCancel, kDelete, kReplace, kEnd };

  OrderId id{0};
  OrderId new_id{0};  // replace only
  Price price;        // add / replace
  Quantity shares{0};
  StockLocate locate{0};
  Kind kind{Kind::kEnd};
  Side side{Side::kBuy};
};
static_assert(sizeof(BookEvent) == 32, "two events per cache line");

/// Apply one event to any book with the common API.
template <class Book>
BookResult apply(Book& book, const BookEvent& e) noexcept {
  switch (e.kind) {
    case BookEvent::Kind::kAdd: return book.add(e.locate, e.id, e.side, e.price, e.shares);
    case BookEvent::Kind::kExecute: return book.execute(e.id, e.shares);
    case BookEvent::Kind::kCancel: return book.cancel(e.id, e.shares);
    case BookEvent::Kind::kDelete: return book.remove(e.id);
    case BookEvent::Kind::kReplace: return book.replace(e.id, e.new_id, e.price, e.shares);
    case BookEvent::Kind::kEnd: break;
  }
  return BookResult::kOk;
}

/// ITCH handler that converts order-flow messages to BookEvents and hands
/// them to `emit` (a callable taking const BookEvent&).
template <class Emit>
class BookEventEncoder {
 public:
  explicit BookEventEncoder(Emit emit) noexcept : emit_(emit) {}

  using K = BookEvent::Kind;

  void on(const itch::AddOrder& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, m.price, m.shares, m.header.stock_locate, K::kAdd,
                    m.side});
  }
  void on(const itch::AddOrderMpid& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, m.price, m.shares, m.header.stock_locate, K::kAdd,
                    m.side});
  }
  void on(const itch::OrderExecuted& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, Price{}, m.executed_shares,
                    m.header.stock_locate, K::kExecute, Side::kBuy});
  }
  void on(const itch::OrderExecutedWithPrice& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, Price{}, m.executed_shares,
                    m.header.stock_locate, K::kExecute, Side::kBuy});
  }
  void on(const itch::OrderCancel& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, Price{}, m.cancelled_shares,
                    m.header.stock_locate, K::kCancel, Side::kBuy});
  }
  void on(const itch::OrderDelete& m) noexcept {
    emit_(BookEvent{m.order_reference_number, 0, Price{}, 0, m.header.stock_locate, K::kDelete,
                    Side::kBuy});
  }
  void on(const itch::OrderReplace& m) noexcept {
    emit_(BookEvent{m.original_order_reference_number, m.new_order_reference_number, m.price,
                    m.shares, m.header.stock_locate, K::kReplace, Side::kBuy});
  }

 private:
  Emit emit_;
};

}  // namespace matchcore

#endif  // MATCHCORE_PIPELINE_BOOK_EVENT_HPP
