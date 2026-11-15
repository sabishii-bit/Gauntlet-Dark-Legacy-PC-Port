"""Sharded lint must retain full coverage and fail on diagnostics or tool failures."""
import concurrent.futures
import contextlib
import fnmatch
import io
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import lint


def setUpModule():
    # A lint run in this process must leave the test runner's own priority alone.
    patcher = mock.patch.object(lint.process_tree, "lower_priority", return_value=True)
    patcher.start()
    unittest.addModuleCleanup(patcher.stop)


class EditorSettingsTests(unittest.TestCase):
    def test_version_is_plain_text_and_not_formatted_on_save(self):
        path = lint.ROOT / ".vscode/settings.json"
        # This file uses standalone JSONC comments, without trailing commas.
        content = "\n".join(line for line in path.read_text(encoding="utf-8").splitlines()
                            if not line.lstrip().startswith("//"))
        settings = json.loads(content)
        self.assertEqual(settings["files.associations"]["VERSION"], "plaintext")
        for action in ("Save", "Paste", "Type"):
            self.assertIs(settings["[plaintext]"][f"editor.formatOn{action}"], False)
        self.assertTrue(settings["[cpp]"]["editor.formatOnSave"])


class ConfigurationTests(unittest.TestCase):
    def test_the_configuration_leaves_the_analyzer_to_the_full_profile(self):
        text = (lint.ROOT / ".clang-tidy").read_text(encoding="utf-8")
        header, _, configuration = text.partition("\n---\n")
        checks = configuration.split("Checks:", 1)[1].split("\n\n", 1)[0]
        self.assertIn("bugprone-*", checks)
        self.assertNotIn("clang-analyzer", checks)
        self.assertIn("scripts/lint.py --profile full", header)


class JobsTests(unittest.TestCase):
    def test_ci_runners_use_every_cpu_and_other_machines_three_quarters(self):
        for environ, cpus, jobs in (({"CI": "true"}, 16, 16), ({"CI": "1"}, 16, 16),
                                    ({}, 16, 12), ({"CI": ""}, 16, 12), ({"CI": "false"}, 16, 12),
                                    ({"CI": "False"}, 16, 12), ({"CI": "0"}, 16, 12),
                                    ({"CI": "false\n"}, 16, 12), ({}, 4, 3), ({}, 1, 1),
                                    ({"CI": "true"}, 1, 1)):
            with self.subTest(environ=environ, cpus=cpus):
                self.assertEqual(lint.default_jobs(environ, cpus), jobs)


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
        with mock.patch.object(lint, "execute", return_value=result):
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


