# MatchCore Benchmarks

All numbers in this file are **measured**, pasted from actual command output —
never estimated. The benchmark harness (Google Benchmark + a replay benchmark)
lands in Phase 7. This phase records only the machine specification so later
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

_No benchmark results yet._ The optimization table (naive `std::map` → flat
levels → pool allocator → SPSC pipeline) with throughput (msgs/sec) and latency
p50/p99/p99.9 is produced in Phase 7.

## perf flame graphs

`perf stat true` works under this WSL2 setup, so basic counter collection is
available. Full instructions and the WSL2 fallback (if `perf record` sampling
is restricted) will be documented alongside the Phase 7 benchmarks.
