#ifndef MATCHCORE_BOOK_ITCH_BOOK_BUILDER_HPP
#define MATCHCORE_BOOK_ITCH_BOOK_BUILDER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "matchcore/book/book_types.hpp"
#include "matchcore/itch/messages.hpp"

namespace matchcore {

/// ITCH parser handler that rebuilds a full-depth book from order flow.
///
/// Works with any type exposing the common book API (ReferenceBook,
/// OrderBook), which is how the same real-file replay drives both books.
///
/// Message mapping (ITCH 5.0 sections 1.3 / 1.4):
///   A, F  add order           -> book.add
///   E, C  executed (partial)  -> book.execute    (C's price is the print, not the book)
///   X     partial cancel      -> book.cancel
///   D     delete              -> book.remove
///   U     replace             -> book.replace    (keeps side, loses priority)
///   P, Q, B                   -> no book effect  (non-displayed / cross / break)
template <class Book>
class ItchBookBuilder {
 public:
  explicit ItchBookBuilder(Book& book) noexcept : book_(book) {}

  void on(const itch::AddOrder& m) noexcept {
    record(book_.add(m.header.stock_locate, m.order_reference_number, m.side, m.price, m.shares));
  }
  void on(const itch::AddOrderMpid& m) noexcept {
    record(book_.add(m.header.stock_locate, m.order_reference_number, m.side, m.price, m.shares));
  }
  void on(const itch::OrderExecuted& m) noexcept {
    record(book_.execute(m.order_reference_number, m.executed_shares));
  }
  void on(const itch::OrderExecutedWithPrice& m) noexcept {
    record(book_.execute(m.order_reference_number, m.executed_shares));
  }
  void on(const itch::OrderCancel& m) noexcept {
    record(book_.cancel(m.order_reference_number, m.cancelled_shares));
  }
  void on(const itch::OrderDelete& m) noexcept { record(book_.remove(m.order_reference_number)); }
  void on(const itch::OrderReplace& m) noexcept {
    record(book_.replace(m.original_order_reference_number, m.new_order_reference_number, m.price,
                         m.shares));
  }

  /// Count of each BookResult seen, indexed by the enum value.
  [[nodiscard]] const std::array<std::uint64_t, 5>& results() const noexcept { return results_; }
  [[nodiscard]] std::uint64_t applied() const noexcept { return results_[0]; }
  [[nodiscard]] std::uint64_t rejected() const noexcept {
    return results_[1] + results_[2] + results_[3] + results_[4];
  }

 private:
  void record(BookResult r) noexcept { ++results_[static_cast<std::size_t>(r)]; }

  Book& book_;
  std::array<std::uint64_t, 5> results_{};
};

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_ITCH_BOOK_BUILDER_HPP
