#!/usr/bin/env python3
"""Check, build and run the launcher, the cargo workspace under src/launcher.

    python scripts/launcher.py check                   # formatting, clippy and every crate's tests
    python scripts/launcher.py check --package gdl-files [--package ...]   # the same for those crates
    python scripts/launcher.py build [--release] [--package <crate> ...]   # into build/launcher
    python scripts/launcher.py build --test-hooks      # the headless program with its test hooks
    python scripts/launcher.py rehearsal-build         # the update rehearsal's two programs
    python scripts/launcher.py run [--release] [-- args]   # the launcher's window, for development
    python scripts/launcher.py extract <n64|dreamcast> <dump> --output <new folder> [--install] [--release]
    python scripts/launcher.py check-folder <folder> [--json] [--release]   # judged for an install
    python scripts/launcher.py parity <arguments for launcher_parity.py, such as --verify>

Cargo runs from src/launcher, so its rust-toolchain.toml and .cargo/config.toml apply, with its
home in build/cargo-home (shared with the netplay crate) and its target directory in
build/launcher, never under src/. The one place a build writes under src/ is
src/launcher/desktop/gen/, which .gitignore hides: Tauri's capability schemas for the window and
the version.json its build script writes from VERSION. On Windows cargo runs in the MSVC
developer environment. rustup's folder joins PATH when a fresh install has not reached this shell
yet. A run that is interrupted, told to stop, or whose caller is gone ends cargo and every
compiler under it, and so does one that outlasts the timeout a caller in this process gives it.

check then runs clippy and the tests once more over the crates that carry the test hooks
(gdl-release, gdl-install, gdl-session and the headless program), with the hooks on, whose code
the plain pass never compiles: with --package, over those of them it names, and not at all when
it names none.

build builds the whole workspace, or only the crates --package names. build --test-hooks builds
the headless program with its test hooks (faults by name, a loopback feed, an install without the
window and the tests' own signing key) in a profile of its own that inherits the release profile,
into build/launcher/test-hooks, so that it never takes the place of the program a release ships.
It builds that program alone: cargo gives every crate of one build the same features.
rehearsal-build builds it twice, at VERSION's version and at the next, the one an update
rehearsal updates an installation of VERSION's to, and copies each program into
build/launcher/rehearsal/<version>/, made afresh, printing both paths. The next version gives a
prerelease one more identifier, 1 (1.0.0-beta becomes 1.0.0-beta.1), and makes a release a
prerelease of its next patch (1.0.0 becomes 1.0.1-rehearsal), dropping build metadata, so that
it ranks above VERSION's and is never a release's own. extract runs the headless
program's --extract, which writes a Legends expansion's tree from a dump into a new folder,
holding the dump as the extraction scripts hold one unless --install asks for an installation's
strictness. check-folder runs the headless program's --check-folder, which judges a folder as one
to install the game into and reports what would refuse the install, what to warn of, and what it
found of the folder and its volume, as JSON with --json. All it writes are the folder probe's
entries, each named .gdl-launcher-probe- and eight random letters and digits, and removed at
once: a folder and a file in the folder when it exists, or else a folder in the nearest folder
above it that exists. The probe also removes such entries that an earlier probe left in that
folder, a minute old or more. It writes nothing in a folder the launcher must not write into,
such as the alpha installer's or another installation's. parity runs the parity gate,
scripts/checks/launcher_parity.py, with this Python and every argument after the word, from the
folder and in the environment this script was started in.
"""

import argparse
import json
import os
import pathlib
import re
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
VERSION_FILE = ROOT / "VERSION"  # the version the launcher's programs carry
TEST_HOOKS_FEATURE = "test-hooks"  # each crate's own name for its test hooks
TEST_HOOKS_PROFILE = "test-hooks"  # the cargo profile, and so the folder, of a test-hooks build
# The crates that carry the test hooks, each turning on those of the crates below it.
TEST_HOOK_PACKAGES = ("gdl-release", "gdl-install", "gdl-session", HEADLESS_PACKAGE)
HEADLESS_PROGRAM = HEADLESS_PACKAGE + (".exe" if devenv.WINDOWS else "")
REHEARSAL = TARGET / "rehearsal"  # the update rehearsal's programs, a folder for each version
VERSION_OVERRIDE = "TAURI_CONFIG"  # the variable whose "version" the programs carry instead
REHEARSAL_IDENTIFIER = "rehearsal"  # the prerelease a release's next rehearsal version takes

