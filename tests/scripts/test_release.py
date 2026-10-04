"""Publication refuses partial, tampered or unexpected release inventories."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from installer.install import retry_locked
from publish_release import validated_assets
from release import digest


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl release tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tag = "v0.1.0-alpha.1"
        self.packages = []
        for platform, extension in (("windows-x64", ".exe"), ("linux-x64", ".tar.gz")):
            stem = f"GauntletDarkLegacy-{self.tag[1:]}-{platform}-setup"
            paths = [self.root / (stem + extension), self.root / (stem + "-licenses.zip")]
            for path in paths:
                path.write_bytes(b"synthetic release")
            checksum = self.root / (stem + ".sha256")
            checksum.write_text("".join(f"{digest(path)}  {path.name}\n" for path in paths), encoding="ascii")
            self.packages.append((paths, checksum))

    def test_both_platforms_are_required(self):
        self.assertEqual(len(validated_assets(self.root, self.tag)), 6)
        self.packages[1][1].unlink()
        with self.assertRaises(FileNotFoundError):
            validated_assets(self.root, self.tag)

    def test_modified_binary_is_rejected(self):
        self.packages[0][0][0].write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "checksum"):
            validated_assets(self.root, self.tag)

    def test_duplicate_or_escaping_paths_are_rejected(self):
        checksum = self.packages[0][1]
        original = checksum.read_text(encoding="ascii")
        for extra in (original.splitlines()[0], "0" * 64 + "  ../outside.exe"):
            with self.subTest(extra=extra):
                checksum.write_text(original + extra + "\n", encoding="ascii")
                with self.assertRaisesRegex(ValueError, "checksum"):
                    validated_assets(self.root, self.tag)

    def test_missing_license_archive_is_rejected(self):
        checksum = self.packages[0][1]
        checksum.write_text(checksum.read_text(encoding="ascii").splitlines()[0] + "\n", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            validated_assets(self.root, self.tag)

    def test_windows_transient_file_lock_is_retried(self):
        operation = mock.Mock(side_effect=[PermissionError("scanner"), 42])
        with mock.patch("installer.install.os.name", "nt"), mock.patch("installer.install.time.sleep") as sleep:
            self.assertEqual(retry_locked(operation), 42)
        self.assertEqual(operation.call_count, 2)
        sleep.assert_called_once_with(0.2)

    def test_windows_file_lock_retry_is_bounded(self):
        operation = mock.Mock(side_effect=PermissionError("permanent"))
        with mock.patch("installer.install.os.name", "nt"), mock.patch(
                "installer.install.time.monotonic", side_effect=[0, 9]):
            with self.assertRaises(PermissionError):
                retry_locked(operation)
        operation.assert_called_once()


if __name__ == "__main__":
    unittest.main()
