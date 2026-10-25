#!/usr/bin/env bash
# Build the Release + benchmark configuration and run every benchmark that
# BENCHMARKS.md reports. Output goes to stdout; paste it into BENCHMARKS.md.
#
# usage: tools/run_benchmarks.sh [path/to/file.NASDAQ_ITCH50]
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
dir="${MATCHCORE_BUILD_ROOT:-$HOME/mc-build}/bench"

CC=${CC:-gcc} CXX=${CXX:-g++} cmake -S "$src" -B "$dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMATCHCORE_BUILD_BENCHMARKS=ON >/dev/null
cmake --build "$dir" >/dev/null

bin="$dir/bin"

echo "### Microbenchmarks (Google Benchmark, 5 repetitions, median shown)"
"$bin/matchcore_bench" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_min_warmup_time=0.5 2>&1 | grep -vE '_mean|_stddev|_cv'

echo
echo "### Replay: optimized OrderBook (synthetic, 5M messages)"
"$bin/replay_bench" --synthetic 5000000 --runs 5

echo
echo "### Replay: ReferenceBook (synthetic, 5M messages)"
"$bin/replay_bench" --synthetic 5000000 --runs 3 --book ref

if [[ $# -ge 1 ]]; then
  echo
  echo "### Replay: optimized OrderBook (real file: $1)"
  "$bin/replay_bench" --file "$1" --runs 3
fi
