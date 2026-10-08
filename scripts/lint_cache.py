"""Content-addressed clean clang-tidy results for GCC-style compilation databases.

Scan dependencies afresh with clang on every invocation: a newly added header can
shadow an existing include even when none of yesterday's dependencies changed.
Hash complete files, not preprocessed text, so comments/NOLINT and directives count.
Any unsupported command or failed dependency scan falls back to uncached analysis.
"""

import hashlib
import json
import os
import pathlib
import shlex
import subprocess
import tempfile
import threading


def dependency_command(entry: dict, compiler: pathlib.Path, depfile: pathlib.Path) -> list[str]:
    arguments = entry.get("arguments") or shlex.split(entry["command"])
    if not arguments or any(arg.startswith("@") for arg in arguments):
        raise ValueError("response files are not cacheable")
    if pathlib.Path(arguments[0]).stem.lower() in ("cl", "clang-cl"):
        raise ValueError("MSVC commands are not cacheable")
    result = [str(compiler)]
    skip = False
    for argument in arguments[1:]:
        if skip:
            skip = False
        elif argument in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif argument in ("-c", "-MD", "-MMD", "-MP", "-MG"):
            continue
        elif argument.startswith(("-o", "-MF", "-MT", "-MQ")):
            continue
        else:
            result.append(argument)
    if skip:
        raise ValueError("incomplete compiler option")
    return result + ["-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH", "-E", "-MD", "-MF", str(depfile),
                     "-MT", "lint"]


def dependencies(output: str, directory: pathlib.Path) -> list[pathlib.Path]:
    output = output.replace("\\\n", "")
    target, separator, paths = output.partition(":")
    if target.strip() != "lint" or not separator:
        raise ValueError("invalid dependency output")
    files = shlex.split(paths, comments=False)
    if not files:
        raise ValueError("empty dependency output")
    return sorted({(directory / name.replace("$$", "$")).resolve() for name in files})


class CleanCache:
    def __init__(self, root: pathlib.Path, directory: pathlib.Path, tidy: str):
        self.root = root
        self.directory = directory
        tidy_path = pathlib.Path(tidy).resolve()
        self.compiler = tidy_path.with_name("clang++" + tidy_path.suffix)
        self.entries: dict[pathlib.Path, list[dict]] = {}
        for entry in json.loads((root / "compile_commands.json").read_text(encoding="utf-8")):
            unit = (pathlib.Path(entry["directory"]) / entry["file"]).resolve()
            self.entries.setdefault(unit, []).append(entry)
        self.hashes: dict[pathlib.Path, tuple[tuple[int, int], bytes]] = {}
        self.lock = threading.Lock()
        self.identity = hashlib.sha256()
        # These drivers control diagnostics, arguments and cache validity. Tool version
        # strings alone cannot identify a locally patched binary.
        for path in (pathlib.Path(tidy).resolve(), self.compiler, pathlib.Path(__file__),
                     root / "scripts/lint.py"):
            self.identity.update(self.file_hash(path))
        self.directory.mkdir(parents=True, exist_ok=True)

    def file_hash(self, path: pathlib.Path) -> bytes:
        stat = path.stat()
        stamp = stat.st_mtime_ns, stat.st_size
        with self.lock:
            cached = self.hashes.get(path)
            if cached and cached[0] == stamp:
                return cached[1]
            digest = hashlib.sha256(path.read_bytes()).digest()
            self.hashes[path] = stamp, digest
            return digest

    def key(self, unit: pathlib.Path) -> str | None:
        try:
            entries = self.entries.get(unit.resolve(), [])
            if len(entries) != 1:
                return None  # clang-tidy may process multiple configurations for one source
            entry = entries[0]
            directory = pathlib.Path(entry["directory"])
            with tempfile.TemporaryDirectory(prefix="gdl-lint-") as scratch:
                depfile = pathlib.Path(scratch) / "inputs.d"
                command = dependency_command(entry, self.compiler, depfile)
                result = subprocess.run(command, cwd=directory, capture_output=True,
                                        check=False, timeout=120)
                if result.returncode:
                    return None
                files = set(dependencies(depfile.read_text(encoding="utf-8"), directory))
            files.add(unit.resolve())
            # InheritParentConfig may read any of the source's ancestor configurations.
            for parent in unit.resolve().parents:
                config = parent / ".clang-tidy"
                if config.is_file():
                    # Compile-affecting YAML options need matching driver support before
                    # caching them. Never guess dependency inputs from an incomplete command.
                    if "ExtraArgs" in config.read_text(encoding="utf-8"):
                        return None
                    files.add(config)
            digest = self.identity.copy()
            digest.update(json.dumps(entry, sort_keys=True).encode())
            # __has_include can change a branch without including the tested file.
            # Volatile predefined macros and compiler predefines matter too.
            digest.update(result.stdout)
            for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "SDKROOT", "ImageVersion"):
                digest.update(f"{name}={os.environ.get(name, '')}\0".encode())
            for path in sorted(files):
                digest.update(str(path).encode())
                digest.update(b"\0")
                digest.update(self.file_hash(path))
            return digest.hexdigest()
        except (OSError, ValueError, subprocess.SubprocessError):
            return None

    def contains(self, key: str) -> bool:
        try:
            return (self.directory / key).read_text(encoding="ascii") == key
        except (OSError, UnicodeError):
            return False

    def remember(self, key: str) -> None:
        # A partial write is never a hit; separate units/threads have separate keys.
        (self.directory / key).write_text(key, encoding="ascii")
