#!/usr/bin/env python3
"""Check, build and run the launcher, the cargo workspace under src/launcher.

    python scripts/launcher.py check                   # formatting, clippy and every crate's tests
    python scripts/launcher.py check --package gdl-files [--package ...]   # the same for those crates
    python scripts/launcher.py build [--release] [--package <crate> ...]   # into build/launcher
    python scripts/launcher.py run [--release] [-- args]   # the launcher's window, for development
    python scripts/launcher.py extract <n64|dreamcast> <dump> --output <new folder> [--install] [--release]
    python scripts/launcher.py parity <arguments for launcher_parity.py, such as --verify>

Cargo runs from src/launcher, so its rust-toolchain.toml and .cargo/config.toml apply, with its
home in build/cargo-home (shared with the netplay crate) and its target directory in
build/launcher, never under src/. The one place a build writes under src/ is
src/launcher/desktop/gen/, which .gitignore hides: Tauri's capability schemas for the window and
the version.json its build script writes from VERSION. On Windows cargo runs in the MSVC
developer environment. rustup's folder joins PATH when a fresh install has not reached this shell
yet. A run that is interrupted, told to stop, or whose caller is gone ends cargo and every
compiler under it, and so does one that outlasts the timeout a caller in this process gives it.

build builds the whole workspace, or only the crates --package names. extract runs the headless
program's --extract, which writes a Legends expansion's tree from a dump into a new folder,
holding the dump as the extraction scripts hold one unless --install asks for an installation's
strictness. parity runs the parity gate, scripts/checks/launcher_parity.py, with this Python and
every argument after the word, from the folder and in the environment this script was started in.
"""

import argparse
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time
from typing import Dict, List, Optional, Sequence

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import devenv  # noqa: E402  the path above makes it importable
import process_tree  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT / "src" / "launcher"
CARGO_HOME = ROOT / "build" / "cargo-home"
TARGET = ROOT / "build" / "launcher"
WINDOW_PACKAGE = "gdl-launcher"
HEADLESS_PACKAGE = "gdl-launcher-headless"  # the launcher's modes without a window
PARITY_GATE = ROOT / "scripts" / "checks" / "launcher_parity.py"
PLATFORMS = ("n64", "dreamcast")  # the expansions the headless program's --extract takes
TIMED_OUT = 124  # the exit code of a run stopped at its timeout
WAIT_SLICE = 0.5  # seconds; on Windows a bare Popen.wait() holds a Ctrl+C until cargo exits
STOP_SIGNALS = ("SIGTERM", "SIGHUP", "SIGQUIT", "SIGBREAK")  # those the platform has


def plan(command: str, packages: Optional[Sequence[str]] = None, release: bool = False,
         arguments: Optional[List[str]] = None) -> List[List[str]]:
    """The commands a launcher command runs, in order: cargo's, or the parity gate's."""
    profile = ["--release"] if release else []
    chosen = [part for name in packages or () for part in ("--package", name)]
    if command == "check":
        return [
            ["cargo", "fmt", *(chosen or ["--all"]), "--check"],
            ["cargo", "clippy", "--locked", *(chosen or ["--workspace"]), "--all-targets", "--",
             "-D", "warnings"],
            ["cargo", "test", "--locked", *(chosen or ["--workspace"])],
        ]
    if command == "build":
        return [["cargo", "build", "--locked", *(chosen or ["--workspace"]), *profile]]
    if command == "run":
        return [["cargo", "run", "--locked", "--package", WINDOW_PACKAGE, *profile, "--",
                 *(arguments or [])]]
    if command == "extract":
        return [["cargo", "run", "--locked", "--package", HEADLESS_PACKAGE, *profile, "--",
                 "--extract", *(arguments or [])]]
    if command == "parity":
        return [[sys.executable, str(PARITY_GATE), *(arguments or [])]]
    raise ValueError(f"unknown launcher command: {command}")


def absolute_as_typed(path: str) -> str:
    """`path` made absolute from this process's folder, every name in it as it was typed:
    os.path.abspath drops a trailing dot or space from a name on Windows, and would so hand the
    program another folder than the one named, where the program refuses such a name on every
    system. A drive's own relative path, such as C:dumps, starts from that drive's folder."""
    drive, rest = os.path.splitdrive(path)
    return os.path.join(os.path.abspath(drive) if drive else os.getcwd(), rest)


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


