# MatchCore Design

This document records the design decisions behind MatchCore and the reasoning
for each. It grows with the project; Phase 1 establishes the skeleton and the
constraints that later phases must honor.

## Goals and constraints

- Parse NASDAQ TotalView-ITCH 5.0 binary data and rebuild the limit order book.
- Match with strict price-time priority.
- **Zero heap allocation on the hot path.** All memory for orders and price
  levels is preallocated at startup via pool/arena allocators.
- No raw `new`/`delete`; RAII everywhere; `noexcept` where appropriate.

## Phase 1 decisions

### Fixed-point prices
ITCH `Price (4)` fields are 4-byte unsigned integers with 4 implied decimal
places. `matchcore::Price` stores the raw integer and never converts to
floating point in the hot path. This keeps comparisons exact and branch-free
and avoids FP rounding differences between the fast and reference books.

### Stock locate as an array index
ITCH assigns each symbol a `Stock Locate` code (starting at 1, daily). The spec
explicitly intends it as an array index. We will index the per-symbol book
table directly with it instead of hashing the 8-char symbol — O(1) with no hash
cost. `StockLocate` is therefore a first-class type from Phase 1.

### Build system
CMake with presets (GCC/Clang x Debug/Release, plus ASan/UBSan and TSan
presets). Warnings and sanitizer flags are isolated into INTERFACE targets so
they apply to MatchCore code but not to fetched third-party dependencies
(GoogleTest). GoogleTest is pulled via `FetchContent` pinned to a release tag
for reproducible builds without a system install.

## Decisions deferred to later phases

- Flat price-level layout vs `std::map` (Phase 3/4): why cache-friendly flat
  arrays / sorted vectors win for the book, with measured before/after numbers.
- Custom open-addressing hash map for order-id lookup (Phase 4).
- SPSC lock-free ring buffer and why acquire/release ordering is sufficient
  (Phase 6).
- Pool/arena allocator design (Phase 4/5).

Each of these will be documented here with the rationale and the benchmark that
justifies it.
