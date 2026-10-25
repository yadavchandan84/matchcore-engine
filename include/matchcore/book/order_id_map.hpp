#ifndef MATCHCORE_BOOK_ORDER_ID_MAP_HPP
#define MATCHCORE_BOOK_ORDER_ID_MAP_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "matchcore/book/index_pool.hpp"
#include "matchcore/types.hpp"

namespace matchcore {

/// OrderId -> PoolIndex hash map with open addressing and linear probing.
///
/// Why not std::unordered_map: it allocates a node per insert and chases a
/// pointer per lookup. Here the table is one flat array allocated up front,
/// a lookup is a hash plus a short linear scan over adjacent 16-byte slots
/// (usually within one cache line), and insert/erase never allocate.
///
/// - Capacity is a power of two >= 2 * max_entries, so the load factor never
///   exceeds 0.5 and probe sequences stay short.
/// - Erase uses backward-shift deletion instead of tombstones, so long-running
///   insert/erase churn (the normal ITCH pattern) never degrades lookups.
/// - An empty slot is marked by value == kNullIndex, so every OrderId,
///   including 0, is a valid key.
class OrderIdMap {
 public:
  explicit OrderIdMap(std::size_t max_entries)
      : slots_(std::bit_ceil(max_entries < 4 ? std::size_t{8} : max_entries * 2)),
        mask_(slots_.size() - 1),
        max_entries_(max_entries) {}

  /// Value for `key`, or kNullIndex if absent.
  [[nodiscard]] PoolIndex find(OrderId key) const noexcept {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.value == kNullIndex) {
        return kNullIndex;
      }
      if (s.key == key) {
        return s.value;
      }
    }
  }

  /// Insert a new key. Returns false if the key already exists or the map is
  /// at its configured maximum. `value` must not be kNullIndex.
  [[nodiscard]] bool insert(OrderId key, PoolIndex value) noexcept {
    if (size_ >= max_entries_) {
      return false;
    }
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.value == kNullIndex) {
        s = {key, value};
        ++size_;
        return true;
      }
      if (s.key == key) {
        return false;
      }
    }
  }

  /// Remove `key`. Returns false if it was not present.
  bool erase(OrderId key) noexcept {
    std::size_t hole = home(key);
    for (;; hole = (hole + 1) & mask_) {
      if (slots_[hole].value == kNullIndex) {
        return false;
      }
      if (slots_[hole].key == key) {
        break;
      }
    }

    // Backward-shift: walk the cluster after the hole and move back any entry
    // whose home slot is not cyclically within (hole, j]. Such an entry would
    // otherwise become unreachable once the hole is emptied.
    for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kNullIndex; j = (j + 1) & mask_) {
      const std::size_t k = home(slots_[j].key);
      const bool stays = (hole <= j) ? (hole < k && k <= j) : (hole < k || k <= j);
      if (!stays) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kNullIndex;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t max_entries() const noexcept { return max_entries_; }
  [[nodiscard]] std::size_t slot_count() const noexcept { return slots_.size(); }

 private:
  struct Slot {
    OrderId key{0};
    PoolIndex value{kNullIndex};
  };

  // Home slot of a key.
  //
  // Default: identity (key & mask). ITCH order reference numbers are issued
  // sequentially, so live orders form a sliding window of nearby ids; identity
  // maps that window onto adjacent slots, turning lookups into near-sequential
  // memory access with almost no collisions. Measured on the replay benchmark
  // this is ~2.4x faster end-to-end than a mixing hash, which scatters
  // neighbouring ids across the whole (cache-missing) table. See BENCHMARKS.md.
  //
  // For id streams that are not sequential or may be adversarial (e.g. ids
  // that are multiples of the table size), define MATCHCORE_IDMAP_MIX_HASH to
  // use a MurmurHash3 64-bit finalizer instead. Correctness is identical
  // either way; only the probe-length distribution changes.
  [[nodiscard]] std::size_t home(OrderId key) const noexcept {
#ifdef MATCHCORE_IDMAP_MIX_HASH
    std::uint64_t h = key;
    h ^= h >> 33U;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33U;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33U;
    return static_cast<std::size_t>(h) & mask_;
#else
    return static_cast<std::size_t>(key) & mask_;
#endif
  }

  std::vector<Slot> slots_;
  std::size_t mask_;
  std::size_t max_entries_;
  std::size_t size_{0};
};

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_ORDER_ID_MAP_HPP
