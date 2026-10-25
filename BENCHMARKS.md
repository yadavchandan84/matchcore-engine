# MatchCore Benchmarks

All numbers in this file are **measured**: they are pasted from actual
command output and never estimated. The machine spec comes first so the
results are reproducible.

## Environment

> Note: this runs on **Windows 11 + WSL2**. Timer noise is higher than on bare
> metal — expect wider tails (p99.9) than a native Linux host. Benchmarks will
> use warmup iterations and multiple runs to compensate.

### OS / kernel

```
Linux LAPTOP-4EORQK1G 6.6.87.2-microsoft-standard-WSL2 #1 SMP PREEMPT_DYNAMIC Thu Jun  5 18:30:46 UTC 2025 x86_64 GNU/Linux
```

### CPU (`lscpu`)

```
Architecture:             x86_64
  CPU op-mode(s):         32-bit, 64-bit
  Byte Order:             Little Endian
CPU(s):                   16
  On-line CPU(s) list:    0-15
Vendor ID:                AuthenticAMD
  Model name:             AMD Ryzen 7 5800H with Radeon Graphics
    CPU family:           25
    Model:                80
    Thread(s) per core:   2
    Core(s) per socket:   8
    Socket(s):            1
Caches (sum of all):
  L1d:                    256 KiB (8 instances)
  L1i:                    256 KiB (8 instances)
  L2:                     4 MiB (8 instances)
  L3:                     16 MiB (1 instance)
NUMA node0 CPU(s):        0-15
```

### Memory (`free -h`)

```
               total        used        free      shared  buff/cache   available
Mem:           7.4Gi       582Mi       6.6Gi       3.5Mi       435Mi       6.9Gi
Swap:          2.0Gi          0B       2.0Gi
```

### Toolchain

```
cmake   4.2.3
ninja   1.13.2
gcc     15.2.0 (Ubuntu 15.2.0-16ubuntu1)
g++     15.2.0
clang   21.1.8 (Ubuntu clang version 21.1.8)
clang++ 21.1.8
perf    7.0.14
git     2.53.0
gh      2.46.0
```

## Results

Measured on the machine above, GCC 15.2 `-O3` (CMake `Release`), on
2026-10-26. Reproduce everything below with:

```bash
tools/run_benchmarks.sh            # optional: pass a real .NASDAQ_ITCH50 file
```

**Input.** The real NASDAQ sample file is not in this environment, so all
numbers use the deterministic synthetic session from `itch::generate_session`
(100 symbols, about 20k resting orders, NASDAQ-like mix: 45% add, 30% delete,
10% replace, 8% execute, 7% cancel). Every event is valid, so both books apply
100% of them. Real files have a different symbol count, depth and id
distribution, so absolute numbers on real data will differ. The real-file
replay row will be added once `run_benchmarks.sh <file>` is run against one.

### Optimization steps: end-to-end replay (parse + book rebuild)

`replay_bench --synthetic 5000000`. Throughput is the median of the timed
runs, each on a fresh book after one discarded warm-up. Latency is
`steady_clock` around each `parse_message()` call (decode + book update),
pooled over all runs. The timer adds about 20 ns, which is **not**
subtracted.

| Step | Book | Throughput | p50 | p99 | p99.9 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1. Naive reference | `std::map` levels, `std::list` FIFO, `std::unordered_map` ids | 5.20 M msgs/s | 201 ns | 661 ns | 1684 ns |
| 2. Flat ladder + intrusive FIFO + pools + open addressing, **mixing hash** | `OrderBook`, Murmur finalizer | 6.02 M msgs/s | 190 ns | 521 ns | 1282 ns |
| 3. Same, **identity hash** (final) | `OrderBook` | **14.67 M msgs/s** | **80 ns** | **311 ns** | **751 ns** |

After these measurements, the synthetic generator was changed to fixed,
non-crossing per-symbol mids, so its book can never be crossed. A re-run on
the new data gave 14.20 M msgs/s (p50 80 / p99 311 / p99.9 681 ns) for
`OrderBook` and 4.95 M msgs/s for the reference book. That is within
run-to-run noise of the table.

