"""Offline update protocol, file ownership and failure recovery tests."""

import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

from test_installer import fixture_payload, synthetic_iso
from installer.install import Cancelled, install, payload_inventory
from installer import releases, update
from installer.versions import is_prerelease, version_order
from release import make_payload


def newer_payload(root, version="0.1.0-alpha.10"):
    root.mkdir(parents=True, exist_ok=True)
    initial = fixture_payload(root)
    metadata, _ = payload_inventory(initial)
    initial.unlink()
    files = []
    for name in metadata["files"]:
        if name == "shaders/test.spv":  # obsolete owned file must be removed
            continue
        path = root / "inputs" / name
        path.write_bytes((version + "\n").encode() if name == "VERSION" else b"new-runtime")
        files.append((path, name))
    new = root / "inputs/shaders/new.spv"
    new.parent.mkdir(parents=True, exist_ok=True)
    new.write_bytes(b"new-shader")
    files.append((new, "shaders/new.spv"))
    make_payload(initial, files, version, "new-commit", metadata["platform"], metadata["executable"])
    return initial


def release_row(version="0.1.0-alpha.10", system="windows-x64", content=b"payload"):
    name = releases.runtime_name(version, system)
    return {"tag_name": "v" + version, "draft": False, "prerelease": is_prerelease(version),
            "assets": [{"name": name, "state": "uploaded", "size": len(content),
                        "digest": "sha256:" + hashlib.sha256(content).hexdigest(),
                        "browser_download_url": f"https://github.com/{releases.REPOSITORY}/releases/download/v{version}/{name}"}]}


def snapshot(root):
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*")
            if p.is_file() and p.name != ".gdl-update.lock"}


