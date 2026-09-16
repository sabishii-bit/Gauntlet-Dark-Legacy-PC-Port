#!/usr/bin/env bash
# Builds (and optionally tests or runs) the project with a CMake build preset on Linux.
#
#   ./scripts/build.sh                             # linux-ninja-debug
#   ./scripts/build.sh linux-ninja-debug --test    # build, then run the unit tests
#   ./scripts/build.sh linux-clang-debug --run -- --no-vsync --frames 300
#
# Arguments after "--" are passed to the executable.

set -euo pipefail

preset="${1:-linux-ninja-debug}"
shift || true

run=0
test=0
app_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --run) run=1; shift ;;
        --test) test=1; shift ;;
        --) shift; app_args=("$@"); break ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

if [[ -z "${VCPKG_ROOT:-}" && -x "$HOME/vcpkg/vcpkg" ]]; then
    export VCPKG_ROOT="$HOME/vcpkg"
fi

binary_dir="build/$preset"
if [[ ! -f "$binary_dir/CMakeCache.txt" ]]; then
    cmake --preset "$preset"
fi

cmake --build --preset "$preset"

if [[ $test -eq 1 ]]; then
    ctest --preset "$preset" -LE gpu
fi

if [[ $run -eq 1 ]]; then
    exec "$binary_dir/bin/gauntlet" "${app_args[@]}"
fi
