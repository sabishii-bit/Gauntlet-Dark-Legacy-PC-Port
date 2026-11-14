#!/usr/bin/env python3
"""Check, build and run the launcher, the cargo workspace under src/launcher.

    python scripts/launcher.py check                   # formatting, clippy and every crate's tests
    python scripts/launcher.py check --package gdl-files [--package ...]   # the same for those crates
    python scripts/launcher.py build [--release]      # the launcher's programs, into build/launcher
    python scripts/launcher.py run [--release] [-- args]   # the launcher's window, for development

Cargo runs from src/launcher, so its rust-toolchain.toml and .cargo/config.toml apply, with its
home in build/cargo-home (shared with the netplay crate) and its target directory in
build/launcher, never under src/. The one thing a build writes under src/ is Tauri's: the window's
capability schemas in src/launcher/desktop/gen/, which .gitignore hides. On Windows cargo runs in
the MSVC developer environment. rustup's folder joins PATH when a fresh install has not reached
this shell yet. A run that is interrupted, told to stop, or whose caller is gone ends cargo and
every compiler under it.
"""

import argparse
import os
import pathlib
import shutil
import signal
import subprocess
import sys
from typing import Dict, List, Optional, Sequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import devenv  # noqa: E402  the path above makes it importable
import process_tree  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT / "src" / "launcher"
CARGO_HOME = ROOT / "build" / "cargo-home"
TARGET = ROOT / "build" / "launcher"
WINDOW_PACKAGE = "gdl-launcher"
WAIT_SLICE = 0.5  # seconds; on Windows a bare Popen.wait() holds a Ctrl+C until cargo exits
STOP_SIGNALS = ("SIGTERM", "SIGHUP", "SIGQUIT", "SIGBREAK")  # those the platform has


def plan(command: str, packages: Optional[Sequence[str]] = None, release: bool = False,
         arguments: Optional[List[str]] = None) -> List[List[str]]:
    """The cargo commands a launcher command runs, in order."""
    profile = ["--release"] if release else []
    if command == "check":
        chosen = [part for name in packages or () for part in ("--package", name)]
        return [
            ["cargo", "fmt", *(chosen or ["--all"]), "--check"],
            ["cargo", "clippy", "--locked", *(chosen or ["--workspace"]), "--all-targets", "--",
             "-D", "warnings"],
            ["cargo", "test", "--locked", *(chosen or ["--workspace"])],
        ]
    if command == "build":
        return [["cargo", "build", "--locked", "--workspace", *profile]]
    if command == "run":
        return [["cargo", "run", "--locked", "--package", WINDOW_PACKAGE, *profile, "--",
                 *(arguments or [])]]
    raise ValueError(f"unknown launcher command: {command}")


def on_path(folder: pathlib.Path, path: str) -> bool:
    wanted = os.path.normcase(os.path.normpath(str(folder)))
    return any(os.path.normcase(os.path.normpath(entry)) == wanted
               for entry in path.split(os.pathsep) if entry)


def environment() -> Dict[str, str]:
    """Cargo's environment: the developer environment on Windows, rustup's folder on PATH when
    this shell predates it, and the build's own cargo home and target directory."""
    env = dict(devenv.environment() if devenv.WINDOWS else os.environ)
    rust = devenv.rust_bin()  # read before CARGO_HOME is replaced below
    if rust.is_dir() and not on_path(rust, env.get("PATH", "")):
        env["PATH"] = os.pathsep.join(part for part in (str(rust), env.get("PATH", "")) if part)
    env["CARGO_HOME"] = str(CARGO_HOME)
    env["CARGO_TARGET_DIR"] = str(TARGET)
    return env


def _interrupt(_signal, _frame) -> None:
    raise KeyboardInterrupt


def stop_on_signals() -> None:
    """Turns the signals that end a run into an interrupt, so `run` takes cargo's tree down too:
    cargo runs in a session of its own, which a closed terminal or a kill never reaches."""
    for name in STOP_SIGNALS:
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), _interrupt)


def run(command: List[str], env: Dict[str, str]) -> int:
    """Runs one cargo command from the workspace, ending its whole tree if this run ends early."""
    executable = shutil.which(command[0], path=env.get("PATH"))
    if executable is None:
        print(f"{command[0]} not found; open a new shell, or run python scripts/setup.py --launcher",
              file=sys.stderr)
        return 1
    child = process_tree.start([executable, *command[1:]], cwd=WORKSPACE, env=env)
    try:
        process_tree.stop_when_gone(os.getppid(), lambda: process_tree.stop(child))
        while True:
            try:
                return child.wait(timeout=WAIT_SLICE)
            except subprocess.TimeoutExpired:
                continue
    except KeyboardInterrupt:
        process_tree.stop(child)
        raise


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="formatting, clippy and the tests")
    check.add_argument("--package", action="append", dest="packages", metavar="CRATE",
                       help="a crate of the workspace, such as gdl-files; repeat for more")
    build = commands.add_parser("build", help="build the launcher's programs")
    build.add_argument("--release", action="store_true", help="the optimised profile")
    window = commands.add_parser("run", help="run the launcher's window")
    window.add_argument("--release", action="store_true", help="the optimised profile")
    window.add_argument("arguments", nargs=argparse.REMAINDER,
                        help="arguments for the launcher, after --")
    args = parser.parse_args(argv)
    arguments = list(getattr(args, "arguments", None) or [])
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    stop_on_signals()
    env = environment()
    for step in plan(args.command, getattr(args, "packages", None),
                     getattr(args, "release", False), arguments):
        code = run(step, env)
        if code != 0:
            return code
    return 0


if __name__ == "__main__":
    sys.exit(main())
