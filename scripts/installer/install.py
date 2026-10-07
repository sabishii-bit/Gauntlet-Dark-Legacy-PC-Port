"""Transactional, offline installation. Release payloads never contain retail data."""

import hashlib
from contextlib import contextmanager
import json
import os
import platform
import re
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
import time
import zipfile

from .disc import CHUNK, DiscImage, safe_component
from .icon import MAX_TPL_BYTES, decode_first_frame, embed_icon, icon_resources, refresh_shell_icon
from .versions import SEMVER

MAX_PAYLOAD = 2 * 1024 * 1024 * 1024
PAYLOAD_ROOTS = {"gauntlet.exe", "gauntlet", "shaders", "data", "licenses", "lib",
                 "version", "build-info.json", "portable.flag"}


class Cancelled(Exception):
    """The user stopped extraction before it was committed."""


def validate_file_names(paths):
    """The only files an installation or update may own (never saves or retail data)."""
    names = set()
    for name in paths:
        if not isinstance(name, str) or len(name) > 1024:
            raise ValueError("Invalid release path")
        parts = name.split("/")
        for part in parts:
            safe_component(part)
        key = name.casefold()
        if key in names:
            raise ValueError("Duplicate release entry")
        names.add(key)
        root = parts[0].casefold()
        if root not in PAYLOAD_ROOTS and not (len(parts) == 1 and root.endswith(".dll")):
            raise ValueError(f"Unexpected release payload file: {name}")
        if root in {"gauntlet", "gauntlet.exe", "version", "build-info.json", "portable.flag"} and len(parts) != 1:
            raise ValueError(f"Unexpected release payload directory: {name}")
        if root == "data" and not key.endswith(".json"):
            raise ValueError(f"Non-configuration file in release data: {name}")
    for name in paths:
        if any(parent.as_posix().casefold() in names
               for parent in PurePosixPath(name).parents if parent.parts):
            raise ValueError("File/directory conflict in release payload")
    return names


def validate_metadata(metadata):
    if not isinstance(metadata, dict) or not all(isinstance(metadata.get(key), str)
            for key in ("version", "commit", "platform", "executable")):
        raise ValueError("Invalid release metadata")
    if not SEMVER.fullmatch(metadata["version"]):
        raise ValueError("Invalid semantic release version")
    expected = metadata.get("files")
    if not isinstance(expected, dict) or len(expected) > 20000:
        raise ValueError("Invalid release file inventory")
    names = validate_file_names([*expected, "build-info.json"])
    if not {"build-info.json", "version", "portable.flag", "data/config.json"} <= names:
        raise ValueError("Incomplete release payload")
    executable = metadata["executable"]
    if executable not in ("gauntlet.exe", "gauntlet") or executable not in expected:
        raise ValueError("Missing release executable")
    for digest in expected.values():
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("Invalid release digest")


def host_platform():
    system = platform.system()
    machine = platform.machine().lower()
    # Under Windows emulation, report the native processor rather than the emulated process.
    if system == "Windows":
        machine = os.environ.get("PROCESSOR_ARCHITEW6432", machine).lower()
    if system not in ("Windows", "Linux") or machine not in ("amd64", "x86_64"):
        raise ValueError(f"This alpha supports Windows x64 and Linux x86-64, not {system} {machine}. "
                         "ARM64 and macOS builds are not available yet.")
    return "windows-x64" if system == "Windows" else "linux-x64"


def check_cancel(cancel):
    if cancel():
        raise Cancelled("Installation cancelled; no game files were installed.")


def retry_locked(operation):
    """Windows virus scanners can briefly deny renames/deletes after file close."""
    deadline = time.monotonic() + 8
    while True:
        try:
            return operation()
        except PermissionError:
            if os.name != "nt" or time.monotonic() >= deadline:
                raise
            time.sleep(0.2)


@contextmanager
def temporary_directory(parent, prefix):
    temporary = tempfile.TemporaryDirectory(prefix=prefix, dir=parent)
    try:
        yield Path(temporary.name)
    finally:
        retry_locked(temporary.cleanup)


def payload_inventory(payload: Path):
    """Validate names, sizes, duplicate paths and the integrity-check inventory.

    Hashes detect corruption, not authenticity; download installers from project releases.
    """
    with zipfile.ZipFile(payload) as archive:
        infos = archive.infolist()
        if len(infos) > 20000 or sum(i.file_size for i in infos) > MAX_PAYLOAD:
            raise ValueError("Release payload is too large")
        names = validate_file_names([i.filename for i in infos])
        for info in infos:
            if info.is_dir() or stat.S_ISLNK(info.external_attr >> 16):
                raise ValueError("Duplicate, directory or linked release entry")
        if not {"build-info.json", "version", "portable.flag", "data/config.json"} <= names:
            raise ValueError("Incomplete release payload")
        metadata_info = archive.getinfo("build-info.json")
        if metadata_info.file_size > 4 * 1024 * 1024:
            raise ValueError("Release metadata is too large")
        metadata = json.loads(archive.read(metadata_info))
        validate_metadata(metadata)
        expected = metadata["files"]
        if set(expected) != {i.filename for i in infos if i.filename != "build-info.json"}:
            raise ValueError("Release file inventory does not match its payload")
        return metadata, tuple(infos)


def no_links(path: Path):
    for part in (path, *path.parents):
        if os.path.lexists(part):
            info = part.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError(f"Choose a directory without links or junctions: {part}")


