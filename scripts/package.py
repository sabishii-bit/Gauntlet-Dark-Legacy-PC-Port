#!/usr/bin/env python3
"""Stage a portable game folder with an unchanged original retail asset tree.

    python scripts/package.py --output out/gauntlet --dry-run
    python scripts/package.py --output out/gauntlet --assets /disc/Gauntlet

Uses the existing Release build; never builds, exports, deletes or replaces files.
The output must not exist. Gauntlet and its sibling carddemo are copied in full,
alongside the executable, compiled shaders, shipped data and runtime libraries.
On Windows the Visual Studio x64 CRT is included from the developer environment;
--runtime-dir can supply it explicitly. The Vulkan driver remains a system prerequisite.
"""

import argparse
from dataclasses import dataclass
import os
import pathlib
import shutil
import stat
import sys
from typing import Optional

import build
import devenv


@dataclass(frozen=True)
class PackagePlan:
    output: pathlib.Path
    files: tuple[tuple[pathlib.Path, pathlib.Path], ...]
    directories: tuple[pathlib.Path, ...]


def child_directory(parent: pathlib.Path, name: str) -> pathlib.Path:
    """Find an original folder without changing its case-sensitive spelling."""
    matches = [path for path in parent.iterdir()
               if path.name.casefold() == name.casefold() and path.is_dir()]
    if len(matches) != 1:
        raise ValueError(f"Expected one {name} directory under {parent}, found {len(matches)}")
    return matches[0]


def require_file(path: pathlib.Path) -> None:
    if not path.is_file():
        raise ValueError(f"Required package input is missing: {path}")


def validate_output(output: pathlib.Path, sources: list[pathlib.Path]) -> pathlib.Path:
    """Refuse existing outputs and any location inside a source tree, including aliases."""
    if os.path.lexists(output):
        raise ValueError(f"Output already exists; choose a new directory: {output}")
    resolved = output.resolve()
    for source in sources:
        source = source.resolve()
        if resolved == source or source in resolved.parents:
            raise ValueError(f"Output must not be inside a package input: {source}")
    return resolved


def runtime_library(path: pathlib.Path) -> bool:
    name = path.name.casefold()
    return name.endswith((".dll", ".dylib", ".so")) or ".so." in name


def linked_entry(path: pathlib.Path) -> bool:
    """Reject links and Windows reparse points, including junctions on Python 3.9-3.11."""
    info = path.lstat()
    return (stat.S_ISLNK(info.st_mode) or
            bool(getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT))


def make_plan(executable: pathlib.Path, assets: pathlib.Path, data: pathlib.Path,
              output: pathlib.Path, runtime_dirs: tuple[pathlib.Path, ...] = ()) -> PackagePlan:
    """Validate all inputs and enumerate the exact copy, without writing anything."""
    executable = executable.resolve()
    assets = assets.resolve()
    data = data.resolve()
    require_file(executable)
    if assets.name.casefold() != "gauntlet" or not assets.is_dir():
        raise ValueError(f"--assets must name the original Gauntlet directory: {assets}")
    carddemo = child_directory(assets.parent, "carddemo")
    shaders = executable.parent / "shaders"
    for shader in ("immediate.vert.spv", "immediate.frag.spv"):
        require_file(shaders / shader)
    require_file(data / "config.json")
    require_file(data / "text/en.json")
    for directory in runtime_dirs:
        if not directory.is_dir():
            raise ValueError(f"Runtime directory not found: {directory}")
    output = validate_output(output, [assets.parent, executable.parent, data, *runtime_dirs])

    files = []
    directories = set()
    destinations = {}

    def add_file(source, destination):
        require_file(source)
        key = destination.as_posix().casefold()
        if key in destinations:
            if destinations[key].resolve() == source.resolve():
                return
            raise ValueError(f"Conflicting package destination: {destination}")
        destinations[key] = source
        files.append((source, destination))

    def add_tree(source, destination):
        def fail_walk(error):
            raise error

        if linked_entry(source):
            raise ValueError(f"Linked entry inside package input: {source}")
        directories.add(destination)
        for folder, subdirs, names in os.walk(source, followlinks=False, onerror=fail_walk):
            folder = pathlib.Path(folder)
            relative = folder.relative_to(source)
            for name in subdirs + names:
                path = folder / name
                # Junctions and symlinks are not original disc records. Refuse to
                # follow them into unrelated files or silently omit their contents.
                if linked_entry(path):
                    raise ValueError(f"Linked entry inside package input: {path}")
            for name in subdirs:
                directories.add(destination / relative / name)
            for name in sorted(names):
                add_file(folder / name, destination / relative / name)

    add_file(executable, pathlib.Path(executable.name))
    for directory in (executable.parent, *runtime_dirs):
        for path in sorted(directory.iterdir()):
            if path.is_file() and runtime_library(path):
                add_file(path, pathlib.Path(path.name))
    add_tree(shaders, pathlib.Path("shaders"))
    add_tree(data, pathlib.Path("data"))
    add_tree(assets, pathlib.Path(assets.name))
    add_tree(carddemo, pathlib.Path(carddemo.name))
    return PackagePlan(output, tuple(files), tuple(sorted(directories)))


