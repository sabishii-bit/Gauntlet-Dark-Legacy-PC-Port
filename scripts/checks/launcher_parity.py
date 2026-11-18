#!/usr/bin/env python3
"""Check the launcher's Legends trees against the extraction scripts' and the reference trees, on
the player's own dumps.

    python scripts/checks/launcher_parity.py --verify [--platform n64|dreamcast ...]
        [--n64-rom <path>] [--dreamcast-disc <path>] [--launcher <path>] [--strict script]
        [--keep] [--jobs <n>]

For the trusted machine, which holds the player's N64 ROM and Dreamcast disc and, under assets/,
the trees the extraction scripts wrote from them. The gate builds the headless launcher in
release, unless --launcher names the program to run, and runs the launcher's tests that read
those files. Then it extracts each platform's tree twice, with the extraction script and with the
launcher, which holds the dump as an installation does unless --strict script gives it the
scripts' rules. The two trees must be identical: the same paths in the same letters, each with
the same bytes. Every file the launcher's manifests name must be in the reference tree as the
launcher wrote it; what else the reference tree holds is reported, and fails nothing. Last, the
Dreamcast fingerprints the launcher carries must still be those of the scripts' tree.

A dump is the one its option names, else the one GDL_LEGENDS_N64_ROM or
GDL_LEGENDS_DREAMCAST_DISC names (the disc's cue sheet), else the one in assets/; a reference tree
is the one GDL_LEGENDS_N64_TREE or GDL_LEGENDS_DREAMCAST_TREE names, else
assets/<platform>/extracted. The launcher's tests read all four whichever platforms --platform
picks, so a missing one fails the run before it starts. The trees go into build/launcher-parity
beside report.json: a passing run removes them unless --keep, and a failing one keeps them to be
looked at. The report is always the latest run's: every run that ends writes it anew, one refused
for what it lacks included, and a run stopped or interrupted before its end leaves none.
"""

from __future__ import annotations

import _thread
import argparse
from collections.abc import Iterable, Mapping
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))  # the top-level helpers
import launcher  # noqa: E402  the path above makes it importable
import process_tree  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# Where a run writes: the folder that holds its trees, and its report beside them.
OUTPUT = ROOT / "build" / "launcher-parity"
REPORT = OUTPUT / "report.json"
REPORT_SCHEMA = 1  # the report's format, numbered anew whenever it changes

# The player's dumps where the trusted machine keeps them: the N64 ROM, and the cue sheet of the
# Dreamcast disc. An option, or else a variable, names another place.
DEFAULT_N64_ROM = ROOT / "assets" / "n64" / "Gauntlet Legends (USA).z64"
DEFAULT_DREAMCAST_DISC = (ROOT / "assets" / "dreamcast" / "Gauntlet Legends (USA)"
                          / "Gauntlet Legends (USA).cue")
DEFAULT_DUMPS = {"n64": DEFAULT_N64_ROM, "dreamcast": DEFAULT_DREAMCAST_DISC}
DUMP_OPTIONS = {"n64": "--n64-rom", "dreamcast": "--dreamcast-disc"}
DUMP_VARIABLES = {"n64": "GDL_LEGENDS_N64_ROM", "dreamcast": "GDL_LEGENDS_DREAMCAST_DISC"}

# The trees the extraction scripts wrote from the player's dumps, and the variables that name
# other places.
REFERENCE = {"n64": ROOT / "assets" / "n64" / "extracted",
             "dreamcast": ROOT / "assets" / "dreamcast" / "extracted"}
TREE_VARIABLES = {"n64": "GDL_LEGENDS_N64_TREE", "dreamcast": "GDL_LEGENDS_DREAMCAST_TREE"}

# The extraction scripts, and the script that checks the fingerprints the launcher carries.
SCRIPTS = {"n64": ROOT / "scripts" / "assets" / "n64_legends.py",
           "dreamcast": ROOT / "scripts" / "assets" / "dreamcast_legends.py"}
FINGERPRINTS = ROOT / "scripts" / "assets" / "legends_fingerprints.py"

# The headless program as cargo builds it in release.
BUILT_PROGRAM = (launcher.TARGET / "release"
                 / (launcher.HEADLESS_PACKAGE + (".exe" if process_tree.WINDOWS else "")))

