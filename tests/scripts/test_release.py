"""Publication refuses partial, tampered or unexpected release inventories."""

from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from installer.install import retry_locked
from publish_release import validated_assets
from release import check_frozen_installer, digest, installer_notices


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

    def test_linux_frozen_wizard_checks_real_x11_plugin_as_well_as_offscreen(self):
        with mock.patch("release.devenv.WINDOWS", False), mock.patch(
                "release.freezer_environment", side_effect=lambda: {}), mock.patch("release.subprocess.run") as run:
            check_frozen_installer(self.root / "setup", self.root)
        self.assertEqual(run.call_count, 3)
        self.assertEqual(run.call_args_list[1].kwargs["env"]["QT_QPA_PLATFORM"], "offscreen")
        self.assertEqual(run.call_args_list[2].args[0][:2], ["xvfb-run", "-a"])
        self.assertEqual(run.call_args_list[2].kwargs["env"]["QT_QPA_PLATFORM"], "xcb")

    def test_windows_frozen_checks_do_not_require_xvfb(self):
        with mock.patch("release.devenv.WINDOWS", True), mock.patch(
                "release.freezer_environment", side_effect=lambda: {}), mock.patch("release.subprocess.run") as run:
            check_frozen_installer(self.root / "setup.exe", self.root)
        self.assertEqual(run.call_count, 2)
        self.assertNotIn("QT_QPA_PLATFORM", run.call_args_list[0].kwargs["env"])

    def test_frozen_desktop_failure_blocks_packaging(self):
        import subprocess
        with mock.patch("release.devenv.WINDOWS", False), mock.patch(
                "release.freezer_environment", side_effect=lambda: {}), mock.patch(
                "release.subprocess.run", side_effect=[None, None, subprocess.CalledProcessError(1, "xvfb-run")]):
            with self.assertRaisesRegex(RuntimeError, "wizard-x11"):
                check_frozen_installer(self.root / "setup", self.root)

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

    def test_linux_qt_wheels_use_matching_upstream_notices(self):
        notice = self.root / "LICENSE.txt"
        notice.write_text("fixture notice", encoding="utf-8")
        empty = mock.Mock(files=[])
        freezer = mock.Mock(files=[Path("LICENSE.txt")])
        freezer.locate_file.return_value = notice
        with mock.patch("release.importlib.metadata.distribution", side_effect=[empty, empty, freezer]), mock.patch(
                "release.importlib.metadata.version", return_value="6.11.2"), mock.patch(
                "release.upstream_license", return_value=(notice, "licenses/upstream.txt")) as upstream:
            files = installer_notices(self.root)
        self.assertEqual(len(files), 11)
        self.assertEqual(upstream.call_count, 10)
        for call in upstream.call_args_list:
            self.assertEqual(call.args[2], "6.11.2")
        self.assertTrue(any(call.args[3] == "LGPL-3.0-only.txt" for call in upstream.call_args_list))

    def test_missing_freezer_notice_still_fails(self):
        with mock.patch("release.importlib.metadata.distribution", return_value=mock.Mock(files=[])):
            with self.assertRaisesRegex(ValueError, "PyInstaller"):
                installer_notices(self.root)


if __name__ == "__main__":
    unittest.main()
