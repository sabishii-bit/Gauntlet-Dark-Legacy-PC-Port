"""Clean results may be reused only for identical source and dependency inputs."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import lint_cache


class DependencyTests(unittest.TestCase):
    def test_removes_build_outputs_and_keeps_language_include_and_macro_flags(self):
        entry = {"command": 'g++ -DNAME=1 -I"my headers" -std=c++26 -MD -MT obj -MF obj.d '
                            '-o obj.o -c "src/my file.cpp"'}
        command = lint_cache.dependency_command(entry, pathlib.Path("clang++"), pathlib.Path("inputs.d"))
        self.assertEqual(command, ["clang++", "-DNAME=1", "-Imy headers", "-std=c++26",
                                  "src/my file.cpp", "-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
                                  "-E", "-MD", "-MF", "inputs.d", "-MT", "lint"])

    def test_response_files_and_incomplete_commands_are_not_cached(self):
        for args in (["g++", "@flags.rsp"], ["g++", "-o"]):
            with self.subTest(args=args), self.assertRaises(ValueError):
                lint_cache.dependency_command({"arguments": args}, pathlib.Path("clang++"),
                                              pathlib.Path("inputs.d"))

    def test_make_escaping_and_continuations(self):
        root = pathlib.Path.cwd()
        result = lint_cache.dependencies("lint: src/a.cpp \\\n folder\\ name/a.h cash$$.h hash\\#.h\n", root)
        self.assertEqual(result, sorted(root / name for name in
                                      ("src/a.cpp", "folder name/a.h", "cash$.h", "hash#.h")))

    def test_empty_or_unrecognized_output_is_not_a_hit(self):
        for output in ("", "lint:", "other: a.cpp"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                lint_cache.dependencies(output, pathlib.Path.cwd())


class CacheTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name).resolve()
        (self.root / "scripts").mkdir()
        (self.root / "src").mkdir()
        for name in ("clang-tidy", "clang++", "scripts/lint.py", "src/a.cpp", "header.h", ".clang-tidy"):
            (self.root / name).write_text(name, encoding="utf-8")
        self.unit = self.root / "src/a.cpp"
        self.entry = {"directory": str(self.root), "file": str(self.unit),
                      "arguments": ["g++", "-I.", "-c", str(self.unit)]}
        self.write_database([self.entry])
        self.dependency_text = "lint: src/a.cpp header.h\n"
        self.preprocessed = b"int main() {}"
        self.returncode = 0
        self.scan = mock.patch.object(lint_cache.subprocess, "run", side_effect=self.scan_result)
        self.run = self.scan.start()
        self.addCleanup(self.scan.stop)
        self.cache = self.create_cache()

    def scan_result(self, command, **kwargs):
        pathlib.Path(command[command.index("-MF") + 1]).write_text(self.dependency_text, encoding="utf-8")
        return subprocess.CompletedProcess(command, self.returncode, self.preprocessed, b"")

    def write_database(self, entries):
        (self.root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def create_cache(self):
        return lint_cache.CleanCache(self.root, self.root / "cache", str(self.root / "clang-tidy"))

    def test_only_explicitly_clean_results_are_hits_and_survive_new_runs(self):
        key = self.cache.key(self.unit)
        self.assertIsNotNone(key)
        self.assertFalse(self.cache.contains(key))
        self.cache.remember(key)
        later = self.create_cache()
        self.assertEqual(later.key(self.unit), key)
        self.assertTrue(later.contains(key))
        self.assertEqual(self.run.call_count, 2)  # dependencies always rescanned

    def test_source_headers_configuration_and_tooling_invalidate(self):
        key = self.cache.key(self.unit)
        for name in ("src/a.cpp", "header.h", ".clang-tidy", "scripts/lint.py", "clang-tidy", "clang++"):
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_text()
                path.write_text(original + "\n// a new NOLINT comment or tool revision", encoding="utf-8")
                self.assertNotEqual(self.create_cache().key(self.unit), key)
                path.write_text(original, encoding="utf-8")

    def test_changed_header_is_detected_within_one_run(self):
        key = self.cache.key(self.unit)
        (self.root / "header.h").write_text("new content", encoding="utf-8")
        self.assertNotEqual(self.cache.key(self.unit), key)

    def test_new_header_shadowing_an_existing_dependency_invalidates(self):
        key = self.cache.key(self.unit)
        (self.root / "src/header.h").write_text("shadow", encoding="utf-8")
        self.dependency_text = "lint: src/a.cpp src/header.h\n"
        self.assertNotEqual(self.cache.key(self.unit), key)

    def test_preprocessor_changes_without_dependency_changes_invalidate(self):
        key = self.cache.key(self.unit)
        self.preprocessed = b"int main() { return 1; }"
        self.assertNotEqual(self.cache.key(self.unit), key)

    def test_compile_flags_in_yaml_fall_back_to_full_analysis(self):
        (self.root / ".clang-tidy").write_text("ExtraArgs: [-DNEW=1]", encoding="utf-8")
        self.assertIsNone(self.cache.key(self.unit))

    def test_nested_configuration_and_compile_flags_invalidate(self):
        key = self.cache.key(self.unit)
        config = self.root / "src/.clang-tidy"
        config.write_text("Checks: '*'", encoding="utf-8")
        self.assertNotEqual(self.cache.key(self.unit), key)
        config.unlink()
        self.entry["arguments"].append("-DNEW=1")
        self.write_database([self.entry])
        self.assertNotEqual(self.create_cache().key(self.unit), key)

    def test_multiple_configs_missing_dependencies_or_scan_failures_fall_back(self):
        self.write_database([self.entry, self.entry])
        self.assertIsNone(self.create_cache().key(self.unit))
        self.returncode = 1
        self.assertIsNone(self.cache.key(self.unit))
        self.returncode = 0
        self.dependency_text = "lint: vanished.h\n"
        self.assertIsNone(self.cache.key(self.unit))
        self.run.side_effect = subprocess.TimeoutExpired("clang++", 120)
        self.assertIsNone(self.cache.key(self.unit))

    def test_partial_marker_is_not_a_hit(self):
        key = self.cache.key(self.unit)
        (self.root / "cache" / key).write_text("partial", encoding="ascii")
        self.assertFalse(self.cache.contains(key))


if __name__ == "__main__":
    unittest.main()
