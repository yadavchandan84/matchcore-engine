#ifndef MATCHCORE_PIPELINE_SPSC_RING_HPP
#define MATCHCORE_PIPELINE_SPSC_RING_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace matchcore {

/// Cache line size assumed for padding. 64 bytes on every x86-64 and most
/// ARM64 cores. (std::hardware_destructive_interference_size is not used
/// because GCC warns that its value is ABI-unstable.)
inline constexpr std::size_t kCacheLine = 64;

/// Bounded single-producer / single-consumer lock-free ring buffer.
///
/// Exactly one thread may call try_push() and exactly one (other) thread may
/// call try_pop(). Under that contract no locks or CAS loops are needed:
///
///   - `head_` is written only by the consumer, `tail_` only by the producer.
///   - The producer writes the slot, then publishes it with a release store to
///     `tail_`. The consumer's acquire load of `tail_` therefore sees the slot
///     contents (release/acquire pairing gives the happens-before edge). The
///     symmetric pairing on `head_` tells the producer a slot is free again.
///     Nothing stronger (seq_cst) is required because each index has a single
///     writer and there is no ordering relationship between the two indices
///     that both threads must agree on.
///   - Each side caches its last view of the other side's index and refreshes
///     it only when the ring looks full/empty, so in steady state a push or
///     pop touches no cache line owned by the other core.
///   - Producer state, consumer state and the slot array sit on separate cache
///     lines to avoid false sharing.
///
/// Indices are free-running 64-bit counters; Capacity must be a power of two
/// so `index & (Capacity - 1)` maps them to slots. Full capacity is usable.
template <class T, std::size_t Capacity>
class SpscRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "slots are copied without destructors");

 public:
  /// Producer only. Returns false if the ring is full.
  bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - producer_cached_head_ == Capacity) {
      producer_cached_head_ = head_.load(std::memory_order_acquire);
      if (tail - producer_cached_head_ == Capacity) {
        return false;
      }
    }
    slots_[tail & kMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  /// Consumer only. Returns false if the ring is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == consumer_cached_tail_) {
      consumer_cached_tail_ = tail_.load(std::memory_order_acquire);
      if (head == consumer_cached_tail_) {
        return false;
      }
    }
    out = slots_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Approximate when called concurrently; exact when the ring is quiescent.
  [[nodiscard]] std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

  static constexpr std::size_t capacity() noexcept { return Capacity; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  // Producer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t producer_cached_head_{0};

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t consumer_cached_tail_{0};

  alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

}  // namespace matchcore

#endif  // MATCHCORE_PIPELINE_SPSC_RING_HPP
