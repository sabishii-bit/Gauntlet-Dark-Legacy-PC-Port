"""In-place runtime updates, with rollback and recoverable interrupted transactions.

Only files owned by build-info.json may be replaced or retired. Disc assets,
saves, config, the updater and unknown files are outside that inventory. Backups
remain on disk until the new receipt commits; recovery never needs the network.
"""

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile
import zipfile

from .disc import CHUNK
from .install import (check_cancel, host_platform, no_links, payload_inventory,
                      retry_locked, temporary_directory, validate_file_names, validate_metadata)
from .releases import download_release
from .versions import version_order

TRANSACTION = ".gdl-update"
RECEIPTS = {"build-info.json", "installation.json"}


def read_json(path, limit=4 * 1024 * 1024):
    no_links(path)
    with path.open("rb") as stream:
        content = stream.read(limit + 1)
    if len(content) > limit:
        raise ValueError(f"Metadata is too large: {path.name}")
    return json.loads(content)


def file_digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(CHUNK):
            result.update(block)
    return result.hexdigest()


def installed_info(destination):
    """Read the old installer's receipt too; no re-extraction or executable launch."""
    no_links(destination)
    metadata = read_json(destination / "build-info.json")
    validate_metadata(metadata)
    receipt = read_json(destination / "installation.json", 65536)
    if not isinstance(receipt, dict) or any(receipt.get(key) != metadata[key]
            for key in ("version", "commit", "platform")) or receipt.get("disc") != "GUNE5D":
        raise ValueError("Installation receipts disagree; choose a complete installed game")
    expected = "gauntlet.exe" if metadata["platform"] == "windows-x64" else "gauntlet"
    if metadata["platform"] != host_platform() or metadata["executable"] != expected:
        raise ValueError("This installation belongs to a different platform")
    version_file = destination / "VERSION"
    no_links(version_file)
    if version_file.stat().st_size > 128 or version_file.read_text(encoding="utf-8").strip() != metadata["version"]:
        raise ValueError("The installed VERSION disagrees with its receipt")
    no_links(destination / metadata["executable"])
    if not (destination / metadata["executable"]).is_file():
        raise ValueError("The installed executable is missing")
    return metadata, receipt


@contextmanager
def update_lock(destination):
    """OS lock survives no process; the empty lock file stays to avoid unlink races."""
    path = destination / ".gdl-update.lock"
    no_links(path)
    with path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"\0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError("Another updater is using this installation") from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)


