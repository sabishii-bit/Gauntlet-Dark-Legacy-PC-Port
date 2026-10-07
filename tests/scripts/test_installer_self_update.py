"""Verified installer replacement, interrupted replacement and real process handoff."""

import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

from test_installer import fixture_payload, synthetic_iso
from test_updater import newer_payload, release_row
from installer import releases, self_update, update
from installer.install import Cancelled, install


def setup_row(row, content):
    version = row["tag_name"][1:]
    system = "windows-x64" if os.name == "nt" else "linux-x64"
    name = releases.setup_name(version, system)
    row["assets"].append({"name": name, "state": "uploaded", "size": len(content),
                          "digest": "sha256:" + hashlib.sha256(content).hexdigest(),
                          "browser_download_url": f"https://github.com/{releases.REPOSITORY}/releases/download/v{version}/{name}"})
    return row


def setup_package(content, version, system):
    if system == "windows-x64":
        return content
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        member = tarfile.TarInfo(f"GauntletDarkLegacy-{version}-{system}-setup")
        member.size = len(content)
        archive.addfile(member, io.BytesIO(content))
    return stream.getvalue()


class SelfUpdateTests(unittest.TestCase):
    def setUp(self):
        for module in ("install", "update"):
            patch = mock.patch(f"installer.{module}.apply_disc_icon")
            patch.start()
            self.addCleanup(patch.stop)
        temporary = tempfile.TemporaryDirectory(prefix="gdl self update ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.game = self.root / "game"
        image = self.root / "game.iso"
        image.write_bytes(synthetic_iso())
        install(image, self.game, fixture_payload(self.root))
        self.setup = self.root / ("Installer" + self_update.SUFFIX)
        self.setup.write_bytes(b"old installer")
        self.retained = self.game / ("GauntletDarkLegacy-Update" + self_update.SUFFIX)
        self.retained.write_bytes(b"retained old installer")
        self.new = newer_payload(self.root / "new", "1.0.0")
        self.system = update.installed_info(self.game)[0]["platform"]
        self.replacement = b"new installer"
        self.package = setup_package(self.replacement, "1.0.0", self.system)
        row = setup_row(release_row("1.0.0", self.system, self.new.read_bytes()), self.package)
        self.release = releases.select_release([row], "0.1.0-alpha.1", self.system)

    def prepare(self):
        with mock.patch.object(releases, "open_release", return_value=io.BytesIO(self.package)):
            self_update.prepare(self.game, self.release, self.setup)

    def ready(self):
        self.prepare()
        update.apply_update(self.game, self.new)
        self_update.mark_ready(self.game)

    def test_updates_both_existing_installers_in_place_and_cleans_only_private_files(self):
        (self.game / "saves/hero.json").write_bytes(b"save")
        with mock.patch.object(releases, "open_release", side_effect=[
                io.BytesIO(self.new.read_bytes()), io.BytesIO(self.package)]):
            update.update_from_release(self.game, self.release, installer=self.setup)
        self.assertTrue(self_update.inspect_pending(self.game))
        self.assertEqual(self.setup.read_bytes(), b"old installer")  # Still running until close.
        self_update.finish_pending(self.game)
        self_update.finish_pending(self.game)  # Re-entry after an interrupted cleanup is safe.
        self.assertEqual(self.setup.read_bytes(), self.replacement)
        self.assertEqual(self.retained.read_bytes(), self.replacement)
        self.assertFalse(self_update.inspect_pending(self.game))
        self.assertFalse((self.game / self_update.DIRECTORY).exists())
        self.assertEqual((self.game / "saves/hero.json").read_bytes(), b"save")
        self.assertEqual(sorted(p.name for p in self.root.glob("*Installer*")), [self.setup.name])

    def test_missing_or_bad_download_never_changes_runtime_or_installer(self):
        before = update.installed_info(self.game)[0]
        for content in (b"", b"wrong setup", self.package + b"extra"):
            with self.subTest(content=content), mock.patch.object(releases, "open_release", side_effect=[
                    io.BytesIO(self.new.read_bytes()), io.BytesIO(content)]):
                with self.assertRaises(ValueError):
                    update.update_from_release(self.game, self.release, installer=self.setup)
            self.assertEqual(update.installed_info(self.game)[0], before)
            self.assertEqual(self.setup.read_bytes(), b"old installer")
            self.assertFalse((self.game / self_update.DIRECTORY).exists())
        release = releases.select_release([release_row("1.0.0", self.system)], "0.1.0", self.system)
        with self.assertRaisesRegex(ValueError, "no verified installer"):
            self_update.prepare(self.game, release, self.setup)

    def test_offline_bundled_update_refreshes_existing_retained_installer(self):
        update.update_with_installer(self.game, self.new, self.setup)
        self_update.finish_pending(self.game)
        self.assertEqual(self.retained.read_bytes(), self.setup.read_bytes())
        self.assertFalse(self_update.inspect_pending(self.game))

    def test_bad_setup_feed_digest_is_not_ignored(self):
        row = setup_row(release_row("1.0.0", self.system), self.package)
        row["assets"][-1]["digest"] = None
        with self.assertRaises(ValueError):
            releases.select_release([row], "0.1.0", self.system)

    @unittest.skipIf(os.name == "nt", "Linux installer archive")
    def test_linux_archive_rejects_links_traversal_extra_entries_and_oversized_members(self):
        for fault in ("symlink", "traversal", "extra", "oversized"):
            with self.subTest(fault=fault):
                stream = io.BytesIO()
                with tarfile.open(fileobj=stream, mode="w:gz") as archive:
                    member = tarfile.TarInfo(f"GauntletDarkLegacy-1.0.0-{self.system}-setup")
                    if fault == "symlink":
                        member.type, member.linkname = tarfile.SYMTYPE, "outside"
                    elif fault == "traversal":
                        member.name = "../outside"
                    elif fault == "oversized":
                        member.size = self_update.MAX_PAYLOAD + 1
                    archive.addfile(member)
                    if fault == "extra":
                        archive.addfile(tarfile.TarInfo("unexpected"))
                content = stream.getvalue()
                row = setup_row(release_row("1.0.0", self.system), content)
                release = releases.select_release([row], "0.1.0", self.system)
                with mock.patch.object(releases, "open_release", return_value=io.BytesIO(content)):
                    with self.assertRaises((ValueError, tarfile.TarError)):
                        self_update.prepare(self.game, release, self.setup)
                self.assertFalse((self.game / self_update.DIRECTORY).exists())
                self.assertEqual(self.setup.read_bytes(), b"old installer")

    def test_cancellation_and_game_failure_discard_only_staging(self):
        with mock.patch.object(releases, "open_release", return_value=io.BytesIO(self.package)):
            with self.assertRaises(Cancelled):
                self_update.prepare(self.game, self.release, self.setup, cancel=lambda: True)
        self.assertFalse((self.game / self_update.DIRECTORY).exists())
        with mock.patch.object(releases, "open_release", side_effect=[
                io.BytesIO(self.new.read_bytes()), io.BytesIO(self.package)]), mock.patch.object(
                update, "require_game_closed", side_effect=ValueError("game open")):
            with self.assertRaisesRegex(ValueError, "game open"):
                update.update_from_release(self.game, self.release, installer=self.setup)
        self.assertFalse((self.game / self_update.DIRECTORY).exists())
        self.assertEqual(self.setup.read_bytes(), b"old installer")

    def test_crash_before_and_after_runtime_commit_reconciles_from_receipt(self):
        self.prepare()
        self.assertFalse(self_update.inspect_pending(self.game))
        self.prepare()
        update.apply_update(self.game, self.new)
        self.assertTrue(self_update.inspect_pending(self.game))
        self.assertEqual(self_update.read_pending(self.game)["phase"], "ready")

    def test_changed_setup_or_replacement_is_never_overwritten(self):
        self.ready()
        replacement = self.game / self_update.DIRECTORY / ("replacement" + self_update.SUFFIX)
        replacement.write_bytes(b"tamper")
        with self.assertRaisesRegex(ValueError, "replacement changed"):
            self_update.finish_pending(self.game)
        replacement.write_bytes(self.replacement)
        self.setup.write_bytes(b"user's replacement")
        with self.assertRaisesRegex(ValueError, "Installer was modified"):
            self_update.finish_pending(self.game)
        self.assertEqual(self.setup.read_bytes(), b"user's replacement")
        self.assertEqual(self.retained.read_bytes(), b"retained old installer")

    def test_partial_replacement_is_atomic_and_resumable(self):
        self.ready()
        replace = os.replace
        def fail(source, target):
            if Path(target) == self.retained:
                raise PermissionError("still mapped")
            return replace(source, target)
        with mock.patch.object(os, "replace", fail), mock.patch.object(
                self_update, "retry_locked", side_effect=lambda operation: operation()):
            with self.assertRaises(PermissionError):
                self_update.finish_pending(self.game)
        self.assertEqual(self.setup.read_bytes(), self.replacement)
        self.assertEqual(self.retained.read_bytes(), b"retained old installer")
        self.assertTrue(self_update.inspect_pending(self.game))
        self_update.finish_pending(self.game)
        self.assertEqual(self.retained.read_bytes(), self.replacement)

    def test_no_retained_copy_is_created_when_none_exists(self):
        self.retained.unlink()
        self.ready()
        self_update.finish_pending(self.game)
        self.assertFalse(self.retained.exists())

    def test_helper_is_isolated_and_rejects_changed_helper(self):
        self.ready()
        with mock.patch.object(subprocess, "Popen") as launch:
            self_update.launch_pending(self.game)
        args = launch.call_args.args[0]
        self.assertEqual(Path(args[0]).parent, self.game / self_update.DIRECTORY)
        self.assertEqual(args[1:3], ["--finish-installer-update", str(self.game)])
        self.assertEqual(launch.call_args.kwargs["env"]["PYINSTALLER_RESET_ENVIRONMENT"], "1")
        self.assertEqual(launch.call_args.kwargs["cwd"], self.game / self_update.DIRECTORY)
        (self.game / self_update.DIRECTORY / ("helper" + self_update.SUFFIX)).write_bytes(b"tamper")
        with self.assertRaisesRegex(ValueError, "helper changed"):
            self_update.launch_pending(self.game)

    def test_waits_for_real_parent_exit_and_refuses_self_wait(self):
        process = subprocess.Popen([sys.executable, "--version"], stdout=subprocess.DEVNULL)
        try:
            self_update.wait_for_process(process.pid)
            self.assertEqual(process.wait(timeout=5), 0)
        finally:
            process.wait(timeout=5)
        with self.assertRaises(ValueError):
            self_update.wait_for_process(os.getpid())

    def test_cleanup_does_not_delete_unexpected_files(self):
        self.ready()
        unexpected = self.game / self_update.DIRECTORY / "do-not-delete"
        unexpected.write_bytes(b"user data")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            self_update.remove_pending(self.game)
        self.assertEqual(unexpected.read_bytes(), b"user data")

    def test_journal_cannot_claim_the_game_or_a_player_file(self):
        self.ready()
        journal = self.game / self_update.DIRECTORY / "journal.json"
        value = json.loads(journal.read_text())
        for name in ("gauntlet.exe" if os.name == "nt" else "gauntlet", "saves/tool.exe"):
            with self.subTest(name=name):
                value["targets"] = {str(self.game / name): "0" * 64}
                update.atomic_json(journal, value)
                with self.assertRaisesRegex(ValueError, "cannot own"):
                    self_update.finish_pending(self.game)


if __name__ == "__main__":
    unittest.main()