class WorkerTests(unittest.TestCase):
    SLEEPER = [sys.executable, "-c", "import time; time.sleep(30)"]

    def test_stop_ends_the_running_workers_unreported_and_refuses_new_ones(self):
        workers = lint.Workers()
        outcome = []

        def work():
            try:
                outcome.append(workers.run(self.SLEEPER))
            except InterruptedError as error:
                outcome.append(error)

        thread = threading.Thread(target=work)
        thread.start()
        deadline = time.monotonic() + 5
        while workers.running() == 0 and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertEqual(workers.running(), 1)
        workers.stop()
        thread.join(timeout=10)
        self.assertFalse(thread.is_alive())
        # A worker the stop ended has no result: its exit would read as clang-tidy failing.
        self.assertIsInstance(outcome[0], InterruptedError)
        with self.assertRaises(InterruptedError):
            workers.run(self.SLEEPER)

    def test_workers_are_tethered_to_the_lint(self):
        command = [sys.executable, "-c", "print('checked')"]
        with mock.patch.object(lint.process_tree, "start_tethered",
                               wraps=lint.process_tree.start_tethered) as start:
            result = lint.Workers().run(command)
        self.assertEqual(result.stdout.strip(), "checked")
        self.assertEqual(start.call_args.args[0], command)


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

    def test_an_interrupt_stops_the_units_still_waiting(self):
        calls = []

        def run_one(_clang_tidy, unit, _profile="full"):
            calls.append(unit)
            if len(calls) == 1:
                raise KeyboardInterrupt
            time.sleep(0.2)
            return []

        with mock.patch.object(lint, "run_one", side_effect=run_one):
            self.assertEqual(lint.main(["--jobs", "1"]), 130)
        self.assertLess(len(calls), len(self.units))

    def test_default_still_lints_every_unit(self):
        with mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--jobs", "2"]), 0)
            self.assertCountEqual([call.args[1] for call in run.call_args_list], self.units)

    def test_the_lint_lowers_its_priority_before_anything_starts(self):
        events = []
        output = io.StringIO()

        def note(event, result=None):
            return lambda *_: events.append(event) or result

        lowering = note("lowered", True)
        with mock.patch.object(lint.process_tree, "lower_priority", side_effect=lowering), \
             mock.patch.object(lint, "watch_parent", side_effect=note("watched")), \
             mock.patch.object(lint, "run_one", side_effect=note("unit", [])), \
             contextlib.redirect_stdout(output):
            self.assertEqual(lint.main(["--jobs", "1"]), 0)
        self.assertEqual(events, ["lowered", "watched", *["unit"] * len(self.units)])
        self.assertIn("1 workers at low priority", output.getvalue())

    def test_a_lint_whose_priority_stays_where_it_was_still_runs(self):
        output = io.StringIO()
        with mock.patch.object(lint.process_tree, "lower_priority", return_value=False), \
             mock.patch.object(lint, "run_one", return_value=[]) as run, \
             contextlib.redirect_stdout(output):
            self.assertEqual(lint.main([]), 0)
        self.assertEqual(run.call_count, len(self.units))
        self.assertNotIn("low priority", output.getvalue())

    def test_the_jobs_default_reaches_the_pool(self):
        for ci, workers in (("true", 8), ("", 6)):
            output = io.StringIO()
            with self.subTest(ci=ci), mock.patch.dict(lint.os.environ, {"CI": ci}), \
                 mock.patch.object(lint.os, "cpu_count", return_value=8), \
                 mock.patch.object(lint, "run_one", return_value=[]), \
                 contextlib.redirect_stdout(output):
                self.assertEqual(lint.main([]), 0)
                self.assertIn(f"{workers} workers", output.getvalue())

    def test_the_default_profile_is_fast_and_full_is_asked_for(self):
        for options, profile in (([], "fast"), (["--profile", "full"], "full")):
            with self.subTest(profile=profile), \
                 mock.patch.object(lint, "run_one", return_value=[]) as run:
                self.assertEqual(lint.main(options), 0)
                self.assertEqual({call.args[2] for call in run.call_args_list}, {profile})

    def test_shard_lints_only_its_partition(self):
        with mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--shard-index", "2", "--shard-count", "4"]), 0)
            self.assertCountEqual([call.args[1] for call in run.call_args_list], self.units[2::4])

    def test_finding_causes_failure(self):
        with mock.patch.object(lint, "run_one", return_value=["header.h:1:2: warning: bad"]):
            self.assertEqual(lint.main([]), 1)

    @staticmethod
    def printed_after(text):
        """What printed after each unit's progress line, by the unit's label."""
        blocks, current = {}, None
        for line in text.splitlines():
            progress = re.match(r"\[\d+/\d+\] (.+) \(\d+\.\ds elapsed\)$", line)
            if progress:
                current = blocks.setdefault(progress.group(1), [])
            elif current is not None:
                current.append(line)
        return blocks

    def test_findings_print_as_their_unit_finishes_and_each_once(self):
        own = "src/0.cpp:1:2: warning: own [c]"
        shared = "src/Shared.h:3:4: warning: shared [c]"
        results = {self.units[0]: [own, shared], self.units[1]: [f"{shared}\n    context"]}
        output = io.StringIO()
        with mock.patch.object(lint, "run_one",
                               side_effect=lambda _tidy, unit, _profile: results.get(unit, [])), \
             contextlib.redirect_stdout(output):
            self.assertEqual(lint.main(["--jobs", "1"]), 1)
        text = output.getvalue()
        first = self.printed_after(text)[str(self.units[0].relative_to(lint.ROOT))]
        self.assertIn(own, first)
        self.assertIn(shared, first)
        self.assertEqual(text.count("shared [c]"), 1)
        self.assertRegex(text.splitlines()[-1],
                         r"^8 translation units checked \(0 cached\), 2 findings in ")

    def test_a_stopped_lint_has_already_shown_its_findings(self):
        own = "src/0.cpp:1:2: warning: own [c]"

        def run_one(_clang_tidy, unit, _profile):
            if unit == self.units[1]:
                raise KeyboardInterrupt
            return [own] if unit == self.units[0] else []

        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(lint, "run_one", side_effect=run_one), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            self.assertEqual(lint.main(["--jobs", "1"]), 130)
        self.assertIn(own, output.getvalue())
        self.assertNotIn("translation units checked", output.getvalue())
        self.assertIn("with 1 findings", errors.getvalue())

    def test_units_the_stop_cut_short_are_never_reported(self):
        own = "src/0.cpp:1:2: warning: own [c]"
        wait = concurrent.futures.wait

        def run_one(_clang_tidy, unit, _profile):
            if unit == self.units[0]:
                return [own]
            if unit == self.units[1]:
                lint.WORKERS.stop()  # as the watcher does once the lint's caller is gone
            raise InterruptedError("the lint was stopped")  # Workers.run's, for what it ended

        output, errors = io.StringIO(), io.StringIO()
        # Every unit in one wait, so what the stop ended comes back beside a whole result.
        with mock.patch.object(lint.concurrent.futures, "wait",
                               side_effect=lambda futures, **_: wait(futures)), \
             mock.patch.object(lint, "run_one", side_effect=run_one), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            self.assertEqual(lint.main(["--jobs", "1"]), 130)
        self.assertIn(own, output.getvalue())
        self.assertNotIn("could not complete", output.getvalue())
        self.assertIn("after 1 of 8 translation units with 1 findings", errors.getvalue())

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

    def test_cache_hits_skip_only_the_verified_clean_units(self):
        cache = mock.Mock()
        cache.key.side_effect = lambda unit: str(unit)
        cache.contains.side_effect = lambda key: key == str(self.units[0])
        with mock.patch.object(lint, "CleanCache", return_value=cache), \
             mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--cache-dir", "build/lint-cache"]), 0)
        self.assertCountEqual([call.args[1] for call in run.call_args_list], self.units[1:])
        self.assertEqual(cache.remember.call_count, len(self.units) - 1)

    def test_failures_are_never_cached(self):
        cache = mock.Mock()
        cache.key.return_value = "key"
        cache.contains.return_value = False
        with mock.patch.object(lint, "CleanCache", return_value=cache), \
             mock.patch.object(lint, "run_one", return_value=["file:1:2: warning: bad"]):
            self.assertEqual(lint.main(["--cache-dir", "build/lint-cache"]), 1)
        cache.remember.assert_not_called()

    def test_unverifiable_dependencies_still_run_lint(self):
        cache = mock.Mock()
        cache.key.return_value = None
        with mock.patch.object(lint, "CleanCache", return_value=cache), \
             mock.patch.object(lint, "run_one", return_value=[]) as run:
            self.assertEqual(lint.main(["--cache-dir", "build/lint-cache"]), 0)
        self.assertEqual(run.call_count, len(self.units))
        cache.remember.assert_not_called()

    def test_changed_inputs_during_analysis_are_not_cached(self):
        cache = mock.Mock()
        cache.key.side_effect = ["before", "after"]
        cache.contains.return_value = False
        with mock.patch.object(lint, "translation_units", return_value=self.units[:1]), \
             mock.patch.object(lint, "CleanCache", return_value=cache), \
             mock.patch.object(lint, "run_one", return_value=[]):
            self.assertEqual(lint.main(["--cache-dir", "build/lint-cache"]), 0)
        cache.remember.assert_not_called()


