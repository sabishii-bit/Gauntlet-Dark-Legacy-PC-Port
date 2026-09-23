#!/usr/bin/env python3
"""Launch a scenario by full name, unique suffix, or JSON path.

    python scripts/scenario.py genie
    python scripts/scenario.py --list
    python scripts/scenario.py dragon --build
    python scripts/scenario.py genie --frames 600 -- --no-vsync

Uses the platform's existing Release build unless --preset selects another.
--build delegates to build.py to configure/build before launching. Paths passed
after -- are interpreted from the repository root, where the game is launched.
"""

import argparse
import pathlib
import shlex
import subprocess
import sys

import build
import devenv

ROOT = devenv.ROOT


def scenarios() -> list[pathlib.Path]:
    return sorted((ROOT / "tests" / "scenarios").glob("*.json"))


def resolve_scenario(name: str) -> pathlib.Path:
    """Prefer exact names; a short name must identify exactly one shipped scenario."""
    path = pathlib.Path(name).expanduser()
    if path.is_file():
        return path.resolve()
    if path.is_absolute() or len(path.parts) > 1:
        raise ValueError(f"Scenario file not found: {name}")
    key = path.stem.casefold() if path.suffix.casefold() == ".json" else name.casefold()
    choices = scenarios()
    exact = [p for p in choices if p.stem.casefold() == key]
    matches = exact or [p for p in choices if p.stem.casefold().endswith("-" + key)]
    if len(matches) == 1:
        return matches[0].resolve()
    if matches:
        raise ValueError(f"Ambiguous scenario '{name}': " + ", ".join(p.stem for p in matches))
    raise ValueError(f"Unknown scenario '{name}'. Use --list to see available scenarios.")


def positive_frames(value: str) -> int:
    frames = int(value)
    if frames <= 0:
        raise argparse.ArgumentTypeError("frame count must be positive")
    return frames


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    game_args = []
    if "--" in argv:
        separator = argv.index("--")
        game_args = argv[separator + 1:]
        argv = argv[:separator]
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("name", nargs="?", help="e.g. genie, level-c5-genie, or a JSON path")
    parser.add_argument("--list", action="store_true", help="list available scenarios")
    parser.add_argument("--preset", default=devenv.release_preset(), help="build preset to run")
    parser.add_argument("--build", action="store_true", help="build before launching")
    parser.add_argument("--frames", type=positive_frames, help="quit after this many frames")
    parser.epilog = "Additional game options go after -- (for example: -- --no-vsync)."
    args = parser.parse_args(argv)
    if args.list or args.name is None:
        print("Available scenarios:")
        for path in scenarios():
            print(f"  {path.stem}")
        return 0
    try:
        scenario = resolve_scenario(args.name)
        presets = build.build_presets()
        if args.preset not in presets:
            raise ValueError(f"Unknown preset '{args.preset}'. Known: {', '.join(presets)}")
        launch_args = ["--scenario", str(scenario)]
        if args.frames is not None:
            launch_args.extend(["--frames", str(args.frames)])
        launch_args.extend(game_args)
        if args.build:
            command = [sys.executable, str(ROOT / "scripts" / "build.py"), args.preset,
                       "--run", "--", *launch_args]
        else:
            executable = ROOT / "build" / presets[args.preset] / "bin" / f"gauntlet{build.EXE}"
            if not executable.is_file():
                raise ValueError(f"Game executable not found: {executable}\n"
                                 "Add --build to build it before launching.")
            command = [str(executable), *launch_args]
        display = subprocess.list2cmdline(command) if devenv.WINDOWS else shlex.join(command)
        print(display, flush=True)
        return subprocess.run(command, cwd=ROOT, check=False).returncode
    except (ValueError, OSError) as error:
        parser.error(str(error))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