# A semantic version as semver.org gives it, build metadata included; ASCII digits alone.
_NUMBER = r"(?:0|[1-9][0-9]*)"
_IDENTIFIER = rf"(?:{_NUMBER}|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
SEMANTIC_VERSION = re.compile(
    rf"(?P<major>{_NUMBER})\.(?P<minor>{_NUMBER})\.(?P<patch>{_NUMBER})"
    rf"(?:-(?P<prerelease>{_IDENTIFIER}(?:\.{_IDENTIFIER})*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?")


def plan(command: str, packages: Optional[Sequence[str]] = None, release: bool = False,
         arguments: Optional[List[str]] = None, test_hooks: bool = False) -> List[List[str]]:
    """The commands a launcher command runs, in order: cargo's, or the parity gate's."""
    profile = ["--release"] if release else []
    chosen = [part for name in packages or () for part in ("--package", name)]
    if test_hooks and (command != "build" or packages or release):
        raise ValueError("a test-hooks build builds the headless program alone, in its own "
                         "profile")
    if command == "check":
        steps = [
            ["cargo", "fmt", *(chosen or ["--all"]), "--check"],
            ["cargo", "clippy", "--locked", *(chosen or ["--workspace"]), "--all-targets", "--",
             "-D", "warnings"],
            ["cargo", "test", "--locked", *(chosen or ["--workspace"])],
        ]
        # The crates that carry the hooks are checked once more with them on, in commands of their
        # own: cargo gives a crate the same features throughout one command, so the plain pass
        # would otherwise build them with the hooks too. Each names its own feature, which turns
        # on those of the crates below it, as the program's test-hooks build does.
        hooked = list(dict.fromkeys(name for name in packages if name in TEST_HOOK_PACKAGES)
                      if packages else TEST_HOOK_PACKAGES)
        if hooked:
            selected = [part for name in hooked for part in ("--package", name)]
            features = ",".join(f"{name}/{TEST_HOOKS_FEATURE}" for name in hooked)
            steps += [
                ["cargo", "clippy", "--locked", *selected, "--features", features,
                 "--all-targets", "--", "-D", "warnings"],
                ["cargo", "test", "--locked", *selected, "--features", features],
            ]
        return steps
    if command == "build":
        if test_hooks:
            return [["cargo", "build", "--locked", "--profile", TEST_HOOKS_PROFILE, "--package",
                     HEADLESS_PACKAGE, "--features", TEST_HOOKS_FEATURE]]
        return [["cargo", "build", "--locked", *(chosen or ["--workspace"]), *profile]]
    if command == "run":
        return [["cargo", "run", "--locked", "--package", WINDOW_PACKAGE, *profile, "--",
                 *(arguments or [])]]
    if command == "extract":
        return [["cargo", "run", "--locked", "--package", HEADLESS_PACKAGE, *profile, "--",
                 "--extract", *(arguments or [])]]
    if command == "check-folder":
        return [["cargo", "run", "--locked", "--package", HEADLESS_PACKAGE, *profile, "--",
                 "--check-folder", *(arguments or [])]]
    if command == "parity":
        return [[sys.executable, str(PARITY_GATE), *(arguments or [])]]
    raise ValueError(f"unknown launcher command: {command}")


def absolute_as_typed(path: str) -> str:
    """`path` made absolute from this process's folder, every name in it as it was typed:
    os.path.abspath drops a trailing dot or space from a name on Windows, and would so hand the
    program another name than the one given, which the program judges itself: --extract refuses
    such an output's name on every system, and --check-folder names the folder as the system makes
    it. A drive's own relative path, such as C:dumps, starts from that drive's folder. An empty
    name stays empty, since it names no folder: the program refuses it, where made absolute it
    would name this process's folder."""
    if not path:
        return path
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


def next_rehearsal_version(version: str) -> str:
    """The version an update rehearsal updates an installation of `version` to: a prerelease gains
    the identifier 1, since a longer list of identifiers ranks higher, and a release becomes the
    rehearsal prerelease of its next patch, which ranks above it and is no release's own. Build
    metadata, which ranks nothing, is dropped."""
    match = SEMANTIC_VERSION.fullmatch(version)
    if match is None:
        raise ValueError(f"not a semantic version: {version!r}")
    core = f"{match['major']}.{match['minor']}"
    if match["prerelease"] is not None:
        return f"{core}.{match['patch']}-{match['prerelease']}.1"
    return f"{core}.{int(match['patch']) + 1}-{REHEARSAL_IDENTIFIER}"