# How long, in seconds, a step may run before it is stopped: an extraction, the launcher's build,
# its tests on the dumps, and the fingerprints' check.
EXTRACTION_TIMEOUT = 1800
BUILD_TIMEOUT = 3600
TEST_TIMEOUT = 3600
FINGERPRINTS_TIMEOUT = 300

# The crates whose ignored tests read the player's dumps and the reference trees, and how many of
# those tests run at once.
TEST_PACKAGES = ("gdl-media", "gdl-extract")
TEST_THREADS = 2

MOST_JOBS = 12  # the most build jobs cargo takes unless --jobs asks for more
HASH_BLOCK = 1 << 20  # bytes read at a time to hash a file
MAX_MANIFEST_BYTES = 64 * 1024 * 1024  # the most of a manifest read; extractions write far less

# What the trees hold beside the files their manifests list: the N64 tree's manifest, and the
# Dreamcast tree's two manifests and the disc's boot area, sixteen sectors of 2,048 bytes; and the
# folders of the Dreamcast tree that its manifests' paths lie in.
N64_MANIFEST = "manifest.json"
DISC_MANIFEST = "disc-manifest.json"
NATIVE_MANIFEST = "native-manifest.json"
IP_BIN = "IP.BIN"
IP_BIN_SIZE = 16 * 2048
DISC_FOLDER = "disc"
NATIVE_FOLDER = "native"

# What the player's dumps hold, as their manifests count it: the N64 cartridge's files and
# executable overlays, and the Dreamcast disc's files and the native files packed in them.
N64_FILES = 618
N64_PROGRAMS = 4
DREAMCAST_DISC_FILES = 156
DREAMCAST_NATIVE_FILES = 1361

# The counts the report gives of each platform's tree, by their keys there.
COUNTED = {"n64": ("files", "programs"), "dreamcast": ("discFiles", "nativeFiles")}

# What the report says of the launcher's tests, of the fingerprints, and of a launcher's tree
# against the reference tree.
TESTS_PASSED, TESTS_FAILED, NOT_RUN = "passed", "failed", "notRun"
CURRENT, NOT_CURRENT, NOT_CHECKED = "current", "notCurrent", "notChecked"
IDENTICAL, DIFFERS, NOT_COMPARED = "identical", "differs", "notCompared"

# What an entry of a tree is. A link of any kind is never followed.
REGULAR_FILE = "a regular file"
LINK = "a link"
EMPTY_FOLDER = "an empty folder"
NOT_A_FILE = "neither a file nor a folder"

SHOWN_DIFFERENCES = 10  # how many of a platform's differences its summary prints
STOP_GRACE = 10  # seconds a run whose caller is gone gives the interrupt before it exits outright
PASSED, FAILED = 0, 1  # what a run exits with


class Failed(Exception):
    """A step that failed, which ends the run."""


class ManifestError(Exception):
    """A manifest of the launcher's tree that names no paths a comparison can take."""


class Folders:
    """Where a run writes, all in one folder: the scripts' trees, the launcher's, and the
    report."""

    def __init__(self, output: Path):
        self.output = output
        self.python = output / "python"
        self.launcher = output / "launcher"
        self.report = output / REPORT.name


class Tree:
    """A tree a run compares: what lies at each of its paths, and each file's size and SHA-256,
    read the first time they are asked for."""

    def __init__(self, root: Path):
        self.root = root
        self.entries = entries_of(root)
        self._files: dict[str, tuple[int, str]] = {}

    def at(self, path: str) -> tuple[int, str] | str | None:
        """What the tree holds at `path`, in exact case: a regular file's size and SHA-256, what
        an entry that is no regular file is, or None where it holds nothing."""
        kind = self.entries.get(path)
        if kind != REGULAR_FILE:
            return kind
        if path not in self._files:
            self._files[path] = fingerprint(self.root / path)
        return self._files[path]


