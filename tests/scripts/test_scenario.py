"""Scenario shortcuts must be unambiguous and preserve the game's arguments and exit status."""
import contextlib
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import scenario


class ScenarioTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl scenario ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        directory = self.root / "tests" / "scenarios"
        directory.mkdir(parents=True)
        for name in ("level-c5-genie", "level-b6-dragon", "tower-turbo", "level-g1-turbo", "tower"):
            (directory / f"{name}.json").write_text("{}", encoding="utf-8")
        self.start_patch(mock.patch.object(scenario, "ROOT", self.root))
        self.start_patch(mock.patch.object(scenario.devenv, "release_preset", return_value="release"))
        self.start_patch(mock.patch.object(scenario.build, "build_presets",
                                          return_value={"release": "release", "debug": "debug"}))

    def start_patch(self, patch):
        self.addCleanup(patch.stop)
        return patch.start()

    def executable(self, preset="release"):
        executable = self.root / "build" / preset / "bin" / f"gauntlet{scenario.build.EXE}"
        executable.parent.mkdir(parents=True, exist_ok=True)
        executable.touch()
        return executable

    def run_main(self, arguments, code=0):
        with mock.patch.object(scenario.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], code)) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            result = scenario.main(arguments)
        return result, run

    def test_unique_suffix_and_full_names_ignore_case(self):
        expected = self.root / "tests/scenarios/level-c5-genie.json"
        for name in ("genie", "GENIE", "level-c5-genie", "level-c5-genie.json", "GENIE.JSON"):
            self.assertEqual(scenario.resolve_scenario(name), expected)

    def test_exact_name_wins_over_suffix(self):
        (self.root / "tests/scenarios/genie.json").write_text("{}", encoding="utf-8")
        self.assertEqual(scenario.resolve_scenario("genie").name, "genie.json")

    def test_ambiguous_and_missing_names_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Ambiguous.*level-g1-turbo.*tower-turbo"):
            scenario.resolve_scenario("turbo")
        with self.assertRaisesRegex(ValueError, "Unknown scenario.*--list"):
            scenario.resolve_scenario("missing")
        with self.assertRaisesRegex(ValueError, "file not found"):
            scenario.resolve_scenario(str(self.root / "missing.json"))

    def test_explicit_custom_file_with_spaces(self):
        path = self.root / "custom scenario.json"
        path.write_text("{}", encoding="utf-8")
        self.assertEqual(scenario.resolve_scenario(str(path)), path)

    def test_list_and_no_arguments_do_not_launch_or_require_build(self):
        for args in ([], ["--list"]):
            output = io.StringIO()
            with mock.patch.object(scenario.subprocess, "run") as run, \
                    contextlib.redirect_stdout(output):
                self.assertEqual(scenario.main(args), 0)
            run.assert_not_called()
            self.assertIn("level-c5-genie", output.getvalue())

    def test_launch_is_root_relative_preserves_arguments_and_returns_exit_code(self):
        executable = self.executable()
        result, run = self.run_main(["genie", "--frames", "12", "--", "--no-vsync",
                                     "--data", "a path with spaces"], code=7)
        self.assertEqual(result, 7)
        run.assert_called_once_with(
            [str(executable), "--scenario", str(self.root / "tests/scenarios/level-c5-genie.json"),
             "--frames", "12", "--no-vsync", "--data", "a path with spaces"],
            cwd=self.root, check=False)

    def test_explicit_preset_selects_its_executable(self):
        executable = self.executable("debug")
        result, run = self.run_main(["genie", "--preset", "debug"])
        self.assertEqual(result, 0)
        self.assertEqual(run.call_args.args[0][0], str(executable))

    def test_build_uses_existing_build_launcher_even_without_binary(self):
        result, run = self.run_main(["genie", "--build", "--frames", "120"], code=3)
        self.assertEqual(result, 3)
        run.assert_called_once_with(
            [sys.executable, str(self.root / "scripts/build.py"), "release", "--run", "--",
             "--scenario", str(self.root / "tests/scenarios/level-c5-genie.json"),
             "--frames", "120"], cwd=self.root, check=False)

    def test_invalid_inputs_never_launch(self):
        for args in (["genie"], ["genie", "--preset", "missing"],
                     ["turbo"], ["missing"], ["genie", "--frames", "0"],
                     ["genie", "--frames", "no"]):
            with self.subTest(args=args), mock.patch.object(scenario.subprocess, "run") as run, \
                    contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                scenario.main(args)
            self.assertEqual(error.exception.code, 2)
            run.assert_not_called()

    def test_launch_error_is_reported_without_traceback(self):
        self.executable()
        with mock.patch.object(scenario.subprocess, "run", side_effect=OSError("cannot launch")), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                self.assertRaises(SystemExit) as error:
            scenario.main(["genie"])
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
