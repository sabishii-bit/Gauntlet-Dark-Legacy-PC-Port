"""Shared SemVer ordering and build-time validation, without compiling the game."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "release"))
from installer.versions import SEMVER, is_prerelease, version_order

# The cases live in a file of their own so that the launcher's Rust tests can read them too.
VECTORS = json.loads((ROOT / "tests" / "scripts" / "data" / "semver-vectors.json").read_text(encoding="utf-8"))
VALID = tuple(VECTORS["strict"]["valid"])
INVALID = tuple(VECTORS["strict"]["invalid"])


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
        for tag in VECTORS["tag"]["invalid"]:
            with self.subTest(tag=tag):
                self.assertIsNone(version_order(tag))
        self.assertIsNone(version_order(None))
        self.assertIsNone(version_order("vv1.2.3"))

    def test_semver_precedence_including_numeric_identifiers_and_stable(self):
        for chain in VECTORS["ascending"]:
            for older, newer in zip(chain, chain[1:]):
                with self.subTest(older=older, newer=newer):
                    self.assertLess(version_order(older), version_order(newer))

    def test_build_metadata_is_not_an_upgrade_or_a_prerelease_label(self):
        for first, second in VECTORS["equal"]:
            with self.subTest(first=first, second=second):
                self.assertIsNotNone(version_order(first))
                self.assertEqual(version_order(first), version_order(second))
        for tag in VECTORS["prerelease"]["yes"]:
            with self.subTest(tag=tag):
                self.assertTrue(is_prerelease(tag))
        for tag in VECTORS["prerelease"]["no"]:
            with self.subTest(tag=tag):
                self.assertFalse(is_prerelease(tag))
        with self.assertRaises(ValueError):
            is_prerelease("main")

    def test_alpha_line_sorts_around_the_new_line(self):
        # The launcher's installations start at 1.0.0-beta, and the published alpha tags fall on
        # both sides of it: being newer never tells the two lines apart.
        first_beta = version_order("1.0.0-beta")
        for tag in VECTORS["betaBoundary"]["below"]:
            with self.subTest(tag=tag):
                self.assertLess(version_order(tag), first_beta)
        for tag in VECTORS["betaBoundary"]["above"]:
            with self.subTest(tag=tag):
                self.assertGreater(version_order(tag), first_beta)

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
