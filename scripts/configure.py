#!/usr/bin/env python3
"""Configure the project with a CMake preset, in the developer environment.

    python scripts/configure.py                     # the platform's Debug Ninja preset
    python scripts/configure.py windows-vs2022      # any configure preset from CMakePresets.json
    python scripts/configure.py --fresh             # discard a stale CMake cache first

The first run lets vcpkg build the dependencies (a few minutes).
"""

import argparse
import sys

import devenv


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("preset", nargs="?", default=devenv.default_preset(),
                        help="configure preset (default: %(default)s)")
    parser.add_argument("--fresh", action="store_true", help="pass --fresh to cmake")
    args = parser.parse_args()

    command = ["cmake", "--preset", args.preset]
    if args.fresh:
        command.append("--fresh")
    devenv.run(command)
    return 0


if __name__ == "__main__":
    sys.exit(main())
