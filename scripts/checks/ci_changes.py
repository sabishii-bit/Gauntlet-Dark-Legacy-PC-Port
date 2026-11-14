#!/usr/bin/env python3
"""Decide whether a push or pull request touches anything the build, tests or lint read.

    python scripts/checks/ci_changes.py --base <sha> --head <sha> [--output $GITHUB_OUTPUT]

Prints `code=true` when any changed path can affect the compiled game, its tests or its
tooling, and `code=false` when every change is documentation (docs/, Markdown, licences),
so CI can skip its build, test and lint jobs for a README edit. An unknown base (a branch's
first push) counts as code.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
DOCUMENT_SUFFIXES = (".md", ".rst")
DOCUMENT_ROOTS = ("docs/",)
DOCUMENT_NAMES = ("LICENSE", "LICENSE.md", "NOTICE", "CODEOWNERS", ".gitattributes",
                  ".gitignore", ".editorconfig")


def is_document(path: str) -> bool:
    """Whether a repository path is read by people only, never by the build, tests or lint."""
    posix = path.replace("\\", "/")
    if posix.startswith(DOCUMENT_ROOTS):
        return True
    name = posix.rsplit("/", 1)[-1]
    if name in DOCUMENT_NAMES or name.startswith("LICENSE"):
        return True
    return name.endswith(DOCUMENT_SUFFIXES) and "/" not in posix.removeprefix(".github/")


def touches_code(paths: list[str]) -> bool:
    return any(not is_document(path) for path in paths)


def changed_paths(base: str | None, head: str, git: str = "git") -> list[str] | None:
    """Paths that differ between base and head; None when the base is unknown."""
    if not base or set(base) == {"0"}:
        return None
    completed = subprocess.run([git, "diff", "--name-only", f"{base}..{head}"], cwd=ROOT,
                               capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        return None
    return [line.strip() for line in completed.stdout.splitlines() if line.strip()]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", help="the commit the change is compared against")
    parser.add_argument("--head", required=True, help="the commit being checked")
    parser.add_argument("--output", type=pathlib.Path,
                        help="append the result to this file (GITHUB_OUTPUT) as well")
    args = parser.parse_args(argv)
    paths = changed_paths(args.base, args.head)
    if paths is None:
        code, reason = True, "no base to compare against"
    else:
        code = touches_code(paths)
        reason = f"{len(paths)} changed paths" + ("" if code else ", documents only")
    line = f"code={'true' if code else 'false'}"
    print(f"{line} ({reason})")
    if args.output:
        with args.output.open("a", encoding="utf-8") as output:
            output.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
