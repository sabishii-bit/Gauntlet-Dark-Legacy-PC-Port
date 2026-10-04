"""Real widget/input checks; enabled when the optional installer dependencies exist."""

import importlib.util
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
        temporary = tempfile.TemporaryDirectory(prefix="gdl wizard ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        from installer.wizard import Wizard
        self.window = Wizard(fixture_payload(self.root))
        self.addCleanup(self.window.close)
        self.dialog = mock.patch("installer.wizard.QMessageBox.critical").start()
        self.addCleanup(mock.patch.stopall)

    def test_initial_window_has_no_destination_or_ready_helper_message(self):
        from PySide6.QtWidgets import QLabel
        from installer.wizard import STRINGS
        self.assertEqual(self.window.status.text(), "")
        self.assertNotIn("destination_help", STRINGS)
        self.assertNotIn("ready", STRINGS)
        labels = [label.text() for label in self.window.findChildren(QLabel)]
        self.assertFalse(any("Defaults to the folder" in text or
                             "Ready to install." in text for text in labels))

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
        from PySide6.QtTest import QTest
        image = self.root / "game.iso"
        image.write_bytes(synthetic_iso())
        self.window.image.setText(str(image))
        self.window.destination.setText(str(self.root / "game"))
        self.window.start.click()
        self.assertFalse(self.window.image.isEnabled())
        self.assertTrue(self.window.worker.wait(10000))
        QTest.qWait(10)
        self.dialog.assert_not_called()
        self.assertIsNotNone(self.window.executable)
        self.assertTrue(self.window.executable.is_file())
        self.assertTrue(self.window.start.isEnabled())
        self.assertFalse(self.window.image.isEnabled())


if __name__ == "__main__":
    unittest.main()
