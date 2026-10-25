// Zero-allocation proof for the hot path.
//
// Replaces the global allocation functions with counting versions, sets up
// every component (allocations allowed), then runs the hot path with counting
// enabled and fails if a single heap allocation happens:
//
//   1. ITCH parse + full book rebuild of a 1M-message synthetic session.
//   2. 200k matching-engine submissions / cancels / modifies.
//
// Plain main() rather than GoogleTest: gtest itself allocates while running a
// test, which would pollute the count.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "matchcore/book/itch_book_builder.hpp"
#include "matchcore/book/order_book.hpp"
#include "matchcore/engine/matching_engine.hpp"
#include "matchcore/itch/parser.hpp"
#include "matchcore/itch/synthetic.hpp"

namespace {
std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void* counted_alloc(std::size_t n) {
  if (g_counting.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* p = std::malloc(n == 0 ? 1 : n)) {  // NOLINT(*-no-malloc)
    return p;
  }
  throw std::bad_alloc();
}
}  // namespace

// NOLINTBEGIN(*-no-malloc, *-owning-memory)
void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t& /*tag*/) noexcept {
  try {
    return counted_alloc(n);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept {
  return operator new(n, tag);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t /*n*/) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t /*n*/) noexcept { std::free(p); }
// NOLINTEND(*-no-malloc, *-owning-memory)

namespace {

using namespace matchcore;

struct Counting {
  Counting() {
    g_allocations.store(0);
    g_counting.store(true);
  }
  ~Counting() { g_counting.store(false); }
  Counting(const Counting&) = delete;
  Counting& operator=(const Counting&) = delete;
  Counting(Counting&&) = delete;
  Counting& operator=(Counting&&) = delete;
};

bool check(const char* what, std::size_t allocations) {
  std::printf("%-44s heap allocations: %zu  %s\n", what, allocations,
              allocations == 0 ? "OK" : "FAIL");
  return allocations == 0;
}

bool replay_is_allocation_free() {
  itch::SyntheticConfig cfg;
  cfg.messages = 1'000'000;
  cfg.symbols = 100;
  const auto session = itch::generate_session(cfg);  // setup: allowed to allocate

  OrderBook::Config bc;
  bc.max_orders = 1 << 17;
  bc.max_levels = 1 << 16;
  bc.levels_per_side = 1024;  // reserve ladder depth up front
  bc.symbols_reserved = cfg.symbols;
  OrderBook book(bc);
  ItchBookBuilder builder(book);

  std::size_t allocs = 0;
  itch::StreamResult r;
  {
    const Counting c;
    r = itch::parse_stream(session, builder);
    allocs = g_allocations.load();
  }
  if (builder.applied() != cfg.messages || r.malformed != 0) {
    std::printf("replay did not apply cleanly (applied=%llu)\n",
                static_cast<unsigned long long>(builder.applied()));
    return false;
  }
  return check("ITCH parse + book rebuild (1M messages)", allocs);
}

bool matching_is_allocation_free() {
  MatchingEngine::Config ec;
  ec.max_orders = 1 << 16;
  ec.max_levels = 1 << 12;
  ec.levels_per_side = 512;
  ec.symbols_reserved = 1;
  MatchingEngine engine(ec);

  struct Sink {
    std::uint64_t fills = 0;
    void on_fill(const Fill& /*f*/) noexcept { ++fills; }
  } sink;

  std::size_t allocs = 0;
  {
    const Counting c;
    std::uint64_t x = 88172645463325252ULL;  // xorshift: no <random> objects
    auto rnd = [&x] {
      x ^= x << 13U;
      x ^= x >> 7U;
      x ^= x << 17U;
      return x;
    };
    for (OrderId id = 1; id <= 200'000; ++id) {
      const auto r = rnd() % 10;
      if (r < 7) {
        NewOrder o{1,
                   id,
                   (rnd() & 1U) != 0 ? Side::kBuy : Side::kSell,
                   OrderType::kLimit,
                   r == 0 ? TimeInForce::kIoc : TimeInForce::kGtc,
                   Price{static_cast<std::uint32_t>(9'900 + rnd() % 200)},
                   static_cast<Quantity>(1 + rnd() % 500)};
        (void)engine.submit(o, sink);
      } else if (r < 9) {
        (void)engine.cancel(1 + rnd() % id);
      } else {
        (void)engine.modify(1 + rnd() % id, Price{static_cast<std::uint32_t>(9'900 + rnd() % 200)},
                            static_cast<Quantity>(1 + rnd() % 500), sink);
      }
    }
    allocs = g_allocations.load();
  }
  std::printf("  (%llu fills)\n", static_cast<unsigned long long>(sink.fills));
  return check("Matching engine (200k requests)", allocs);
}

}  // namespace

int main() {
  const bool a = replay_is_allocation_free();
  const bool b = matching_is_allocation_free();
  return (a && b) ? EXIT_SUCCESS : EXIT_FAILURE;
}