def require_game_closed(executable):
    """Do not kill a game or update underneath it, including Linux's renameable binaries."""
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.CreateFileW(str(executable), 0x40000000, 0, None, 3, 0, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ValueError("Close the game before updating; its executable is busy or not writable")
        kernel.CloseHandle(handle)
    else:
        for process in Path("/proc").iterdir():
            if not process.name.isdigit():
                continue
            try:
                if (process / "exe").samefile(executable):
                    raise ValueError("Close the game before updating")
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                continue


def atomic_json(path, value):
    temporary = path.with_suffix(".pending")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def _recover(destination):
    transaction = destination / TRANSACTION
    no_links(transaction)
    # Cleanup can be interrupted after deleting the journal but before rmdir.
    if not any(transaction.iterdir()):
        transaction.rmdir()
        return
    journal = read_json(transaction / "journal.json")
    if (not isinstance(journal, dict) or journal.get("schema") != 1 or
            journal.get("phase") not in ("prepared", "committed", "rolled_back")):
        raise ValueError("Unrecognized update journal; recovery files have been retained")
    for key in ("old", "new"):
        paths = journal.get(key)
        if not isinstance(paths, list) or len(paths) > 20002 or not all(isinstance(p, str) for p in paths):
            raise ValueError("Invalid update recovery inventory")
        validate_file_names([p for p in paths if p != "installation.json"])
        if len(set(paths)) != len(paths):
            raise ValueError("Duplicate recovery entries")
        for name in paths:
            no_links(destination / name)
            no_links(transaction / "backup" / name)
            no_links(transaction / "stage" / name)
    if journal["phase"] == "prepared":
        old = set(journal["old"])
        for name in reversed(journal["new"]):
            target = destination / name
            staged = transaction / "stage" / name
            if name not in old and not staged.exists() and target.exists():
                retry_locked(target.unlink)
        for name in reversed(journal["old"]):
            backup = transaction / "backup" / name
            if backup.exists():
                target = destination / name
                target.parent.mkdir(parents=True, exist_ok=True)
                retry_locked(lambda: os.replace(backup, target))
        journal["phase"] = "rolled_back"
        atomic_json(transaction / "journal.json", journal)
    # Keep the journal until the last cleanup step. A partial rmtree must not
    # leave backups without the information needed to distinguish commit/rollback.
    for path in transaction.iterdir():
        if path.name not in {"stage", "backup", "journal.json", "journal.pending"}:
            raise ValueError("Unexpected recovery file; the update backup was retained")
        no_links(path)
    for name in ("stage", "backup"):
        path = transaction / name
        if path.exists():
            retry_locked(lambda: shutil.rmtree(path))
    (transaction / "journal.pending").unlink(missing_ok=True)
    (transaction / "journal.json").unlink()
    transaction.rmdir()


def recover_update(destination):
    destination = Path(os.path.abspath(destination))
    no_links(destination)
    with update_lock(destination):
        # A failed transaction may already have moved the executable to backup.
        for name in ("gauntlet.exe", "gauntlet"):
            for parent in (destination, destination / TRANSACTION / "backup"):
                executable = parent / name
                no_links(executable)
                if executable.is_file():
                    require_game_closed(executable)
        _recover(destination)
    return installed_info(destination)[0]


def apply_update(destination, payload, progress=lambda _done, _total, _name: None,
                 cancel=lambda: False, expected_version=None):
    destination = Path(os.path.abspath(destination))
    no_links(destination)
    with update_lock(destination):
        if (destination / TRANSACTION).exists():
            raise ValueError("Recover the interrupted update before installing another version")
        old, receipt = installed_info(destination)
        metadata, infos = payload_inventory(payload)
        if (metadata["platform"] != old["platform"] or metadata["executable"] != old["executable"] or
                (expected_version is not None and metadata["version"] != expected_version)):
            raise ValueError("Downloaded runtime does not match the selected version and platform")
        if version_order(metadata["version"]) <= version_order(old["version"]):
            raise ValueError("An update must be newer than the installed version")
        old_names = sorted(set(old["files"]) | RECEIPTS)
        new_names = sorted(set(metadata["files"]) | RECEIPTS)
        combined = {name.casefold(): name for name in old_names}
        for name in new_names:
            if name.casefold() in combined and combined[name.casefold()] != name:
                raise ValueError("Update changes a managed path's letter case")
            combined[name.casefold()] = name
        validate_file_names([n for n in combined.values() if n != "installation.json"])
        for name in combined.values():
            target = destination / name
            no_links(target)
            if name not in old_names and target.exists():
                raise ValueError(f"Update would overwrite an unowned file: {name}")
        for name, digest in old["files"].items():
            target = destination / name
            if not target.is_file() or file_digest(target) != digest:
                raise ValueError(f"Installed runtime file was modified or removed: {name}. "
                                 "Keep personal settings in config/settings.json.")
        total = sum(info.file_size for info in infos)
        if shutil.disk_usage(destination).free < total + 64 * 1024 * 1024:
            raise ValueError("Not enough disk space to stage the update safely")
        require_game_closed(destination / old["executable"])
        check_cancel(cancel)
        with temporary_directory(destination, ".gdl-stage-") as temporary:
            stage = temporary / "stage"
            done = 0
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
                        output.flush()
                        os.fsync(output.fileno())
                    if info.filename != "build-info.json" and digest.hexdigest() != metadata["files"][info.filename]:
                        raise ValueError(f"Damaged update payload: {info.filename}")
                    target.chmod(0o755 if info.filename == metadata["executable"] else 0o644)
            if (stage / "VERSION").read_text(encoding="utf-8").strip() != metadata["version"]:
                raise ValueError("Update VERSION disagrees with its metadata")
            receipt.update({key: metadata[key] for key in ("version", "commit", "platform")})
            atomic_json(stage / "installation.json", receipt)
            journal = {"schema": 1, "phase": "prepared", "old": old_names, "new": new_names}
            atomic_json(temporary / "journal.json", journal)
            check_cancel(cancel)
            require_game_closed(destination / old["executable"])
            transaction = destination / TRANSACTION
            # Move the complete transaction out of temporary cleanup before changing originals.
            temporary.rename(transaction)
            try:
                for name in old_names:
                    target = destination / name
                    no_links(target)
                    backup = transaction / "backup" / name
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    retry_locked(lambda: target.rename(backup))
                for name in new_names:
                    target = destination / name
                    no_links(target)
                    if target.exists():
                        raise ValueError(f"Destination changed during update: {name}")
                    target.parent.mkdir(parents=True, exist_ok=True)
                    retry_locked(lambda: (transaction / "stage" / name).rename(target))
                journal["phase"] = "committed"
                atomic_json(transaction / "journal.json", journal)
            except BaseException:
                # A second failure leaves the complete recovery journal and backups intact.
                _recover(destination)
                raise
            _recover(destination)  # committed journal: remove backups, not the new installation
        progress(total, total, "Complete")
    return destination / metadata["executable"]


def update_from_release(destination, release, progress=lambda _done, _total, _name: None,
                        cancel=lambda: False):
    with tempfile.TemporaryDirectory(prefix="gdl-download-") as temporary:
        payload = download_release(release, Path(temporary) / "runtime.zip", progress, cancel)
        return apply_update(destination, payload, progress, cancel, release.version)
