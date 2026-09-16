#!/usr/bin/env python3
"""Run clang-tidy over every translation unit in compile_commands.json.

Findings in headers are reported once. Exit status is 1 when anything is
reported. Configure and build first so compile_commands.json exists.

    python scripts/lint.py             # everything
    python scripts/lint.py src/game    # a subtree or single file
"""

import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

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
        path = pathlib.Path(entry["file"]).resolve()
        if any(path == w or w in path.parents for w in wanted):
            units.append(path)
    return sorted(set(units))


def run_one(clang_tidy: str, unit: pathlib.Path) -> list[str]:
    command = [clang_tidy, "-p", str(ROOT), "--quiet",
               "--extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
               # The LLVM 18 analyzer crashes while inlining the MSVC 14.44 <format> sources;
               # keep it out of standard-library bodies.
               "--extra-arg=-Xclang", "--extra-arg=-analyzer-config",
               "--extra-arg=-Xclang", "--extra-arg=c++-stdlib-inlining=false",
               str(unit)]
    completed = subprocess.run(command, capture_output=True, text=True, cwd=ROOT, check=False)
    findings = []
    current = None
    for line in completed.stdout.splitlines():
        match = DIAGNOSTIC.match(line)
        if match:
            current = line
            findings.append(line)
        elif current is not None and line.strip():
            findings[-1] += "\n" + line
    if completed.returncode not in (0, 1) and not findings:
        findings.append(f"{unit}: clang-tidy failed:\n{completed.stderr.strip()}")
    return findings


def main() -> int:
    clang_tidy = find_clang_tidy()
    units = translation_units(sys.argv[1:])
    seen: set[str] = set()
    ordered: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for findings in pool.map(lambda u: run_one(clang_tidy, u), units):
            for finding in findings:
                key = finding.split("\n", 1)[0]
                if key not in seen:
                    seen.add(key)
                    ordered.append(finding)
    for finding in ordered:
        print(finding)
        print()
    print(f"{len(units)} translation units linted, {len(ordered)} findings")
    return 1 if ordered else 0


if __name__ == "__main__":
    sys.exit(main())
