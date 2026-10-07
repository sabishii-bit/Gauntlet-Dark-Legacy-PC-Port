"""Entry point frozen into the standalone installer; no Python needed by players."""

import argparse
import hashlib
import os
from pathlib import Path
import sys
import zipfile

from installer.install import payload_inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, default=Path(__file__).with_name("runtime.zip"))
    parser.add_argument("--check-payload", action="store_true",
                        help="validate the embedded release without opening a window")
    parser.add_argument("--check-wizard", action="store_true",
                        help="load native GUI libraries and exercise the wizard offscreen")
    parser.add_argument("--directory", type=Path, help="preselect an existing game or install directory")
    parser.add_argument("--diagnostic-log", type=Path,
                        help="write automated-check failures here (windowed builds have no stderr)")
    args = parser.parse_args()
    if args.check_payload:
        metadata, _ = payload_inventory(args.payload)
        with zipfile.ZipFile(args.payload) as archive:
            for name, expected in metadata["files"].items():
                digest = hashlib.sha256()
                with archive.open(name) as source:
                    while chunk := source.read(1024 * 1024):
                        digest.update(chunk)
                if digest.hexdigest() != expected:
                    raise ValueError(f"Damaged release file: {name}")
        print(f"Verified {metadata['version']} ({metadata['platform']})")
        return 0
    if args.check_wizard:
        # Release checks also exercise the actual X11 plugin under Xvfb. An
        # offscreen-only smoke test cannot detect missing desktop dependencies.
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtWidgets import QApplication
        from installer.wizard import Wizard
        app = QApplication([])
        window = Wizard(args.payload, destination=args.directory, network=False)
        window.show()
        app.processEvents()
        window.close()
        return 0
    from installer.wizard import main as wizard_main
    return wizard_main(args.payload, args.directory)


if __name__ == "__main__":
    # Automated frozen-app checks must fail promptly, not wait on PyInstaller's
    # windowed exception dialog on a headless release runner.
    if "--check-wizard" in sys.argv or "--check-payload" in sys.argv:
        try:
            sys.exit(main())
        except Exception:
            import traceback
            if "--diagnostic-log" in sys.argv:
                index = sys.argv.index("--diagnostic-log")
                if index + 1 < len(sys.argv):
                    Path(sys.argv[index + 1]).write_text(traceback.format_exc(), encoding="utf-8")
            if sys.stderr is not None:
                traceback.print_exc()
            sys.exit(1)
    else:
        sys.exit(main())
