#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <vector>

#include "matchcore/engine/matching_engine.hpp"

namespace matchcore {
namespace {

constexpr StockLocate kSym = 1;

struct RecordingSink {
  std::vector<Fill> fills;
  void on_fill(const Fill& f) { fills.push_back(f); }
};

NewOrder limit(OrderId id, Side side, std::uint32_t px, Quantity qty,
               TimeInForce tif = TimeInForce::kGtc) {
  return {kSym, id, side, OrderType::kLimit, tif, Price{px}, qty};
}

NewOrder market(OrderId id, Side side, Quantity qty, TimeInForce tif = TimeInForce::kIoc) {
  return {kSym, id, side, OrderType::kMarket, tif, Price{}, qty};
}

class EngineTest : public ::testing::Test {
 protected:
  MatchingEngine::Config small() {
    MatchingEngine::Config c;
    c.max_orders = 4096;
    c.max_levels = 1024;
    return c;
  }
  MatchingEngine engine{small()};
  RecordingSink sink;

  ExecReport submit(const NewOrder& o) { return engine.submit(o, sink); }
};

TEST_F(EngineTest, NonCrossingLimitRests) {
  const auto r = submit(limit(1, Side::kBuy, 100, 10));
  EXPECT_EQ(r, (ExecReport{1, OrderStatus::kResting, RejectReason::kNone, 0, 10}));
  EXPECT_TRUE(sink.fills.empty());
  EXPECT_EQ(engine.book().best(kSym, Side::kBuy)->price, Price{100});
}

TEST_F(EngineTest, CrossingLimitTradesAtMakerPrice) {
  submit(limit(1, Side::kSell, 100, 10));
  const auto r = submit(limit(2, Side::kBuy, 105, 10));
  EXPECT_EQ(r.status, OrderStatus::kFilled);
  EXPECT_EQ(r.filled, 10U);
  ASSERT_EQ(sink.fills.size(), 1U);
  EXPECT_EQ(sink.fills[0], (Fill{1, kSym, 2, 1, Side::kBuy, Price{100}, 10, 0}));
  EXPECT_FALSE(engine.book().best(kSym, Side::kSell).has_value());
  EXPECT_FALSE(engine.book().best(kSym, Side::kBuy).has_value());
}

TEST_F(EngineTest, PriceTimePriority) {
  // Two asks at 101 (ids 1 then 2), one better ask at 100 (id 3).
  submit(limit(1, Side::kSell, 101, 5));
  submit(limit(2, Side::kSell, 101, 5));
  submit(limit(3, Side::kSell, 100, 5));
  const auto r = submit(limit(4, Side::kBuy, 101, 12));
  EXPECT_EQ(r.status, OrderStatus::kFilled);
  ASSERT_EQ(sink.fills.size(), 3U);
  EXPECT_EQ(sink.fills[0].maker_id, 3U);  // best price first
  EXPECT_EQ(sink.fills[0].price, Price{100});
  EXPECT_EQ(sink.fills[1].maker_id, 1U);  // then FIFO at 101
  EXPECT_EQ(sink.fills[2].maker_id, 2U);
  EXPECT_EQ(sink.fills[2].quantity, 2U);
  EXPECT_EQ(sink.fills[2].maker_remaining, 3U);
  EXPECT_EQ(engine.book().find(2)->shares, 3U);
  // Match numbers increase monotonically.
  EXPECT_EQ(sink.fills[0].match_number + 1, sink.fills[1].match_number);
  EXPECT_EQ(engine.last_match_number(), sink.fills[2].match_number);
}

TEST_F(EngineTest, PartialFillRestsRemainderAtLimit) {
  submit(limit(1, Side::kSell, 100, 4));
  const auto r = submit(limit(2, Side::kBuy, 100, 10));
  EXPECT_EQ(r, (ExecReport{2, OrderStatus::kResting, RejectReason::kNone, 4, 6}));
  EXPECT_EQ(engine.book().best(kSym, Side::kBuy)->price, Price{100});
  EXPECT_EQ(engine.book().best(kSym, Side::kBuy)->total_shares, 6U);
}

TEST_F(EngineTest, LimitDoesNotTradeThroughItsPrice) {
  submit(limit(1, Side::kSell, 100, 5));
  submit(limit(2, Side::kSell, 102, 5));
  const auto r = submit(limit(3, Side::kBuy, 101, 10));
  EXPECT_EQ(r.filled, 5U);
  EXPECT_EQ(r.remaining, 5U);
  EXPECT_EQ(engine.book().best(kSym, Side::kSell)->price, Price{102});
  EXPECT_EQ(engine.book().best(kSym, Side::kBuy)->price, Price{101});
}

TEST_F(EngineTest, MarketOrderSweepsLevelsAndCancelsRemainder) {
  submit(limit(1, Side::kBuy, 100, 5));
  submit(limit(2, Side::kBuy, 99, 5));
  const auto r = submit(market(3, Side::kSell, 12));
  EXPECT_EQ(r, (ExecReport{3, OrderStatus::kCancelled, RejectReason::kNone, 10, 0}));
  ASSERT_EQ(sink.fills.size(), 2U);
  EXPECT_EQ(sink.fills[0].price, Price{100});
  EXPECT_EQ(sink.fills[1].price, Price{99});
  EXPECT_EQ(engine.book().order_count(), 0U);
}

TEST_F(EngineTest, MarketOrderOnEmptyBookIsCancelled) {
  const auto r = submit(market(1, Side::kBuy, 10));
  EXPECT_EQ(r.status, OrderStatus::kCancelled);
  EXPECT_EQ(r.filled, 0U);
}

TEST_F(EngineTest, MarketGtcIsRejected) {
  const auto r = submit(market(1, Side::kBuy, 10, TimeInForce::kGtc));
  EXPECT_EQ(r.reason, RejectReason::kInvalidTimeInForce);
}

TEST_F(EngineTest, IocFillsWhatItCanAndNeverRests) {
  submit(limit(1, Side::kSell, 100, 3));
  const auto r = submit(limit(2, Side::kBuy, 100, 10, TimeInForce::kIoc));
  EXPECT_EQ(r, (ExecReport{2, OrderStatus::kCancelled, RejectReason::kNone, 3, 0}));
  EXPECT_FALSE(engine.book().find(2).has_value());
}

TEST_F(EngineTest, FokFillsCompletelyAcrossLevels) {
  submit(limit(1, Side::kSell, 100, 3));
  submit(limit(2, Side::kSell, 101, 7));
  const auto r = submit(limit(3, Side::kBuy, 101, 10, TimeInForce::kFok));
  EXPECT_EQ(r.status, OrderStatus::kFilled);
  EXPECT_EQ(sink.fills.size(), 2U);
}

TEST_F(EngineTest, FokThatCannotFillLeavesBookUntouched) {
  submit(limit(1, Side::kSell, 100, 3));
  submit(limit(2, Side::kSell, 102, 7));  // beyond the FOK limit
  const auto r = submit(limit(3, Side::kBuy, 101, 10, TimeInForce::kFok));
  EXPECT_EQ(r.reason, RejectReason::kFokNotFillable);
  EXPECT_TRUE(sink.fills.empty());
  EXPECT_EQ(engine.book().find(1)->shares, 3U);
  EXPECT_EQ(engine.book().order_count(), 2U);
}

TEST_F(EngineTest, MarketFok) {
  submit(limit(1, Side::kSell, 100, 3));
  EXPECT_EQ(submit(market(2, Side::kBuy, 5, TimeInForce::kFok)).reason,
            RejectReason::kFokNotFillable);
  EXPECT_EQ(submit(market(3, Side::kBuy, 3, TimeInForce::kFok)).status, OrderStatus::kFilled);
}

TEST_F(EngineTest, ValidationRejects) {
  submit(limit(1, Side::kBuy, 100, 10));
  EXPECT_EQ(submit(limit(1, Side::kBuy, 100, 10)).reason, RejectReason::kDuplicateOrderId);
  EXPECT_EQ(submit(limit(2, Side::kBuy, 100, 0)).reason, RejectReason::kInvalidQuantity);
  EXPECT_EQ(submit(limit(3, Side::kBuy, 0, 10)).reason, RejectReason::kInvalidPrice);
  EXPECT_EQ(engine.book().order_count(), 1U);
}

TEST_F(EngineTest, Cancel) {
  submit(limit(1, Side::kBuy, 100, 10));
  EXPECT_EQ(engine.cancel(1).status, OrderStatus::kCancelled);
  EXPECT_EQ(engine.cancel(1).reason, RejectReason::kUnknownOrderId);
  EXPECT_EQ(engine.book().order_count(), 0U);
}

TEST_F(EngineTest, ModifyDownKeepsPriority) {
  submit(limit(1, Side::kBuy, 100, 10));
  submit(limit(2, Side::kBuy, 100, 10));
  const auto r = engine.modify(1, Price{100}, 4, sink);
  EXPECT_EQ(r, (ExecReport{1, OrderStatus::kResting, RejectReason::kNone, 0, 4}));
  EXPECT_EQ(engine.book().front(kSym, Side::kBuy)->id, 1U);
  EXPECT_EQ(engine.book().best(kSym, Side::kBuy)->total_shares, 14U);
}

TEST_F(EngineTest, ModifyUpLosesPriority) {
  submit(limit(1, Side::kBuy, 100, 10));
  submit(limit(2, Side::kBuy, 100, 10));
  engine.modify(1, Price{100}, 15, sink);
  EXPECT_EQ(engine.book().front(kSym, Side::kBuy)->id, 2U);
  EXPECT_EQ(engine.book().find(1)->shares, 15U);
}

TEST_F(EngineTest, ModifyPriceAcrossSpreadMatches) {
  submit(limit(1, Side::kSell, 105, 10));
  submit(limit(2, Side::kBuy, 100, 10));
  const auto r = engine.modify(2, Price{105}, 6, sink);
  EXPECT_EQ(r.status, OrderStatus::kFilled);
  ASSERT_EQ(sink.fills.size(), 1U);
  EXPECT_EQ(sink.fills[0].maker_id, 1U);
  EXPECT_EQ(engine.book().find(1)->shares, 4U);
}

TEST_F(EngineTest, ModifyRejects) {
  EXPECT_EQ(engine.modify(9, Price{1}, 1, sink).reason, RejectReason::kUnknownOrderId);
  submit(limit(1, Side::kBuy, 100, 10));
  EXPECT_EQ(engine.modify(1, Price{100}, 0, sink).reason, RejectReason::kInvalidQuantity);
  EXPECT_EQ(engine.modify(1, Price{0}, 5, sink).reason, RejectReason::kInvalidPrice);
  EXPECT_EQ(engine.book().find(1)->shares, 10U);
}

// Invariants under random flow: the book is never crossed, and shares are
// conserved (submitted == resting + 2 * traded + cancelled/killed).
TEST(EngineProperty, RandomFlowKeepsInvariants) {
  MatchingEngine::Config cfg;
  cfg.max_orders = 1 << 16;
  cfg.max_levels = 1 << 12;
  MatchingEngine engine(cfg);
  RecordingSink sink;
  std::mt19937_64 rng(7);

  std::uint64_t submitted = 0;
  std::uint64_t dropped = 0;  // IOC/market remainders, cancels, modify reductions
  OrderId next = 1;

  auto resting_total = [&] {
    std::uint64_t t = 0;
    for (const Side s : {Side::kBuy, Side::kSell}) {
      engine.book().for_each_level(kSym, s, [&](const LevelInfo& l) {
        t += l.total_shares;
        return true;
      });
    }
    return t;
  };

  for (int step = 0; step < 50'000; ++step) {
    const auto roll = rng() % 20;
    if (roll < 15) {
      NewOrder o;
      o.locate = kSym;
      o.id = next++;
      o.side = rng() % 2 ? Side::kBuy : Side::kSell;
      o.type = rng() % 10 == 0 ? OrderType::kMarket : OrderType::kLimit;
      const auto t = rng() % 6;
      o.tif = t == 0 ? TimeInForce::kIoc : t == 1 ? TimeInForce::kFok : TimeInForce::kGtc;
      if (o.type == OrderType::kMarket && o.tif == TimeInForce::kGtc) {
        o.tif = TimeInForce::kIoc;
      }
      o.price = Price{static_cast<std::uint32_t>(950 + rng() % 100)};
      o.quantity = static_cast<Quantity>(1 + rng() % 200);
      const auto r = engine.submit(o, sink);
      if (r.status != OrderStatus::kRejected) {
        submitted += o.quantity;
        if (r.status == OrderStatus::kCancelled) {
          dropped += o.quantity - r.filled;
        }
      }
    } else {
      const OrderId id = 1 + rng() % next;
      const auto cur = engine.book().find(id);
      if (roll < 18 || !cur) {
        if (cur && engine.cancel(id).status == OrderStatus::kCancelled) {
          dropped += cur->shares;
        }
      } else {
        const auto px = Price{static_cast<std::uint32_t>(950 + rng() % 100)};
        const auto qty = static_cast<Quantity>(1 + rng() % 200);
        const auto r = engine.modify(id, px, qty, sink);
        ASSERT_NE(r.status, OrderStatus::kRejected);
        // Model modify as: drop the old shares, submit `qty` new ones.
        dropped += cur->shares;
        submitted += qty;
      }
    }

    const auto bid = engine.book().best(kSym, Side::kBuy);
    const auto ask = engine.book().best(kSym, Side::kSell);
    if (bid && ask) {
      ASSERT_LT(bid->price, ask->price) << "crossed book at step " << step;
    }
  }

  std::uint64_t traded = 0;
  for (const Fill& f : sink.fills) {
    traded += f.quantity;
  }
  EXPECT_EQ(submitted, resting_total() + 2 * traded + dropped);
}

}  // namespace
}  // namespace matchcore
