#!/usr/bin/env bash
# Sample the replay benchmark with perf and print the hottest symbols.
# Optional: pass --flamegraph to also render flamegraph.svg (needs
# https://github.com/brendangregg/FlameGraph on PATH: stackcollapse-perf.pl,
# flamegraph.pl).
#
# usage: tools/profile.sh [--flamegraph]
set -euo pipefail

src=$(cd "$(dirname "$0")/.." && pwd)
dir="${MATCHCORE_BUILD_ROOT:-$HOME/mc-build}/profile"

# Release optimizations, plus frame pointers and symbols for usable stacks.
CC=${CC:-gcc} CXX=${CXX:-g++} cmake -S "$src" -B "$dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMATCHCORE_BUILD_TESTS=OFF \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O3 -DNDEBUG -g -fno-omit-frame-pointer" >/dev/null
cmake --build "$dir" >/dev/null

out="$dir/perf.data"
perf record -F 2999 -g -o "$out" -- "$dir/bin/replay_bench" --synthetic 5000000 --runs 2 >/dev/null 2>&1
perf report -i "$out" --stdio --no-children --percent-limit 2 --sort symbol -g none 2>/dev/null |
  grep -vE '^#|^$' | head -25

if [[ "${1:-}" == "--flamegraph" ]]; then
  perf script -i "$out" | stackcollapse-perf.pl | flamegraph.pl > "$src/docs/flamegraph.svg"
  echo "wrote docs/flamegraph.svg"
fi
