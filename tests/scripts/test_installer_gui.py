"""Real widget/input checks; enabled when the optional installer dependencies exist."""

import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from test_installer import fixture_payload, synthetic_iso

HAS_QT = importlib.util.find_spec("PySide6") is not None


@unittest.skipUnless(HAS_QT, "installer tools are not installed")
class WizardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        for module in ("install", "update"):
            icon = mock.patch(f"installer.{module}.apply_disc_icon")
            icon.start()
            self.addCleanup(icon.stop)
        temporary = tempfile.TemporaryDirectory(prefix="gdl wizard ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        from installer.wizard import Wizard
        self.window = Wizard(fixture_payload(self.root), network=False)
        self.addCleanup(self.window.close)
        self.dialog = mock.patch("installer.wizard.QMessageBox.critical").start()
        self.addCleanup(mock.patch.stopall)
        self.app.processEvents()

    def wait_for_operation(self):
        from PySide6.QtTest import QTest
        self.assertIsNotNone(self.window.worker)
        self.assertTrue(self.window.worker.wait(10000))
        QTest.qWait(10)
        self.assertFalse(self.window.busy())

    def installed_folder(self):
        from installer.install import install
        image = self.root / "game.iso"
        image.write_bytes(synthetic_iso())
        folder = self.root / "game"
        install(image, folder, self.window.payload)
        self.window.destination.setText(str(folder))
        return folder

    def test_initial_window_has_no_destination_or_ready_helper_message(self):
        from PySide6.QtWidgets import QLabel
        from installer.wizard import STRINGS
        self.assertEqual(self.window.status.text(), "")
        self.assertEqual(self.window.windowTitle(), "Gauntlet Dark Legacy - Installer")
        self.assertEqual(STRINGS["destination"], "Install directory")
        self.assertEqual(STRINGS["choose_directory"], "Select install directory")
        self.assertNotIn("destination_help", STRINGS)
        self.assertNotIn("ready", STRINGS)
        labels = [label.text() for label in self.window.findChildren(QLabel)]
        self.assertFalse(any("Defaults to the folder" in text or
                             "Ready to install." in text for text in labels))

    def test_check_button_queries_without_an_installation_or_disc(self):
        from installer import releases
        from test_updater import release_row
        folder = self.root / "not installed"
        self.window.destination.setText(str(folder))
        self.window.network = True
        self.window.inspect_destination()
        self.assertTrue(self.window.check.isEnabled())
        self.assertEqual(self.window.image.text(), "")
        with mock.patch.object(releases, "open_release", return_value=io.BytesIO(json.dumps([
                release_row("1.0.0", system=self.window.metadata["platform"])]).encode())) as opened:
            self.window.check.click()
            self.assertFalse(self.window.check.isEnabled())
            self.wait_for_operation()
        opened.assert_called_once()
        self.assertIn("Version 1.0.0 is available", self.window.status.text())
        self.assertIn("Select your existing install directory", self.window.status.text())
        self.assertEqual(self.window.mode, "install")
        self.assertIsNone(self.window.release)
        self.assertIsNone(self.window.installed)
        self.assertTrue(self.window.check.isEnabled())
        self.assertTrue(self.window.image.isEnabled())
        self.assertFalse(folder.exists())
        self.dialog.assert_not_called()

    def test_check_without_receipt_can_be_current_or_retry_an_offline_failure(self):
        self.window.destination.setText(str(self.root / "new game"))
        self.window.network = True
        self.window.inspect_destination()
        with mock.patch("installer.wizard.check_updates", side_effect=[OSError("offline"), None]) as check:
            self.window.check.click()
            self.wait_for_operation()
            self.assertIn("Could not check for updates: offline", self.window.status.text())
            self.assertTrue(self.window.check.isEnabled())
            self.window.check.click()
            self.wait_for_operation()
        self.assertEqual(check.call_count, 2)
        self.assertEqual(check.call_args.args[:2],
                         (self.window.metadata["version"], self.window.metadata["platform"]))
        self.assertIn("Checked GitHub", self.window.status.text())
        self.assertIn("bundled version", self.window.status.text())
        self.assertEqual(self.window.mode, "install")
        self.dialog.assert_not_called()

    def test_manual_check_rescans_new_path_before_its_debounce(self):
        folder = self.installed_folder()
        self.window.inspect_destination()
        self.assertIsNotNone(self.window.installed)
        self.window.destination.setText(str(self.root / "new folder"))
        self.window.network = True
        self.window.refresh_controls()
        with mock.patch("installer.wizard.check_updates", return_value=None) as check:
            self.window.check.click()
            self.wait_for_operation()
        check.assert_called_once()
        self.assertEqual(self.window.mode, "install")
        self.assertIsNone(self.window.installed)
        self.assertIsNone(self.window.executable)
        self.assertTrue((folder / "installation.json").is_file())

    def test_check_keeps_invalid_receipt_error_and_does_not_enable_install(self):
        folder = self.installed_folder()
        (folder / "installation.json").write_text("{}")
        self.window.network = True
        self.window.inspect_destination()
        original = self.window.status.text()
        self.assertEqual(self.window.mode, "invalid")
        with mock.patch("installer.wizard.check_updates", return_value=None):
            self.window.check.click()
            self.wait_for_operation()
        self.assertIn(original, self.window.status.text())
        self.assertFalse(self.window.start.isEnabled())
        self.assertTrue(self.window.check.isEnabled())
        self.assertEqual(self.window.mode, "invalid")

    def test_frozen_default_is_installer_folder_not_bundle_temp(self):
        from installer.wizard import installer_directory
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(
                sys, "executable", str(self.root / "setup.exe")):
            self.assertEqual(installer_directory(), self.root.resolve())

    def test_frozen_wizard_check_preserves_requested_desktop_plugin(self):
        from install_game import main
        with mock.patch.dict(os.environ, {"QT_QPA_PLATFORM": "xcb"}), mock.patch.object(
                sys, "argv", ["setup", "--check-wizard"]), mock.patch(
                "PySide6.QtWidgets.QApplication"), mock.patch("installer.wizard.Wizard"):
            self.assertEqual(main(), 0)
            self.assertEqual(os.environ["QT_QPA_PLATFORM"], "xcb")

    def test_drop_local_image_and_reject_remote_url(self):
        from PySide6.QtCore import QMimeData, QPointF, Qt, QUrl
        from PySide6.QtGui import QDropEvent
        mime = QMimeData()
        mime.setUrls([QUrl.fromLocalFile(str(self.root / "game.iso"))])
        event = QDropEvent(QPointF(), Qt.DropAction.CopyAction, mime,
                           Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        self.window.dropEvent(event)
        self.assertEqual(Path(self.window.image.text()), self.root / "game.iso")
        mime.setUrls([QUrl("https://example.com/game.iso")])
        event = QDropEvent(QPointF(), Qt.DropAction.CopyAction, mime,
                           Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)
        self.window.dropEvent(event)
        self.assertEqual(Path(self.window.image.text()), self.root / "game.iso")

    def test_actual_install_button_finishes_and_enables_launch(self):
        image = self.root / "game.iso"
        image.write_bytes(synthetic_iso())
        self.window.image.setText(str(image))
        self.window.destination.setText(str(self.root / "game"))
        self.window.start.click()
        self.assertFalse(self.window.image.isEnabled())
        self.wait_for_operation()
        self.dialog.assert_not_called()
        self.assertIsNotNone(self.window.executable)
        self.assertTrue(self.window.executable.is_file())
        self.assertTrue(self.window.start.isEnabled())
        self.assertFalse(self.window.image.isEnabled())

    def test_existing_install_updates_without_an_image_or_browser(self):
        from installer import releases, update
        from test_updater import newer_payload, release_row
        folder = self.installed_folder()
        personal = folder / "saves/hero.json"
        personal.write_bytes(b"keep my hero")
        raw = newer_payload(self.root / "next", "1.0.0").read_bytes()
        system = update.installed_info(folder)[0]["platform"]
        self.window.network = True
        with mock.patch.object(releases, "open_release", side_effect=[
                io.BytesIO(json.dumps([release_row("1.0.0", system=system, content=raw)]).encode()),
                io.BytesIO(raw)]):
            self.window.inspect_destination()
            self.wait_for_operation()
            self.assertEqual(self.window.mode, "update")
            self.assertIn("1.0.0", self.window.start.text())
            self.assertFalse(self.window.image.isEnabled())
            self.assertEqual(self.window.image.text(), "")
            self.assertIn("No disc image is needed", self.window.intro.text())
            self.window.start.click()
            self.wait_for_operation()
        self.dialog.assert_not_called()
        self.assertEqual(self.window.mode, "launch")
        self.assertEqual(update.installed_info(folder)[0]["version"], "1.0.0")
        self.assertEqual(personal.read_bytes(), b"keep my hero")

    def test_network_failure_still_allows_bundled_update_then_launch(self):
        from installer import update
        from test_updater import newer_payload
        self.installed_folder()
        self.window.payload = newer_payload(self.root / "next")
        self.window.metadata, _ = update.payload_inventory(self.window.payload)
        self.window.network = True
        with mock.patch("installer.wizard.check_updates", side_effect=OSError("offline")):
            self.window.inspect_destination()
            self.wait_for_operation()
        self.assertIn("Could not check", self.window.status.text())
        self.assertEqual(self.window.mode, "update")
        self.assertTrue(self.window.check.isEnabled())
        self.window.start.click()
        self.wait_for_operation()
        self.assertEqual(self.window.mode, "launch")
        self.dialog.assert_not_called()
        with mock.patch("installer.wizard.subprocess.Popen") as launch:
            self.window.start.click()
            self.assertEqual(launch.call_args.args[0], [str(self.window.executable)])

    def test_newer_installation_never_offers_a_downgrade(self):
        from installer import update
        from test_updater import newer_payload
        folder = self.installed_folder()
        update.apply_update(folder, newer_payload(self.root / "next"))
        self.window.inspect_destination()
        self.assertEqual(self.window.mode, "launch")
        self.assertIn("0.1.0-alpha.10", self.window.status.text())
        self.assertFalse(self.window.image.isEnabled())

    def test_changed_folder_is_rechecked_before_an_update_click(self):
        self.installed_folder()
        self.window.inspect_destination()
        self.assertEqual(self.window.mode, "launch")
        self.window.destination.setText(str(self.root / "different game"))
        with mock.patch("installer.wizard.subprocess.Popen") as launch:
            self.window.start.click()
            launch.assert_not_called()
        self.assertEqual(self.window.mode, "install")
        self.assertIsNone(self.window.executable)

    def test_recovery_button_works_offline_without_a_receipt_in_place(self):
        from installer import update
        folder = self.installed_folder()
        transaction = folder / update.TRANSACTION
        backup = transaction / "backup"
        backup.mkdir(parents=True)
        (folder / "installation.json").rename(backup / "installation.json")
        update.atomic_json(transaction / "journal.json", {"schema": 1, "phase": "prepared",
                                                          "old": ["installation.json"], "new": []})
        self.window.inspect_destination()
        self.assertEqual(self.window.mode, "recover")
        self.assertFalse(self.window.image.isEnabled())
        self.window.start.click()
        self.wait_for_operation()
        self.assertEqual(self.window.mode, "launch")
        self.assertFalse(transaction.exists())
        self.dialog.assert_not_called()

    def test_committed_cleanup_is_launchable_and_retried_on_reopen(self):
        from installer import update
        from test_updater import newer_payload
        import shutil
        folder = self.installed_folder()
        remove = shutil.rmtree
        def locked(path, *args, **kwargs):
            if Path(path).name == "backup":
                raise PermissionError("loaded CRT")
            return remove(path, *args, **kwargs)
        with mock.patch.object(shutil, "rmtree", locked), mock.patch.object(
                update, "retry_locked", side_effect=lambda operation: operation()):
            update.apply_update(folder, newer_payload(self.root / "next"))
            self.window.inspect_destination()
            self.assertEqual(self.window.mode, "launch")
            self.assertIn("update is installed", self.window.status.text())
            self.assertTrue(self.window.start.isEnabled())
        self.window.inspect_startup()
        self.wait_for_operation()
        self.assertEqual(self.window.mode, "launch")
        self.assertFalse((folder / update.TRANSACTION).exists())
        self.dialog.assert_not_called()

    def test_self_update_finishes_on_close_not_as_an_extra_installer_download(self):
        from installer import releases, self_update, update
        from test_updater import newer_payload, release_row
        from test_installer_self_update import setup_package, setup_row
        from PySide6.QtGui import QCloseEvent
        folder = self.installed_folder()
        setup = self.root / ("Installer" + self_update.SUFFIX)
        setup.write_bytes(b"old installer")
        raw = newer_payload(self.root / "next", "1.0.0").read_bytes()
        system = update.installed_info(folder)[0]["platform"]
        package = setup_package(b"new installer", "1.0.0", system)
        row = setup_row(release_row("1.0.0", system, raw), package)
        self.window.network = True
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch.object(
                sys, "executable", str(setup)), mock.patch.object(releases, "open_release", side_effect=[
                io.BytesIO(json.dumps([row]).encode()), io.BytesIO(raw), io.BytesIO(package)]):
            self.window.inspect_destination()
            self.wait_for_operation()
            self.window.start.click()
            self.wait_for_operation()
        self.assertTrue(self.window.installer_pending)
        self.assertEqual(setup.read_bytes(), b"old installer")
        self.assertFalse(self.window.destination.isEnabled())
        self.assertFalse(self.window.check.isEnabled())
        self.assertTrue(self.window.start.isEnabled())
        self.assertIn("Close this window", self.window.status.text())
        event = QCloseEvent()
        with mock.patch.object(self_update, "launch_pending") as launch:
            self.window.closeEvent(event)
            launch.assert_called_once_with(folder)
        self.assertTrue(event.isAccepted())
        self_update.finish_pending(folder)
        self.assertEqual(setup.read_bytes(), b"new installer")
        self.dialog.assert_not_called()

    def test_installer_can_catch_up_after_game_already_updated(self):
        from installer import releases, self_update, update
        from test_updater import newer_payload, release_row
        from test_installer_self_update import setup_package, setup_row
        folder = self.installed_folder()
        update.apply_update(folder, newer_payload(self.root / "next", "1.0.0"))
        system = update.installed_info(folder)[0]["platform"]
        package = setup_package(b"new installer", "1.0.0", system)
        row = setup_row(release_row("1.0.0", system), package)
        release = releases.select_release([row], self.window.metadata["version"], system)
        self.window.network = True
        with mock.patch.object(sys, "frozen", True, create=True), mock.patch(
                "installer.wizard.check_updates", return_value=release) as check:
            self.window.inspect_destination()
            self.wait_for_operation()
            self.assertEqual(check.call_args.args[0], self.window.metadata["version"])
            self.assertEqual(self.window.mode, "installer_update")
            self.assertIn("Update installer", self.window.start.text())

    def test_invalid_cleanup_journal_does_not_crash_wizard(self):
        from installer import update
        folder = self.installed_folder()
        transaction = folder / update.TRANSACTION
        transaction.mkdir()
        (transaction / "journal.json").write_text("incomplete")
        self.window.inspect_destination()
        self.assertEqual(self.window.mode, "recover")

    def test_cancelling_a_check_waits_for_the_worker_and_keeps_installation(self):
        import threading
        from installer.install import check_cancel
        from test_updater import snapshot
        from PySide6.QtGui import QCloseEvent
        started = threading.Event()
        finish = threading.Event()
        self.addCleanup(finish.set)
        folder = self.installed_folder()
        before = snapshot(folder)
        self.window.network = True
        def check(_version, _system, cancel):
            started.set()
            finish.wait(5)
            check_cancel(cancel)
        with mock.patch("installer.wizard.check_updates", side_effect=check):
            self.window.inspect_destination()
            self.assertTrue(started.wait(5))
            event = QCloseEvent()
            self.window.closeEvent(event)
            self.assertFalse(event.isAccepted())
            self.assertTrue(self.window.worker.cancelled.is_set())
            finish.set()
            self.wait_for_operation()
        self.assertEqual(snapshot(folder), before)
        self.assertEqual(self.window.mode, "launch")
        self.dialog.assert_not_called()


if __name__ == "__main__":
    unittest.main()
