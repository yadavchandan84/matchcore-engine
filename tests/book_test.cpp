#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "itch_builder.hpp"
#include "matchcore/book/index_pool.hpp"
#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/book/order_id_map.hpp"
#include "matchcore/book/reference_book.hpp"
#include "matchcore/itch/parser.hpp"

namespace matchcore {
namespace {

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------

TEST(IndexPool, AllocatesUntilFullThenReusesReleasedSlots) {
  IndexPool<int> pool(3);
  const PoolIndex a = pool.allocate();
  const PoolIndex b = pool.allocate();
  const PoolIndex c = pool.allocate();
  EXPECT_NE(a, kNullIndex);
  EXPECT_NE(b, kNullIndex);
  EXPECT_NE(c, kNullIndex);
  EXPECT_EQ(pool.allocate(), kNullIndex);
  EXPECT_EQ(pool.in_use(), 3U);
  pool.release(b);
  EXPECT_EQ(pool.allocate(), b);
  EXPECT_EQ(pool.in_use(), 3U);
}

TEST(IndexPool, ZeroCapacityNeverAllocates) {
  IndexPool<int> pool(0);
  EXPECT_EQ(pool.allocate(), kNullIndex);
}

TEST(OrderIdMap, InsertFindEraseBasics) {
  OrderIdMap m(16);
  EXPECT_EQ(m.find(0), kNullIndex);
  EXPECT_TRUE(m.insert(0, 10));  // key 0 is a valid order id
  EXPECT_TRUE(m.insert(42, 11));
  EXPECT_FALSE(m.insert(42, 12));  // duplicate
  EXPECT_EQ(m.find(0), 10U);
  EXPECT_EQ(m.find(42), 11U);
  EXPECT_TRUE(m.erase(42));
  EXPECT_FALSE(m.erase(42));
  EXPECT_EQ(m.find(42), kNullIndex);
  EXPECT_EQ(m.size(), 1U);
}

TEST(OrderIdMap, RejectsInsertBeyondMaxEntries) {
  OrderIdMap m(2);
  EXPECT_TRUE(m.insert(1, 1));
  EXPECT_TRUE(m.insert(2, 2));
  EXPECT_FALSE(m.insert(3, 3));
}

TEST(OrderIdMap, MatchesUnorderedMapUnderRandomChurn) {
  // Small key space + many erases exercises long probe clusters and the
  // backward-shift deletion wrap-around logic.
  constexpr std::size_t kMax = 512;
  OrderIdMap m(kMax);
  std::unordered_map<OrderId, PoolIndex> ref;
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<OrderId> key(0, 2000);

  for (int step = 0; step < 200'000; ++step) {
    const OrderId k = key(rng);
    switch (rng() % 3) {
      case 0: {
        const auto v = static_cast<PoolIndex>(step);
        const bool expect = ref.size() < kMax && !ref.contains(k);
        ASSERT_EQ(m.insert(k, v), expect) << "step " << step;
        if (expect) {
          ref[k] = v;
        }
        break;
      }
      case 1: ASSERT_EQ(m.erase(k), ref.erase(k) == 1) << "step " << step; break;
      default: {
        const auto it = ref.find(k);
        ASSERT_EQ(m.find(k), it == ref.end() ? kNullIndex : it->second) << "step " << step;
      }
    }
    ASSERT_EQ(m.size(), ref.size());
  }
}

TEST(OrderIdMap, StaysCorrectUnderWorstCaseClustering) {
  // Keys that are multiples of the slot count all share one home slot under
  // the identity hash: one long probe chain. Slow, but must stay correct,
  // including backward-shift deletion from the middle of the chain.
  OrderIdMap m(64);
  const OrderId stride = m.slot_count();
  for (OrderId i = 0; i < 64; ++i) {
    ASSERT_TRUE(m.insert(i * stride, static_cast<PoolIndex>(i)));
  }
  for (OrderId i = 0; i < 64; i += 2) {
    ASSERT_TRUE(m.erase(i * stride));
  }
  for (OrderId i = 0; i < 64; ++i) {
    EXPECT_EQ(m.find(i * stride), i % 2 == 0 ? kNullIndex : static_cast<PoolIndex>(i)) << i;
  }
}

// ---------------------------------------------------------------------------
// Behaviour shared by both books (typed tests)
// ---------------------------------------------------------------------------

template <class Book>
Book make_book() {
  if constexpr (std::is_same_v<Book, OrderBook>) {
    OrderBook::Config cfg;
    cfg.max_orders = 1024;
    cfg.max_levels = 256;
    return OrderBook(cfg);
  } else {
    return Book{};
  }
}

template <class Book>
class BookTest : public ::testing::Test {
 protected:
  Book book = make_book<Book>();
};

using BookTypes = ::testing::Types<ReferenceBook, OrderBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

constexpr StockLocate kSym = 7;

template <class Book>
std::vector<LevelInfo> depth(const Book& b, Side side, StockLocate loc = kSym) {
  std::vector<LevelInfo> out;
  b.for_each_level(loc, side, [&](const LevelInfo& li) {
    out.push_back(li);
    return true;
  });
  return out;
}

TYPED_TEST(BookTest, EmptyBookHasNoTopOfBook) {
  EXPECT_FALSE(this->book.best(kSym, Side::kBuy).has_value());
  EXPECT_FALSE(this->book.best(kSym, Side::kSell).has_value());
  EXPECT_FALSE(this->book.front(kSym, Side::kBuy).has_value());
  EXPECT_EQ(this->book.order_count(), 0U);
}

TYPED_TEST(BookTest, LevelsSortBestFirstOnEachSide) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 2, Side::kBuy, Price{102}, 20), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 3, Side::kBuy, Price{101}, 30), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 4, Side::kSell, Price{105}, 40), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 5, Side::kSell, Price{103}, 50), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 6, Side::kSell, Price{104}, 60), BookResult::kOk);

  EXPECT_EQ(depth(b, Side::kBuy), (std::vector<LevelInfo>{
                                      {Price{102}, 20, 1}, {Price{101}, 30, 1}, {Price{100}, 10, 1}}));
  EXPECT_EQ(depth(b, Side::kSell), (std::vector<LevelInfo>{
                                       {Price{103}, 50, 1}, {Price{104}, 60, 1}, {Price{105}, 40, 1}}));
  EXPECT_EQ(b.best(kSym, Side::kBuy)->price, Price{102});
  EXPECT_EQ(b.best(kSym, Side::kSell)->price, Price{103});
}

