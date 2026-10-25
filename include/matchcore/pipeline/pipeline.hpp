#ifndef MATCHCORE_PIPELINE_PIPELINE_HPP
#define MATCHCORE_PIPELINE_PIPELINE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>

#include "matchcore/itch/parser.hpp"
#include "matchcore/pipeline/book_event.hpp"
#include "matchcore/pipeline/spsc_ring.hpp"

namespace matchcore {

/// Counters from one pipeline run.
struct PipelineStats {
  std::uint64_t events{0};           ///< book events passed through the ring
  std::uint64_t producer_stalls{0};  ///< times the parser found the ring full
  std::array<std::uint64_t, 5> results{};  ///< BookResult counts, by enum value
};

namespace detail {

/// Spin briefly, then yield. Keeps latency low when the other side is about
/// to catch up, without burning a core indefinitely if it is descheduled
/// (which matters on WSL2 and small CI VMs with few cores).
class Backoff {
 public:
  void pause() noexcept {
    if (++spins_ > kSpinLimit) {
      std::this_thread::yield();
    }
  }
  void reset() noexcept { spins_ = 0; }

 private:
  static constexpr unsigned kSpinLimit = 64;
  unsigned spins_{0};
};

}  // namespace detail

/// Two-thread market-data pipeline:
///
///   [caller thread]  parse ITCH -> BookEvent --SPSC ring--> [engine thread] apply to book
///
/// The calling thread is the producer (it runs `parse_fn`); a dedicated
/// consumer thread owns the book for the duration of run(). The book must not
/// be touched by anyone else until run() returns; run() joins the consumer
/// before returning, which also publishes the final book state to the caller.
///
/// The ring is allocated once per Pipeline object; run() itself does not
/// allocate apart from starting the consumer thread.
template <std::size_t RingCapacity = std::size_t{1} << 16U>
class Pipeline {
 public:
  using Ring = SpscRing<BookEvent, RingCapacity>;

  Pipeline() : ring_(std::make_unique<Ring>()) {}

  /// `parse_fn(handler)` must feed every ITCH message to `handler` (for
  /// example by calling itch::parse_stream over a file chunk by chunk).
  template <class Book, class ParseFn>
  PipelineStats run(Book& book, ParseFn&& parse_fn) {
    PipelineStats stats;
    Ring& ring = *ring_;

    std::thread consumer([&ring, &book, &stats] {
      detail::Backoff backoff;
      BookEvent e;
      std::uint64_t events = 0;
      std::array<std::uint64_t, 5> results{};
      while (true) {
        if (!ring.try_pop(e)) {
          backoff.pause();
          continue;
        }
        backoff.reset();
        if (e.kind == BookEvent::Kind::kEnd) {
          break;
        }
        ++results[static_cast<std::size_t>(apply(book, e))];
        ++events;
      }
      stats.events = events;
      stats.results = results;
    });

    std::uint64_t stalls = 0;
    auto push = [&ring, &stalls](const BookEvent& ev) noexcept {
      if (ring.try_push(ev)) {
        return;
      }
      ++stalls;
      detail::Backoff backoff;
      while (!ring.try_push(ev)) {
        backoff.pause();
      }
    };

    BookEventEncoder encoder(push);
    parse_fn(encoder);
    push(BookEvent{});  // default Kind is kEnd

    consumer.join();
    stats.producer_stalls = stalls;
    return stats;
  }

  /// Convenience: run over an in-memory length-prefixed ITCH buffer.
  template <class Book>
  PipelineStats run(Book& book, std::span<const std::byte> stream) {
    return run(book, [stream](auto& handler) { (void)itch::parse_stream(stream, handler); });
  }

 private:
  std::unique_ptr<Ring> ring_;
};

}  // namespace matchcore

#endif  // MATCHCORE_PIPELINE_PIPELINE_HPP