class ReleaseFeedTests(unittest.TestCase):
    def test_alpha_identifiers_are_numeric_and_drafts_wrong_platforms_and_old_versions_are_ignored(self):
        draft = release_row("0.2.0-alpha.1")
        draft["draft"] = True
        rows = [release_row("0.1.0-alpha.9"), release_row("0.1.0-alpha.11", "linux-x64"),
                draft, release_row(), {"tag_name": "main", "draft": False},
                {"tag_name": None, "draft": False},
                {"tag_name": "v0.1.0-alpha.99", "draft": False, "assets": []}]
        self.assertEqual(releases.select_release(rows, "0.1.0-alpha.2", "windows-x64").version,
                         "0.1.0-alpha.10")
        self.assertIsNone(releases.select_release(rows, "0.1.0-alpha.10", "windows-x64"))
        self.assertGreater(version_order("0.2.0-alpha.1"), version_order("0.1.99-alpha.99"))
        for value in ("0.1.0-alpha.01", "main", None):
            self.assertIsNone(version_order(value))

    def test_all_published_release_types_follow_semver_not_publication_order(self):
        versions = ("1.0.0-alpha.2", "1.0.0-beta.10", "1.0.0-rc.1", "1.0.0", "1.0.1",
                    "1.1.0-preview.1")
        for index, version in enumerate(versions):
            with self.subTest(version=version):
                rows = [release_row(v) for v in reversed(versions[:index + 1])]
                result = releases.select_release(rows, "0.1.0-alpha.2", "windows-x64")
                self.assertEqual(result.version, version)
                self.assertIsNone(releases.select_release(rows, version, "windows-x64"))
        draft = release_row("9.0.0")
        draft["draft"] = True
        self.assertIsNone(releases.select_release([draft], "1.0.0", "windows-x64"))

    def test_build_metadata_urls_work_but_metadata_only_changes_are_not_upgrades(self):
        row = release_row("1.0.0+build.1")
        asset = row["assets"][0]
        asset["browser_download_url"] = asset["browser_download_url"].replace("+", "%2B")
        self.assertEqual(releases.select_release([row], "1.0.0-rc.1", "windows-x64").version,
                         "1.0.0+build.1")
        self.assertIsNone(releases.select_release([row], "1.0.0+build.0", "windows-x64"))

    def test_unverified_or_redirected_release_metadata_is_rejected(self):
        for key, value in (("digest", None), ("size", -1), ("state", "new"),
                           ("browser_download_url", "https://evil.example/payload.zip")):
            row = release_row()
            row["assets"][0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                releases.select_release([row], "0.1.0-alpha.1", "windows-x64")
        for url in ("http://github.com/file", "https://github.com.evil.example/file",
                    "file:///tmp/payload", "https://user@github.com/file", "https://github.com:444/file"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                releases.trusted_url(url)
        releases.trusted_url("https://release-assets.githubusercontent.com/github-production-release-asset/abc?token=x")
        with self.assertRaises(ValueError):
            releases.ReleaseRedirect().redirect_request(None, None, 302, "", {}, "http://github.com/file")

    def test_feed_pagination_includes_alpha_releases_without_authentication(self):
        first = [{"tag_name": "old", "draft": False}] * 100
        with mock.patch.object(releases, "open_release", side_effect=[
                io.BytesIO(json.dumps(first).encode()), io.BytesIO(json.dumps([release_row()]).encode())]) as opened:
            result = releases.check_updates("0.1.0-alpha.2", "windows-x64")
        self.assertEqual(result.version, "0.1.0-alpha.10")
        self.assertIn("page=2", opened.call_args.args[0])
        with mock.patch.object(releases, "open_release", return_value=io.BytesIO(b"{}")):
            with self.assertRaises(ValueError):
                releases.check_updates("0.1.0-alpha.2", "windows-x64")

    def test_download_is_bounded_verified_and_cancelable(self):
        selected = releases.select_release([release_row()], "0.1.0-alpha.2", "windows-x64")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "download.zip"
            for content in (b"", b"pay", b"invalid", b"payload-extra"):
                with self.subTest(content=content), mock.patch.object(releases, "open_release", return_value=io.BytesIO(content)):
                    with self.assertRaises(ValueError):
                        releases.download_release(selected, path)
                    self.assertFalse(path.exists())
            with mock.patch.object(releases, "open_release", return_value=io.BytesIO(b"payload")):
                cancelled = False
                def progress(*_):
                    nonlocal cancelled
                    cancelled = True
                with self.assertRaises(Cancelled):
                    releases.download_release(selected, path, progress, lambda: cancelled)
                self.assertFalse(path.exists())
            with mock.patch.object(releases, "open_release", return_value=io.BytesIO(b"payload")):
                releases.download_release(selected, path)
            self.assertEqual(path.read_bytes(), b"payload")
            with self.assertRaises(FileExistsError):
                releases.download_release(selected, path)
            self.assertEqual(path.read_bytes(), b"payload")


class UpdateTests(unittest.TestCase):
    def setUp(self):
        for module in ("install", "update"):
            icon = mock.patch(f"installer.{module}.apply_disc_icon")
            icon.start()
            self.addCleanup(icon.stop)
        temporary = tempfile.TemporaryDirectory(prefix="gdl update tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.image = self.root / "game.iso"
        self.image.write_bytes(synthetic_iso())
        self.payload = fixture_payload(self.root)
        self.destination = self.root / "game"
        self.executable = install(self.image, self.destination, self.payload)
        for name in ("saves/hero.json", "config/settings.json", "data/local.json", "notes.txt"):
            (self.destination / name).write_bytes(b"user-owned")
        self.new = newer_payload(self.root / "next")
        self.original = snapshot(self.destination)

    def test_upgrade_preserves_assets_saves_settings_unknown_files_and_receipt_fields(self):
        result = update.apply_update(self.destination, self.new)
        self.assertEqual(result, self.executable)
        current = snapshot(self.destination)
        for name, content in self.original.items():
            if name.startswith(("Gauntlet/", "carddemo/", "config/", "saves/")) or name in ("data/local.json", "notes.txt"):
                self.assertEqual(current[name], content, name)
        self.assertNotIn("shaders/test.spv", current)
        self.assertEqual(current["shaders/new.spv"], b"new-shader")
        metadata, receipt = update.installed_info(self.destination)
        self.assertEqual(metadata["version"], "0.1.0-alpha.10")
        self.assertEqual(receipt["discRevision"], 1)
        self.assertFalse((self.destination / update.TRANSACTION).exists())

    def test_old_or_mismatched_payload_never_replaces_a_file(self):
        for payload, expected in ((self.payload, None), (self.new, "0.1.0-alpha.3")):
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                update.apply_update(self.destination, payload, expected_version=expected)
            self.assertEqual(snapshot(self.destination), self.original)

    def test_installation_can_upgrade_through_beta_rc_stable_and_next_preview(self):
        versions = ("1.0.0-beta.1", "1.0.0-rc.1", "1.0.0", "1.1.0-preview.1")
        for index, version in enumerate(versions):
            with self.subTest(version=version):
                payload = newer_payload(self.root / f"release-{index}", version)
                update.apply_update(self.destination, payload, expected_version=version)
                self.assertEqual(update.installed_info(self.destination)[0]["version"], version)
                self.assertEqual((self.destination / "saves/hero.json").read_bytes(), b"user-owned")
        before = snapshot(self.destination)
        with self.assertRaisesRegex(ValueError, "newer"):
            update.apply_update(self.destination, newer_payload(self.root / "old-rc", "1.0.0-rc.2"))
        self.assertEqual(snapshot(self.destination), before)

    def test_modified_or_unowned_runtime_is_not_overwritten(self):
        for name in (self.executable.name, "shaders/new.spv"):
            path = self.destination / name
            original = path.read_bytes() if path.exists() else None
            path.write_bytes(b"personal")
            before = snapshot(self.destination)
            with self.assertRaises(ValueError):
                update.apply_update(self.destination, self.new)
            self.assertEqual(snapshot(self.destination), before)
            if original is None:
                path.unlink()
            else:
                path.write_bytes(original)

    def test_tampered_payload_or_cancelled_staging_preserves_installation(self):
        cancel = False
        def progress(*_):
            nonlocal cancel
            cancel = True
        with self.assertRaises(Cancelled):
            update.apply_update(self.destination, self.new, progress, lambda: cancel)
        self.assertEqual(snapshot(self.destination), self.original)
        with zipfile.ZipFile(self.new) as archive:
            content = {name: archive.read(name) for name in archive.namelist()}
        content["data/config.json"] = b"bad"
        with zipfile.ZipFile(self.new, "w") as archive:
            for name, data in content.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(ValueError, "Damaged"):
            update.apply_update(self.destination, self.new)
        self.assertEqual(snapshot(self.destination), self.original)

    def test_every_file_move_failure_rolls_back(self):
        # Includes the staged-directory rename, every old backup and every new file.
        metadata, _ = update.installed_info(self.destination)
        new, _ = payload_inventory(self.new)
        moves = 1 + len(metadata["files"]) + len(new["files"]) + 4
        original_rename = Path.rename
        for failed_move in range(1, moves + 1):
            count = 0
            def fail(path, target):
                nonlocal count
                count += 1
                if count == failed_move:
                    raise OSError("injected move failure")
                return original_rename(path, target)
            with self.subTest(move=failed_move), mock.patch.object(Path, "rename", fail):
                with self.assertRaisesRegex(OSError, "injected"):
                    update.apply_update(self.destination, self.new)
            self.assertEqual(snapshot(self.destination), self.original)

    def test_receipt_commit_failure_restores_originals(self):
        original = update.atomic_json
        def fail(path, value):
            if value.get("phase") == "committed":
                raise OSError("commit failure")
            original(path, value)
        with mock.patch.object(update, "atomic_json", fail), self.assertRaises(OSError):
            update.apply_update(self.destination, self.new)
        self.assertEqual(snapshot(self.destination), self.original)

    def test_failed_rollback_retains_backups_for_retry(self):
        original = update.atomic_json
        def fail(path, value):
            if value.get("phase") == "committed":
                raise OSError("commit failure")
            original(path, value)
        with mock.patch.object(update, "atomic_json", fail), mock.patch.object(
                update, "_recover", side_effect=OSError("recovery temporarily blocked")):
            with self.assertRaisesRegex(OSError, "recovery temporarily"):
                update.apply_update(self.destination, self.new)
        self.assertTrue((self.destination / update.TRANSACTION / "backup" / self.executable.name).is_file())
        update.recover_update(self.destination)
        self.assertEqual(snapshot(self.destination), self.original)

    def test_recovery_after_success_keeps_the_new_version(self):
        with mock.patch.object(update, "_recover", side_effect=OSError("cleanup blocked")):
            with self.assertRaises(OSError):
                update.apply_update(self.destination, self.new)
        self.assertEqual(update.recover_update(self.destination)["version"], "0.1.0-alpha.10")
        self.assertEqual(self.executable.read_bytes(), b"new-runtime")

    def test_partial_cleanup_retains_journal_until_backup_removal_finishes(self):
        original = shutil.rmtree
        def fail(path, *args, **kwargs):
            if Path(path).name == "backup":
                raise OSError("backup cleanup interrupted")
            return original(path, *args, **kwargs)
        with mock.patch.object(shutil, "rmtree", fail), self.assertRaisesRegex(OSError, "cleanup interrupted"):
            update.apply_update(self.destination, self.new)
        transaction = self.destination / update.TRANSACTION
        self.assertFalse((transaction / "stage").exists())
        self.assertTrue((transaction / "journal.json").exists())
        self.assertEqual(update.recover_update(self.destination)["version"], "0.1.0-alpha.10")
        # The final journal unlink/rmdir gap is also safe to retry after a crash.
        transaction.mkdir()
        self.assertEqual(update.recover_update(self.destination)["version"], "0.1.0-alpha.10")

    def test_locked_backup_after_commit_is_success_and_can_be_cleaned_later(self):
        original = shutil.rmtree
        def locked(path, *args, **kwargs):
            if Path(path).name == "backup":
                raise PermissionError("mapped runtime image")
            return original(path, *args, **kwargs)
        with mock.patch.object(shutil, "rmtree", locked), mock.patch.object(
                update, "retry_locked", side_effect=lambda operation: operation()):
            self.assertEqual(update.apply_update(self.destination, self.new), self.executable)
            self.assertTrue(update.committed_cleanup_pending(self.destination))
            self.assertEqual(update.installed_info(self.destination)[0]["version"], "0.1.0-alpha.10")
            self.assertEqual(update.recover_update(self.destination)["version"], "0.1.0-alpha.10")
            with self.assertRaisesRegex(ValueError, "Close and reopen"):
                update.apply_update(self.destination, self.new)
        self.assertEqual(update.recover_update(self.destination)["version"], "0.1.0-alpha.10")
        self.assertFalse((self.destination / update.TRANSACTION).exists())
        self.assertEqual((self.destination / "saves/hero.json").read_bytes(), b"user-owned")

    @unittest.skipUnless(os.name == "nt", "Windows mapped-image locking")
    def test_real_mapped_crt_is_not_moved_when_unchanged_and_defers_cleanup_when_changed(self):
        import ctypes
        from ctypes import wintypes
        source = Path(sys.base_prefix) / "vcruntime140.dll"
        self.assertTrue(source.is_file(), "The Windows Python fixture must provide its CRT")
        dll = self.destination / "vcruntime140.dll"
        dll.write_bytes(source.read_bytes())
        metadata, _ = update.installed_info(self.destination)
        metadata["files"][dll.name] = update.file_digest(dll)
        update.atomic_json(self.destination / "build-info.json", metadata)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.LoadLibraryW.argtypes = (wintypes.LPCWSTR,)
        kernel.LoadLibraryW.restype = wintypes.HMODULE
        kernel.FreeLibrary.argtypes = (wintypes.HMODULE,)
        kernel.FreeLibrary.restype = wintypes.BOOL
        handle = kernel.LoadLibraryW(str(dll))
        self.assertTrue(handle, ctypes.get_last_error())
        try:
            for index, changed in enumerate((False, True)):
                root = self.root / f"mapped-{index}"
                payload = newer_payload(root, f"1.0.{index}")
                info, _ = payload_inventory(payload)
                runtime = root / "inputs" / dll.name
                runtime.write_bytes(source.read_bytes() + (b"new CRT image" if changed else b""))
                payload.unlink()
                make_payload(payload, [(root / "inputs" / name, name) for name in info["files"]] +
                             [(runtime, dll.name)], info["version"], "mapped-crt", info["platform"], info["executable"])
                with mock.patch.object(update, "retry_locked", side_effect=lambda operation: operation()):
                    update.apply_update(self.destination, payload)
                self.assertEqual(update.committed_cleanup_pending(self.destination), changed)
                self.assertEqual(dll.read_bytes(), runtime.read_bytes())
        finally:
            self.assertTrue(kernel.FreeLibrary(handle))
        self.assertEqual(update.recover_update(self.destination)["version"], "1.0.1")
        self.assertFalse((self.destination / update.TRANSACTION).exists())

    def test_process_death_at_every_commit_move_is_recoverable(self):
        metadata, _ = update.installed_info(self.destination)
        new, _ = payload_inventory(self.new)
        moves = 1 + len(metadata["files"]) + len(new["files"]) + 4
        for interrupted_move in range(1, moves + 1):
            with self.subTest(move=interrupted_move):
                result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                         "--interrupt-update", str(self.destination), str(self.new),
                                         str(interrupted_move)], capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 71, result.stderr)
                self.assertTrue((self.destination / update.TRANSACTION).exists())
                update.recover_update(self.destination)
                self.assertEqual(snapshot(self.destination), self.original)

    def test_inconsistent_receipts_and_wrong_platform_are_rejected(self):
        path = self.destination / "installation.json"
        receipt = json.loads(path.read_bytes())
        receipt["version"] = "0.1.0-alpha.2"
        path.write_text(json.dumps(receipt))
        before = snapshot(self.destination)
        with self.assertRaisesRegex(ValueError, "receipts disagree"):
            update.apply_update(self.destination, self.new)
        self.assertEqual(snapshot(self.destination), before)
        path.write_bytes(self.original["installation.json"])
        with mock.patch.object(update, "host_platform", return_value="different-platform"):
            with self.assertRaisesRegex(ValueError, "different platform"):
                update.apply_update(self.destination, self.new)
        self.assertEqual(snapshot(self.destination), self.original)

    def test_linked_runtime_never_writes_outside_the_installation(self):
        outside = self.root / "outside.json"
        outside.write_bytes(self.original["data/config.json"])
        probe = self.root / "link-probe"
        try:
            probe.symlink_to(outside)
        except OSError as error:
            self.skipTest(f"Creating symbolic links is unavailable: {error}")
        probe.unlink()
        target = self.destination / "data/config.json"
        target.unlink()
        target.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "links or junctions"):
            update.apply_update(self.destination, self.new)
        self.assertEqual(outside.read_bytes(), self.original["data/config.json"])
        self.assertTrue(target.is_symlink())

    def test_recovery_and_old_inventory_cannot_claim_saves(self):
        path = self.destination / "build-info.json"
        metadata = json.loads(path.read_bytes())
        metadata["files"]["saves/hero.json"] = hashlib.sha256(b"user-owned").hexdigest()
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            update.apply_update(self.destination, self.new)
        transaction = self.destination / update.TRANSACTION
        transaction.mkdir()
        (transaction / "journal.json").write_text(json.dumps({"schema": 1, "phase": "prepared",
                                                              "old": [], "new": ["saves/hero.json"]}))
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            update.recover_update(self.destination)
        self.assertEqual((self.destination / "saves/hero.json").read_bytes(), b"user-owned")

    def test_running_game_low_space_and_concurrent_updater_fail_before_mutation(self):
        with mock.patch.object(update, "require_game_closed", side_effect=ValueError("Close the game")):
            with self.assertRaisesRegex(ValueError, "Close"):
                update.apply_update(self.destination, self.new)
        with mock.patch.object(shutil, "disk_usage", return_value=mock.Mock(free=0)):
            with self.assertRaisesRegex(ValueError, "disk space"):
                update.apply_update(self.destination, self.new)
        with update.update_lock(self.destination):
            with self.assertRaisesRegex(ValueError, "Another updater"):
                update.apply_update(self.destination, self.new)
        self.assertEqual(snapshot(self.destination), self.original)

    def test_fresh_install_retains_updater_without_publishing_it_as_game_data(self):
        installer = self.root / "setup"
        installer.write_bytes(b"frozen installer")
        destination = self.root / "new game"
        install(self.image, destination, self.payload, updater=installer)
        name = "GauntletDarkLegacy-Update.exe" if os.name == "nt" else "GauntletDarkLegacy-Update"
        self.assertEqual((destination / name).read_bytes(), b"frozen installer")
        self.assertEqual(installer.read_bytes(), b"frozen installer")

    def test_real_download_to_update_flow_requires_the_selected_version(self):
        raw = self.new.read_bytes()
        metadata, _ = payload_inventory(self.new)
        selected = releases.select_release([release_row(metadata["version"], metadata["platform"], raw)],
                                           "0.1.0-alpha.1", metadata["platform"])
        with mock.patch.object(releases, "open_release", return_value=io.BytesIO(raw)):
            update.update_from_release(self.destination, selected)
        self.assertEqual(update.installed_info(self.destination)[0]["version"], selected.version)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--interrupt-update":
        # Actual process death bypasses finally/rollback and releases the OS lock.
        original_rename = Path.rename
        remaining = int(sys.argv[4])
        def interrupt_after_move(path, target):
            global remaining
            result = original_rename(path, target)
            remaining -= 1
            if remaining == 0:
                os._exit(71)
            return result
        Path.rename = interrupt_after_move
        with mock.patch.object(update, "apply_disc_icon"):
            update.apply_update(Path(sys.argv[2]), Path(sys.argv[3]))
        sys.exit(1)  # the requested interruption must have occurred
    unittest.main()
