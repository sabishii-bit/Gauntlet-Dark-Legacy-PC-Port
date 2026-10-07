"""Small Qt desktop wizard; extraction and validation live in the testable backend."""

import json
import os
from pathlib import Path
import subprocess
import sys
import threading

from PySide6.QtCore import QThread, QTimer, Signal, Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (QApplication, QFileDialog, QFormLayout, QHBoxLayout,
                              QLabel, QLineEdit, QMessageBox, QProgressBar,
                              QPushButton, QVBoxLayout, QWidget)

from .install import Cancelled, install, payload_inventory
from .releases import check_updates
from . import self_update
from .update import (TRANSACTION, committed_cleanup_pending, installed_info,
                     recover_update, update_from_release, update_with_installer)
from .versions import version_order

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


class OperationThread(QThread):
    progress = Signal(int, str)
    success = Signal(object)
    failure = Signal(str, bool)

    def __init__(self, operation, parent):
        super().__init__(parent)
        self.operation = operation
        self.cancelled = threading.Event()

    def run(self):
        try:
            result = self.operation(lambda done, total, name: self.progress.emit(
                int(done * 100 / max(total, 1)), name), self.cancelled.is_set)
            self.success.emit(result)
        except Exception as error:
            self.failure.emit(str(error), isinstance(error, Cancelled))


