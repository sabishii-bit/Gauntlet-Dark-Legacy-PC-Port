#!/usr/bin/env python3
"""Run clang-tidy over every translation unit in compile_commands.json.

Findings in headers are reported once. Exit status is 1 when anything is
reported. Configure and build first so compile_commands.json exists. A lint that is
interrupted, told to stop, or whose caller is gone ends its clang-tidy workers. CI can
instead configure with CMAKE_CXX_SCAN_FOR_MODULES=OFF (no C++ modules are used)
and build only compile_commands, avoiding build-only module mapper files.

    python scripts/lint.py             # everything
    python scripts/lint.py src/game    # a subtree or single file
    python scripts/lint.py --shard-index 0 --shard-count 4  # first CI shard
    python scripts/lint.py --changed origin/main   # only what a change touches
    python scripts/lint.py --changed origin/main --quick   # the hook's gate
    python scripts/lint.py --profile full src/game/level   # with the static analyzer

The fast profile, the default, runs the checks .clang-tidy enables, as clangd does in the
editor. The full profile adds the clang static analyzer (clang-analyzer-*, less the families
that cannot fire here), which costs from nothing to two thirds of a unit's time (a typical
test unit takes a third longer) and has reported only false positives and its own crashes
in this tree, so it runs only when asked for. The pre-push hook and CI run fast.

--changed lints the translation units affected by the files that differ from a git
revision (or a range such as a..b): the changed sources themselves and every unit
that includes a changed header, directly or through other headers. --quick keeps one
unit per changed header (its own .cpp when there is one) instead of the whole fan-out;
the pre-push hook under scripts/hooks runs it over what is being pushed, and CI lints
everything, cached. Nothing affected passes.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

from lint_cache import CleanCache
import process_tree

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIAGNOSTIC = re.compile(r"^(.+?):(\d+):(\d+): (warning|error): (.*)$")


def find_clang_tidy() -> str:
    candidates = [
        os.environ.get("CLANG_TIDY"),
        shutil.which("clang-tidy"),
        r"C:\Program Files\LLVM\bin\clang-tidy.exe",
        "/usr/bin/clang-tidy",
        "/usr/local/bin/clang-tidy",
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists():
            return candidate
    sys.exit("clang-tidy not found; set CLANG_TIDY or put it on PATH")


def translation_units(filters: list[str]) -> list[pathlib.Path]:
    database = ROOT / "compile_commands.json"
    if not database.exists():
        sys.exit("compile_commands.json not found at the repository root; configure and build first")
    wanted = [(ROOT / f).resolve() for f in filters] or [ROOT / "src", ROOT / "tests"]
    units = []
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = pathlib.Path(entry["file"])
        if not path.is_absolute():
            path = pathlib.Path(entry["directory"]) / path
        path = path.resolve()
        if any(path == w or w in path.parents for w in wanted):
            units.append(path)
    return sorted(set(units))


INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.MULTILINE)
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp", ".inl")


def changed_files(revision: str, git: str = "git") -> list[pathlib.Path]:
    """Sources that differ from `revision` (a commit, or a range such as a..b), including
    the working tree when a single revision is given."""
    completed = subprocess.run([git, "diff", "--name-only", "--diff-filter=ACMR", revision],
                               cwd=ROOT, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        sys.exit(f"git diff {revision} failed: {completed.stderr.strip()}")
    return [(ROOT / line.strip()).resolve() for line in completed.stdout.splitlines()
            if line.strip().endswith(SOURCE_SUFFIXES)]


def include_name(path: pathlib.Path) -> str:
    """How a header is written in an #include: its path under src or tests."""
    relative = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else path.as_posix()
    for prefix in ("src/", "tests/"):
        if relative.startswith(prefix):
            return relative[len(prefix):]
    return relative


def affected_units(roster: list[pathlib.Path], changed: list[pathlib.Path],
                   headers: list[pathlib.Path]) -> list[pathlib.Path]:
    """The roster units to lint for `changed`: changed units themselves, plus every unit
    that includes a changed header, following headers that include headers."""
    includes_of = {}
    for path in [*headers, *roster]:
        try:
            includes_of[path] = set(INCLUDE.findall(path.read_text(encoding="utf-8",
                                                                   errors="replace")))
        except OSError:
            includes_of[path] = set()
    header_set = set(headers)
    affected_names = {include_name(path) for path in changed if path in header_set}
    grown = True
    while grown:
        grown = False
        for header in headers:
            name = include_name(header)
            if name not in affected_names and includes_of[header] & affected_names:
                affected_names.add(name)
                grown = True
    changed_set = set(changed)
    return sorted(unit for unit in roster
                  if unit in changed_set or includes_of[unit] & affected_names)


