"""Shared SemVer ordering and build-time validation, without compiling the game."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from installer.versions import SEMVER, is_prerelease, version_order

VALID = (
    "0.0.0", "0.1.0-alpha.1", "0.1.0-alpha.0", "1.0.0-beta.10", "1.0.0-rc.1",
    "1.0.0", "1.0.1", "2.0.0-preview", "1.2.3-0", "1.2.3-01a",
    "1.2.3-alpha.beta", "1.2.3-x-y-z.--", "1.2.3+001", "1.2.3+sha-with-dashes",
    "1.2.3-rc.1+build.001", "10.11.12-nightly.20261007",
)
INVALID = (
    "", "main", "1", "1.2", "01.2.3", "1.02.3", "1.2.03", "v1.2.3", "1.2.3.4",
    "1.2.3-", "1.2.3-alpha..1", "1.2.3-01", "1.2.3-rc.01", "1.2.3-α",
    "1.2.3+", "1.2.3+foo..bar", "1.2.3+x+y", "1.2.3-foo_bar", "1.2.3 ",
)


class VersionTests(unittest.TestCase):
    def test_valid_and_invalid_versions(self):
        for version in VALID:
            with self.subTest(version=version):
                self.assertIsNotNone(SEMVER.fullmatch(version))
                self.assertIsNotNone(version_order(version))
                self.assertEqual(version_order("v" + version), version_order(version))
        for version in INVALID:
            with self.subTest(version=version):
                self.assertIsNone(SEMVER.fullmatch(version))
                if not version.startswith("v"):
                    self.assertIsNone(version_order(version))
        self.assertIsNone(version_order(None))
        self.assertIsNone(version_order("vv1.2.3"))

    def test_semver_precedence_including_numeric_identifiers_and_stable(self):
        ordered = ["1.0.0-0", "1.0.0-1", "1.0.0-alpha", "1.0.0-alpha.0",
                   "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                   "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0", "1.0.1-alpha.1", "1.0.1",
                   "1.9.0", "1.10.0", "2.0.0-alpha.1"]
        for older, newer in zip(ordered, ordered[1:]):
            with self.subTest(older=older, newer=newer):
                self.assertLess(version_order(older), version_order(newer))

    def test_build_metadata_is_not_an_upgrade_or_a_prerelease_label(self):
        for version in ("1.2.3", "1.2.3-alpha.1"):
            self.assertEqual(version_order(version + "+001"), version_order(version + "+999"))
            self.assertEqual(version_order(version), version_order(version + "+sha-with-dashes"))
        self.assertFalse(is_prerelease("1.2.3+sha-with-dashes"))
        self.assertTrue(is_prerelease("v1.2.3-rc.1+release"))
        with self.assertRaises(ValueError):
            is_prerelease("main")

    @unittest.skipUnless(shutil.which("cmake"), "CMake is not installed")
    def test_cmake_and_python_accept_the_same_version_grammar(self):
        with tempfile.TemporaryDirectory(prefix="gdl version ") as directory:
            path = Path(directory) / "version.cmake"
            path.write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                f'include("{(ROOT / "cmake/GdlVersion.cmake").as_posix()}")\n'
                'file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION-input" TEST_VERSION)\n'
                'gdl_version_core("${TEST_VERSION}" core)\nmessage(STATUS "Core=${core}")\n',
                encoding="utf-8")
            for version in (*VALID, *INVALID):
                with self.subTest(version=version):
                    # CMake strips whitespace from -D arguments; use an exact file input.
                    (path.parent / "VERSION-input").write_text(version, encoding="utf-8")
                    result = subprocess.run(["cmake", "-P", str(path)],
                                            capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode == 0, version in VALID, result.stdout + result.stderr)
                    if version in VALID:
                        core = version.split("+", 1)[0].split("-", 1)[0]
                        self.assertIn("Core=" + core, result.stdout)


if __name__ == "__main__":
    unittest.main()
