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


if __name__ == "__main__":
    unittest.main()