Step 2 → 3 was the largest single gain, and it came from the order-id hash,
not the ladder. ITCH order ids are sequential, so the live orders form a
sliding window of nearby ids. A mixing hash scatters that window across a
2 MiB table, so almost every lookup misses cache. The identity hash maps it
onto adjacent slots instead. The isolated lookup benchmark below shows the
same effect.

### Microbenchmarks (Google Benchmark)

`matchcore_bench --benchmark_repetitions=5`, median of 5. The session is
1M order messages (about 30 MB).

| Benchmark | What it measures | Time | Rate |
| --- | --- | ---: | ---: |
| `BM_ParseFramingOnly` | framing + type dispatch + length check, no decode | 7.42 ms | 134.8 M msgs/s, 3.85 GiB/s |
| `BM_ParseDecodeAll` | full decode of every message, trivial handler | 9.52 ms | 105.0 M msgs/s, 3.00 GiB/s |
| `BM_ReplayReferenceBook` | parse + `ReferenceBook` | 284 ms | 3.52 M msgs/s |
| `BM_ReplayOrderBook` | parse + `OrderBook` | 85.6 ms | 11.69 M msgs/s |
| `BM_ReplayPipeline` | parser thread → SPSC ring → book thread (wall time) | 80.3 ms | 12.46 M msgs/s |
| `BM_IdLookupUnorderedMap` | 1M hits on 131k sequential ids | 4.70 ms | 223 M lookups/s |
| `BM_IdLookupOrderIdMap` | same, `OrderIdMap` (identity hash) | 1.98 ms | 530 M lookups/s |
| `BM_MatchingEngineMixedFlow` | 200k limit/IOC submits + cancels, matching | 9.18 ms | 21.8 M requests/s |
| `BM_SpscRingTransfer` | 5M `uint64_t` across two threads | 39.3 ms | 127 M items/s |

With the mixing hash, `BM_IdLookupOrderIdMap` measured 124 M lookups/s,
slower than `std::unordered_map`. libstdc++ hashes integers with the
identity function, so it already had the locality advantage. That
measurement is what led to the switch.

### Reading the numbers honestly

- **Noise.** `BM_ReplayReferenceBook` measured 5.40 M msgs/s in an earlier
  run of the same binary and 3.52 M msgs/s in this one. The allocation-heavy
  reference book is the most sensitive to WSL2 and host noise. Compare rows
  from the same run.
- **Pipeline.** Parsing is about 10 ms of the about 85 ms replay. Moving it
  to its own thread can save at most that, which matches the 6% measured
  gain. The pipeline matters more when the producer does real I/O and
  decompression, or when the engine must never stall behind the parser.
- **Tails.** The max latency (hundreds of µs to ms) is preemption by the
  WSL2 VM and host, not engine work. p99.9 is the more meaningful tail on
  this machine.
- **Zero allocation** is checked, not just claimed. `alloc_test` counts
  global `operator new` calls during a 1M-message replay and 200k engine
  requests, and the result is 0 for both.

## Profile

`tools/profile.sh` builds `-O3 -g -fno-omit-frame-pointer` and samples
`replay_bench` with `perf record`. `perf record` sampling works under this
WSL2 kernel. Top symbols (2% cutoff):

```
15.31%  matchcore::OrderBook::unlink_order(unsigned int)
14.15%  __vdso_clock_gettime                      <- latency-pass instrumentation
10.84%  matchcore::itch::generate_session(...)    <- input setup, not the hot path
 9.99%  matchcore::OrderBook::remove(unsigned long)
 9.68%  matchcore::OrderBook::find_or_create_level(...)
 9.46%  matchcore::OrderBook::add(...)
 7.17%  matchcore::OrderBook::reduce(unsigned long, unsigned int)
 6.40%  matchcore::itch::parse_message<ItchBookBuilder<OrderBook>>(...)
 4.54%  matchcore::OrderBook::replace(...)
```

Book mutation dominates. The parser is about 6% of samples. Within the
book, `unlink_order` and `remove` are the order-id lookup plus the
dependent loads of the order and level nodes. The next optimization would
be co-locating hot `Order` fields or prefetching the id slot. A flame graph
needs Brendan Gregg's FlameGraph scripts on `PATH`:
`tools/profile.sh --flamegraph` writes `docs/flamegraph.svg`.