def default_jobs() -> int:
    """Cargo's build jobs where --jobs gives none: half the machine's threads, at least one and at
    most MOST_JOBS, so that the machine stays usable."""
    return max(1, min(MOST_JOBS, (os.cpu_count() or 1) // 2))


def arguments(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--verify", action="store_true", help="run the gate")
    parser.add_argument("--platform", action="append", choices=launcher.PLATFORMS,
                        dest="platforms", help="extract and compare only this expansion's tree; "
                                               "repeat for both, the default")
    parser.add_argument("--n64-rom", type=Path, metavar="PATH",
                        help="the player's N64 ROM (default: what GDL_LEGENDS_N64_ROM names, "
                             "else the one in assets/n64)")
    parser.add_argument("--dreamcast-disc", type=Path, metavar="PATH",
                        help="the cue sheet of the player's Dreamcast disc (default: what "
                             "GDL_LEGENDS_DREAMCAST_DISC names, else the one in assets/dreamcast)")
    parser.add_argument("--launcher", type=Path, metavar="PATH",
                        help="the headless program to run, which is then not built")
    parser.add_argument("--strict", choices=("script",),
                        help="have the launcher hold the dumps as the extraction scripts do, "
                             "rather than as an installation does")
    parser.add_argument("--keep", action="store_true", help="keep the trees of a passing run")
    parser.add_argument("--jobs", type=int, default=default_jobs(),
                        help="cargo's build jobs (default: %(default)s, half the machine's "
                             f"threads and {MOST_JOBS} at most)")
    args = parser.parse_args(argv)
    if not args.verify:
        parser.error("pass --verify to run the gate")
    if args.jobs < 1:
        parser.error("--jobs takes 1 or more")
    return args


def resolved(given: Path | None, variable: str, default: Path,
             environment: Mapping[str, str]) -> Path:
    """A file or folder the run reads: the one an option gives, else the one `variable` names in
    `environment`, else `default`; made absolute, since the launcher's tests run from their
    crates' folders."""
    if given is None:
        given = Path(environment[variable]) if environment.get(variable) else default
    return Path(os.path.abspath(given))


def missing_inputs(dumps: dict[str, Path], trees: dict[str, Path],
                   program: Path | None) -> list[str]:
    """What the run needs and does not find, in words, one for each: any dump or reference tree,
    all of which the launcher's tests read whichever platforms are compared, and the program
    --launcher names."""
    missing = [f"the {platform} dump is not at {dump}; pass {DUMP_OPTIONS[platform]} or set "
               f"{DUMP_VARIABLES[platform]}"
               for platform, dump in dumps.items() if not dump.is_file()]
    missing += [f"the {platform} reference tree is not at {tree}; set {TREE_VARIABLES[platform]}"
                for platform, tree in trees.items() if not tree.is_dir()]
    if program is not None and not program.is_file():
        missing.append(f"there is no launcher at {program}")
    return missing


def end_with_caller() -> None:
    """Ends this run once the process that started it is gone. The interrupt ends a cargo run and
    its tree, as an interrupt at the terminal does; a run that the interrupt has not ended
    STOP_GRACE seconds later, such as one waiting on a child, exits outright, which ends every
    child tethered to it."""
    def end() -> None:
        _thread.interrupt_main()
        time.sleep(STOP_GRACE)
        os._exit(FAILED)

    process_tree.stop_when_gone(os.getppid(), end)


def announce(step: str) -> None:
    """Says which step the run is at, before that step's children write lines of their own."""
    print(f"launcher parity: {step}", flush=True)


def remove(folder: Path) -> None:
    """Removes a folder of trees and everything in it, where it is there."""
    if folder.is_symlink() or folder.is_file():
        folder.unlink()
    elif folder.exists():
        shutil.rmtree(folder)


def clear_report(folders: Folders) -> None:
    """Removes an earlier run's report, which would otherwise stand for this run should it be
    stopped or interrupted before it writes its own."""
    try:
        remove(folders.report)
    except OSError as error:
        raise Failed(f"the last run's report could not be removed: {error}") from error


def make_tree_folders(folders: Folders) -> None:
    """Makes the folders the trees go into anew and empty, parents included, removing what an
    earlier run kept: the launcher refuses an output whose parent is not there, though the
    scripts make theirs."""
    try:
        for folder in (folders.python, folders.launcher):
            remove(folder)
            folder.mkdir(parents=True)
    except OSError as error:
        raise Failed(f"the folders for the trees could not be made anew: {error}") from error


def cargo_failure(what: str, code: int, timeout: float) -> str:
    """Why a cargo run failed, in words."""
    if code == launcher.TIMED_OUT:
        return f"{what} ran longer than {timeout:g} s and was stopped"
    return f"{what} exited {code}"


def run_child(command: list[str], what: str, timeout: float) -> tuple[bool, str | None]:
    """Runs a child tethered to this run, so that it ends should this run end first, and stops it
    once it outlasts `timeout` seconds: whether it ran to its end, and what went wrong, None when
    it exited 0."""
    try:
        done = process_tree.run_tethered(command, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, f"{what} ran longer than {timeout:g} s and was stopped"
    except OSError as error:
        return False, f"{what} did not start: {error}"
    return True, None if done.returncode == 0 else f"{what} exited {done.returncode}"


def timed_run(command: list[str], what: str) -> tuple[str | None, float]:
    """Runs an extraction as run_child runs a child, within EXTRACTION_TIMEOUT: what went wrong,
    if anything, and how many seconds it took, to a tenth."""
    started = time.monotonic()
    _, problem = run_child(command, what, EXTRACTION_TIMEOUT)
    return problem, round(time.monotonic() - started, 1)


def is_link(entry: os.DirEntry) -> bool:
    """Whether a folder's entry is a link of any kind: a symbolic link, or on Windows a junction
    or another reparse point."""
    attributes = getattr(entry.stat(follow_symlinks=False), "st_file_attributes", 0)
    return entry.is_symlink() or bool(attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def entries_of(root: Path) -> dict[str, str]:
    """What lies at every path under `root`, by the path from the root in the letters the system
    stores, "/" between its components: REGULAR_FILE, or what the entry is instead. A folder that
    holds something shows only through what it holds; one that holds nothing has its path end in
    "/". A tree that is not there holds nothing."""
    found: dict[str, str] = {}
    if not root.is_dir():
        return found
    folders = [(root, "")]
    while folders:
        folder, above = folders.pop()
        with os.scandir(folder) as listing:
            children = list(listing)
        if not children and above:
            found[above] = EMPTY_FOLDER
        for entry in children:
            path = above + entry.name
            if is_link(entry):
                found[path] = LINK
            elif entry.is_dir(follow_symlinks=False):
                folders.append((Path(entry.path), path + "/"))
            elif entry.is_file(follow_symlinks=False):
                found[path] = REGULAR_FILE
            else:
                found[path] = NOT_A_FILE
    return found


def fingerprint(path: Path) -> tuple[int, str]:
    """A file's size and SHA-256, read a block at a time."""
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(HASH_BLOCK), b""):
            digest.update(block)
            size += len(block)
    return size, digest.hexdigest()


def described(held: tuple[int, str] | str | None) -> str:
    """What a tree holds at a path, in words."""
    if held is None:
        return "nothing"
    if isinstance(held, tuple):
        size, digest = held
        return f"{size} bytes (SHA-256 {digest})"
    return held


def differences(made: Tree, other: Tree, paths: Iterable[str], other_name: str) -> list[str]:
    """Each of `paths` at which the launcher's tree `made` and `other`, which `other_name` names,
    do not hold the same regular file, as a line."""
    found = []
    for path in paths:
        mine, theirs = made.at(path), other.at(path)
        if not (isinstance(mine, tuple) and mine == theirs):
            found.append(f"{path}: the launcher's tree holds {described(mine)}, {other_name} "
                         f"{described(theirs)}")
    return found


def read_manifest(made: Tree, name: str) -> dict:
    """The manifest `name` of the launcher's tree, read as JSON."""
    if made.entries.get(name) != REGULAR_FILE:
        raise ManifestError(f"the launcher's tree holds no {name}")
    with (made.root / name).open("rb") as stream:
        data = stream.read(MAX_MANIFEST_BYTES + 1)
    if len(data) > MAX_MANIFEST_BYTES:
        raise ManifestError(f"{name} is over {MAX_MANIFEST_BYTES} bytes")
    try:
        manifest = json.loads(data.decode("utf-8"))
    except (ValueError, RecursionError) as error:
        raise ManifestError(f"{name} is no JSON: {error}") from error
    if not isinstance(manifest, dict):
        raise ManifestError(f"{name} is no JSON object")
    return manifest


def listed(manifest: dict, key: str, name: str, folder: str = "") -> list[str]:
    """The tree paths of the rows the manifest `name` lists under `key`, each row's path taken in
    `folder` where one is given."""
    rows = manifest.get(key)
    if not isinstance(rows, list):
        raise ManifestError(f"{name} lists no {key}")
    paths = []
    for number, row in enumerate(rows):
        path = row.get("path") if isinstance(row, dict) else None
        if not isinstance(path, str):
            raise ManifestError(f"{name} has no path in its {key} row {number}")
        paths.append(f"{folder}/{path}" if folder else path)
    return paths


def expected(platform: str, made: Tree) -> tuple[list[str], dict[str, int], list[str]]:
    """The paths the launcher's tree of `platform` must hold, as its manifests name them, with the
    names every such tree holds; the counts the report gives of the tree; and each way in which it
    is not what the player's dump holds."""
    if platform == "n64":
        manifest = read_manifest(made, N64_MANIFEST)
        files = listed(manifest, "files", N64_MANIFEST)
        programs = listed(manifest, "programs", N64_MANIFEST)
        wrong = [f"{N64_MANIFEST} lists {count} {key}, not {wanted}"
                 for key, count, wanted in (("files", len(files), N64_FILES),
                                            ("programs", len(programs), N64_PROGRAMS))
                 if count != wanted]
        counts = {"files": len(files), "programs": len(programs)}
        return [N64_MANIFEST, *files, *programs], counts, wrong
    disc = listed(read_manifest(made, DISC_MANIFEST), "files", DISC_MANIFEST, DISC_FOLDER)
    native = listed(read_manifest(made, NATIVE_MANIFEST), "files", NATIVE_MANIFEST, NATIVE_FOLDER)
    wrong = [f"{name} lists {count} files, not {wanted}"
             for name, count, wanted in ((DISC_MANIFEST, len(disc), DREAMCAST_DISC_FILES),
                                         (NATIVE_MANIFEST, len(native), DREAMCAST_NATIVE_FILES))
             if count != wanted]
    boot = made.at(IP_BIN)
    if boot is None:
        wrong.append(f"the launcher's tree holds no {IP_BIN}")
    elif not isinstance(boot, tuple):
        wrong.append(f"{IP_BIN} is {boot}")
    elif boot[0] != IP_BIN_SIZE:
        wrong.append(f"{IP_BIN} holds {boot[0]} bytes, not {IP_BIN_SIZE}")
    counts = {"discFiles": len(disc), "nativeFiles": len(native)}
    return [IP_BIN, DISC_MANIFEST, NATIVE_MANIFEST, *disc, *native], counts, wrong


def uncompared(platform: str) -> dict:
    """A platform's counts and verdicts in the report before its trees are compared."""
    return {**{key: None for key in COUNTED[platform]}, "identical": False,
            "reference": NOT_COMPARED, "referenceExtras": [], "differences": []}


def compare_trees(platform: str, made_root: Path, written_root: Path,
                  reference_root: Path) -> dict:
    """The launcher's tree of `platform` at `made_root` against the scripts' at `written_root`,
    path for path, and against the reference tree at `reference_root` at every path the
    launcher's manifests name: the platform's counts and verdicts for the report, and every
    difference found."""
    made, written, reference = Tree(made_root), Tree(written_root), Tree(reference_root)
    result = uncompared(platform)
    found = differences(made, written, sorted(set(made.entries) | set(written.entries)),
                        "the scripts'")
    result["identical"] = not found
    try:
        paths, counts, wrong = expected(platform, made)
    except ManifestError as error:
        result["differences"] = [*found, str(error)]
        return result
    named = sorted(set(paths))
    against_reference = differences(made, reference, named, "the reference tree")
    result.update(counts)
    result["reference"] = DIFFERS if against_reference else IDENTICAL
    result["referenceExtras"] = sorted(set(reference.entries) - set(named))
    result["differences"] = [*found, *against_reference, *wrong]
    return result


def check_platform(platform: str, dump: Path, reference: Path, program: Path,
                   strictness: list[str], folders: Folders) -> dict:
    """Extracts the platform's tree from `dump` with its script and with the launcher, then
    compares the launcher's: the platform's record for the report, which lists no difference
    once the platform has passed."""
    written, made = folders.python / platform, folders.launcher / platform
    record = {"dump": str(dump), "scriptSeconds": None, "launcherSeconds": None,
              **uncompared(platform)}
    announce(f"{platform}: the extraction script")
    # Isolated: the scripts need nothing but the standard library, and none of the caller's
    # Python settings reach them.
    script = [sys.executable, "-I", str(SCRIPTS[platform]), str(dump), "--output", str(written)]
    problem, record["scriptSeconds"] = timed_run(script, "the extraction script")
    if problem is None:
        announce(f"{platform}: the launcher")
        extraction = [str(program), "--extract", platform, str(dump), "--output", str(made),
                      *strictness]
        problem, record["launcherSeconds"] = timed_run(extraction, "the launcher")
    if problem is not None:
        record["differences"].append(problem)
        return record
    announce(f"{platform}: comparing the trees")
    try:
        record.update(compare_trees(platform, made, written, reference))
    except OSError as error:
        record["differences"].append(f"the trees could not be read: {error}")
    return record


def dump_variables(dumps: dict[str, Path], trees: dict[str, Path]) -> dict[str, str]:
    """The variables that tell the launcher's ignored tests where the dumps and the reference
    trees are."""
    return {**{DUMP_VARIABLES[platform]: str(dump) for platform, dump in dumps.items()},
            **{TREE_VARIABLES[platform]: str(tree) for platform, tree in trees.items()}}


def counted(count: int, noun: str) -> str:
    return f"{count} {noun}" + ("" if count == 1 else "s")


def seconds(value: float | None) -> str:
    return "not run" if value is None else f"{value:.1f} s"


def contents(platform: str, record: dict) -> str:
    """What a platform's identical tree holds, in words."""
    if platform == "n64":
        return f"{record['files']} files, {record['programs']} programs, manifest"
    return (f"{record['discFiles']} disc files, IP.BIN, {record['nativeFiles']} native files, "
            "2 manifests")


def extras_named(extras: list[str]) -> str:
    """The reference tree's extras in words: each one at its root by its name, and each folder at
    its root by its name and how many extras lie in it."""
    groups: dict[str, int] = {}
    for path in extras:
        top, slash, below = path.partition("/")
        groups[top + slash] = groups.get(top + slash, 0) + (1 if below else 0)
    if not groups:
        return "none"
    return ", ".join(f"{name} ({counted(count, 'file')})" if name.endswith("/") else name
                     for name, count in groups.items())


def summary(platform: str, record: dict, strictness: str, report: Path) -> list[str]:
    """What a run prints of a platform: a line, and below it, where the platform failed, its
    first differences."""
    times = (f"scripts {seconds(record['scriptSeconds'])}, launcher "
             f"{seconds(record['launcherSeconds'])} ({strictness})")
    found = record["differences"]
    if not found:
        return [f"{platform}: identical ({contents(platform, record)}); {times}; reference "
                f"extras: {extras_named(record['referenceExtras'])}"]
    lines = [f"{platform}: {counted(len(found), 'difference')}; {times}; all of them in {report}"]
    lines += [f"  {line}" for line in found[:SHOWN_DIFFERENCES]]
    if len(found) > SHOWN_DIFFERENCES:
        lines.append(f"  and {len(found) - SHOWN_DIFFERENCES} more")
    return lines


def write_report(path: Path, report: dict) -> None:
    """Writes the report as indented JSON in ASCII, ending in a newline, LF on every system."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes((json.dumps(report, indent=2) + "\n").encode("ascii"))


def saved(folders: Folders, report: dict) -> bool:
    """Writes the report, saying so where it cannot: whether it was written."""
    try:
        write_report(folders.report, report)
    except OSError as error:
        print(f"launcher parity: the report could not be written: {error}", file=sys.stderr)
        return False
    return True


def new_report(args: argparse.Namespace, program: Path) -> dict:
    """The report of a run that has done nothing yet: the program it runs, and the strictness the
    launcher holds the dumps to."""
    return {"schema": REPORT_SCHEMA, "launcher": str(program),
            "strictness": "script" if args.strict else "install", "ignoredTests": NOT_RUN,
            "fingerprints": NOT_CHECKED, "platforms": {}}


def verify(args: argparse.Namespace, dumps: dict[str, Path], trees: dict[str, Path],
           program: Path, folders: Folders) -> int:
    """Runs the gate's steps in order, the first that fails ending the run, and removes a passing
    run's trees unless --keep; then writes the report and prints each compared platform's
    summary."""
    report = new_report(args, program)
    strict = ["--strict", "script"] if args.strict else []
    chosen = args.platforms or launcher.PLATFORMS
    platforms = [platform for platform in launcher.PLATFORMS if platform in chosen]
    try:
        clear_report(folders)
        make_tree_folders(folders)
        env = launcher.environment()
        env["CARGO_BUILD_JOBS"] = str(args.jobs)
        # Cargo runs in this process, through launcher.run, which alone can end its whole tree:
        # cargo starts in a session of its own, which a child's timeout would never reach.
        if args.launcher is None:
            announce(f"building {launcher.HEADLESS_PACKAGE} (release)")
            build = ["cargo", "build", "--locked", "--release", "--package",
                     launcher.HEADLESS_PACKAGE]
            code = launcher.run(build, env, timeout=BUILD_TIMEOUT)
            if code != 0:
                raise Failed(cargo_failure("the launcher's build", code, BUILD_TIMEOUT))
        announce("the launcher's tests on the dumps and the reference trees")
        packages = [part for name in TEST_PACKAGES for part in ("--package", name)]
        tests = ["cargo", "test", "--locked", *packages, "--", "--ignored", "--test-threads",
                 str(TEST_THREADS)]
        code = launcher.run(tests, {**env, **dump_variables(dumps, trees)}, timeout=TEST_TIMEOUT)
        report["ignoredTests"] = TESTS_PASSED if code == 0 else TESTS_FAILED
        if code != 0:
            raise Failed(cargo_failure("the launcher's tests on the dumps", code, TEST_TIMEOUT))
        for platform in platforms:
            record = check_platform(platform, dumps[platform], trees[platform], program, strict,
                                    folders)
            report["platforms"][platform] = record
            if record["differences"]:
                raise Failed(f"the {platform} trees did not pass")
        if "dreamcast" in platforms:
            announce("the Dreamcast fingerprints the launcher carries")
            check = [sys.executable, "-I", str(FINGERPRINTS), "--tree",
                     str(folders.python / "dreamcast"), "--check"]
            finished, problem = run_child(check, "the fingerprints' check", FINGERPRINTS_TIMEOUT)
            if not finished:
                raise Failed("the Dreamcast fingerprints the launcher carries could not be "
                             f"checked: {problem}")
            report["fingerprints"] = CURRENT if problem is None else NOT_CURRENT
            if problem is not None:
                raise Failed("the Dreamcast fingerprints the launcher carries are not those of "
                             f"the scripts' tree: {problem}")
        if not args.keep:
            # Before the report is written, so that no report of a pass stands beside trees the
            # run could not remove.
            try:
                remove(folders.python)
                remove(folders.launcher)
            except OSError as error:
                raise Failed(f"the trees could not be removed: {error}") from error
    except Failed as failed:
        report["failure"] = str(failed)
    if not saved(folders, report):
        return FAILED
    # Flushed, so that the summaries come before a failure's line wherever the two streams go.
    for platform, record in report["platforms"].items():
        for line in summary(platform, record, report["strictness"], folders.report):
            print(line, flush=True)
    if "failure" in report:
        print(f"launcher parity: {report['failure']}; the report is {folders.report}, and what "
              f"the run extracted stays in {folders.output}", file=sys.stderr)
        return FAILED
    print(f"launcher parity: passed; the report is {folders.report}")
    return PASSED


def main(argv: list[str] | None = None, environment: Mapping[str, str] = os.environ,
         output: Path = OUTPUT) -> int:
    """Runs the gate on the dumps and trees the arguments and `environment` name, writing its
    trees and report in `output`."""
    args = arguments(argv)
    given = {"n64": args.n64_rom, "dreamcast": args.dreamcast_disc}
    dumps = {platform: resolved(given[platform], DUMP_VARIABLES[platform],
                                DEFAULT_DUMPS[platform], environment)
             for platform in launcher.PLATFORMS}
    trees = {platform: resolved(None, TREE_VARIABLES[platform], REFERENCE[platform], environment)
             for platform in launcher.PLATFORMS}
    program = BUILT_PROGRAM if args.launcher is None else Path(os.path.abspath(args.launcher))
    folders = Folders(output)
    missing = missing_inputs(dumps, trees, None if args.launcher is None else program)
    if missing:
        for problem in missing:
            print(f"launcher parity: {problem}", file=sys.stderr)
        # Reported as every run that ends is, so that no earlier run's report stands for it.
        report = new_report(args, program)
        report["failure"] = "; ".join(missing)
        saved(folders, report)
        return FAILED
    # Before any thread starts: on Linux the nice value is a thread's, which the threads started
    # later, and the children they start, take from this one.
    process_tree.lower_priority()
    # A signal that ends this run never reaches cargo, which runs in a session of its own; as an
    # interrupt, it ends cargo's tree too.
    launcher.stop_on_signals()
    end_with_caller()
    return verify(args, dumps, trees, program, folders)


if __name__ == "__main__":
    sys.exit(main())
