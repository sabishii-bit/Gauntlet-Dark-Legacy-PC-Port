"""Small Qt desktop wizard; extraction and validation live in the testable backend."""

import json
import os
from pathlib import Path
import subprocess
import sys
import threading

from PySide6.QtCore import QThread, Signal, Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (QApplication, QFileDialog, QFormLayout, QHBoxLayout,
                              QLabel, QLineEdit, QMessageBox, QProgressBar,
                              QPushButton, QVBoxLayout, QWidget)

from .install import Cancelled, install, payload_inventory

STRINGS = json.loads(Path(__file__).with_name("strings.json").read_text(encoding="utf-8"))


def installer_directory() -> Path:
    # A one-file bundle's __file__ lives in a temporary extraction directory.
    return Path(sys.executable if getattr(sys, "frozen", False) else sys.argv[0]).resolve().parent


class ImagePath(QLineEdit):
    def __init__(self):
        super().__init__()
        self.setAcceptDrops(True)
        self.setPlaceholderText(STRINGS["drop"])

    def dragEnterEvent(self, event):
        urls = event.mimeData().urls()
        if len(urls) == 1 and urls[0].isLocalFile():
            event.acceptProposedAction()

    def dropEvent(self, event):
        urls = event.mimeData().urls()
        if len(urls) == 1 and urls[0].isLocalFile():
            self.setText(urls[0].toLocalFile())
            event.acceptProposedAction()


class InstallThread(QThread):
    progress = Signal(int, str)
    success = Signal(str)
    failure = Signal(str, bool)

    def __init__(self, image, destination, payload, parent):
        super().__init__(parent)
        self.image, self.destination, self.payload = image, destination, payload
        self.cancelled = threading.Event()

    def run(self):
        try:
            executable = install(self.image, self.destination, self.payload,
                                 lambda done, total, name: self.progress.emit(
                                     int(done * 100 / max(total, 1)), name),
                                 self.cancelled.is_set)
            self.success.emit(str(executable))
        except Exception as error:
            self.failure.emit(str(error), isinstance(error, Cancelled))


class Wizard(QWidget):
    def __init__(self, payload):
        super().__init__()
        self.payload = payload
        self.metadata, _ = payload_inventory(payload)
        self.worker = None
        self.executable = None
        self.setWindowTitle(STRINGS["title"] + " " + self.metadata["version"])
        self.setMinimumWidth(620)
        self.setAcceptDrops(True)
        layout = QVBoxLayout(self)
        intro = QLabel(STRINGS["intro"])
        intro.setWordWrap(True)
        layout.addWidget(intro)
        form = QFormLayout()
        self.image = ImagePath()
        self.destination = QLineEdit(str(installer_directory()))
        self.image_browse = QPushButton(STRINGS["browse"])
        self.folder_browse = QPushButton(STRINGS["browse"])
        for key, edit, button in (("image", self.image, self.image_browse),
                                  ("destination", self.destination, self.folder_browse)):
            row = QHBoxLayout()
            row.addWidget(edit, 1)
            row.addWidget(button)
            form.addRow(STRINGS[key], row)
        layout.addLayout(form)
        hint = QLabel(STRINGS["destination_help"])
        hint.setWordWrap(True)
        layout.addWidget(hint)
        self.status = QLabel(STRINGS["ready"])
        self.status.setTextFormat(Qt.TextFormat.PlainText)
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        row = QHBoxLayout()
        self.start = QPushButton(STRINGS["install"])
        self.open = QPushButton(STRINGS["open"])
        self.open.hide()
        self.cancel = QPushButton(STRINGS["close"])
        row.addStretch()
        for button in (self.open, self.start, self.cancel):
            row.addWidget(button)
        layout.addLayout(row)
        self.image_browse.clicked.connect(self.browse_image)
        self.folder_browse.clicked.connect(self.browse_folder)
        self.start.clicked.connect(self.begin)
        self.cancel.clicked.connect(self.close)
        self.open.clicked.connect(lambda: QDesktopServices.openUrl(
            QUrl.fromLocalFile(str(self.executable.parent))))

    def dragEnterEvent(self, event):
        if not self.busy():
            self.image.dragEnterEvent(event)

    def dropEvent(self, event):
        if not self.busy():
            self.image.dropEvent(event)

    def busy(self):
        return self.worker is not None and self.worker.isRunning()

    def browse_image(self):
        path, _ = QFileDialog.getOpenFileName(self, STRINGS["choose_image"],
                                              self.image.text(), STRINGS["image_filter"])
        if path:
            self.image.setText(path)

    def browse_folder(self):
        path = QFileDialog.getExistingDirectory(self, STRINGS["choose_directory"],
                                                self.destination.text())
        if path:
            self.destination.setText(path)

    def begin(self):
        if self.executable:
            environment = dict(os.environ)
            if sys.platform.startswith("linux"):
                if "LD_LIBRARY_PATH_ORIG" in environment:
                    environment["LD_LIBRARY_PATH"] = environment.pop("LD_LIBRARY_PATH_ORIG")
                else:
                    environment.pop("LD_LIBRARY_PATH", None)
            elif getattr(sys, "frozen", False) and os.name == "nt":
                import ctypes
                ctypes.windll.kernel32.SetDllDirectoryW(None)
            try:
                subprocess.Popen([str(self.executable)], cwd=self.executable.parent, env=environment)
            except OSError as error:
                QMessageBox.critical(self, STRINGS["failed"], str(error))
            return
        if not self.image.text().strip() or not self.destination.text().strip():
            self.status.setText(STRINGS["drop"])
            return
        self.status.setText(STRINGS["working"])
        self.progress.setValue(0)
        for widget in (self.image, self.destination, self.image_browse, self.folder_browse, self.start):
            widget.setEnabled(False)
        self.cancel.setText(STRINGS["cancel"])
        self.worker = InstallThread(Path(self.image.text().strip().strip('"')),
                                    Path(self.destination.text().strip().strip('"')),
                                    self.payload, self)
        self.worker.progress.connect(self.advanced)
        self.worker.success.connect(self.completed)
        self.worker.failure.connect(self.failed)
        self.worker.finished.connect(self.finished)
        self.worker.start()

    def advanced(self, percent, name):
        self.progress.setValue(percent)
        self.status.setText(name)

    def completed(self, executable):
        self.executable = Path(executable)
        self.status.setText(STRINGS["complete"].format(version=self.metadata["version"]))
        self.start.setText(STRINGS["launch"])
        self.open.show()

    def failed(self, message, cancelled):
        self.status.setText(message)
        if not cancelled:
            QMessageBox.critical(self, STRINGS["failed"], message)

    def finished(self):
        self.cancel.setText(STRINGS["close"])
        self.start.setEnabled(True)
        if not self.executable:
            for widget in (self.image, self.destination, self.image_browse, self.folder_browse):
                widget.setEnabled(True)

    def closeEvent(self, event):
        if self.busy():
            self.worker.cancelled.set()
            self.status.setText(STRINGS["cancelling"])
            event.ignore()
        else:
            event.accept()


def main(payload):
    app = QApplication(sys.argv)
    window = Wizard(payload)
    window.show()
    return app.exec()