def stage(plan: PackagePlan) -> None:
    """Create the new package, using exclusive writes so no existing file is overwritten."""
    plan.output.mkdir(parents=True, exist_ok=False)
    for directory in plan.directories:
        (plan.output / directory).mkdir(parents=True, exist_ok=True)
    for source, relative in plan.files:
        destination = plan.output / relative
        with source.open("rb") as reader, destination.open("xb") as writer:
            shutil.copyfileobj(reader, writer)
        shutil.copystat(source, destination)


def msvc_runtime_directory() -> pathlib.Path:
    """Use the installed redistributable CRT, never DLLs harvested from System32."""
    environment = devenv.environment()
    redist = environment.get("VCToolsRedistDir")
    candidates = sorted((pathlib.Path(redist) / "x64").glob("Microsoft.VC*.CRT")) if redist else []
    if not candidates:
        raise ValueError("MSVC redistributable CRT not found; supply its x64 directory "
                         "with --runtime-dir")
    return candidates[-1]


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", default=devenv.release_preset())
    parser.add_argument("--executable", type=pathlib.Path, help="override the preset's executable")
    parser.add_argument("--assets", type=pathlib.Path, help="original Gauntlet tree; carddemo is its sibling")
    parser.add_argument("--data", type=pathlib.Path, help="shipped configuration/text defaults")
    parser.add_argument("--runtime-dir", type=pathlib.Path, action="append", default=[],
                        help="extra runtime-library directory (repeatable); overrides CRT discovery")
    parser.add_argument("--output", type=pathlib.Path, required=True, help="new, nonexistent package directory")
    parser.add_argument("--dry-run", action="store_true", help="validate and list the copy without writing")
    args = parser.parse_args(argv)
    try:
        presets = build.build_presets()
        if args.preset not in presets:
            raise ValueError(f"Unknown build preset: {args.preset}")
        binary = devenv.ROOT / "build" / presets[args.preset]
        executable = args.executable or binary / "bin" / f"gauntlet{build.EXE}"
        assets = args.assets or build.cache_path(binary, "GDL_ASSET_DIR",
                                                 devenv.ROOT / "assets/GUNE5D/Gauntlet")
        data = args.data or build.cache_path(binary, "GDL_DATA_DIR", devenv.ROOT / "data")
        runtime_dirs = args.runtime_dir
        if devenv.WINDOWS and executable.suffix.casefold() == ".exe" and not runtime_dirs:
            runtime_dirs = [msvc_runtime_directory()]
        plan = make_plan(executable, assets, data, args.output, tuple(runtime_dirs))
        total = sum(source.stat().st_size for source, _ in plan.files)
        print(f"{'Would stage' if args.dry_run else 'Staging'} {len(plan.files)} files "
              f"({total:,} bytes) in {plan.output}")
        if args.dry_run:
            for source, destination in plan.files:
                print(f"  {source} -> {destination}")
        else:
            stage(plan)
            print(f"Ready: {plan.output / executable.name}")
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    sys.exit(main())
