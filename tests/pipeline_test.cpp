#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/book/reference_book.hpp"
#include "matchcore/itch/parser.hpp"
#include "matchcore/itch/synthetic.hpp"
#include "matchcore/pipeline/pipeline.hpp"
#include "matchcore/pipeline/spsc_ring.hpp"

namespace matchcore {
namespace {

TEST(SpscRing, SingleThreadFifoAndCapacity) {
  SpscRing<int, 4> ring;
  int v = 0;
  EXPECT_FALSE(ring.try_pop(v));
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.try_push(i));
  }
  EXPECT_FALSE(ring.try_push(99));  // full: all 4 slots usable
  EXPECT_EQ(ring.size_approx(), 4U);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(ring.try_pop(v));
}

TEST(SpscRing, WrapsAroundManyTimes) {
  SpscRing<std::uint64_t, 8> ring;
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(ring.try_push(i));
    ASSERT_TRUE(ring.try_pop(v));
    ASSERT_EQ(v, i);
  }
}

TEST(SpscRing, TwoThreadsPreserveOrderAndLoseNothing) {
  // Small ring forces constant full/empty transitions: the interesting cases
  // for the cached-index logic and the acquire/release pairing (run under TSan).
  constexpr std::uint64_t kCount = 2'000'000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 64>>();

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!ring->try_push(i)) {
        std::this_thread::yield();
      }
    }
  });

  std::uint64_t expected = 0;
  std::uint64_t sum = 0;
  std::uint64_t v = 0;
  bool in_order = true;
  while (expected < kCount) {
    if (ring->try_pop(v)) {
      in_order = in_order && (v == expected);
      sum += v;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_TRUE(in_order);
  EXPECT_EQ(sum, kCount * (kCount - 1) / 2);
}

TEST(SyntheticSession, IsDeterministicAndFullyValid) {
  itch::SyntheticConfig cfg;
  cfg.messages = 50'000;
  cfg.symbols = 20;
  cfg.target_resting = 2'000;
  const auto a = itch::generate_session(cfg);
  const auto b = itch::generate_session(cfg);
  EXPECT_EQ(a, b);

  ReferenceBook ref;
  ItchBookBuilder builder(ref);
  const auto r = itch::parse_stream(a, builder);
  EXPECT_EQ(r.bytes_consumed, a.size());
  EXPECT_EQ(r.malformed, 0U);
  EXPECT_EQ(r.unknown_types, 0U);
  EXPECT_EQ(r.messages, cfg.messages + cfg.symbols + 2);
  EXPECT_EQ(builder.applied(), cfg.messages);  // every order event is valid
  EXPECT_EQ(builder.rejected(), 0U);
  EXPECT_GT(ref.order_count(), 0U);

  // Like a real exchange feed, the rebuilt book must never be crossed.
  for (StockLocate loc = 1; loc <= cfg.symbols; ++loc) {
    const auto bid = ref.best(loc, Side::kBuy);
    const auto ask = ref.best(loc, Side::kSell);
    if (bid && ask) {
      EXPECT_LT(bid->price, ask->price) << "locate " << loc;
    }
  }
}

template <class Book>
std::vector<LevelInfo> depth(const Book& b, StockLocate loc, Side side) {
  std::vector<LevelInfo> out;
  b.for_each_level(loc, side, [&](const LevelInfo& li) {
    out.push_back(li);
    return true;
  });
  return out;
}

TEST(Pipeline, TwoThreadReplayMatchesSingleThreadReference) {
  itch::SyntheticConfig cfg;
  cfg.messages = 200'000;
  cfg.symbols = 25;
  cfg.target_resting = 5'000;
  const auto session = itch::generate_session(cfg);

  // Reference: single-threaded, obviously-correct book.
  ReferenceBook ref;
  ItchBookBuilder builder(ref);
  (void)itch::parse_stream(session, builder);

  // Pipeline: parser thread -> SPSC ring -> engine thread -> OrderBook.
  // A tiny ring maximises producer/consumer contention.
  OrderBook::Config bc;
  bc.max_orders = 1 << 15;
  bc.max_levels = 1 << 13;
  OrderBook fast(bc);
  Pipeline<256> pipeline;
  const auto stats = pipeline.run(fast, std::span<const std::byte>{session});

  EXPECT_EQ(stats.events, cfg.messages);
  EXPECT_EQ(stats.results[static_cast<std::size_t>(BookResult::kOk)], cfg.messages);
  EXPECT_EQ(fast.order_count(), ref.order_count());
  for (StockLocate loc = 1; loc <= cfg.symbols; ++loc) {
    for (const Side side : {Side::kBuy, Side::kSell}) {
      ASSERT_EQ(depth(fast, loc, side), depth(ref, loc, side)) << "locate " << loc;
      ASSERT_EQ(fast.front(loc, side), ref.front(loc, side)) << "locate " << loc;
    }
  }
}

TEST(Pipeline, CanBeRunRepeatedly) {
  itch::SyntheticConfig cfg;
  cfg.messages = 10'000;
  cfg.symbols = 5;
  cfg.target_resting = 500;
  const auto session = itch::generate_session(cfg);
  Pipeline<1024> pipeline;
  for (int i = 0; i < 3; ++i) {
    OrderBook::Config bc;
    bc.max_orders = 1 << 12;
    bc.max_levels = 1 << 10;
    OrderBook book(bc);
    EXPECT_EQ(pipeline.run(book, std::span<const std::byte>{session}).events, cfg.messages);
  }
}

}  // namespace
}  // namespace matchcore
