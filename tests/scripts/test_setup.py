"""The setup preflight must detect missing build dependencies on provisioned runners."""
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import setup


class LinuxSetupTests(unittest.TestCase):
    def test_all_supported_managers_install_autotools(self):
        for manager, packages in setup.PACKAGES.items():
            with self.subTest(manager=manager):
                self.assertTrue({"autoconf", "autoconf-archive", "automake", "libtool"}
                                <= set(packages["build"]))

    @mock.patch.object(setup, "output")
    def test_debian_requires_installed_not_removed_or_unknown(self, output):
        output.side_effect = ["install ok installed", "deinstall ok config-files", ""]
        self.assertEqual(setup.missing_packages("apt-get", ["good", "removed", "absent"]),
                         ["removed", "absent"])
        output.assert_any_call(["dpkg-query", "-W", "-f=${Status}", "good"])

    @mock.patch.object(setup.subprocess, "run")
    def test_rpm_and_pacman_use_query_exit_status(self, run):
        for manager, query in [("dnf", "rpm"), ("pacman", "pacman")]:
            with self.subTest(manager=manager):
                run.side_effect = [subprocess.CompletedProcess([], 0),
                                   subprocess.CompletedProcess([], 1)]
                self.assertEqual(setup.missing_packages(manager, ["good", "absent"]), ["absent"])
                self.assertEqual(run.call_args.args[0][0], query)

    def test_preinstalled_compiler_and_loader_do_not_hide_missing_headers(self):
        with mock.patch.object(setup, "package_manager", return_value="apt-get"), \
             mock.patch.object(setup, "missing_packages", return_value=["autoconf-archive"]), \
             mock.patch.object(setup, "newest_compiler", return_value="/usr/bin/g++-14"), \
             mock.patch.object(setup.shutil, "which", return_value="/usr/bin/tool"), \
             mock.patch.object(setup, "version_of", return_value=(3, 31, 0)), \
             mock.patch.object(setup, "check_vcpkg"), \
             mock.patch.object(setup.ctypes.util, "find_library", return_value="libvulkan.so.1"), \
             mock.patch.object(setup, "package_install") as install:
            report = setup.Report()
            setup.check_linux(report, tooling=False)
            missing = report.missing()
            self.assertEqual([item.name for item in missing], ["Linux build packages"])
            self.assertIn("autoconf-archive", missing[0].advice)
            install.assert_any_call(setup.PACKAGES["apt-get"]["build"])

    def test_shared_package_fix_runs_once(self):
        fix = mock.Mock()
        report = setup.Report([setup.Requirement("headers", None, fix),
                               setup.Requirement("compiler", None, fix)])
        after = setup.Report([setup.Requirement("packages", "installed")])
        with mock.patch.object(setup, "gather", return_value=after):
            self.assertTrue(setup.install_missing(report, yes=True))
        fix.assert_called_once()


class HookTests(unittest.TestCase):
    def test_hooks_are_installed_once_and_existing_hooks_are_left_alone(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "scripts" / "hooks").mkdir(parents=True)
            (root / "scripts" / "hooks" / "pre-push").write_text("#!/usr/bin/env python\n")
            (root / "scripts" / "hooks" / "__pycache__").mkdir()
            # No repository: nothing to do, nothing wrong.
            self.assertTrue(setup.install_hooks(root, out=lambda *_: None))
            (root / ".git" / "hooks").mkdir(parents=True)
            messages = []
            self.assertTrue(setup.install_hooks(root, out=messages.append))
            installed = root / ".git" / "hooks" / "pre-push"
            self.assertEqual(installed.read_text(), "#!/usr/bin/env python\n")
            self.assertEqual(len(messages), 1)
            self.assertFalse((root / ".git" / "hooks" / "__pycache__").exists())
            # A second run keeps the installed copy quietly; a foreign hook is reported.
            self.assertTrue(setup.install_hooks(root, out=messages.append))
            installed.write_text("#!/bin/sh\nexit 0\n")
            self.assertFalse(setup.install_hooks(root, out=messages.append))
            self.assertIn("scripts/hooks/pre-push", messages[-1])
            installed.write_text("#!/bin/sh\npython scripts/hooks/pre-push\n")
            self.assertTrue(setup.install_hooks(root, out=messages.append))


