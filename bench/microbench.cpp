// Google Benchmark microbenchmarks for MatchCore.
//
// Each optimization step from the roadmap has a benchmark pair so the
// before/after numbers in BENCHMARKS.md can be regenerated with one command:
//
//   matchcore_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
//
// Inputs are synthetic but deterministic (fixed seed), so runs are comparable.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <memory>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/book/order_id_map.hpp"
#include "matchcore/book/reference_book.hpp"
#include "matchcore/engine/matching_engine.hpp"
#include "matchcore/itch/parser.hpp"
#include "matchcore/itch/synthetic.hpp"
#include "matchcore/pipeline/pipeline.hpp"
#include "matchcore/pipeline/spsc_ring.hpp"

namespace {

using namespace matchcore;

constexpr std::size_t kSessionMessages = 1'000'000;

const std::vector<std::byte>& session() {
  static const std::vector<std::byte> s = [] {
    itch::SyntheticConfig cfg;
    cfg.messages = kSessionMessages;
    cfg.symbols = 100;
    return itch::generate_session(cfg);
  }();
  return s;
}

OrderBook::Config book_config() {
  OrderBook::Config c;
  c.max_orders = 1 << 17;
  c.max_levels = 1 << 16;
  c.levels_per_side = 1024;
  c.symbols_reserved = 100;
  return c;
}

/// DoNotOptimize on a mutable copy (the const-ref overload is deprecated
/// because it lets the compiler optimize around the value).
template <class T>
void keep(T value) {
  benchmark::DoNotOptimize(value);
}

void set_counters(benchmark::State& state, std::size_t messages, std::size_t bytes) {
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(messages));
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(bytes));
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// Decodes every message type fully (handler accepts everything).
struct DecodeAll {
  std::uint64_t sink = 0;
  template <class M>
  void on(const M& m) noexcept {
    sink += m.header.timestamp;
  }
};

void BM_ParseDecodeAll(benchmark::State& state) {
  const auto& s = session();
  for (auto _ : state) {
    DecodeAll h;
    const auto r = itch::parse_stream(s, h);
    benchmark::DoNotOptimize(h.sink);
    keep(r);
  }
  set_counters(state, kSessionMessages, s.size());
}
BENCHMARK(BM_ParseDecodeAll)->Unit(benchmark::kMillisecond);

/// Framing + dispatch only: no message is decoded (no `on` overloads).
void BM_ParseFramingOnly(benchmark::State& state) {
  struct None {};
  const auto& s = session();
  for (auto _ : state) {
    None h;
    keep(itch::parse_stream(s, h));
  }
  set_counters(state, kSessionMessages, s.size());
}
BENCHMARK(BM_ParseFramingOnly)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// Book rebuild: reference (std::map / std::list / std::unordered_map) vs
// optimized (flat ladder + intrusive FIFO + pools + open addressing)
// ---------------------------------------------------------------------------

void BM_ReplayReferenceBook(benchmark::State& state) {
  const auto& s = session();
  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<ReferenceBook>();
    state.ResumeTiming();
    ItchBookBuilder b(*book);
    keep(itch::parse_stream(s, b));
    state.PauseTiming();
    book.reset();  // exclude teardown
    state.ResumeTiming();
  }
  set_counters(state, kSessionMessages, s.size());
}
BENCHMARK(BM_ReplayReferenceBook)->Unit(benchmark::kMillisecond);

void BM_ReplayOrderBook(benchmark::State& state) {
  const auto& s = session();
  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(book_config());
    state.ResumeTiming();
    ItchBookBuilder b(*book);
    keep(itch::parse_stream(s, b));
    state.PauseTiming();
    book.reset();
    state.ResumeTiming();
  }
  set_counters(state, kSessionMessages, s.size());
}
BENCHMARK(BM_ReplayOrderBook)->Unit(benchmark::kMillisecond);

void BM_ReplayPipeline(benchmark::State& state) {
  const auto& s = session();
  Pipeline<> pipeline;
  for (auto _ : state) {
    state.PauseTiming();
    auto book = std::make_unique<OrderBook>(book_config());
    state.ResumeTiming();
    keep(pipeline.run(*book, std::span<const std::byte>{s}));
    state.PauseTiming();
    book.reset();
    state.ResumeTiming();
  }
  set_counters(state, kSessionMessages, s.size());
}
BENCHMARK(BM_ReplayPipeline)->Unit(benchmark::kMillisecond)->UseRealTime();