class Wizard(QWidget):
    def __init__(self, payload, destination=None, network=True):
        super().__init__()
        self.payload = payload
        self.metadata, _ = payload_inventory(payload)
        self.worker = None
        self.executable = None
        self.mode = "install"
        self.installed = None
        self.release = None
        self.operation = None
        self.check_context = ""
        self.network = network
        self.installer_pending = False
        self.setWindowTitle(STRINGS["title"])
        self.setMinimumWidth(620)
        self.setAcceptDrops(True)
        layout = QVBoxLayout(self)
        self.intro = QLabel(STRINGS["intro"])
        self.intro.setWordWrap(True)
        layout.addWidget(self.intro)
        form = QFormLayout()
        self.image = ImagePath()
        self.destination = QLineEdit(str(destination or installer_directory()))
        self.image_browse = QPushButton(STRINGS["browse"])
        self.folder_browse = QPushButton(STRINGS["browse"])
        for key, edit, button in (("image", self.image, self.image_browse),
                                  ("destination", self.destination, self.folder_browse)):
            row = QHBoxLayout()
            row.addWidget(edit, 1)
            row.addWidget(button)
            form.addRow(STRINGS[key], row)
        layout.addLayout(form)
        self.status = QLabel()
        self.status.setTextFormat(Qt.TextFormat.PlainText)
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        row = QHBoxLayout()
        self.start = QPushButton(STRINGS["install"])
        self.check = QPushButton(STRINGS["check_updates"])
        self.check.setEnabled(self.network)
        self.open = QPushButton(STRINGS["open"])
        self.open.hide()
        self.cancel = QPushButton(STRINGS["close"])
        row.addStretch()
        for button in (self.check, self.open, self.start, self.cancel):
            row.addWidget(button)
        layout.addLayout(row)
        self.image_browse.clicked.connect(self.browse_image)
        self.folder_browse.clicked.connect(self.browse_folder)
        self.start.clicked.connect(self.begin)
        self.check.clicked.connect(self.check_online)
        self.cancel.clicked.connect(self.close)
        self.open.clicked.connect(lambda: QDesktopServices.openUrl(
            QUrl.fromLocalFile(str(self.executable.parent))))
        self.scan_timer = QTimer(self)
        self.scan_timer.setSingleShot(True)
        self.scan_timer.setInterval(400)
        self.scan_timer.timeout.connect(self.inspect_destination)
        self.destination.textChanged.connect(lambda: self.scan_timer.start())
        # No network is required for a fresh installation or the release GUI smoke.
        QTimer.singleShot(0, self.inspect_startup)

    def inspect_startup(self):
        # A previous successful update may have retained a CRT image mapped by
        # its own frozen loader. Retry disposal once, in the worker, on reopening.
        try:
            pending = committed_cleanup_pending(self.folder())
        except (OSError, ValueError):
            pending = False
        if pending:
            self.status.setText(STRINGS["cleaning"])
            folder = self.folder()
            self.run_operation("recover", lambda _progress, _cancel: recover_update(folder))
        else:
            self.inspect_destination()

    def dragEnterEvent(self, event):
        if not self.busy():
            self.image.dragEnterEvent(event)

    def dropEvent(self, event):
        if not self.busy():
            self.image.dropEvent(event)

    def busy(self):
        return self.operation is not None

    def folder(self):
        return Path(os.path.abspath(self.destination.text().strip().strip('"')))

    def inspect_destination(self, auto_check=True):
        if self.busy():
            return
        self.scan_timer.stop()
        self.release = None
        self.installed = None
        self.executable = None
        self.open.hide()
        self.mode = "install"
        self.status.clear()
        folder = self.folder()
        try:
            self.installer_pending = self_update.inspect_pending(folder)
        except (OSError, ValueError) as error:
            self.mode = "invalid"
            self.status.setText(str(error))
            self.refresh_controls()
            return
        if self.installer_pending:
            self.mode = "launch"
            self.installed, _ = installed_info(folder)
            self.executable = folder / self.installed["executable"]
            self.status.setText(STRINGS["installer_pending"])
            self.open.show()
            self.refresh_controls()
            return
        if (folder / TRANSACTION).exists() and not committed_cleanup_pending(folder):
            self.mode = "recover"
            self.status.setText(STRINGS["recovery_needed"])
        elif (folder / "installation.json").exists():
            try:
                self.installed, _ = installed_info(folder)
                self.offer_update()
            except Exception as error:
                self.mode = "invalid"
                self.status.setText(str(error))
        self.refresh_controls()
        if self.installed and self.network and auto_check:
            self.check_online()

    def offer_update(self):
        version = self.release.version if self.release else self.metadata["version"]
        self.executable = self.folder() / self.installed["executable"]
        if version_order(version) > version_order(self.installed["version"]):
            self.mode = "update"
            self.status.setText(STRINGS["available"].format(
                installed=self.installed["version"], version=version))
        elif (self.release and version == self.installed["version"] and getattr(sys, "frozen", False) and
              version_order(version) > version_order(self.metadata["version"])):
            self.mode = "installer_update"
            self.status.setText(STRINGS["installer_available"].format(version=version))
        else:
            self.mode = "launch"
            self.status.setText(STRINGS["up_to_date"].format(version=self.installed["version"]))
        if committed_cleanup_pending(self.folder()):
            self.status.setText(self.status.text() + "\n" + STRINGS["cleanup_pending"])
        self.open.show()

    def refresh_controls(self):
        free = not self.busy()
        self.intro.setText(STRINGS["intro"] if self.mode == "install" else STRINGS["intro_update"])
        for widget in (self.destination, self.folder_browse):
            widget.setEnabled(free and not self.installer_pending)
        for widget in (self.image, self.image_browse):
            widget.setEnabled(free and self.mode == "install")
        self.check.setEnabled(free and self.network and not self.installer_pending)
        self.start.setEnabled(free and self.mode != "invalid")
        self.open.setEnabled(free)
        if self.mode == "update":
            version = self.release.version if self.release else self.metadata["version"]
            self.start.setText(STRINGS["update"].format(version=version))
        elif self.mode == "installer_update":
            self.start.setText(STRINGS["update_installer"].format(version=self.release.version))
        else:
            self.start.setText(STRINGS.get(self.mode, STRINGS["install"]))
        self.cancel.setText(STRINGS["cancel"] if self.busy() else STRINGS["close"])

    def run_operation(self, kind, operation):
        self.scan_timer.stop()
        self.operation = kind
        self.progress.setValue(0)
        self.refresh_controls()
        self.worker = OperationThread(operation, self)
        self.worker.progress.connect(self.advanced)
        self.worker.success.connect(self.completed)
        self.worker.failure.connect(self.failed)
        self.worker.finished.connect(self.finished)
        self.worker.start()

    def check_online(self):
        if self.busy() or not self.network or self.installer_pending:
            return
        # Re-read the chosen folder even if the user clicks before its debounce.
        # A check without a receipt compares with the bundle; it never turns a
        # fresh/invalid destination into an in-place update target.
        self.inspect_destination(auto_check=False)
        if self.installer_pending:
            return
        self.check_context = self.status.text() if self.mode in ("recover", "invalid") else ""
        reference = self.installed or self.metadata
        version, system = reference["version"], reference["platform"]
        if getattr(sys, "frozen", False) and version_order(self.metadata["version"]) < version_order(version):
            version = self.metadata["version"]  # The game can be newer than the installer.
        self.status.setText(STRINGS["checking"])
        self.run_operation("check", lambda _progress, cancel: check_updates(
            version, system, cancel))

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
        if self.busy():
            return
        # A click before the path's debounce fires must not use a previous folder's state.
        if self.scan_timer.isActive():
            self.scan_timer.stop()
            self.inspect_destination()
            if self.busy():
                return
        if self.mode == "launch" and self.executable:
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
        if self.mode == "recover":
            self.status.setText(STRINGS["recovering"])
            folder = self.folder()
            self.run_operation("recover", lambda _progress, _cancel: recover_update(folder))
            return
        if self.mode == "installer_update":
            folder, release, installer = self.folder(), self.release, Path(sys.executable)
            def update_installer(progress, cancel):
                self_update.prepare(folder, release, installer, progress, cancel)
                self_update.mark_ready(folder)
                return folder / self.installed["executable"]
            self.run_operation("update", update_installer)
            return
        if self.mode == "update":
            self.status.setText(STRINGS["updating"])
            folder, release = self.folder(), self.release
            installer = Path(sys.executable) if getattr(sys, "frozen", False) else None
            self.run_operation("update", lambda progress, cancel:
                update_from_release(folder, release, progress, cancel, installer=installer) if release else
                update_with_installer(folder, self.payload, installer, progress, cancel, self.metadata["version"]))
            return
        if self.mode != "install":
            return
        if not self.image.text().strip() or not self.destination.text().strip():
            self.status.setText(STRINGS["drop"])
            return
        self.status.setText(STRINGS["working"])
        image, folder = Path(self.image.text().strip().strip('"')), self.folder()
        updater = Path(sys.executable) if getattr(sys, "frozen", False) else None
        self.run_operation("install", lambda progress, cancel:
            install(image, folder, self.payload, progress, cancel, updater=updater))

    def advanced(self, percent, name):
        self.progress.setValue(percent)
        self.status.setText(name)

    def completed(self, result):
        if self.operation == "check":
            if self.installed is None:
                message = (STRINGS["bundle_available"].format(
                    version=result.version, bundled=self.metadata["version"]) if result else
                    STRINGS["bundle_current"].format(version=self.metadata["version"]))
                self.status.setText("\n".join(part for part in (self.check_context, message) if part))
                return
            # Prefer the embedded payload if it is newer than the published runtime.
            self.release = result if result and version_order(result.version) > version_order(self.metadata["version"]) else None
            self.offer_update()
            return
        if self.operation == "recover":
            self.installed = result
            self.offer_update()
            return
        self.executable = Path(result)
        self.installed, _ = installed_info(self.executable.parent)
        self.status.setText(STRINGS["updated" if self.operation == "update" else "complete"].format(
            version=self.installed["version"]))
        if committed_cleanup_pending(self.executable.parent):
            self.status.setText(self.status.text() + "\n" + STRINGS["cleanup_pending"])
        self.installer_pending = self_update.inspect_pending(self.executable.parent)
        if self.installer_pending:
            self.status.setText(self.status.text() + "\n" + STRINGS["installer_pending"])
        self.mode = "launch"
        self.open.show()

    def failed(self, message, cancelled):
        if self.operation == "check":
            if self.installed is not None:
                self.offer_update()
            key = "offline" if self.installed is not None else "check_failed"
            notice = STRINGS["check_cancelled"] if cancelled else STRINGS[key].format(error=message)
            self.status.setText("\n".join(part for part in (self.check_context, notice) if part))
            return
        self.status.setText(message)
        if not cancelled:
            QMessageBox.critical(self, STRINGS["failed"], message)

    def finished(self):
        recovered = self.operation == "recover"
        self.operation = None
        if (self.folder() / TRANSACTION).exists() and not committed_cleanup_pending(self.folder()):
            self.mode = "recover"
        self.refresh_controls()
        if recovered and self.network and not (self.folder() / TRANSACTION).exists():
            self.inspect_destination()

    def closeEvent(self, event):
        if self.busy():
            self.worker.cancelled.set()
            self.status.setText(STRINGS["cancelling"])
            event.ignore()
        else:
            if self.installer_pending:
                try:
                    self_update.launch_pending(self.folder())
                    self.installer_pending = False
                except (OSError, ValueError) as error:
                    QMessageBox.critical(self, STRINGS["failed"], str(error))
                    event.ignore()
                    return
            event.accept()


def main(payload, destination=None):
    app = QApplication(sys.argv)
    window = Wizard(payload, destination)
    window.show()
    return app.exec()