def apply_disc_icon(stage, disc_root, metadata):
    """Personalize verified staging only, then record its installed-byte digest.

    Keep the release digest as provenance. Updates verify the installed digest
    before replacing anything, and repeat this step on their new staged binary.
    Neither public payloads nor the user's native icon file are modified.
    """
    if metadata["platform"] != "windows-x64":
        return
    icon = disc_root / "carddemo" / "icon.tpl"
    no_links(icon)
    with icon.open("rb") as stream:
        data = stream.read(MAX_TPL_BYTES + 1)
    group, images = icon_resources(*decode_first_frame(data))
    executable = stage / metadata["executable"]
    embed_icon(executable, group, images)
    digest = hashlib.sha256()
    with executable.open("r+b") as stream:
        while block := stream.read(CHUNK):
            digest.update(block)
        os.fsync(stream.fileno())
    metadata["discIcon"] = {
        "path": "carddemo/icon.tpl", "sha256": hashlib.sha256(data).hexdigest(),
        "releaseExecutableSha256": metadata["files"][metadata["executable"]],
    }
    metadata["files"][metadata["executable"]] = digest.hexdigest()
    with (stage / "build-info.json").open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def install(image: Path, destination: Path, payload: Path,
            progress=lambda _done, _total, _name: None, cancel=lambda: False,
            updater=None) -> Path:
    """Copy into private staging, then publish only previously absent top-level entries.

    Existing saves/settings/installations are never overwritten. Interrupted extraction
    cleans only this invocation's staging; a failed commit rolls back its own moves.
    """
    destination = Path(os.path.abspath(destination))
    no_links(destination)
    metadata, infos = payload_inventory(payload)
    expected_platform = host_platform()
    if metadata["platform"] != expected_platform:
        raise ValueError("This installer was built for a different operating system")
    with DiscImage(image) as disc:
        roots = {e.path.parts[0] for e in disc.entries}
        roots.update(i.filename.split("/")[0] for i in infos)
        roots.update(("saves", "config", "installation.json"))
        updater_name = "GauntletDarkLegacy-Update.exe" if os.name == "nt" else "GauntletDarkLegacy-Update"
        keep_updater = updater is not None and Path(updater).resolve() != destination / updater_name
        if keep_updater:
            roots.add(updater_name)
        existed = destination.exists()
        if existed and not destination.is_dir():
            raise ValueError("Install destination is not a directory")
        existing = {p.name.casefold() for p in destination.iterdir()} if existed else set()
        collisions = sorted(name for name in roots if name.casefold() in existing)
        if collisions:
            raise ValueError("Existing installation/files would be overwritten: " +
                             ", ".join(collisions) + ". Choose a fresh directory.")
        total = sum(e.size for e in disc.entries) + sum(i.file_size for i in infos)
        if keep_updater:
            total += Path(updater).stat().st_size
        ancestor = destination
        while not ancestor.exists():
            ancestor = ancestor.parent
        if shutil.disk_usage(ancestor).free < total + 64 * 1024 * 1024:
            raise ValueError("Not enough free disk space for the installation")
        check_cancel(cancel)
        destination.mkdir(parents=True, exist_ok=True)
        lock = destination / ".gdl-install.lock"
        # Exclusive creation protects simultaneous installers. Never remove another lock.
        with lock.open("x", encoding="utf-8"):
            pass
        done = 0
        moved = []
        try:
            with temporary_directory(destination, ".gdl-install-") as temporary:
                stage = Path(temporary)
                with zipfile.ZipFile(payload) as archive:
                    for info in infos:
                        check_cancel(cancel)
                        target = stage / info.filename
                        target.parent.mkdir(parents=True, exist_ok=True)
                        digest = hashlib.sha256()
                        with archive.open(info) as source, target.open("xb") as output:
                            while block := source.read(CHUNK):
                                check_cancel(cancel)
                                output.write(block)
                                digest.update(block)
                                done += len(block)
                                progress(done, total, info.filename)
                        if info.filename != "build-info.json" and digest.hexdigest() != metadata["files"][info.filename]:
                            raise ValueError(f"Damaged release payload: {info.filename}")
                        target.chmod(0o755 if info.filename == metadata["executable"] else 0o644)
                for entry in disc.entries:
                    check_cancel(cancel)
                    target = stage.joinpath(*entry.path.parts)
                    if entry.directory:
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with target.open("xb") as output:
                        copied = 0
                        while copied < entry.size:
                            check_cancel(cancel)
                            size = min(CHUNK, entry.size - copied)
                            output.write(disc.read(entry.offset + copied, size))
                            copied += size
                            done += size
                            progress(done, total, entry.path.as_posix())
                check_cancel(cancel)
                apply_disc_icon(stage, stage, metadata)
                (stage / "saves").mkdir()
                (stage / "config").mkdir()
                if keep_updater:
                    shutil.copyfile(updater, stage / updater_name)
                    (stage / updater_name).chmod(0o755)
                    done += (stage / updater_name).stat().st_size
                    progress(done, total, updater_name)
                (stage / "installation.json").write_text(json.dumps({
                    "version": metadata["version"], "commit": metadata["commit"],
                    "platform": metadata["platform"], "disc": "GUNE5D",
                    "discRevision": disc.revision,
                }, indent=2) + "\n", encoding="utf-8")
                check_cancel(cancel)
                no_links(destination)
                for name in sorted(roots):
                    target = destination / name
                    if os.path.lexists(target):
                        raise ValueError(f"Destination changed during installation: {target}")
                    retry_locked(lambda: (stage / name).rename(target))
                    moved.append(target)
                progress(total, total, "Complete")
        except BaseException:
            for path in reversed(moved):
                # These paths were exclusively created by this transaction, never originals.
                if path.is_dir():
                    retry_locked(lambda: shutil.rmtree(path))
                else:
                    retry_locked(path.unlink)
            raise
        finally:
            lock.unlink()
            if not existed and not any(destination.iterdir()):
                destination.rmdir()
    executable = destination / metadata["executable"]
    refresh_shell_icon(executable)
    return executable