// ---------------------------------------------------------------------------
// Order-id lookup: std::unordered_map vs OrderIdMap
// ---------------------------------------------------------------------------

constexpr std::size_t kIds = 1 << 17;

std::vector<OrderId> lookup_keys() {
  std::vector<OrderId> keys(1 << 20);
  std::mt19937_64 rng(3);
  for (auto& k : keys) {
    k = 1 + rng() % kIds;  // all hits
  }
  return keys;
}

void BM_IdLookupUnorderedMap(benchmark::State& state) {
  std::unordered_map<OrderId, PoolIndex> m;
  m.reserve(kIds);
  for (OrderId i = 1; i <= kIds; ++i) {
    m.emplace(i, static_cast<PoolIndex>(i));
  }
  const auto keys = lookup_keys();
  for (auto _ : state) {
    std::uint64_t sum = 0;
    for (const OrderId k : keys) {
      sum += m.find(k)->second;
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * keys.size()));
}
BENCHMARK(BM_IdLookupUnorderedMap)->Unit(benchmark::kMillisecond);

void BM_IdLookupOrderIdMap(benchmark::State& state) {
  OrderIdMap m(kIds);
  for (OrderId i = 1; i <= kIds; ++i) {
    (void)m.insert(i, static_cast<PoolIndex>(i));
  }
  const auto keys = lookup_keys();
  for (auto _ : state) {
    std::uint64_t sum = 0;
    for (const OrderId k : keys) {
      sum += m.find(k);
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * keys.size()));
}
BENCHMARK(BM_IdLookupOrderIdMap)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// Matching engine
// ---------------------------------------------------------------------------

void BM_MatchingEngineMixedFlow(benchmark::State& state) {
  constexpr std::size_t kOps = 200'000;
  // Pre-generate requests so RNG cost is not measured.
  struct Op {
    NewOrder order;
    bool cancel;
  };
  std::vector<Op> ops(kOps);
  std::mt19937_64 rng(11);
  for (std::size_t i = 0; i < kOps; ++i) {
    const auto id = static_cast<OrderId>(i + 1);
    const bool cancel = rng() % 4 == 0;
    ops[i].cancel = cancel;
    ops[i].order = NewOrder{1,
                            cancel ? 1 + rng() % id : id,
                            rng() % 2 ? Side::kBuy : Side::kSell,
                            OrderType::kLimit,
                            rng() % 8 == 0 ? TimeInForce::kIoc : TimeInForce::kGtc,
                            Price{static_cast<std::uint32_t>(9'900 + rng() % 200)},
                            static_cast<Quantity>(1 + rng() % 500)};
  }
  struct Sink {
    std::uint64_t fills = 0;
    void on_fill(const Fill& /*f*/) noexcept { ++fills; }
  };

  for (auto _ : state) {
    state.PauseTiming();
    MatchingEngine::Config c;
    c.max_orders = 1 << 18;
    c.max_levels = 1 << 12;
    c.levels_per_side = 512;
    c.symbols_reserved = 1;
    auto engine = std::make_unique<MatchingEngine>(c);
    Sink sink;
    state.ResumeTiming();
    for (const Op& op : ops) {
      if (op.cancel) {
        keep(engine->cancel(op.order.id));
      } else {
        keep(engine->submit(op.order, sink));
      }
    }
    benchmark::DoNotOptimize(sink.fills);
    state.PauseTiming();
    engine.reset();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kOps));
}
BENCHMARK(BM_MatchingEngineMixedFlow)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
// SPSC ring: cross-thread transfer throughput
// ---------------------------------------------------------------------------

void BM_SpscRingTransfer(benchmark::State& state) {
  constexpr std::uint64_t kCount = 5'000'000;
  auto ring = std::make_unique<SpscRing<std::uint64_t, 1 << 14>>();
  for (auto _ : state) {
    std::thread consumer([&] {
      std::uint64_t v = 0;
      std::uint64_t got = 0;
      while (got < kCount) {
        if (ring->try_pop(v)) {
          ++got;
        }
      }
      benchmark::DoNotOptimize(v);
    });
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!ring->try_push(i)) {
      }
    }
    consumer.join();
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * kCount));
}
BENCHMARK(BM_SpscRingTransfer)->Unit(benchmark::kMillisecond)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