class ChangedTests(unittest.TestCase):
    """--changed lints the changed units and one unit per changed header, or with
    --all-includers every unit reached through a changed header."""

    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(directory.cleanup)
        cls.root = pathlib.Path(directory.name).resolve()
        cls.files = {
            "src/engine/core/Types.h": "",
            "src/engine/core/Clock.h": "",
            "src/engine/world/Scene.h": '#include "engine/core/Types.h"\n',
            "src/game/world/Level.h": '#include "engine/world/Scene.h"\n',
            "src/engine/world/Scene.cpp":
                '#include "engine/world/Scene.h"\n#include "engine/core/Clock.h"\n',
            "src/game/world/Level.cpp":
                '#include "game/world/Level.h"\n#include "engine/core/Clock.h"\n',
            "src/game/app/App.cpp": '#include "engine/core/Log.h"\n',
            "tests/engine/core/TypesTests.cpp": '#include "engine/core/Types.h"\n',
            # Named after Clock.h, but never includes it.
            "tests/engine/core/ClockTests.cpp": '#include "engine/core/Log.h"\n',
            "tests/game/world/LevelTests.cpp": '#include "game/world/Level.h"\n',
            # A test fixture header, also included by a test that sorts before its own.
            "tests/game/world/Arena.h": "",
            "tests/game/audio/EchoTests.cpp": '#include "game/world/Arena.h"\n',
            "tests/game/world/ArenaTests.cpp": '#include "game/world/Arena.h"\n',
            # A test support header with both a .cpp of its own and a test.
            "tests/fixtures/Recorder.h": "",
            "tests/fixtures/Recorder.cpp": '#include "fixtures/Recorder.h"\n',
            "tests/fixtures/RecorderTests.cpp": '#include "fixtures/Recorder.h"\n',
            # A header that nothing includes, and a unit outside the roster, which lints the
            # units under src and tests.
            "src/engine/core/Unused.h": "",
            "tools/dump/Reader.cpp": '#include "engine/core/Types.h"\n',
        }
        for name, text in cls.files.items():
            path = cls.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        cls.roster = sorted(cls.root / name for name in cls.files
                            if name.endswith(".cpp") and name.startswith(("src/", "tests/")))

    def setUp(self):
        patcher = mock.patch.object(lint, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)

    def names(self, units):
        return [unit.relative_to(self.root).as_posix() for unit in units]

    def affected(self, *changed):
        return self.names(lint.affected_units(self.roster, [self.root / c for c in changed],
                                              lint.header_roster()))

    def representatives(self, *changed):
        return self.names(lint.representative_units(self.roster, [self.root / c for c in changed],
                                                    lint.header_roster()))

    def linted(self, *options, changed="src/game/world/Level.h"):
        with mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"), \
             mock.patch.object(lint, "translation_units", return_value=self.roster), \
             mock.patch.object(lint, "changed_files", return_value=[self.root / changed]), \
             mock.patch.object(lint, "run_one", return_value=[]) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lint.main(["--changed", "a..b", *options]), 0)
        return self.names(call.args[1] for call in run.call_args_list)

    def test_all_includers_are_the_changed_sources_and_every_unit_reaching_a_changed_header(self):
        self.assertEqual(self.affected("src/game/app/App.cpp"), ["src/game/app/App.cpp"])
        self.assertEqual(self.affected("src/game/world/Level.h"),
                         ["src/game/world/Level.cpp", "tests/game/world/LevelTests.cpp"])
        # A header reached through two other headers still counts.
        self.assertEqual(self.affected("src/engine/core/Types.h"),
                         ["src/engine/world/Scene.cpp", "src/game/world/Level.cpp",
                          "tests/engine/core/TypesTests.cpp", "tests/game/world/LevelTests.cpp"])
        self.assertEqual(self.affected("docs/notes.md"), [])
        self.assertEqual(self.affected("src/engine/core/Unused.h", "tools/dump/Reader.cpp"), [])

    def test_one_unit_exercises_each_changed_header(self):
        # The header's own .cpp first, under src or tests.
        self.assertEqual(self.representatives("src/engine/world/Scene.h"),
                         ["src/engine/world/Scene.cpp"])
        self.assertEqual(self.representatives("src/game/world/Level.h"),
                         ["src/game/world/Level.cpp"])
        self.assertEqual(self.representatives("tests/fixtures/Recorder.h"),
                         ["tests/fixtures/Recorder.cpp"])
        # Then its mirrored test, under tests for a src header and beside one under tests,
        # though other includers sort before it.
        self.assertEqual(self.representatives("src/engine/core/Types.h"),
                         ["tests/engine/core/TypesTests.cpp"])
        self.assertEqual(self.representatives("tests/game/world/Arena.h"),
                         ["tests/game/world/ArenaTests.cpp"])
        # A test named after the header that does not include it is passed over for the
        # first unit that does.
        self.assertEqual(self.representatives("src/engine/core/Clock.h"),
                         ["src/engine/world/Scene.cpp"])
        # Changed units are always their own representatives; documents never are.
        self.assertEqual(self.representatives("src/game/app/App.cpp", "src/game/world/Level.h",
                                              "docs/a.md"),
                         ["src/game/app/App.cpp", "src/game/world/Level.cpp"])
        self.assertEqual(self.representatives("src/unused/Nothing.h"), [])
        # A header that nothing includes has no unit, and a unit outside the roster is not one.
        self.assertEqual(self.representatives("src/engine/core/Unused.h"), [])
        self.assertEqual(self.representatives("tools/dump/Reader.cpp"), [])

    def test_an_own_source_outside_the_database_gives_way_to_the_mirrored_test(self):
        header = self.root / "src/game/net/Room.h"
        mirror = self.root / "tests/game/net/RoomTests.cpp"
        self.assertEqual(lint.mirrored_test(header), mirror)
        self.assertEqual(lint.representative(header, [self.root / "src/game/net/Lobby.cpp", mirror]),
                         mirror)
        self.assertEqual(lint.mirrored_test(self.root / "tests/Fake.h"), self.root / "tests/FakeTests.cpp")
        self.assertIsNone(lint.mirrored_test(self.root / "tools/dump/Reader.h"))

    def test_changed_lints_one_unit_per_header_unless_every_includer_is_asked_for(self):
        self.assertEqual(self.linted(), ["src/game/world/Level.cpp"])
        # The flag the hook and older callers pass names the default.
        self.assertEqual(self.linted("--quick"), ["src/game/world/Level.cpp"])
        self.assertEqual(self.linted("--all-includers"),
                         ["src/game/world/Level.cpp", "tests/game/world/LevelTests.cpp"])
        for options in (["--quick"], ["--all-includers"],
                        ["--changed", "a..b", "--quick", "--all-includers"]):
            with self.subTest(options=options), self.assertRaises(SystemExit), \
                 contextlib.redirect_stderr(io.StringIO()):
                lint.main(options)

    def test_the_selection_and_the_fast_profile_reach_clang_tidy(self):
        with mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"), \
             mock.patch.object(lint, "translation_units", return_value=self.roster), \
             mock.patch.object(lint, "changed_files", return_value=[self.root / "src/game/world/Level.h"]), \
             mock.patch.object(lint, "run_one", return_value=[]) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lint.main(["--changed", "a..b", "--quick", "--profile", "fast"]), 0)
        self.assertEqual([call.args[1:] for call in run.call_args_list],
                         [(self.root / "src/game/world/Level.cpp", "fast")])

    def command_for(self, *profile):
        completed = subprocess.CompletedProcess([], 0, "", "")
        with mock.patch.object(lint, "execute", return_value=completed) as run:
            lint.run_one("clang-tidy", self.root / "src/game/app/App.cpp", *profile)
        return run.call_args.args[0]

    @staticmethod
    def enabled(command, check):
        """What this command's --checks globs make of `check`, as clang-tidy reads them after
        .clang-tidy's: in order, the last glob that matches decides; None leaves it to the
        configuration."""
        verdict = None
        for argument in command:
            if argument.startswith("--checks="):
                for glob in argument[len("--checks="):].split(","):
                    if fnmatch.fnmatchcase(check, glob.lstrip("-")):
                        verdict = not glob.startswith("-")
        return verdict

    def test_fast_adds_nothing_to_the_configured_checks(self):
        for command in (self.command_for(), self.command_for("fast")):
            self.assertEqual([arg for arg in command if arg.startswith("--checks")], [])
            # The analyzer's own configuration goes only where the analyzer runs.
            self.assertFalse([arg for arg in command if "analyzer" in arg])

    def test_full_adds_the_analyzer_that_can_fire_here(self):
        command = self.command_for("full")
        for check in ("clang-analyzer-core.NullDereference", "clang-analyzer-cplusplus.Move",
                      "clang-analyzer-optin.performance.Padding",
                      "clang-analyzer-deadcode.DeadStores"):
            self.assertIs(self.enabled(command, check), True, check)
        for check in ("clang-analyzer-optin.core.EnumCastOutOfRange",
                      "clang-analyzer-osx.cocoa.RetainCount", "clang-analyzer-osx.API",
                      "clang-analyzer-optin.osx.cocoa.localizability.NonLocalizedStringChecker",
                      "clang-analyzer-webkit.NoUncountedMemberChecker",
                      "clang-analyzer-fuchsia.HandleChecker",
                      "clang-analyzer-optin.mpi.MPI-Checker",
                      "clang-analyzer-optin.performance.GCDAntipattern",
                      "clang-analyzer-nullability.NullPassedToNonnull"):
            self.assertIs(self.enabled(command, check), False, check)
        # Everything else stays as .clang-tidy has it.
        self.assertIsNone(self.enabled(command, "readability-braces-around-statements"))
        # The LLVM 18 analyzer stays out of standard-library bodies, where it crashes.
        self.assertIn("--extra-arg=c++-stdlib-inlining=false", command)

    def test_include_names_drop_the_source_roots(self):
        self.assertEqual(lint.include_name(self.root / "src/engine/core/Types.h"), "engine/core/Types.h")
        self.assertEqual(lint.include_name(self.root / "tests/Fake.h"), "Fake.h")

    def test_changed_files_come_from_git_and_keep_only_sources(self):
        completed = subprocess.CompletedProcess([], 0, "src/a.cpp\nREADME.md\nsrc/b.h\n", "")
        with mock.patch.object(lint.subprocess, "run", return_value=completed) as run:
            self.assertEqual(lint.changed_files("origin/main"),
                             [self.root / "src/a.cpp", self.root / "src/b.h"])
        self.assertIn("origin/main", run.call_args.args[0])
        failed = subprocess.CompletedProcess([], 128, "", "bad revision")
        with mock.patch.object(lint.subprocess, "run", return_value=failed), \
             self.assertRaises(SystemExit):
            lint.changed_files("nope")

    def test_nothing_affected_passes_without_running_clang_tidy(self):
        with mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"), \
             mock.patch.object(lint, "translation_units", return_value=self.roster), \
             mock.patch.object(lint, "changed_files", return_value=[self.root / "README.md"]), \
             mock.patch.object(lint, "run_one") as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lint.main(["--changed", "origin/main"]), 0)
        run.assert_not_called()

    def test_changed_mode_lints_only_the_affected_units(self):
        with mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"), \
             mock.patch.object(lint, "translation_units", return_value=self.roster), \
             mock.patch.object(lint, "changed_files", return_value=[self.root / "src/game/world/Level.h"]), \
             mock.patch.object(lint, "run_one", return_value=["Level.h:1:1: warning: x [c]"]) as run, \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lint.main(["--changed", "a..b", "--all-includers"]), 1)
        self.assertCountEqual([call.args[1] for call in run.call_args_list],
                              [self.root / "src/game/world/Level.cpp",
                               self.root / "tests/game/world/LevelTests.cpp"])


if __name__ == "__main__":
    unittest.main()
