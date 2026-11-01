#!/usr/bin/env python3
"""Build and test expansion audio with the optional MusyX backend disabled.

    python scripts/checks/expansion_audio.py
    python scripts/checks/expansion_audio.py --preset windows-ninja-release

Uses a separate build/expansion-audio-off cache, never the ordinary game cache.
An already installed dependency tree for the selected preset is reused without
running vcpkg against it. The smoke test needs neither native assets nor a GUI.
"""

import argparse
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import build
import devenv


def read_cache(directory: pathlib.Path) -> dict[str, str]:
    """Read ordinary CMake cache entries without evaluating CMake code."""
    path = directory / "CMakeCache.txt"
    if not path.is_file():
        return {}
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")) or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if ":" in key:
            values[key.split(":", 1)[0]] = value
    return values


def compiler_identity(directory: pathlib.Path, cache: dict[str, str]) -> tuple[str, ...]:
    """Compare CMake's detected compiler, not an unresolved preset spelling like cl.

    Read the active CMake-version record without evaluating it. Ambiguous or
    incomplete records fail closed; matching basenames never prove equivalence.
    """
    version = ".".join(cache.get(f"CMAKE_CACHE_{part}_VERSION", "")
                       for part in ("MAJOR", "MINOR", "PATCH"))
    records = list((directory / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if all(version.split(".")):
        records = [path for path in records
                   if path.parent.name == version or path.parent.name.startswith(version + "-")]
    identities = set()
    keys = ("COMPILER_ID", "COMPILER_VERSION", "COMPILER_ARG1", "COMPILER_TARGET",
            "COMPILER_ARCHITECTURE_ID", "COMPILER_FRONTEND_VARIANT", "SIMULATE_ID",
            "SIMULATE_VERSION")
    for record in records:
        text = record.read_text(encoding="utf-8")
        values = {key: quoted or bare
                  for key, quoted, bare in re.findall(
                      r'^set\((CMAKE_CXX_[A-Z0-9_]+)\s+(?:"([^"]*)"|([^()\s]+))\)',
                      text, re.MULTILINE)}
        compiler = pathlib.Path(values.get("CMAKE_CXX_COMPILER", ""))
        if (not compiler.is_absolute() or not values.get("CMAKE_CXX_COMPILER_ID") or
                not values.get("CMAKE_CXX_COMPILER_VERSION")):
            raise ValueError(f"Incomplete detected compiler record: {record}")
        identities.add((os.path.normcase(str(compiler.resolve())),
                        *(values.get("CMAKE_CXX_" + key, "") for key in keys)))
    if len(identities) != 1:
        raise ValueError(f"Cannot establish one detected compiler identity in {directory}")
    return identities.pop()


def commands(root: pathlib.Path, configure_preset: str, jobs: int) -> list[list[str]]:
    """Plan the isolated check; refuse to repurpose a foreign/incompatible cache."""
    if jobs < 1 or jobs > 4:
        raise ValueError("jobs must be between 1 and 4")
    ordinary = root / "build" / configure_preset
    isolated = root / "build" / "expansion-audio-off"
    source = read_cache(ordinary)
    destination = read_cache(isolated)
    for directory, cache in ((ordinary, source), (isolated, destination)):
        if cache and (pathlib.Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() !=
                      root.resolve() or cache.get("CMAKE_GENERATOR") != "Ninja"):
            raise ValueError(f"Not this project's Ninja cache: {directory}")
    if destination:
        if not source:
            raise ValueError("Configure the selected ordinary preset first to validate the existing OFF cache")
        for key in ("CMAKE_BUILD_TYPE", "VCPKG_TARGET_TRIPLET"):
            if destination.get(key) != source.get(key):
                raise ValueError(f"Existing OFF cache has different {key}; use its matching --preset")
        if compiler_identity(ordinary, source) != compiler_identity(isolated, destination):
            raise ValueError("Existing OFF cache uses a different detected C++ compiler; use its matching --preset")

    configure = ["cmake", "--preset", configure_preset, "-B", str(isolated),
                 "-DGDL_LEGENDS_AUDIO=OFF", "-DGDL_ENABLE_NETPLAY=OFF",
                 "-DGDL_EMBED_DISC_ICON=OFF", "-DGDL_BUILD_TESTS=ON",
                 f"-DGDL_ASSET_DIR={isolated / 'no-assets'}",
                 f"-DGDL_UNPACKED_DIR={isolated / 'no-unpacked'}"]
    installed = pathlib.Path(source.get("VCPKG_INSTALLED_DIR", str(ordinary / "vcpkg_installed")))
    triplet = source.get("VCPKG_TARGET_TRIPLET", "")
    if (source and triplet and (installed / triplet / "include").is_dir() and
            (installed / "vcpkg/status").is_file()):
        configure += [f"-DVCPKG_INSTALLED_DIR={installed}", "-DVCPKG_MANIFEST_INSTALL=OFF"]
    else:
        configure += [f"-DVCPKG_INSTALLED_DIR={isolated / 'vcpkg_installed'}",
                      "-DVCPKG_MANIFEST_INSTALL=ON"]
    return [configure,
            ["cmake", "--build", str(isolated), "--target", "expansion_audio_tests",
             "--parallel", str(jobs)],
            ["ctest", "--test-dir", str(isolated), "-R", "^expansion_audio_smoke$",
             "--no-tests=error", "--output-on-failure"]]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    presets = build.build_presets()
    ninja = {name: configured for name, configured in presets.items()
             if "-ninja-" in name or "-clang-" in name}
    parser.add_argument("--preset", choices=sorted(ninja), default=devenv.default_preset(),
                        help="ordinary Ninja build preset (default: %(default)s)")
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=4)
    args = parser.parse_args(argv)
    try:
        planned = commands(devenv.ROOT, ninja[args.preset], args.jobs)
    except ValueError as error:
        parser.error(str(error))
    for command in planned:
        devenv.run(command)
    return 0


if __name__ == "__main__":
    sys.exit(main())
