"""The [serial] split of the Catch2 discovery filter, evaluated without configuring the game."""
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake/GdlTestDiscovery.cmake"


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for script-only contract checks")
class TestDiscoverySpecsTests(unittest.TestCase):
    def run_specs(self, value):
        parent = ROOT / "build"
        with tempfile.TemporaryDirectory(prefix="test-discovery-",
                                         dir=parent if parent.is_dir() else None) as temporary:
            # CMake strips whitespace from -D arguments; the filter is read from a file as given.
            (pathlib.Path(temporary) / "filter").write_text(value, encoding="utf-8")
            script = pathlib.Path(temporary) / "specs.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.30)\n'
                f'include("{HELPER.as_posix()}")\n'
                'file(READ "${CMAKE_CURRENT_LIST_DIR}/filter" filter)\n'
                'gdl_test_discovery_specs("${filter}" parallel serial)\n'
                'message("PARALLEL=<${parallel}>")\n'
                'message("SERIAL=<${serial}>")\n',
                encoding="utf-8")
            return subprocess.run([shutil.which("cmake"), "-P", str(script)], cwd=temporary,
                                  capture_output=True, text=True, check=False)

    def assert_specs(self, value, parallel, serial):
        result = self.run_specs(value)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"PARALLEL=<{parallel}>", result.stderr)
        self.assertIn(f"SERIAL=<{serial}>", result.stderr)

    def test_tag_patterns_split_into_parallel_and_serial_specs(self):
        # A filter of exclusions lists no hidden case, so neither may its serial spec.
        for value, parallel, serial in (
                ("~[gpu]", "~[gpu]~[serial]", "~[gpu][serial]~[.]"),
                ("~[gpu]~[assets]~[unpacked]", "~[gpu]~[assets]~[unpacked]~[serial]",
                 "~[gpu]~[assets]~[unpacked][serial]~[.]"),
                ("", "~[serial]", "[serial]~[.]"),
                (" ~[gpu] ", " ~[gpu] ~[serial]", " ~[gpu] [serial]~[.]"),
                # A required tag lists its hidden cases, so the serial spec keeps them.
                ("[netplay]~[gpu]", "[netplay]~[gpu]~[serial]", "[netplay]~[gpu][serial]"),
                ("~[gpu] [audio]", "~[gpu] [audio]~[serial]", "~[gpu] [audio][serial]"),
                ("[.]", "[.]~[serial]", "[.][serial]"),
                ("[.long]", "[.long]~[serial]", "[.long][serial]")):
            with self.subTest(value=value):
                self.assert_specs(value, parallel, serial)

    def test_names_commas_and_stray_characters_are_rejected(self):
        for value in ("[a],[b]", "~[gpu],[assets]", "*javelin*", "~*javelin*", "[gpu]name",
                      "name[gpu]", '"quoted name"', "exclude:[gpu]", "[gpu", "[]", "~", "[gpu]~",
                      "~ [gpu]", "~~[gpu]", "[gpu;audio]", "[gpu]\\", "\t[gpu]"):
            with self.subTest(value=value):
                result = self.run_specs(value)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                # CMake wraps diagnostics to its output width, independently of semantics.
                self.assertIn("GDL_TEST_DISCOVERY_FILTER takes only [tag] and ~[tag] patterns",
                              " ".join(result.stderr.split()), result.stderr)

    def test_the_configured_filters_are_accepted(self):
        tests = (ROOT / "tests/CMakeLists.txt").read_text(encoding="utf-8")
        release = (ROOT / "scripts/release/release.py").read_text(encoding="utf-8")
        default = re.search(r'set\(GDL_TEST_DISCOVERY_FILTER "([^"]*)" CACHE', tests)
        public = re.search(r'-DGDL_TEST_DISCOVERY_FILTER=([^"]*)"', release)
        self.assertIsNotNone(default)
        self.assertIsNotNone(public)
        for value in (default.group(1), public.group(1)):
            with self.subTest(value=value):
                result = self.run_specs(value)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"PARALLEL=<{value}~[serial]>", result.stderr)


if __name__ == "__main__":
    unittest.main()
