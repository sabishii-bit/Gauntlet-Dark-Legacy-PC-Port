#!/usr/bin/env python3
"""Build the project with a CMake build preset, then optionally test, unpack or run.

    python scripts/build.py                          # the platform's Debug Ninja preset
    python scripts/build.py --test                   # build, then run the unit tests
    python scripts/build.py --unpack                 # build, then unpack the console assets
    python scripts/build.py --run -- --title         # build, then launch the game with arguments
    python scripts/build.py linux-clang-release      # any build preset from CMakePresets.json

Configures first when the preset has never been configured. Arguments after "--"
go to the game.
"""

import argparse
import json
import pathlib
import re
import sys

import devenv

ROOT = devenv.ROOT
EXE = ".exe" if devenv.WINDOWS else ""


def build_presets() -> dict[str, str]:
    """Build preset name -> configure preset name."""
    presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
    return {p["name"]: p["configurePreset"] for p in presets["buildPresets"]}


def cache_path(binary_dir: pathlib.Path, variable: str, default: pathlib.Path) -> pathlib.Path:
    """A PATH entry from CMakeCache.txt, or `default` when it is absent."""
    cache = binary_dir / "CMakeCache.txt"
    match = re.search(rf"^{variable}:(?:PATH|FILEPATH|STRING)=(.*)$", cache.read_text(encoding="utf-8"),
                      re.MULTILINE)
    return pathlib.Path(match.group(1)) if match and match.group(1) else default


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("preset", nargs="?", default=devenv.default_preset(),
                        help="build preset (default: %(default)s)")
    parser.add_argument("--test", action="store_true", help="run the unit tests (ctest -LE gpu)")
    parser.add_argument("--unpack", action="store_true",
                        help="run gdlunpack from the asset directory into the unpacked directory")
    parser.add_argument("--levels", action="store_true",
                        help="with --unpack: also unpack the level folders (about 20 MB each)")
    parser.add_argument("--run", action="store_true", help="launch the game afterwards")
    parser.epilog = "Arguments after -- are passed to the game."
    argv = sys.argv[1:]
    app_args: list[str] = []
    if "--" in argv:
        separator = argv.index("--")
        app_args = argv[separator + 1:]
        argv = argv[:separator]
    args = parser.parse_args(argv)

    presets = build_presets()
    if args.preset not in presets:
        sys.exit(f"Unknown build preset '{args.preset}'. Known: {', '.join(presets)}")
    configure_preset = presets[args.preset]
    binary_dir = ROOT / "build" / configure_preset
    bin_dir = binary_dir / "bin"

    if not (binary_dir / "CMakeCache.txt").exists():
        devenv.run(["cmake", "--preset", configure_preset])
    devenv.run(["cmake", "--build", "--preset", args.preset])

    if args.test:
        devenv.run(["ctest", "--preset", args.preset, "-LE", "gpu"])

    if args.unpack:
        assets = cache_path(binary_dir, "GDL_ASSET_DIR", ROOT / "assets" / "GUNE5D" / "Gauntlet")
        unpacked = cache_path(binary_dir, "GDL_UNPACKED_DIR", ROOT / "assets" / "unpacked")
        unpack_args = [str(bin_dir / f"gdlunpack{EXE}"), str(assets), str(unpacked)]
        if args.levels:
            unpack_args.append("--levels")
        devenv.run(unpack_args)

    if args.run:
        return devenv.run([str(bin_dir / f"gauntlet{EXE}"), *app_args]).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
