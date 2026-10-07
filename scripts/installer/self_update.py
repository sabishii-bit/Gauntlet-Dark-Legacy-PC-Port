"""Replace existing installer executables after their owning window has closed.

The helper is a private copy of the current installer, not downloaded code. It
runs outside the game's DLL directory and replaces only hash-pinned executables.
The journal makes replacement retryable; the next launch removes the helper.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

from .install import MAX_PAYLOAD, check_cancel, no_links, retry_locked
from .releases import download_release
from .update import (atomic_json, committed_cleanup_pending, file_digest,
                     installed_info, read_json, recover_update, update_lock)
from .versions import version_order

DIRECTORY = ".gdl-installer-update"
SUFFIX = ".exe" if os.name == "nt" else ""
FILES = {"journal.json", "journal.pending", "helper" + SUFFIX,
         "replacement" + SUFFIX, "download", "error.txt"}


def read_pending(folder):
    root = folder / DIRECTORY
    no_links(root)
    value = read_json(root / "journal.json")
    if (not isinstance(value, dict) or value.get("schema") != 1 or
            value.get("phase") not in ("prepared", "ready", "committed") or
            version_order(value.get("version")) is None):
        raise ValueError("Invalid installer update journal; files have been retained")
    targets = value.get("targets")
    if not isinstance(targets, dict) or not 1 <= len(targets) <= 2:
        raise ValueError("Invalid installer replacement targets")
    for target, digest in targets.items():
        path = Path(target)
        no_links(path)
        if (not path.is_absolute() or path.is_relative_to(root) or
                (os.name == "nt" and path.suffix.lower() != ".exe") or
                not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest)):
            raise ValueError("Invalid installer replacement target")
        if path.is_relative_to(folder):
            first = path.relative_to(folder).parts[0].casefold()
            if first in {"gauntlet", "gauntlet.exe", "carddemo", "saves", "config", "data", "shaders", "lib", "licenses"}:
                raise ValueError("Installer replacement cannot own game or player files")
    for key in ("sha256", "helper_sha256"):
        if not isinstance(value.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", value[key]):
            raise ValueError("Invalid installer update digest")
    for path in root.iterdir():
        no_links(path)
        if path.name not in FILES or not path.is_file():
            raise ValueError("Unexpected installer recovery file; retained for inspection")
    return value


def remove_pending(folder):
    """Delete only the private, validated journal files, never an install tree."""
    root = folder / DIRECTORY
    read_pending(folder)
    for path in root.iterdir():
        if path.name not in {"journal.json", "journal.pending"}:
            retry_locked(path.unlink)
    (root / "journal.pending").unlink(missing_ok=True)
    (root / "journal.json").unlink()
    root.rmdir()


def inspect_pending(folder):
    root = folder / DIRECTORY
    if not root.exists():
        return False
    no_links(root)
    if not any(root.iterdir()):
        root.rmdir()
        return False
    value = read_pending(folder)
    if value["phase"] == "prepared":
        # A crash may fall between the runtime commit and marking the installer ready.
        if installed_info(folder)[0]["version"] != value["version"]:
            remove_pending(folder)
            return False
        mark_ready(folder)
    if value["phase"] == "committed":
        remove_pending(folder)
        return False
    return True


def prepare(folder, release, executable, progress=lambda *_: None, cancel=lambda: False,
            bundled_version=None):
    """Verify both downloads before changing the game; never execute a downloaded setup."""
    folder, executable = Path(folder).absolute(), Path(executable).absolute()
    if release is not None and release.installer is None:
        raise ValueError("This release has no verified installer update; please try again later")
    version = release.version if release else bundled_version
    if version_order(version) is None:
        raise ValueError("Invalid installer version")
    if release and (release.installer.version != version or release.installer.platform != release.platform):
        raise ValueError("Installer does not belong to the selected release")
    no_links(executable)
    root = folder / DIRECTORY
    with update_lock(folder):
        if root.exists():
            raise ValueError("Close and reopen the installer to finish its pending update first")
        root.mkdir()
        try:
            replacement = root / ("replacement" + SUFFIX)
            package = root / "download"
            if release:
                download_release(release.installer, package, progress, cancel)
            else:
                shutil.copyfile(executable, package)
            if os.name == "nt" or release is None:
                package.rename(replacement)
            else:
                expected = f"GauntletDarkLegacy-{release.version}-{release.platform}-setup"
                with tarfile.open(package, "r:gz") as archive:
                    member = archive.next()
                    if (member is None or member.name != expected or not member.isfile() or
                            not 0 < member.size <= MAX_PAYLOAD):
                        raise ValueError("Invalid Linux installer archive")
                    with archive.extractfile(member) as source, replacement.open("xb") as output:
                        remaining = member.size
                        while remaining:
                            check_cancel(cancel)
                            block = source.read(min(1024 * 1024, remaining))
                            if not block:
                                raise ValueError("Truncated installer archive")
                            output.write(block)
                            remaining -= len(block)
                    if archive.next() is not None:
                        raise ValueError("Unexpected extra installer archive entry")
                package.unlink()
            replacement.chmod(0o755)
            helper = root / ("helper" + SUFFIX)
            shutil.copyfile(executable, helper)
            helper.chmod(0o755)
            targets = {str(executable): file_digest(executable)}
            retained = folder / ("GauntletDarkLegacy-Update" + SUFFIX)
            no_links(retained)
            if retained.exists() and retained != executable:
                targets[str(retained)] = file_digest(retained)
            check_cancel(cancel)
            atomic_json(root / "journal.json", {
                "schema": 1, "phase": "prepared", "version": version,
                "sha256": file_digest(replacement), "helper_sha256": file_digest(helper),
                "targets": targets,
            })
        except BaseException:
            # Only the exclusively created private staging directory is removed.
            shutil.rmtree(root)
            raise


def mark_ready(folder):
    value = read_pending(folder)
    if installed_info(folder)[0]["version"] != value["version"]:
        raise ValueError("Finish the game update before replacing its installer")
    value["phase"] = "ready"
    atomic_json(folder / DIRECTORY / "journal.json", value)


def launch_pending(folder):
    """Called on close, before the parent exits. No shell or downloaded helper code."""
    value = read_pending(folder)
    if value["phase"] != "ready":
        raise ValueError("Installer update is not ready")
    helper = folder / DIRECTORY / ("helper" + SUFFIX)
    if file_digest(helper) != value["helper_sha256"]:
        raise ValueError("Installer helper changed after verification")
    environment = dict(os.environ)
    environment["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    if sys.platform.startswith("linux"):
        if "LD_LIBRARY_PATH_ORIG" in environment:
            environment["LD_LIBRARY_PATH"] = environment.pop("LD_LIBRARY_PATH_ORIG")
        else:
            environment.pop("LD_LIBRARY_PATH", None)
    elif getattr(sys, "frozen", False):
        import ctypes
        ctypes.windll.kernel32.SetDllDirectoryW(None)
    try:
        subprocess.Popen([str(helper), "--finish-installer-update", str(folder),
                          "--wait-for-process", str(os.getpid())], cwd=helper.parent,
                         env=environment, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL,
                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
                         start_new_session=os.name != "nt")
    finally:
        if os.name == "nt" and getattr(sys, "frozen", False):
            ctypes.windll.kernel32.SetDllDirectoryW(getattr(sys, "_MEIPASS", None))


def wait_for_process(pid, timeout=120):
    if pid <= 0 or pid == os.getpid():
        raise ValueError("Invalid installer parent process")
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE, not termination access
        if not handle:
            if ctypes.get_last_error() == 87:  # already exited
                return
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if kernel.WaitForSingleObject(handle, timeout * 1000) != 0:
                raise TimeoutError("The previous installer has not closed")
        finally:
            kernel.CloseHandle(handle)
    else:
        deadline = time.monotonic() + timeout
        while Path(f"/proc/{pid}").exists():
            try:
                # A child whose caller has not wait()ed yet remains as a zombie;
                # it has exited and no longer holds an executable or DLL mapping.
                state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
                if state in {"Z", "X"}:
                    return
            except FileNotFoundError:
                return
            if time.monotonic() >= deadline:
                raise TimeoutError("The previous installer has not closed")
            time.sleep(0.1)


def finish_pending(folder):
    """Idempotent atomic replacements; interruption never deletes the old executable."""
    folder = Path(folder).absolute()
    with update_lock(folder):
        value = read_pending(folder)
        if value["phase"] not in ("ready", "committed"):
            raise ValueError("Installer update is not ready")
        if installed_info(folder)[0]["version"] != value["version"]:
            raise ValueError("Installed game version changed during installer update")
        replacement = folder / DIRECTORY / ("replacement" + SUFFIX)
        if file_digest(replacement) != value["sha256"]:
            raise ValueError("Installer replacement changed after verification")
        for name, previous in value["targets"].items():
            target = Path(name)
            digest = file_digest(target)
            if digest == value["sha256"]:
                continue  # Prior attempt already replaced this target.
            if digest != previous:
                raise ValueError("Installer was modified since the update began")
            descriptor, name = tempfile.mkstemp(prefix=".gdl-replace-", dir=target.parent)
            staged = Path(name)
            try:
                with os.fdopen(descriptor, "wb") as output, replacement.open("rb") as source:
                    shutil.copyfileobj(source, output)
                    output.flush()
                    os.fsync(output.fileno())
                staged.chmod(0o755)
                retry_locked(lambda: os.replace(staged, target))
            finally:
                staged.unlink(missing_ok=True)
        value["phase"] = "committed"
        atomic_json(folder / DIRECTORY / "journal.json", value)
    # The old loader is now gone, so dispose of its mapped CRT backups too.
    if committed_cleanup_pending(folder):
        try:
            recover_update(folder)
        except (OSError, ValueError):
            pass  # The game may have been launched; normal startup retries cleanup.