def header_roster() -> list[pathlib.Path]:
    return sorted(path.resolve() for folder in ("src", "tests")
                  for path in (ROOT / folder).rglob("*") if path.suffix in (".h", ".hpp", ".inl"))


def quick_units(roster: list[pathlib.Path], changed: list[pathlib.Path],
                headers: list[pathlib.Path]) -> list[pathlib.Path]:
    """--quick: the changed units and, for each changed header, one unit that includes it,
    its own .cpp when that is one of them, else the first; the full fan-out is CI's."""
    roster_set = set(roster)
    header_set = set(headers)
    units = {unit for unit in changed if unit in roster_set}
    for header in changed:
        if header not in header_set:
            continue
        reached = affected_units(roster, [header], headers)
        if reached:
            home = header.with_suffix(".cpp")
            units.add(home if home in reached else reached[0])
    return sorted(units)


def select_shard(units: list[pathlib.Path], index: int, count: int) -> list[pathlib.Path]:
    """Partition the complete sorted roster, without depending on machine paths or hashes."""
    if count < 1 or not 0 <= index < count:
        raise ValueError("shard count must be positive and 0 <= shard index < shard count")
    return units[index::count]


# The clang static analyzer, which .clang-tidy leaves out, less its enum range check and the
# families that cannot fire here: Apple, WebKit, Fuchsia and MPI APIs, Grand Central Dispatch
# and the nullability qualifiers, which no source uses.
ANALYZER_CHECKS = ",".join([
    "clang-analyzer-*", "-clang-analyzer-optin.core.EnumCastOutOfRange",
    *(f"-clang-analyzer-{family}" for family in (
        "osx.*", "optin.osx.*", "webkit.*", "fuchsia.*", "optin.mpi.*",
        "optin.performance.GCDAntipattern", "nullability.*")),
])
# The LLVM 18 analyzer crashes while inlining the MSVC 14.44 <format> sources; keep it out of
# standard-library bodies. Nothing but the analyzer reads this.
ANALYZER_CONFIG = ["--extra-arg=-Xclang", "--extra-arg=-analyzer-config",
                   "--extra-arg=-Xclang", "--extra-arg=c++-stdlib-inlining=false"]
PROFILES = {"fast": [], "full": [f"--checks={ANALYZER_CHECKS}", *ANALYZER_CONFIG]}
DEFAULT_PROFILE = "fast"