TYPED_TEST(BookTest, SamePriceAggregatesInTimePriority) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 2, Side::kBuy, Price{100}, 20), BookResult::kOk);
  EXPECT_EQ(depth(b, Side::kBuy), (std::vector<LevelInfo>{{Price{100}, 30, 2}}));
  EXPECT_EQ(b.front(kSym, Side::kBuy)->id, 1U);
  ASSERT_EQ(b.remove(1), BookResult::kOk);
  EXPECT_EQ(b.front(kSym, Side::kBuy)->id, 2U);
}

TYPED_TEST(BookTest, PartialExecuteAndCancelReduceShares) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kSell, Price{200}, 100), BookResult::kOk);
  ASSERT_EQ(b.execute(1, 30), BookResult::kOk);
  ASSERT_EQ(b.cancel(1, 20), BookResult::kOk);
  EXPECT_EQ(b.find(1)->shares, 50U);
  EXPECT_EQ(b.best(kSym, Side::kSell)->total_shares, 50U);
}

TYPED_TEST(BookTest, FullExecutionRemovesOrderAndEmptyLevel) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kSell, Price{200}, 100), BookResult::kOk);
  ASSERT_EQ(b.execute(1, 100), BookResult::kOk);
  EXPECT_FALSE(b.find(1).has_value());
  EXPECT_FALSE(b.best(kSym, Side::kSell).has_value());
  EXPECT_EQ(b.order_count(), 0U);
}

