#!/usr/bin/env bash
# Run every check for MatchCore in one go: the full CI build matrix,
# sanitizers, the demo, the ITCH tools, and the replay benchmark.
#
# usage: bash tools/verify_all.sh            # everything except the long benchmark suite
#        bash tools/verify_all.sh --bench    # also run tools/run_benchmarks.sh (several minutes)
set -euo pipefail

cd "$(dirname "$0")/.."
bin="${MATCHCORE_BUILD_ROOT:-$HOME/mc-build}/gcc-Release/bin"
fixture=/tmp/matchcore_verify.NASDAQ_ITCH50
trap 'rm -f "$fixture"' EXIT

step() { printf '\n==== %s ====\n' "$1"; }

step "1/5 Build + test: GCC and Clang, Debug and Release"
bash tools/check.sh gcc Release
bash tools/check.sh gcc Debug
bash tools/check.sh clang Debug
bash tools/check.sh clang Release

step "2/5 Sanitizers: ASan + UBSan, TSan"
bash tools/check.sh clang Debug asan
bash tools/check.sh clang Debug tsan

step "3/5 Demo"
"$bin/matchcore_demo"

step "4/5 ITCH tools: generate a 2M-message file and inspect it"
"$bin/itch_gen" "$fixture" 2000000 50
"$bin/itch_inspect" "$fixture"

step "5/5 Replay benchmark (5M messages)"
"$bin/replay_bench" --synthetic 5000000

if [[ "${1:-}" == "--bench" ]]; then
  step "Full benchmark suite"
  bash tools/run_benchmarks.sh
fi

printf '\nALL CHECKS PASSED\n'