def run(command: List[str], env: Dict[str, str], timeout: Optional[float] = None,
        cwd: Optional[pathlib.Path] = WORKSPACE) -> int:
    """Runs one command from `cwd` (the workspace, where cargo runs, unless told otherwise; None
    is this process's own folder), ending its whole tree if this run ends early. A command still
    running `timeout` seconds after it started has its tree ended too, and the run gives back
    TIMED_OUT."""
    executable = shutil.which(command[0], path=env.get("PATH"))
    if executable is None:
        print(f"{command[0]} not found; open a new shell, or run python scripts/setup.py --launcher",
              file=sys.stderr)
        return 1
    child = process_tree.start([executable, *command[1:]], cwd=cwd, env=env)
    deadline = None if timeout is None else time.monotonic() + timeout
    try:
        process_tree.stop_when_gone(os.getppid(), lambda: process_tree.stop(child))
        while True:
            try:
                return child.wait(timeout=WAIT_SLICE)
            except subprocess.TimeoutExpired:
                pass
            if deadline is not None and time.monotonic() >= deadline:
                print(f"{' '.join(command)} ran longer than {timeout:g} s and was stopped",
                      file=sys.stderr)
                process_tree.stop(child)
                return TIMED_OUT
    except KeyboardInterrupt:
        process_tree.stop(child)
        raise


def main(argv: Optional[List[str]] = None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="formatting, clippy and the tests")
    check.add_argument("--package", action="append", dest="packages", metavar="CRATE",
                       help="a crate of the workspace, such as gdl-files; repeat for more")
    build = commands.add_parser("build", help="build the launcher's programs, or the crates named")
    build.add_argument("--release", action="store_true", help="the optimised profile")
    build.add_argument("--package", action="append", dest="packages", metavar="CRATE",
                       help="only this crate of the workspace, such as gdl-launcher-headless; "
                            "repeat for more")
    window = commands.add_parser("run", help="run the launcher's window")
    window.add_argument("--release", action="store_true", help="the optimised profile")
    window.add_argument("arguments", nargs=argparse.REMAINDER,
                        help="arguments for the launcher, after --")
    extract = commands.add_parser("extract", help="extract a Legends expansion's tree from a dump "
                                                  "into a new folder")
    extract.add_argument("platform", choices=PLATFORMS, help="the expansion the dump is of")
    extract.add_argument("dump", help="the N64 ROM; for Dreamcast a cue sheet or a GDI, the "
                                      "folder holding one, or a track file one names")
    extract.add_argument("--output", required=True, metavar="FOLDER",
                         help="the new folder the tree goes into, whose parent exists")
    extract.add_argument("--install", action="store_true",
                         help="hold the dump as an installation does, to the trust the launcher "
                              "carries, rather than as the extraction scripts do")
    extract.add_argument("--release", action="store_true", help="the optimised profile")
    # Listed for --help alone: every argument after the word goes to the gate unread, since
    # argparse would refuse the gate's options as ones it does not know.
    commands.add_parser("parity", add_help=False,
                        help="run the parity gate, scripts/checks/launcher_parity.py, with every "
                             "argument that follows, such as --verify")
    if argv[:1] == ["parity"]:
        args, arguments = argparse.Namespace(command="parity"), argv[1:]
    else:
        args = parser.parse_args(argv)
        arguments = list(getattr(args, "arguments", None) or [])
        if arguments[:1] == ["--"]:
            arguments = arguments[1:]
    if args.command == "extract":
        # Cargo starts the program in the workspace, so the paths are made absolute here, where
        # they still mean what was typed. Without --install the dump is held as the extraction
        # scripts hold one, as the program does only when asked.
        strictness = [] if args.install else ["--strict", "script"]
        arguments = [args.platform, absolute_as_typed(args.dump), "--output",
                     absolute_as_typed(args.output), *strictness]
    # The gate runs as it does when started by hand: from the folder this script was started in,
    # where a relative path given to it means what was typed, and in this process's environment,
    # from which it makes cargo's itself. Made again inside cargo's, that would lose rustup's
    # folder for a shell older than rustup: the developer environment puts back the PATH it was
    # first given, and CARGO_HOME then names the build's cargo home, not rustup's proxies' one.
    gate = args.command == "parity"
    folder = None if gate else WORKSPACE
    stop_on_signals()
    env = dict(os.environ) if gate else environment()
    for step in plan(args.command, getattr(args, "packages", None),
                     getattr(args, "release", False), arguments):
        code = run(step, env, cwd=folder)
        if code != 0:
            return code
    return 0


if __name__ == "__main__":
    sys.exit(main())
