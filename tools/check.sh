#!/usr/bin/env bash
# Build and test one configuration out-of-tree (fast on WSL: build dir lives on
# the Linux filesystem, not the mounted Windows drive).
#
# usage: tools/check.sh <gcc|clang> <Debug|Release> [asan|tsan]
set -euo pipefail

compiler=${1:?compiler}
build_type=${2:?build type}
san=${3:-}

case "$compiler" in
  gcc) cc=gcc; cxx=g++ ;;
  clang) cc=clang; cxx=clang++ ;;
  *) echo "unknown compiler: $compiler" >&2; exit 1 ;;
esac

extra=()
case "$san" in
  "") ;;
  asan) extra+=(-DMATCHCORE_ENABLE_ASAN=ON -DMATCHCORE_ENABLE_UBSAN=ON) ;;
  tsan) extra+=(-DMATCHCORE_ENABLE_TSAN=ON) ;;
  *) echo "unknown sanitizer: $san" >&2; exit 1 ;;
esac

src=$(cd "$(dirname "$0")/.." && pwd)
dir="${MATCHCORE_BUILD_ROOT:-$HOME/mc-build}/${compiler}-${build_type}${san:+-$san}"

CC=$cc CXX=$cxx cmake -S "$src" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" "${extra[@]}" >/dev/null
log=$(cmake --build "$dir" 2>&1) || { echo "$log"; echo "BUILD FAILED: $dir"; exit 1; }
warnings=$(grep -cE 'warning:' <<<"$log" || true)
if [[ "$warnings" != 0 ]]; then grep -E 'warning:' <<<"$log" | sort -u; fi
result=$(ctest --test-dir "$dir" --output-on-failure 2>&1) || { echo "$result" | tail -40; echo "TESTS FAILED: $dir"; exit 1; }
echo "$dir: warnings=$warnings :: $(grep 'tests passed' <<<"$result")"
