import json
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
from io import StringIO
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts" / "netplay"))
from native_determinism import (MAX_LINE_BYTES, MAX_RENDER_EVERY, MAX_TICKS, METADATA_FIELDS,
                                ReportError, compare_reports, main, run_pair, validate_transport)


class TransportEvidenceTests(unittest.TestCase):
    def evidence(self):
        return {"local_peer": 1, "links": [
            {"peer": peer, "samples": 5, "relay_samples": 4, "direct_samples": 0,
             "unavailable_samples": 1, "rtt_p50_ms": 30.0, "rtt_p95_ms": 40.0,
             "rtt_max_ms": 44.0} for peer in (2, 3, 4)]}

    def test_selected_relay_samples_cover_every_link_and_unknown_is_not_proof(self):
        validate_transport(self.evidence(), "relay", 4)
        invalid = (
            {"relay_samples": 0, "unavailable_samples": 5, "rtt_p50_ms": 0,
             "rtt_p95_ms": 0, "rtt_max_ms": 0},
            {"relay_samples": 3, "direct_samples": 1},
            {"samples": 0}, {"samples": True}, {"relay_samples": -1},
            {"peer": 2}, {"peer": 1}, {"peer": 5}, {"extra": 1},
            {"rtt_p50_ms": 45}, {"rtt_p95_ms": float("nan")},
            {"rtt_max_ms": float("inf")}, {"rtt_max_ms": True},
        )
        for change in invalid:
            with self.subTest(change=change):
                value = self.evidence()
                value["links"][1].update(change)
                with self.assertRaises(ReportError):
                    validate_transport(value, "relay", 4)
        value = self.evidence()
        value["links"].pop()
        with self.assertRaises(ReportError):
            validate_transport(value, "relay", 4)

    def test_rtt_is_local_measurement_and_local_only_must_not_report_relay(self):
        value = self.evidence()
        with self.assertRaises(ReportError):
            validate_transport(value, "local", 4)
        for link in value["links"]:
            link.update(relay_samples=0, direct_samples=4,
                        rtt_p50_ms=0, rtt_p95_ms=1, rtt_max_ms=3)
        validate_transport(value, "local", 4)
        value = {"local_peer": 4, "links": [deepcopy(value["links"][0])]}
        value["links"][0]["peer"] = 1
        validate_transport(value, "local", 4)


def observations(ticks=3, cadence=0):
    metadata = {
        "type": "metadata", "schema": 1, "simulation": "PlayScene::update",
        "stage": "G1", "ticks": ticks, "players": 2, "tick_rate": 60,
        "render_every": cadence, "workload": "idle", "input_version": 1,
        "input_delivery": "local", "input_delay": 0,
        "initialization": "native-defaults-v1", "complete_state": False,
        "assets_digest": "a" * 64, "data_digest": "d" * 64, "config_digest": "b" * 64,
        "unobserved": ["audio", "hud"], "subsystems": ["party", "world"],
    }
    rows = [metadata]
    for tick in range(1, ticks + 1):
        rows.append({"type": "tick", "tick": tick, "input_digest": f"{tick:064x}",
                     "subsystems": {"party": f"{tick + 10:064x}", "world": "0" * 64},
                     "unavailable": {}})
    rows.append({"type": "complete", "ticks": ticks, "last_tick": ticks})
    return rows


def write_rows(path, rows):
    path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


class NativeReportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-determinism-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()  # as the scripts do
        self.left, self.right = self.directory / "left.jsonl", self.directory / "right.jsonl"
        write_rows(self.left, observations())
        write_rows(self.right, observations(cadence=1))

    def compare(self):
        return compare_reports(self.left, self.right)

    def test_distinct_render_cadences_compare_only_observed_state(self):
        for first, second in ((0, 1), (1, 2), (2, 0), (1, 1)):
            with self.subTest(first=first, second=second):
                write_rows(self.left, observations(cadence=first))
                write_rows(self.right, observations(cadence=second))
                result = self.compare()
                self.assertEqual(result["status"], "observed-equal")
                self.assertIs(result["complete_state"], False)
                self.assertEqual(result["render_every"], [first, second])
                self.assertEqual(result["metadata"]["unobserved"], ["audio", "hud"])
                self.assertEqual(result["metadata"]["ticks"], 3)

    def test_reports_must_be_distinct_and_nonempty(self):
        with self.assertRaisesRegex(ReportError, "distinct"):
            compare_reports(self.left, self.left)
        self.right.write_bytes(b"")
        with self.assertRaisesRegex(ReportError, "empty or truncated"):
            self.compare()
        self.right.unlink()
        with self.assertRaises(OSError):
            self.compare()

    def test_network_reports_require_two_to_four_independent_player_processes(self):
        for players in (1, 2, 3, 4, 5):
            with self.subTest(players=players):
                for path, cadence in ((self.left, 0), (self.right, 1)):
                    rows = observations(cadence=cadence)
                    rows[0].update(players=players, input_delivery="iroh-lockstep", input_delay=3)
                    write_rows(path, rows)
                if 2 <= players <= 4:
                    self.assertEqual(self.compare()["metadata"]["players"], players)
                else:
                    with self.assertRaises(ReportError):
                        self.compare()

    def test_every_metadata_field_is_required(self):
        for field in METADATA_FIELDS:
            with self.subTest(field=field):
                rows = observations()
                del rows[0][field]
                write_rows(self.right, rows)
                with self.assertRaises(ReportError):
                    self.compare()

    def test_metadata_identity_is_not_replaced_by_matching_hashes(self):
        for field, value in (("stage", "A1"), ("players", 3), ("workload", "combat"),
                             ("assets_digest", "c" * 64), ("config_digest", "c" * 64),
                             ("data_digest", "c" * 64),
                             ("unobserved", ["different"]), ("ticks", 4)):
            with self.subTest(field=field):
                rows = observations()
                rows[0][field] = value
                write_rows(self.right, rows)
                with self.assertRaisesRegex(ReportError, f"Metadata differs: {field}"):
                    self.compare()

    def test_invalid_metadata_does_not_pass_when_both_reports_agree(self):
        mutations = {
            "schema": (True, 0, 2, "1"), "input_version": (True, 2),
            "tick_rate": (True, 30, "60"), "ticks": (False, 0, -1, MAX_TICKS + 1),
            "players": (True, 0, 5), "render_every": (True, -1, MAX_RENDER_EVERY + 1),
            "simulation": (None, "Replay::update"), "initialization": ("", "unknown"),
            "complete_state": (True, 0, None), "stage": (None, "", "G1\x00"),
            "workload": (None, "unknown"), "unobserved": (None, ["audio", "audio"], [1]),
            "subsystems": (None, [], ["world", "party"], ["party", "party"], [False]),
            "assets_digest": (None, "a" * 63, "A" * 64, "g" * 64),
            "data_digest": (None, "d" * 63, "D" * 64, "g" * 64),
            "config_digest": (False, "", "b" * 65),
            "input_delivery": (None, "unknown"), "input_delay": (True, -1, 7, 1),
        }
        for field, values in mutations.items():
            for value in values:
                with self.subTest(field=field, value=value):
                    rows = observations()
                    rows[0][field] = value
                    write_rows(self.left, rows)
                    write_rows(self.right, rows)
                    with self.assertRaises(ReportError):
                        self.compare()

    def test_unobserved_and_observed_rosters_cannot_overlap(self):
        rows = observations()
        rows[0]["unobserved"] = ["party"]
        write_rows(self.right, rows)
        with self.assertRaisesRegex(ReportError, "overlap"):
            self.compare()

    def test_native_observer_names_and_long_coverage_descriptions_are_supported(self):
        names = ["opponent_generators", "opponent_missiles", "opponent_placements",
                 "opponent_swarm", "party_combat", "scene_control", "stage"]
        description = ("Application travel/shop/secret-parent ownership, persistence/unlock "
                       "callbacks, config and input-source state outside the sampled scene scalars")
        self.assertGreater(len(description), 128)
        for path, cadence in ((self.left, 0), (self.right, 60)):
            rows = observations(cadence=cadence)
            rows[0]["subsystems"] = names
            rows[0]["unobserved"] = [description, "x" * 1024]
            for row in rows[1:-1]:
                row["subsystems"] = {name: f"{index:064x}" for index, name in enumerate(names)}
            write_rows(path, rows)
        result = self.compare()
        self.assertEqual(result["metadata"]["subsystems"], names)
        self.assertEqual(result["metadata"]["unobserved"][0], description)
        for description in ("x" * 1025, "\n", " ", "omitted\x00owner"):
            rows[0]["unobserved"] = [description]
            write_rows(self.right, rows)
            with self.assertRaises(ReportError):
                self.compare()

    def test_requested_args_are_verified_against_each_process(self):
        with self.assertRaisesRegex(ReportError, "Requested render_every"):
            compare_reports(self.left, self.right, {"render_every": 0}, {"render_every": 2})
        with self.assertRaisesRegex(ReportError, "Requested players"):
            compare_reports(self.left, self.right, {"players": 3})
        self.assertEqual(compare_reports(self.left, self.right,
                                        {"ticks": 3}, {"render_every": 1})["status"],
                         "observed-equal")

    def test_first_differing_tick_and_subsystem_are_stable(self):
        rows = observations(cadence=1)
        rows[2]["subsystems"]["world"] = "f" * 64
        rows[3]["subsystems"]["party"] = "e" * 64
        write_rows(self.right, rows)
        with self.assertRaisesRegex(ReportError, "First difference at tick 2, subsystem world"):
            self.compare()
        rows[2]["input_digest"] = "d" * 64
        write_rows(self.right, rows)
        with self.assertRaisesRegex(ReportError, "First difference at tick 2, input_digest"):
            self.compare()

    def test_input_digest_named_subsystem_is_still_compared(self):
        for path in (self.left, self.right):
            rows = observations()
            rows[0]["subsystems"] = ["input_digest", "party", "world"]
            for row in rows[1:-1]:
                row["subsystems"]["input_digest"] = "1" * 64
            if path == self.right:
                rows[2]["subsystems"]["input_digest"] = "2" * 64
            write_rows(path, rows)
        with self.assertRaisesRegex(ReportError, "tick 2, subsystem input_digest"):
            self.compare()

    def test_missing_duplicate_reordered_extra_and_zero_ticks_fail(self):
        for numbers in ((1, 3), (1, 1, 3), (2, 1, 3), (1, 2, 3, 4), (0, 1, 2)):
            with self.subTest(numbers=numbers):
                rows = observations()
                ticks = []
                for number in numbers:
                    row = deepcopy(rows[1])
                    row["tick"] = number
                    ticks.append(row)
                write_rows(self.right, [rows[0], *ticks, rows[-1]])
                with self.assertRaises(ReportError):
                    self.compare()

    def test_missing_malformed_or_unknown_tick_fields_fail(self):
        for field in ("type", "tick", "input_digest", "subsystems", "unavailable"):
            with self.subTest(field=field):
                rows = observations()
                del rows[1][field]
                write_rows(self.right, rows)
                with self.assertRaises(ReportError):
                    self.compare()
        for field, value in (("tick", True), ("input_digest", "a" * 63),
                             ("subsystems", None), ("unavailable", []), ("extra", 1)):
            with self.subTest(field=field, value=value):
                rows = observations()
                rows[1][field] = value
                write_rows(self.right, rows)
                with self.assertRaises(ReportError):
                    self.compare()

    def test_hash_roster_is_complete_for_every_tick(self):
        for hashes in ({}, {"party": "0" * 64}, {"party": "z" * 64, "world": "0" * 64},
                       {"party": None, "world": "0" * 64},
                       {"party": "0" * 64, "world": "0" * 64, "extra": "0" * 64}):
            with self.subTest(hashes=hashes):
                rows = observations()
                rows[2]["subsystems"] = hashes
                write_rows(self.right, rows)
                with self.assertRaisesRegex(ReportError, "tick 2"):
                    self.compare()

    def test_matching_unavailable_subsystems_are_not_equal_evidence(self):
        rows = observations()
        del rows[2]["subsystems"]["world"]
        rows[2]["unavailable"] = {"world": "checkpoint unavailable"}
        write_rows(self.left, rows)
        write_rows(self.right, rows)
        with self.assertRaisesRegex(ReportError, "Unavailable subsystem world at tick 2"):
            self.compare()

    def test_inconsistent_unavailable_maps_fail(self):
        for unavailable in ({"world": ""}, {"world": None}, {"unknown": "missing"},
                            {"world": "overlaps a valid hash"}):
            with self.subTest(unavailable=unavailable):
                rows = observations()
                rows[1]["unavailable"] = unavailable
                write_rows(self.right, rows)
                with self.assertRaises(ReportError):
                    self.compare()

    def test_footer_and_exact_eof_are_required_even_after_a_difference(self):
        for footer in (None, {"type": "complete", "ticks": 2, "last_tick": 3},
                       {"type": "complete", "ticks": 3, "last_tick": True},
                       {"type": "complete", "ticks": 3},
                       {"type": "complete", "ticks": 3, "last_tick": 3, "extra": 0}):
            with self.subTest(footer=footer):
                rows = observations()
                rows[1]["input_digest"] = "e" * 64
                write_rows(self.right, rows[:-1] + ([footer] if footer is not None else []))
                with self.assertRaises(ReportError) as error:
                    self.compare()
                self.assertNotIn("First difference", str(error.exception))
        for suffix in ("\n", "{}\n", json.dumps(observations()[0]) + "\n"):
            write_rows(self.right, observations())
            with self.right.open("a", encoding="utf-8") as stream:
                stream.write(suffix)
            with self.assertRaisesRegex(ReportError, "after completion"):
                self.compare()

    def test_truncated_invalid_duplicate_and_nonfinite_json_fail_closed(self):
        valid = self.right.read_bytes()
        corruptions = [valid[:-1], valid[:-15], b"\xff\n", b"\n", b"[]\n",
                       valid.replace(b'"schema": 1', b'"schema": 1, "schema": 1'),
                       valid.replace(b'"party":', b'"party": "0", "party":', 1)]
        for value in (b"NaN", b"Infinity", b"-Infinity", b"1e999"):
            corruptions.append(valid.replace(b'"schema": 1', b'"schema": ' + value))
        for content in corruptions:
            with self.subTest(content=content[:80]):
                self.right.write_bytes(content)
                with self.assertRaises(ReportError):
                    self.compare()

    def test_report_line_byte_limit_is_enforced(self):
        self.right.write_bytes(b" " * MAX_LINE_BYTES + b"\n")
        with self.assertRaisesRegex(ReportError, "byte bound"):
            self.compare()

    def test_compare_cli_has_honest_scope_and_nonzero_failure(self):
        output = StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(["--compare", str(self.left), str(self.right)]), 0)
        self.assertEqual(json.loads(output.getvalue())["status"], "observed-equal")
        self.right.write_bytes(b"")
        with redirect_stderr(StringIO()):
            self.assertEqual(main(["--compare", str(self.left), str(self.right)]), 1)


class NativeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="native-runner-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()  # as the scripts do
        self.probe = self.directory / "nativeprobe.exe"
        self.probe.touch()

    def run_pair(self, **changes):
        options = {"ticks": 3, "players": 2, "stage": "G1", "workload": "idle",
                   "render_every": [0, 1], "timeout": 7}
        options.update(changes)
        return run_pair(self.probe, self.directory, **options)

    def emit(self, command, **kwargs):
        flags = dict(zip(command[1::2], command[2::2]))
        rows = observations(int(flags["--ticks"]), int(flags["--render-every"]))
        rows[0].update(stage=flags["--stage"], players=int(flags["--players"]),
                       workload=flags["--workload"])
        write_rows(Path(flags["--report"]), rows)
        self.assertIs(kwargs["stdin"], subprocess.DEVNULL)
        self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
        self.assertEqual(kwargs["timeout"], 7)
        self.assertFalse(kwargs["check"])
        self.assertTrue(Path(flags["--report"]).is_absolute())
        return SimpleNamespace(returncode=0)

    @patch("native_determinism.subprocess.run")
    def test_two_sequential_processes_receive_the_same_native_inputs(self, run):
        finished = []

        def execute(command, **kwargs):
            index = len(finished)
            self.assertEqual(command[0], str(self.probe))
            self.assertEqual(Path(command[2]).name, f"run-{index + 1}.jsonl")
            result = self.emit(command, **kwargs)
            finished.append(result)
            return result

        run.side_effect = execute
        result = self.run_pair(assets=self.directory / "native assets", data=self.directory / "data")
        self.assertEqual(result["status"], "observed-equal")
        self.assertEqual(run.call_count, 2)
        self.assertEqual(len(finished), 2)
        first, second = [call.args[0] for call in run.call_args_list]
        self.assertEqual(first[3:9], second[3:9])
        self.assertEqual(first[11:], second[11:])
        self.assertEqual(first[10], "0")
        self.assertEqual(second[10], "1")
        self.assertIn(str(self.directory / "native assets"), first)
        self.assertTrue((self.directory / "run-1.log").is_file())
        self.assertTrue((self.directory / "run-2.log").is_file())

    @patch("native_determinism.subprocess.run")
    def test_both_processes_switch_on_the_same_mechanics(self, run):
        run.side_effect = self.emit
        result = self.run_pair(mechanics=["aggressor", "calm-mode"])
        self.assertEqual(result["status"], "observed-equal")
        for call in run.call_args_list:
            command = call.args[0]
            switched = [command[i + 1] for i, flag in enumerate(command) if flag == "--mechanic"]
            self.assertEqual(switched, ["aggressor", "calm-mode"])

    def test_a_mechanic_must_be_named_as_the_catalog_names_it(self):
        with redirect_stderr(StringIO()), self.assertRaises(SystemExit):
            main(["--probe", str(self.probe), "--mechanic", "Not A Name"])

    @patch("native_determinism.subprocess.run")
    def test_failure_timeout_and_missing_report_do_not_start_second_process(self, run):
        for result in (SimpleNamespace(returncode=4), SimpleNamespace(returncode=0),
                       subprocess.TimeoutExpired("nativeprobe", 7)):
            with self.subTest(result=result):
                run.reset_mock()
                run.side_effect = result if isinstance(result, Exception) else None
                run.return_value = result
                log = self.directory / "run-1.log"
                if log.exists():
                    log.unlink()
                with self.assertRaises((ReportError, subprocess.TimeoutExpired)):
                    self.run_pair()
                self.assertEqual(run.call_count, 1)

    @patch("native_determinism.subprocess.run")
    def test_crash_cannot_pass_even_with_a_complete_report(self, run):
        def crash(command, **kwargs):
            self.emit(command, **kwargs)
            return SimpleNamespace(returncode=1)
        run.side_effect = crash
        with self.assertRaisesRegex(ReportError, "exit 1"):
            self.run_pair()
        self.assertEqual(run.call_count, 1)

    @patch("native_determinism.subprocess.run")
    def test_existing_reports_and_logs_are_never_reused(self, run):
        write_rows(self.directory / "run-1.jsonl", observations())
        with self.assertRaisesRegex(ReportError, "existing report"):
            self.run_pair()
        run.assert_not_called()
        (self.directory / "run-1.jsonl").unlink()
        (self.directory / "run-1.log").write_text("previous log", encoding="utf-8")
        with self.assertRaises(FileExistsError):
            self.run_pair()
        run.assert_not_called()

    @patch("native_determinism.subprocess.run")
    def test_ignored_probe_cadence_argument_fails_requested_metadata(self, run):
        def ignore_cadence(command, **kwargs):
            command = list(command)
            command[command.index("--render-every") + 1] = "0"
            return self.emit(command, **kwargs)
        run.side_effect = ignore_cadence
        with self.assertRaisesRegex(ReportError, "Requested render_every"):
            self.run_pair()

    @patch("native_determinism.subprocess.run")
    def test_cli_invalid_options_never_launch(self, run):
        for arguments in (("--ticks", "0"), ("--players", "5"), ("--timeout", "nan"),
                          ("--timeout", "inf"), ("--timeout", "0"),
                          ("--ticks", "36001"), ("--render-every", "0", "61"),
                          ("--render-every", "-1", "1"),
                          ("--compare", "a", "b", "--ticks", "3")):
            with self.subTest(arguments=arguments), redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    main(list(arguments))
                self.assertEqual(error.exception.code, 2)
        run.assert_not_called()

    @patch("native_determinism.run_pair")
    def test_cli_accepts_exact_native_tick_and_render_bounds(self, run):
        run.return_value = {"status": "observed-equal", "complete_state": False}
        with redirect_stdout(StringIO()):
            self.assertEqual(main(["--probe", str(self.probe),
                                   "--output", str(self.directory / "bounded"),
                                   "--ticks", "36000", "--render-every", "0", "60"]), 0)
        self.assertEqual(run.call_args.kwargs["ticks"], 36000)
        self.assertEqual(run.call_args.kwargs["render_every"], [0, 60])

    @patch("native_determinism.subprocess.run")
    def test_cli_preserves_outputs_and_uses_existing_binary_only(self, run):
        run.side_effect = self.emit
        output = self.directory / "output"
        with redirect_stdout(StringIO()):
            self.assertEqual(main(["--probe", str(self.probe), "--output", str(output),
                                   "--ticks", "3", "--timeout", "7"]), 0)
        self.assertEqual(run.call_count, 2)
        self.assertTrue((output / "run-2.jsonl").is_file())
        with redirect_stderr(StringIO()):
            self.assertEqual(main(["--probe", str(self.probe), "--output", str(output)]), 1)
        self.assertEqual(run.call_count, 2)


if __name__ == "__main__":
    unittest.main()
