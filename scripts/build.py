#!/usr/bin/env python3
"""Build the project with a CMake build preset, then optionally test, unpack or run.

    python scripts/build.py                          # the platform's Debug Ninja preset
    python scripts/build.py --test                   # build, then run the unit tests
    python scripts/build.py --unpack                 # build, then unpack the console assets
    python scripts/build.py --run -- --title         # build the release preset, then launch the game
    python scripts/build.py --run -- --demo          # preview the idle level flyby
    python scripts/build.py --run -- --screensaver   # preview the flaming weapons
    python scripts/build.py linux-clang-release      # any build preset from CMakePresets.json

Configures first when the preset has never been configured. Arguments after "--"
go to the game. Without a preset, --run builds and launches the platform's release
build (the Debug build runs the tower at well under its frame rate); everything
else uses the Debug build.
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
    if not cache.is_file():
        return default
    match = re.search(rf"^{variable}:(?:PATH|FILEPATH|STRING)=(.*)$", cache.read_text(encoding="utf-8"),
                      re.MULTILINE)
    return pathlib.Path(match.group(1)) if match and match.group(1) else default


def refresh_player_effects(binary_dir: pathlib.Path, app_args: list[str],
                           root: pathlib.Path | None = None) -> None:
    """Upgrade legacy player exports missing authored texture-animation bindings.

    Inspect the actual schema rather than mtimes: a new executable alone cannot
    repair old unpacked assets. No asset tree means an ordinary asset-free build.
    """
    root = ROOT if root is None else root
    assets = cache_path(binary_dir, "GDL_ASSET_DIR", root / "assets/GUNE5D/Gauntlet")
    unpacked = cache_path(binary_dir, "GDL_UNPACKED_DIR", root / "assets/unpacked")
    for flag, value in zip(app_args, app_args[1:]):
        if flag == "--data":
            assets = root / value
        elif flag == "--unpacked":
            unpacked = root / value
    stale = []
    for manifest in sorted((unpacked / "PLAYERS").glob("*/SFX*/animations.json")):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        if data.get("textureBindingVersion", 0) < 1:
            stale.append(manifest)
    if not stale:
        return
    unpacker = binary_dir / "bin" / f"gdlunpack{EXE}"
    if not (assets / "PLAYERS").is_dir() or not unpacker.is_file():
        raise ValueError("Player effects need re-exporting. Provide the original PLAYERS assets "
                         "and build gdlunpack before launching.")
    print(f"Refreshing {len(stale)} legacy player effect archives", flush=True)
    devenv.run([str(unpacker), str(assets), str(unpacked), "--only", "PLAYERS"])
    for manifest in stale:
        if json.loads(manifest.read_text(encoding="utf-8")).get("textureBindingVersion", 0) < 1:
            raise ValueError(f"Player effect refresh did not upgrade {manifest}. "
                             "Rebuild gdlunpack and check the original asset files.")


def refresh_item_collision(binary_dir: pathlib.Path, app_args: list[str], root=None) -> None:
    """Re-export levels whose old manifests omitted their items' collision triangles."""
    root = ROOT if root is None else root
    assets = cache_path(binary_dir, "GDL_ASSET_DIR", root / "assets/GUNE5D/Gauntlet")
    unpacked = cache_path(binary_dir, "GDL_UNPACKED_DIR", root / "assets/unpacked")
    for flag, value in zip(app_args, app_args[1:]):
        if flag == "--data":
            assets = root / value
        elif flag == "--unpacked":
            unpacked = root / value

    def incomplete(manifest):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        return any(item.get("triangleCount", 0) > 0 and
                   len(item.get("collision", [])) != item["triangleCount"]
                   for item in data.get("itemInstances", []))

    stale = [manifest for manifest in sorted((unpacked / "LEVELS").glob("*/world.json"))
             if incomplete(manifest)]
    if not stale:
        return
    unpacker = binary_dir / "bin" / f"gdlunpack{EXE}"
    if not (assets / "LEVELS").is_dir() or not unpacker.is_file():
        raise ValueError("Item collision needs re-exporting. Provide the original LEVELS assets "
                         "and build gdlunpack before launching.")
    print(f"Refreshing collision data in {len(stale)} legacy level exports", flush=True)
    for manifest in stale:
        devenv.run([str(unpacker), str(assets), str(unpacked), "--only", manifest.parent.name])
        if incomplete(manifest):
            raise ValueError(f"Item collision refresh did not upgrade {manifest}. "
                             "Rebuild gdlunpack and check the original asset files.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("preset", nargs="?", default=None,
                        help="build preset (default: the platform's Debug Ninja preset, or its "
                             "release preset with --run)")
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
    if args.preset is None:
        args.preset = devenv.release_preset() if args.run else devenv.default_preset()

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
        refresh_player_effects(binary_dir, app_args)
        refresh_item_collision(binary_dir, app_args)
        return devenv.run([str(bin_dir / f"gauntlet{EXE}"), *app_args]).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
