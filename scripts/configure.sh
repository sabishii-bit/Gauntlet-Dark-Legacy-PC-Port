#!/usr/bin/env bash
# Configures the project with a CMake preset on Linux.
#
#   ./scripts/configure.sh                 # linux-ninja-debug
#   ./scripts/configure.sh linux-clang-debug
#
# Requires VCPKG_ROOT (see README.md for the vcpkg and apt package setup).
# The first run lets vcpkg build the dependencies (a few minutes).

set -euo pipefail

preset="${1:-linux-ninja-debug}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${VCPKG_ROOT:-}" ]]; then
    if [[ -x "$HOME/vcpkg/vcpkg" ]]; then
        export VCPKG_ROOT="$HOME/vcpkg"
    else
        echo "VCPKG_ROOT is not set. Point it at your vcpkg checkout (https://github.com/microsoft/vcpkg)." >&2
        exit 1
    fi
fi

cd "$root"
cmake --preset "$preset"