class Workers:
    """The clang-tidy processes under way, so a stopped lint ends them instead of
    leaving them to finish for nobody."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._running: set[subprocess.Popen] = set()
        self.stopped = False

    def run(self, command: list[str]) -> subprocess.CompletedProcess:
        with self._lock:
            if self.stopped:
                raise InterruptedError("the lint was stopped")
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, cwd=ROOT)
            self._running.add(process)
        try:
            stdout, stderr = process.communicate()
        finally:
            with self._lock:
                self._running.discard(process)
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)

    def running(self) -> int:
        with self._lock:
            return len(self._running)

    def stop(self) -> None:
        with self._lock:
            self.stopped = True
            running = list(self._running)
        for process in running:
            try:
                process.kill()
            except OSError:
                pass  # already gone


WORKERS = Workers()


def execute(command: list[str]) -> subprocess.CompletedProcess:
    """One clang-tidy run under the worker registry."""
    return WORKERS.run(command)


def run_one(clang_tidy: str, unit: pathlib.Path, profile: str = DEFAULT_PROFILE) -> list[str]:
    command = [clang_tidy, "-p", str(ROOT), "--quiet", *PROFILES[profile],
               "--extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH", str(unit)]
    completed = execute(command)
    findings = []
    current = None
    for line in (completed.stdout + "\n" + completed.stderr).splitlines():
        match = DIAGNOSTIC.match(line)
        if match:
            current = line
            findings.append(line)
        elif current is not None and line.strip():
            findings[-1] += "\n" + line
    if completed.returncode != 0 and not findings:
        findings.append(f"{unit}: clang-tidy failed (exit {completed.returncode}):\n"
                        f"{completed.stdout.strip()}\n{completed.stderr.strip()}")
    return findings


_parent_watch: threading.Thread | None = None


def watch_parent() -> None:
    """Stop the workers once the process this lint answers to is gone."""
    global _parent_watch
    if _parent_watch is None:
        _parent_watch = process_tree.stop_when_gone(os.getppid(), lambda: WORKERS.stop())


def _raise_interrupt() -> None:
    raise KeyboardInterrupt


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("paths", nargs="*", help="source files or subtrees (default: src and tests)")
    parser.add_argument("--shard-index", type=int, default=0, help="zero-based CI shard index")
    parser.add_argument("--shard-count", type=int, default=1, help="number of disjoint CI shards")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4,
                        help="maximum concurrent clang-tidy processes")
    parser.add_argument("--cache-dir", type=pathlib.Path,
                        help="reuse clean results after hashing clang dependencies (GCC-style commands)")
    parser.add_argument("--changed", metavar="REVISION",
                        help="lint only the units affected by the sources that differ from REVISION")
    parser.add_argument("--quick", action="store_true",
                        help="with --changed: one unit per changed header instead of every includer")
    parser.add_argument("--profile", choices=sorted(PROFILES), default=DEFAULT_PROFILE,
                        help="fast (the default; the hook's and CI's) runs the checks "
                             ".clang-tidy enables; full adds the clang static analyzer, up to "
                             "three times a unit's time")
    args = parser.parse_args(argv)
    if args.quick and not args.changed:
        parser.error("--quick needs --changed")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        select_shard([], args.shard_index, args.shard_count)
    except ValueError as error:
        parser.error(str(error))
    global WORKERS
    WORKERS = Workers()
    watch_parent()
    signal.signal(signal.SIGTERM, lambda _signal, _frame: _raise_interrupt())
    clang_tidy = find_clang_tidy()
    roster = translation_units(args.paths)
    if not roster:
        parser.error("no translation units found; check the paths and compilation database")
    if args.changed:
        select = quick_units if args.quick else affected_units
        roster = select(roster, changed_files(args.changed), header_roster())
        if not roster:
            print(f"No translation units are affected by the changes since {args.changed}.")
            return 0
    units = select_shard(roster, args.shard_index, args.shard_count)
    started = time.monotonic()
    cache = None
    if args.cache_dir:
        try:
            cache = CleanCache(ROOT, args.cache_dir, clang_tidy, args.profile)
        except (OSError, ValueError) as error:
            print(f"Lint cache unavailable; checking every unit: {error}", flush=True)

    def check(unit):
        key = cache.key(unit) if cache else None
        if key and cache.contains(key):
            return [], True
        findings = run_one(clang_tidy, unit, args.profile)
        if not findings and key and cache.key(unit) == key:
            try:
                cache.remember(key)
            except OSError:
                pass  # an unwritable cache must not hide or fail a clean analysis
        return findings, False

    print(f"Lint shard {args.shard_index + 1}/{args.shard_count}: "
          f"{len(units)} of {len(roster)} translation units, {args.jobs} workers, "
          f"{args.profile} profile", flush=True)
    seen: set[str] = set()
    ordered: list[str] = []
    hits = 0
    interrupted = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(check, unit): unit for unit in units}
        remaining = set(pending)
        done = 0
        try:
            while remaining:
                if WORKERS.stopped:
                    raise KeyboardInterrupt
                # A short wait keeps an interrupt prompt on Windows, where a bare wait
                # cannot be interrupted.
                finished, remaining = concurrent.futures.wait(
                    remaining, timeout=0.5, return_when=concurrent.futures.FIRST_COMPLETED)
                for future in finished:
                    done += 1
                    unit = pending[future]
                    try:
                        findings, reused = future.result()
                        hits += int(reused)
                    except Exception as error:
                        findings = [f"{unit}: clang-tidy could not complete: {error}"]
                    label = unit.relative_to(ROOT) if unit.is_relative_to(ROOT) else unit
                    print(f"[{done}/{len(units)}] {label} "
                          f"({time.monotonic() - started:.1f}s elapsed)", flush=True)
                    for finding in findings:
                        key = finding.split("\n", 1)[0]
                        if key not in seen:
                            seen.add(key)
                            ordered.append(finding)
        except KeyboardInterrupt:
            interrupted = True
            WORKERS.stop()
            pool.shutdown(wait=False, cancel_futures=True)
    if interrupted:
        print(f"Lint stopped after {done} of {len(units)} translation units; "
              "its clang-tidy workers were ended", file=sys.stderr, flush=True)
        return 130
    for finding in sorted(ordered):
        print(finding)
        print()
    print(f"{len(units)} translation units checked ({hits} cached), {len(ordered)} findings "
          f"in {time.monotonic() - started:.1f}s", flush=True)
    return 1 if ordered else 0


if __name__ == "__main__":
    sys.exit(main())