TYPED_TEST(BookTest, ReplaceMovesPriceAndLosesPriority) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 2, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.replace(1, 3, Price{100}, 15), BookResult::kOk);  // same price, new id
  EXPECT_FALSE(b.find(1).has_value());
  EXPECT_EQ(b.front(kSym, Side::kBuy)->id, 2U);  // 3 went to the back of the queue
  ASSERT_EQ(b.replace(3, 4, Price{101}, 5), BookResult::kOk);
  EXPECT_EQ(b.best(kSym, Side::kBuy)->price, Price{101});
  EXPECT_EQ(b.find(4)->side, Side::kBuy);
  EXPECT_EQ(b.find(4)->locate, kSym);
}

TYPED_TEST(BookTest, SymbolsAreIndependent) {
  auto& b = this->book;
  ASSERT_EQ(b.add(1, 1, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.add(2, 2, Side::kBuy, Price{500}, 10), BookResult::kOk);
  EXPECT_EQ(b.best(1, Side::kBuy)->price, Price{100});
  EXPECT_EQ(b.best(2, Side::kBuy)->price, Price{500});
}

TYPED_TEST(BookTest, RejectedOperationsLeaveBookUnchanged) {
  auto& b = this->book;
  ASSERT_EQ(b.add(kSym, 1, Side::kBuy, Price{100}, 10), BookResult::kOk);
  ASSERT_EQ(b.add(kSym, 2, Side::kSell, Price{110}, 10), BookResult::kOk);

  EXPECT_EQ(b.add(kSym, 1, Side::kSell, Price{120}, 5), BookResult::kDuplicateOrderId);
  EXPECT_EQ(b.add(kSym, 9, Side::kSell, Price{120}, 0), BookResult::kInvalidQuantity);
  EXPECT_EQ(b.execute(99, 1), BookResult::kUnknownOrderId);
  EXPECT_EQ(b.execute(1, 11), BookResult::kInvalidQuantity);
  EXPECT_EQ(b.cancel(1, 0), BookResult::kInvalidQuantity);
  EXPECT_EQ(b.remove(99), BookResult::kUnknownOrderId);
  EXPECT_EQ(b.replace(99, 100, Price{1}, 1), BookResult::kUnknownOrderId);
  EXPECT_EQ(b.replace(1, 2, Price{1}, 1), BookResult::kDuplicateOrderId);
  EXPECT_EQ(b.replace(1, 3, Price{1}, 0), BookResult::kInvalidQuantity);

  EXPECT_EQ(b.order_count(), 2U);
  EXPECT_EQ(depth(b, Side::kBuy), (std::vector<LevelInfo>{{Price{100}, 10, 1}}));
  EXPECT_EQ(depth(b, Side::kSell), (std::vector<LevelInfo>{{Price{110}, 10, 1}}));
}

TYPED_TEST(BookTest, RebuildsFromItchStream) {
  using itch::test::MessageBuilder;
  std::vector<std::byte> s;
  MessageBuilder m;
  m.header('A', kSym, 0, 1).u64(1).ch('B').u32(100).alpha("AAPL", 8).u32(1000);
  itch::test::append(s, m);
  m = {};
  m.header('F', kSym, 0, 2).u64(2).ch('S').u32(200).alpha("AAPL", 8).u32(1010).alpha("GSCO", 4);
  itch::test::append(s, m);
  m = {};
  m.header('E', kSym, 0, 3).u64(1).u32(40).u64(1);
  itch::test::append(s, m);
  m = {};
  m.header('C', kSym, 0, 4).u64(2).u32(50).u64(2).ch('Y').u32(1009);
  itch::test::append(s, m);
  m = {};
  m.header('X', kSym, 0, 5).u64(2).u32(50);
  itch::test::append(s, m);
  m = {};
  m.header('U', kSym, 0, 6).u64(1).u64(3).u32(70).u32(1001);
  itch::test::append(s, m);
  m = {};
  m.header('P', kSym, 0, 7).u64(0).ch('B').u32(10).alpha("AAPL", 8).u32(1005).u64(3);
  itch::test::append(s, m);
  m = {};
  m.header('D', kSym, 0, 8).u64(99);  // unknown id: counted as rejected
  itch::test::append(s, m);

  ItchBookBuilder builder(this->book);
  const auto r = itch::parse_stream(s, builder);
  EXPECT_EQ(r.messages, 8U);
  EXPECT_EQ(builder.applied(), 6U);
  EXPECT_EQ(builder.rejected(), 1U);

  EXPECT_EQ(depth(this->book, Side::kBuy), (std::vector<LevelInfo>{{Price{1001}, 70, 1}}));
  EXPECT_EQ(depth(this->book, Side::kSell), (std::vector<LevelInfo>{{Price{1010}, 100, 1}}));
}

// ---------------------------------------------------------------------------
// OrderBook-specific: capacity limits
// ---------------------------------------------------------------------------

TEST(OrderBook, ReportsOrderCapacityExhaustion) {
  OrderBook::Config cfg;
  cfg.max_orders = 2;
  cfg.max_levels = 8;
  OrderBook b(cfg);
  ASSERT_EQ(b.add(1, 1, Side::kBuy, Price{1}, 1), BookResult::kOk);
  ASSERT_EQ(b.add(1, 2, Side::kBuy, Price{2}, 1), BookResult::kOk);
  EXPECT_EQ(b.add(1, 3, Side::kBuy, Price{3}, 1), BookResult::kCapacityExceeded);
  EXPECT_EQ(b.order_count(), 2U);
  ASSERT_EQ(b.remove(1), BookResult::kOk);
  EXPECT_EQ(b.add(1, 3, Side::kBuy, Price{3}, 1), BookResult::kOk);
}

TEST(OrderBook, ReportsLevelCapacityExhaustionWithoutSideEffects) {
  OrderBook::Config cfg;
  cfg.max_orders = 8;
  cfg.max_levels = 2;
  OrderBook b(cfg);
  ASSERT_EQ(b.add(1, 1, Side::kBuy, Price{1}, 1), BookResult::kOk);
  ASSERT_EQ(b.add(1, 2, Side::kBuy, Price{2}, 1), BookResult::kOk);
  ASSERT_EQ(b.add(1, 3, Side::kBuy, Price{2}, 1), BookResult::kOk);  // existing level: fine
  EXPECT_EQ(b.add(1, 4, Side::kBuy, Price{3}, 1), BookResult::kCapacityExceeded);
  EXPECT_EQ(b.order_count(), 3U);
  // Replace of order 2 (level 2 still has order 3) to a brand-new level must
  // be refused up front, not after removing order 2.
  EXPECT_EQ(b.replace(2, 5, Price{9}, 1), BookResult::kCapacityExceeded);
  EXPECT_TRUE(b.find(2).has_value());
  // Replace of the sole order on level 1 frees that level, so it succeeds.
  EXPECT_EQ(b.replace(1, 6, Price{9}, 1), BookResult::kOk);
  EXPECT_EQ(b.best(1, Side::kBuy)->price, Price{9});
}

// ---------------------------------------------------------------------------
// Differential test: OrderBook must agree with ReferenceBook on every step
// ---------------------------------------------------------------------------

template <class Book>
std::vector<LevelInfo> full_depth(const Book& b, StockLocate loc, Side side) {
  return depth(b, side, loc);
}

TEST(BookDifferential, RandomOperationsAgreeWithReference) {
  constexpr int kSteps = 100'000;
  constexpr StockLocate kSymbols = 4;
  OrderBook::Config cfg;
  // Large enough that capacity never binds: the reference book has no limit,
  // and capacity behaviour is covered by the dedicated tests above.
  cfg.max_orders = 1 << 17;
  cfg.max_levels = 1 << 12;  // 4 symbols x 2 sides x 200 prices = 1600 max
  OrderBook fast(cfg);
  ReferenceBook ref;

  std::mt19937_64 rng(2024);
  std::vector<OrderId> live;  // ids believed resting (may be stale)
  OrderId next_id = 1;

  auto rand_price = [&] { return Price{static_cast<std::uint32_t>(9'900 + rng() % 200)}; };
  auto rand_live = [&]() -> OrderId {
    // Mostly live ids, sometimes a random (likely unknown) one.
    if (live.empty() || rng() % 20 == 0) {
      return rng() % (next_id + 5);
    }
    return live[rng() % live.size()];
  };

  for (int step = 0; step < kSteps; ++step) {
    const auto op = rng() % 10;
    BookResult rf{};
    BookResult rr{};
    if (op < 4) {
      const auto loc = static_cast<StockLocate>(1 + rng() % kSymbols);
      const Side side = rng() % 2 ? Side::kBuy : Side::kSell;
      const auto px = rand_price();
      const auto qty = static_cast<Quantity>(rng() % 500);  // includes 0 (invalid)
      const OrderId id = rng() % 50 == 0 ? rand_live() : next_id++;
      rf = fast.add(loc, id, side, px, qty);
      rr = ref.add(loc, id, side, px, qty);
      if (rr == BookResult::kOk) {
        live.push_back(id);
      }
    } else if (op < 6) {
      const OrderId id = rand_live();
      const auto qty = static_cast<Quantity>(rng() % 300);
      rf = fast.execute(id, qty);
      rr = ref.execute(id, qty);
    } else if (op < 7) {
      const OrderId id = rand_live();
      const auto qty = static_cast<Quantity>(rng() % 300);
      rf = fast.cancel(id, qty);
      rr = ref.cancel(id, qty);
    } else if (op < 9) {
      const OrderId id = rand_live();
      rf = fast.remove(id);
      rr = ref.remove(id);
    } else {
      const OrderId id = rand_live();
      const OrderId nid = rng() % 30 == 0 ? rand_live() : next_id++;
      const auto px = rand_price();
      const auto qty = static_cast<Quantity>(rng() % 500);
      rf = fast.replace(id, nid, px, qty);
      rr = ref.replace(id, nid, px, qty);
      if (rr == BookResult::kOk) {
        live.push_back(nid);
      }
    }
    ASSERT_EQ(rf, rr) << "step " << step;
    ASSERT_EQ(fast.order_count(), ref.order_count()) << "step " << step;

    // Periodically compact the live list and compare full state.
    if (step % 1000 == 0) {
      std::erase_if(live, [&](OrderId id) { return !ref.find(id).has_value(); });
      for (const OrderId id : live) {
        ASSERT_EQ(fast.find(id), ref.find(id)) << "step " << step << " id " << id;
      }
      for (StockLocate loc = 1; loc <= kSymbols; ++loc) {
        for (const Side side : {Side::kBuy, Side::kSell}) {
          ASSERT_EQ(full_depth(fast, loc, side), full_depth(ref, loc, side)) << "step " << step;
          ASSERT_EQ(fast.front(loc, side), ref.front(loc, side)) << "step " << step;
        }
      }
    } else {
      const auto loc = static_cast<StockLocate>(1 + rng() % kSymbols);
      ASSERT_EQ(fast.best(loc, Side::kBuy), ref.best(loc, Side::kBuy)) << "step " << step;
      ASSERT_EQ(fast.best(loc, Side::kSell), ref.best(loc, Side::kSell)) << "step " << step;
    }
  }
}

}  // namespace
}  // namespace matchcore
