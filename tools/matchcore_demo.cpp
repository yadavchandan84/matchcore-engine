// matchcore_demo: a guided tour of what MatchCore does, printed to the terminal.
//
//   1. Rebuild order books from an ITCH 5.0 stream and print one symbol's depth.
//   2. Run the matching engine through a few scenarios and print the fills.
//
// Usage: matchcore_demo

#include <cstdint>
#include <cstdio>
#include <vector>

#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/engine/matching_engine.hpp"
#include "matchcore/itch/parser.hpp"
#include "matchcore/itch/synthetic.hpp"

namespace {

using namespace matchcore;

double px(Price p) { return static_cast<double>(p.raw()) / Price::kScale; }

/// Print the top `depth` levels of both sides, asks above bids.
void print_ladder(const OrderBook& book, StockLocate loc, int depth) {
  std::vector<LevelInfo> bids;
  std::vector<LevelInfo> asks;
  auto collect = [depth](std::vector<LevelInfo>& out) {
    return [&out, depth](const LevelInfo& l) {
      out.push_back(l);
      return static_cast<int>(out.size()) < depth;
    };
  };
  book.for_each_level(loc, Side::kBuy, collect(bids));
  book.for_each_level(loc, Side::kSell, collect(asks));

  std::printf("      %10s  %10s  %7s\n", "price", "shares", "orders");
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
    std::printf("  ASK %10.4f  %10llu  %7u\n", px(it->price),
                static_cast<unsigned long long>(it->total_shares), it->order_count);
  }
  std::printf("  ---------------------------------- spread\n");
  for (const auto& l : bids) {
    std::printf("  BID %10.4f  %10llu  %7u\n", px(l.price),
                static_cast<unsigned long long>(l.total_shares), l.order_count);
  }
}

struct PrintSink {
  void on_fill(const Fill& f) {
    std::printf("    TRADE #%llu  taker %llu %s %u @ %.4f  vs maker %llu (maker left: %u)\n",
                static_cast<unsigned long long>(f.match_number),
                static_cast<unsigned long long>(f.taker_id),
                f.taker_side == Side::kBuy ? "BUY " : "SELL", f.quantity, px(f.price),
                static_cast<unsigned long long>(f.maker_id), f.maker_remaining);
  }
};

const char* status_name(OrderStatus s) {
  switch (s) {
    case OrderStatus::kResting: return "RESTING";
    case OrderStatus::kFilled: return "FILLED";
    case OrderStatus::kCancelled: return "CANCELLED";
    case OrderStatus::kRejected: return "REJECTED";
  }
  return "?";
}

void report(const char* what, const ExecReport& r) {
  std::printf("  -> %-34s %s  filled=%u resting=%u\n", what, status_name(r.status), r.filled,
              r.remaining);
}

}  // namespace

int main() {
  // ------------------------------------------------------------------------
  std::printf("=== 1. ITCH 5.0 -> order book rebuild ===\n\n");
  itch::SyntheticConfig cfg;
  cfg.messages = 200'000;
  cfg.symbols = 10;
  cfg.target_resting = 3'000;
  const auto session = itch::generate_session(cfg);
  std::printf("Generated ITCH session: %zu bytes, %zu order messages, %u symbols\n",
              session.size(), cfg.messages, static_cast<unsigned>(cfg.symbols));

  OrderBook::Config bc;
  bc.max_orders = 1 << 16;
  bc.max_levels = 1 << 14;
  OrderBook book(bc);
  ItchBookBuilder builder(book);
  const auto r = itch::parse_stream(session, builder);
  std::printf("Parsed %llu messages (unknown %llu, malformed %llu)\n",
              static_cast<unsigned long long>(r.messages),
              static_cast<unsigned long long>(r.unknown_types),
              static_cast<unsigned long long>(r.malformed));
  std::printf("Book events applied %llu, rejected %llu; %zu orders resting on %zu levels\n\n",
              static_cast<unsigned long long>(builder.applied()),
              static_cast<unsigned long long>(builder.rejected()), book.order_count(),
              book.level_count());

  std::printf("Top of book, symbol SYM0001 (locate 1), 5 levels each side:\n");
  print_ladder(book, 1, 5);

  // ------------------------------------------------------------------------
  std::printf("\n=== 2. Matching engine (price-time priority) ===\n\n");
  MatchingEngine::Config ec;
  ec.max_orders = 1024;
  ec.max_levels = 256;
  MatchingEngine engine(ec);
  PrintSink sink;
  auto limit = [](OrderId id, Side s, std::uint32_t raw_px, Quantity q,
                  TimeInForce tif = TimeInForce::kGtc) {
    return NewOrder{1, id, s, OrderType::kLimit, tif, Price{raw_px}, q};
  };

  std::printf("Seed the book with resting sell orders:\n");
  report("#1 SELL 100 @ 100.00", engine.submit(limit(1, Side::kSell, 1'000'000, 100), sink));
  report("#2 SELL 200 @ 100.00 (behind #1)", engine.submit(limit(2, Side::kSell, 1'000'000, 200), sink));
  report("#3 SELL 150 @ 100.05", engine.submit(limit(3, Side::kSell, 1'000'500, 150), sink));
  report("#4 BUY  300 @ 99.90", engine.submit(limit(4, Side::kBuy, 999'000, 300), sink));
  print_ladder(engine.book(), 1, 5);

  std::printf("\nAggressive BUY 250 @ 100.05: fills #1 first (time priority), then #2:\n");
  report("#5 BUY 250 @ 100.05", engine.submit(limit(5, Side::kBuy, 1'000'500, 250), sink));

  std::printf("\nFill-or-kill BUY 500 @ 100.05: not enough shares, so nothing trades:\n");
  report("#6 FOK BUY 500 @ 100.05",
         engine.submit(limit(6, Side::kBuy, 1'000'500, 500, TimeInForce::kFok), sink));

  std::printf("\nMarket SELL 400 (IOC): sweeps the bids, leftover is cancelled:\n");
  report("#7 MARKET SELL 400",
         engine.submit(NewOrder{1, 7, Side::kSell, OrderType::kMarket, TimeInForce::kIoc, Price{}, 400},
                       sink));

  std::printf("\nModify #3 down to 50 shares (same price, keeps priority), then cancel it:\n");
  report("modify #3 -> 50 @ 100.05", engine.modify(3, Price{1'000'500}, 50, sink));
  report("cancel #3", engine.cancel(3));

  std::printf("\nFinal book:\n");
  print_ladder(engine.book(), 1, 5);
  std::printf("\nDone. %llu trades executed.\n",
              static_cast<unsigned long long>(engine.last_match_number()));
  return 0;
}