class LauncherSetupTests(unittest.TestCase):
    def test_every_manager_lists_the_window_packages(self):
        expected = {"apt-get": {"libwebkit2gtk-4.1-dev", "xvfb"},
                    "dnf": {"webkit2gtk4.1-devel", "xorg-x11-server-Xvfb"},
                    "pacman": {"webkit2gtk-4.1", "xorg-server-xvfb"}}
        for manager, packages in setup.PACKAGES.items():
            with self.subTest(manager=manager):
                self.assertTrue(expected[manager] <= set(packages["launcher"]))

    def test_the_toolchain_is_the_one_the_workspace_pins(self):
        pin = (setup.LAUNCHER / "rust-toolchain.toml").read_text(encoding="utf-8")
        self.assertIn(f'channel = "{setup.launcher_toolchain()}"', pin)

    def test_the_toolchain_is_ready_only_with_rustfmt_and_clippy(self):
        listing = "stable-x86_64-pc-windows-msvc (default)\n1.98.0-x86_64-pc-windows-msvc\n"
        both = "cargo-x86_64-pc-windows-msvc\nclippy-x86_64-pc-windows-msvc\n" \
               "rustfmt-x86_64-pc-windows-msvc\n"
        without_clippy = "cargo-x86_64-pc-windows-msvc\nrustfmt-x86_64-pc-windows-msvc\n"
        with mock.patch.object(setup, "rustup", return_value="rustup"):
            with mock.patch.object(setup, "output", side_effect=[listing, both]):
                self.assertEqual(setup.rust_toolchain_ready("1.98.0"), "rustup (1.98.0)")
            with mock.patch.object(setup, "output", side_effect=[listing, without_clippy]):
                self.assertIsNone(setup.rust_toolchain_ready("1.98.0"))
            with mock.patch.object(setup, "output", return_value="stable-x86_64 (default)\n"):
                self.assertIsNone(setup.rust_toolchain_ready("1.98.0"))
        with mock.patch.object(setup, "rustup", return_value=None):
            self.assertIsNone(setup.rust_toolchain_ready("1.98.0"))

    def test_the_toolchain_installs_minimal_with_rustfmt_and_clippy(self):
        with mock.patch.object(setup, "rustup", return_value="rustup"), \
                mock.patch.object(setup, "run") as run:
            setup.install_rust_toolchain()
        run.assert_called_once_with(["rustup", "toolchain", "install", "--no-self-update",
                                     setup.launcher_toolchain(), "--profile", "minimal",
                                     "--component", "rustfmt", "--component", "clippy"])

    def test_rustup_is_looked_for_where_rustup_keeps_it_first(self):
        with mock.patch.object(setup.devenv, "rust_bin", return_value=pathlib.Path("cargo-bin")), \
                mock.patch.object(setup, "program", return_value="found") as program:
            self.assertEqual(setup.rustup(), "found")
        program.assert_called_once_with("rustup", pathlib.Path("cargo-bin"))

    def test_webview2_is_only_advised_on_windows(self):
        with mock.patch.object(setup, "WINDOWS", True), \
                mock.patch.object(setup, "rust_toolchain_ready", return_value="rustup (1.98.0)"), \
                mock.patch.object(setup, "webview2_version", return_value=None):
            report = setup.Report()
            setup.check_launcher(report)
        missing = report.missing()
        self.assertEqual([item.name for item in missing], ["WebView2 runtime"])
        self.assertTrue(missing[0].optional)
        self.assertIsNone(missing[0].fix)

    def test_missing_window_packages_are_named_and_installed_together_on_linux(self):
        with mock.patch.object(setup, "WINDOWS", False), \
                mock.patch.object(setup, "rust_toolchain_ready", return_value="rustup (1.98.0)"), \
                mock.patch.object(setup, "package_manager", return_value="apt-get"), \
                mock.patch.object(setup, "missing_packages", return_value=["xvfb"]), \
                mock.patch.object(setup, "package_install") as install:
            report = setup.Report()
            setup.check_launcher(report)
        missing = report.missing()
        self.assertEqual([item.name for item in missing], ["Launcher window packages"])
        self.assertIn("xvfb", missing[0].advice)
        install.assert_called_once_with(setup.PACKAGES["apt-get"]["launcher"])

    def test_launcher_checks_join_the_report_only_when_asked(self):
        with mock.patch.object(setup, "check_common"), \
                mock.patch.object(setup, "check_windows"), \
                mock.patch.object(setup, "check_linux"), \
                mock.patch.object(setup, "check_launcher") as launcher:
            setup.gather(tooling=False)
            launcher.assert_not_called()
            setup.gather(tooling=False, launcher=True)
            launcher.assert_called_once()


if __name__ == "__main__":
    unittest.main()
