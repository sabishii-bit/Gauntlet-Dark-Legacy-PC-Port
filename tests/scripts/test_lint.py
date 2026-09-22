"""Sharded lint must retain full coverage and fail on diagnostics or tool failures."""
import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import lint


class ShardTests(unittest.TestCase):
    def test_every_unit_belongs_to_exactly_one_shard(self):
        for size in (1, 3, 4, 5, 280, 281):
            for count in (1, 2, 4, 8):
                with self.subTest(size=size, count=count):
                    units = [pathlib.Path(f"src/{i:03}.cpp") for i in range(size)]
                    shards = [lint.select_shard(units, i, count) for i in range(count)]
                    combined = [unit for shard in shards for unit in shard]
                    self.assertCountEqual(combined, units)
                    self.assertEqual(len(set(combined)), len(units))
                    self.assertLessEqual(max(map(len, shards)) - min(map(len, shards)), 1)

    def test_invalid_shards_are_rejected(self):
        for index, count in ((0, 0), (0, -1), (-1, 4), (4, 4)):
            with self.subTest(index=index, count=count), self.assertRaises(ValueError):
                lint.select_shard([], index, count)

    def test_roster_is_sorted_unique_and_resolves_relative_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            entries = [
                {"directory": str(root), "file": "src/z.cpp"},
                {"directory": str(root), "file": "tests/a.cpp"},
                {"directory": str(root), "file": str(root / "src/z.cpp")},
                {"directory": str(root), "file": "tools/tool.cpp"},
            ]
            (root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
            with mock.patch.object(lint, "ROOT", root):
                self.assertEqual(lint.translation_units([]),
                                 [root / "src/z.cpp", root / "tests/a.cpp"])
                self.assertEqual(lint.translation_units(["src"]), [root / "src/z.cpp"])
                self.assertEqual(lint.translation_units(["src/z.cpp"]), [root / "src/z.cpp"])
                self.assertEqual(lint.translation_units(["src/missing.cpp"]), [])

    def test_missing_database_is_an_error(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(lint, "ROOT", pathlib.Path(directory)), \
             self.assertRaises(SystemExit):
            lint.translation_units([])


class ToolResultTests(unittest.TestCase):
    def run_result(self, code=0, stdout="", stderr=""):
        result = subprocess.CompletedProcess([], code, stdout, stderr)
        with mock.patch.object(lint.subprocess, "run", return_value=result):
            return lint.run_one("clang-tidy", lint.ROOT / "src/test.cpp")

    def test_clean_run(self):
        self.assertEqual(self.run_result(stderr="100 warnings suppressed.\n"), [])

    def test_diagnostics_on_either_stream_fail_even_with_zero_exit(self):
        diagnostic = "src/test.cpp:1:2: warning: unsafe [test-check]"
        for stream in ("stdout", "stderr"):
            with self.subTest(stream=stream):
                self.assertEqual(self.run_result(**{stream: diagnostic}), [diagnostic])

    def test_nonzero_exit_without_parsed_diagnostic_is_not_success(self):
        for code in (1, 2, -11):
            with self.subTest(code=code):
                findings = self.run_result(code, stdout="failure detail", stderr="fatal failure")
                self.assertEqual(len(findings), 1)
                self.assertIn(f"exit {code}", findings[0])
                self.assertIn("failure detail", findings[0])
                self.assertIn("fatal failure", findings[0])


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.units = [lint.ROOT / "src" / f"{i}.cpp" for i in range(8)]
        self.contexts = contextlib.ExitStack()
        self.addCleanup(self.contexts.close)
        for patcher in (
            mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"),
            mock.patch.object(lint, "translation_units", return_value=self.units),
            contextlib.redirect_stdout(io.StringIO()),
            contextlib.redirect_stderr(io.StringIO()),
        ):
            self.contexts.enter_context(patcher)

    def test_default_still_lints_every_unit(self):
        with mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--jobs", "2"]), 0)
            self.assertCountEqual([call.args[1] for call in run.call_args_list], self.units)

    def test_shard_lints_only_its_partition(self):
        with mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--shard-index", "2", "--shard-count", "4"]), 0)
            self.assertCountEqual([call.args[1] for call in run.call_args_list], self.units[2::4])

    def test_finding_causes_failure(self):
        with mock.patch.object(lint, "run_one", return_value=["header.h:1:2: warning: bad"]):
            self.assertEqual(lint.main([]), 1)

    def test_launch_failure_causes_failure(self):
        with mock.patch.object(lint, "run_one", side_effect=OSError("cannot launch")):
            self.assertEqual(lint.main([]), 1)

    def test_empty_roster_cannot_pass(self):
        with mock.patch.object(lint, "translation_units", return_value=[]), \
             self.assertRaises(SystemExit) as error:
            lint.main([])
        self.assertEqual(error.exception.code, 2)

    def test_invalid_cli_options_cannot_pass(self):
        for options in (["--jobs", "0"], ["--shard-count", "0"], ["--shard-index", "-1"],
                        ["--shard-index", "4", "--shard-count", "4"]):
            with self.subTest(options=options), self.assertRaises(SystemExit) as error:
                lint.main(options)
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