def without_version_override(env: Dict[str, str]) -> Dict[str, str]:
    """`env` without the variable that gives the programs a version in place of VERSION's, in any
    letter case, since Windows reads its variables' names so."""
    return {name: value for name, value in env.items() if name.upper() != VERSION_OVERRIDE}


def fresh_folder(folder: pathlib.Path) -> None:
    """Makes `folder` anew, empty: what an earlier build left in it is removed first."""
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)


def rehearsal_build() -> int:
    """Builds the headless program with its test hooks at VERSION's version and at the next, and
    copies each into the rehearsal's folder of its version. Both folders are made afresh before the
    first build, so a build that fails leaves no program of an earlier one to rehearse with."""
    candidate = VERSION_FILE.read_text(encoding="utf-8").strip()
    try:
        following = next_rehearsal_version(candidate)
    except ValueError as error:
        print(f"VERSION: {error}", file=sys.stderr)
        return 1
    built = TARGET / TEST_HOOKS_PROFILE / HEADLESS_PROGRAM
    env = without_version_override(environment())
    builds = ((candidate, env),
              (following, {**env, VERSION_OVERRIDE: json.dumps({"version": following})}))
    for version, _ in builds:
        fresh_folder(REHEARSAL / version)
    (command,) = plan("build", test_hooks=True)
    copies = []
    for version, build_env in builds:
        code = run(command, build_env)
        if code != 0:
            return code
        copy = REHEARSAL / version / HEADLESS_PROGRAM
        shutil.copy2(built, copy)
        copies.append(copy)
    for copy in copies:
        print(copy)
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="formatting, clippy and the tests")
    check.add_argument("--package", action="append", dest="packages", metavar="CRATE",
                       help="a crate of the workspace, such as gdl-files; repeat for more")
    build = commands.add_parser("build", help="build the launcher's programs, or the crates named")
    profiles = build.add_mutually_exclusive_group()
    profiles.add_argument("--release", action="store_true", help="the optimised profile")
    profiles.add_argument("--test-hooks", action="store_true",
                          help="the headless program alone, with its test hooks, in a profile of "
                               "its own that inherits the optimised one; never a release's")
    build.add_argument("--package", action="append", dest="packages", metavar="CRATE",
                       help="only this crate of the workspace, such as gdl-launcher-headless; "
                            "repeat for more")
    commands.add_parser("rehearsal-build",
                        help="build the update rehearsal's two programs with their test hooks, "
                             "at VERSION's version and the next, into build/launcher/rehearsal")
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
    check_folder = commands.add_parser("check-folder", help="report what a folder is to an "
                                                            "install of the game into it")
    check_folder.add_argument("folder", help="the folder to judge, which need not exist yet")
    check_folder.add_argument("--json", action="store_true", help="the report as JSON")
    check_folder.add_argument("--release", action="store_true", help="the optimised profile")
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
    test_hooks = getattr(args, "test_hooks", False)
    if test_hooks and args.packages:
        build.error(f"--test-hooks builds {HEADLESS_PACKAGE} alone, so it takes no --package")
    if args.command == "extract":
        # Cargo starts the program in the workspace, so the paths are made absolute here, where
        # they still mean what was typed. Without --install the dump is held as the extraction
        # scripts hold one, as the program does only when asked.
        strictness = [] if args.install else ["--strict", "script"]
        arguments = [args.platform, absolute_as_typed(args.dump), "--output",
                     absolute_as_typed(args.output), *strictness]
    if args.command == "check-folder":
        # Made absolute here for the same reason; an empty name reaches the program as it is, to
        # be refused there, rather than become this process's folder, which the check would probe.
        arguments = [absolute_as_typed(args.folder), *(["--json"] if args.json else [])]
    # The gate runs as it does when started by hand: from the folder this script was started in,
    # where a relative path given to it means what was typed, and in this process's environment,
    # from which it makes cargo's itself. Made again inside cargo's, that would lose rustup's
    # folder for a shell older than rustup: the developer environment puts back the PATH it was
    # first given, and CARGO_HOME then names the build's cargo home, not rustup's proxies' one.
    gate = args.command == "parity"
    folder = None if gate else WORKSPACE
    stop_on_signals()
    if args.command == "rehearsal-build":
        return rehearsal_build()
    env = dict(os.environ) if gate else environment()
    for step in plan(args.command, getattr(args, "packages", None),
                     getattr(args, "release", False), arguments, test_hooks):
        code = run(step, env, cwd=folder)
        if code != 0:
            return code
    return 0


if __name__ == "__main__":
    sys.exit(main())
