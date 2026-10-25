#ifndef MATCHCORE_BOOK_INDEX_POOL_HPP
#define MATCHCORE_BOOK_INDEX_POOL_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace matchcore {

/// 32-bit handle into a pool. Half the size of a pointer, which matters for
/// the intrusive links stored in every order node.
using PoolIndex = std::uint32_t;

/// Sentinel "null" handle.
inline constexpr PoolIndex kNullIndex = std::numeric_limits<PoolIndex>::max();

/// Fixed-capacity object pool addressed by 32-bit indices.
///
/// All storage is allocated once in the constructor. allocate()/release() are
/// O(1) free-list operations and never touch the heap, so the pool can be used
/// on the hot path. When the pool is exhausted allocate() returns kNullIndex
/// instead of growing: running out of capacity is a configuration problem that
/// must surface, not be papered over with a hidden allocation.
template <class T>
class IndexPool {
 public:
  explicit IndexPool(std::size_t capacity)
      : items_(capacity), next_free_(capacity), free_head_(capacity == 0 ? kNullIndex : 0) {
    assert(capacity < kNullIndex);
    for (std::size_t i = 0; i < capacity; ++i) {
      next_free_[i] = (i + 1 < capacity) ? static_cast<PoolIndex>(i + 1) : kNullIndex;
    }
  }

  /// Take a free slot. The slot's previous contents are left as-is; callers
  /// overwrite every field. Returns kNullIndex when the pool is full.
  [[nodiscard]] PoolIndex allocate() noexcept {
    const PoolIndex idx = free_head_;
    if (idx != kNullIndex) {
      free_head_ = next_free_[idx];
      ++in_use_;
    }
    return idx;
  }

  /// Return a slot to the pool.
  void release(PoolIndex idx) noexcept {
    assert(idx < items_.size());
    next_free_[idx] = free_head_;
    free_head_ = idx;
    --in_use_;
  }

  [[nodiscard]] T& operator[](PoolIndex idx) noexcept {
    assert(idx < items_.size());
    return items_[idx];
  }
  [[nodiscard]] const T& operator[](PoolIndex idx) const noexcept {
    assert(idx < items_.size());
    return items_[idx];
  }

  [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return items_.size(); }

 private:
  std::vector<T> items_;
  std::vector<PoolIndex> next_free_;  // free-list links, kept apart from T
  PoolIndex free_head_;
  std::size_t in_use_{0};
};

}  // namespace matchcore

#endif  // MATCHCORE_BOOK_INDEX_POOL_HPP
