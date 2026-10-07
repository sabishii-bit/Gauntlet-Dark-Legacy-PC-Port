"""Publication refuses partial, tampered or unexpected release inventories."""

from pathlib import Path
import os
import json
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from installer.install import retry_locked
from publish_release import check_publication, main as publish_main, require_new_version, validated_assets
from release import check_frozen_installer, digest, installer_notices, make_payload
from installer.releases import runtime_name


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl release tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tag = "v0.1.0-alpha.1"
        self.packages = []
        for platform, extension in (("windows-x64", ".exe"), ("linux-x64", ".tar.gz")):
            stem = f"GauntletDarkLegacy-{self.tag[1:]}-{platform}-setup"
            paths = [self.root / (stem + extension), self.root / (stem + "-licenses.zip"),
                     self.root / runtime_name(self.tag[1:], platform)]
            for path in paths:
                path.write_bytes(b"synthetic release")
            paths[-1].unlink()
            executable = "gauntlet.exe" if platform == "windows-x64" else "gauntlet"
            files = []
            for name, content in {executable: b"runtime", "VERSION": self.tag[1:].encode(),
                                  "portable.flag": b"", "data/config.json": b"{}"}.items():
                source = self.root / "inputs" / platform / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(content)
                files.append((source, name))
            make_payload(paths[-1], files, self.tag[1:], "test", platform, executable)
            checksum = self.root / (stem + ".sha256")
            checksum.write_text("".join(f"{digest(path)}  {path.name}\n" for path in paths), encoding="ascii")
            self.packages.append((paths, checksum))

    def test_version_must_increase_without_reusing_a_published_or_draft_release(self):
        for candidate, existing in (
                ("v0.1.0-alpha.2", "v0.1.0-alpha.2"),
                ("v0.1.0-alpha.1", "v0.1.0-alpha.2"),
                ("v0.1.0-alpha.3", "v0.2.0-alpha.1")):
            with self.subTest(candidate=candidate, existing=existing), self.assertRaises(ValueError):
                require_new_version(candidate, [existing])
        require_new_version("v0.1.0-alpha.10", ["v0.1.0-alpha.9"])
        require_new_version("v0.2.0-alpha.1", ["v0.1.0-alpha.99"])
        require_new_version(self.tag, [])

    def test_manual_and_branch_runs_cannot_publish(self):
        for event, ref in (("workflow_dispatch", "refs/heads/main"),
                           ("workflow_dispatch", f"refs/tags/{self.tag}"),
                           ("push", "refs/heads/main"),
                           ("push", "refs/tags/v0.1.0-alpha.2")):
            with self.subTest(event=event, ref=ref), mock.patch.dict(os.environ, {
                    "GITHUB_EVENT_NAME": event, "GITHUB_REF": ref, "RELEASE_TAG": self.tag}), mock.patch(
                    "publish_release.validate_tag", return_value=self.tag), mock.patch(
                    "publish_release.subprocess.check_output") as query, mock.patch(
                    "publish_release.subprocess.run") as publish:
                with self.assertRaises(ValueError):
                    publish_main([])
                query.assert_not_called()
                publish.assert_not_called()

    def test_duplicate_release_or_failed_remote_check_never_publishes(self):
        for result in (self.tag + "\n", subprocess.CalledProcessError(1, "gh api")):
            with self.subTest(result=result), mock.patch.dict(os.environ, {
                    "GITHUB_EVENT_NAME": "push", "GITHUB_REF": f"refs/tags/{self.tag}",
                    "RELEASE_TAG": self.tag}), mock.patch(
                    "publish_release.validate_tag", return_value=self.tag), mock.patch(
                    "publish_release.subprocess.check_output", side_effect=[result]), mock.patch(
                    "publish_release.subprocess.run") as publish:
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    publish_main([])
                publish.assert_not_called()

    def test_preflight_is_read_only_and_publication_creates_but_never_updates(self):
        with mock.patch.dict(os.environ, {
                "GITHUB_EVENT_NAME": "push", "GITHUB_REF": f"refs/tags/{self.tag}",
                "RELEASE_TAG": self.tag}), mock.patch(
                "publish_release.validate_tag", return_value=self.tag), mock.patch(
                "publish_release.subprocess.check_output", return_value="") as query, mock.patch(
                "publish_release.subprocess.run") as publish, mock.patch(
                "publish_release.ROOT", self.root):
            self.assertEqual(check_publication(), self.tag)
            self.assertIn("--paginate", query.call_args.args[0])
            publish_main(["--check"])
            publish.assert_not_called()
            with mock.patch("publish_release.validated_assets", return_value=[self.root / "setup.exe"]):
                publish_main([])
            publish.assert_called_once()
            command = publish.call_args.args[0]
            self.assertEqual(command[:4], ["gh", "release", "create", self.tag])
            self.assertIn("--verify-tag", command)
            self.assertNotIn("--clobber", command)

    def test_release_workflow_has_no_branch_push_and_serializes_all_versions(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/release.yml").read_text()
        triggers = workflow.split("permissions:", 1)[0]
        self.assertIn("tags: ['v*']", triggers)
        self.assertNotIn("branches:", triggers)
        self.assertIn("group: release\n  cancel-in-progress: false", workflow)
        self.assertIn("needs: preflight", workflow)
        publication = workflow.split("  publish:", 1)[1]
        self.assertIn("if: github.event_name == 'push' && startsWith(github.ref, 'refs/tags/')", publication)
        self.assertIn("needs: installers", publication)

    def test_both_platforms_are_required(self):
        self.assertEqual(len(validated_assets(self.root, self.tag)), 8)
        self.packages[1][1].unlink()
        with self.assertRaises(FileNotFoundError):
            validated_assets(self.root, self.tag)

    def test_manual_rebuild_can_select_source_without_bypassing_publication_guard(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/release.yml").read_text()
        inputs = workflow.split("  workflow_dispatch:", 1)[1].split("permissions:", 1)[0]
        self.assertIn("source_ref:", inputs)
        self.assertIn("required: false", inputs)
        installers = workflow.split("  installers:", 1)[1].split("  publish:", 1)[0]
        self.assertIn("ref: ${{ inputs.source_ref || github.ref }}", installers)
        publication = workflow.split("  publish:", 1)[1]
        self.assertIn("if: github.event_name == 'push' && startsWith(github.ref, 'refs/tags/')", publication)
        self.assertNotIn("inputs.source_ref", publication)

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

    def test_update_archive_is_required_and_must_match_its_release(self):
        paths, checksum = self.packages[0]
        original_checksum = checksum.read_text(encoding="ascii")
        checksum.write_text("\n".join(original_checksum.splitlines()[:2]) + "\n", encoding="ascii")
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            validated_assets(self.root, self.tag)
        with zipfile.ZipFile(paths[-1]) as archive:
            contents = {name: archive.read(name) for name in archive.namelist()}
        metadata = json.loads(contents["build-info.json"])
        metadata["version"] = "0.1.0-alpha.99"
        contents["build-info.json"] = json.dumps(metadata).encode()
        with zipfile.ZipFile(paths[-1], "w") as archive:
            for name, content in contents.items():
                archive.writestr(name, content)
        checksum.write_text("".join(f"{digest(path)}  {path.name}\n" for path in paths), encoding="ascii")
        with self.assertRaisesRegex(ValueError, "disagrees"):
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
