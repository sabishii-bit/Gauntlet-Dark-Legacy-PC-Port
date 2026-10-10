"""The network runner must never mistake a crash, timeout or empty log for success."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import netplay_test


class NetplayRunnerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="network test ")
        self.addCleanup(temporary.cleanup)
        self.directory = pathlib.Path(temporary.name)
        self.log = self.directory / "host.log"
        self.log.write_text("", encoding="utf-8")

    def test_partial_lines_and_library_diagnostics_are_not_verdicts(self):
        self.log.write_text('library diagnostic\n[]\n{"event": "passed"', encoding="utf-8")
        self.assertEqual(netplay_test.records(self.log), [])
        with self.assertRaisesRegex(RuntimeError, "without a passing verdict"):
            netplay_test.verify_log(self.log, "host", 0)

    def test_online_session_requires_live_inputs_snapshots_and_all_load_barriers(self):
        valid = {"event": "passed", "role": "session-client", "epochs": 4,
                 "barriers": 4, "players": 3, "inputs": 240, "startup_ms": 300,
                 "snapshots": 70, "third_epoch_tick": 177, "interpolated": 200}
        self.log.write_text(json.dumps(valid), encoding="utf-8")
        netplay_test.verify_session(self.log, "session-client", 3)
        for field in ("epochs", "barriers", "players", "inputs", "startup_ms",
                      "snapshots", "third_epoch_tick", "interpolated"):
            for value in (None, 0, True, "4"):
                row = dict(valid, **{field: value})
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    netplay_test.verify_session(self.log, "session-client", 3)
        valid["role"] = "session-host"
        self.log.write_text(json.dumps(valid), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "never paused"):
            netplay_test.verify_session(self.log, "session-host", 3)
        valid["paused"] = True
        self.log.write_text(json.dumps(valid), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "never stalled"):
            netplay_test.verify_session(self.log, "session-host", 3)
        valid["stalled"] = True
        self.log.write_text(json.dumps(valid), encoding="utf-8")
        netplay_test.verify_session(self.log, "session-host", 3)
        self.log.write_text(json.dumps(valid) + "\n" + json.dumps(valid), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "unique"):
            netplay_test.verify_session(self.log, "session-host", 3)

    def test_exit_status_failure_record_and_role_all_matter(self):
        for code, entries in ((1, [{"event": "passed", "role": "host"}]),
                              (0, [{"event": "failed"}, {"event": "passed", "role": "host"}]),
                              (0, [{"event": "passed", "role": "client"}])):
            self.log.write_text("\n".join(json.dumps(row) for row in entries), encoding="utf-8")
            with self.assertRaises(RuntimeError):
                netplay_test.verify_log(self.log, "host", code)
        self.log.write_text('{"event":"passed","role":"host"}\n', encoding="utf-8")
        netplay_test.verify_log(self.log, "host", 0)

    def test_startup_failure_is_not_a_timeout(self):
        process = mock.Mock(returncode=7)
        process.poll.return_value = 7
        with self.assertRaisesRegex(RuntimeError, "exit 7"):
            netplay_test.listener_port(process, self.log, float("inf"))

    def test_diagnostics_must_not_log_handshake_credentials(self):
        for text in ('pwd_frag: "test-secret"', 'private_key: "test-secret"',
                     '{"token":"test-secret"}'):
            self.log.write_text(text + '\n{"event":"passed","role":"host"}', encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "private handshake material"):
                netplay_test.verify_log(self.log, "host", 0)

    def test_slow_room_startup_is_not_a_passing_verdict(self):
        for value in (None, 0, True, "500", 10001):
            row = {"event": "passed", "role": "room-host", "simulation_ticks": 120,
                   "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": value}
            self.log.write_text(json.dumps(row), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "Room setup stalled"):
                netplay_test.verify_room_motion(self.log, "room-host")

    def test_room_motion_requires_simulation_and_live_snapshots(self):
        for role, field, value in (("room-host", "simulation_ticks", 120),
                                   ("room-client", "snapshots_received", 20)):
            row = {"event": "passed", "role": role, "ticks": 120, field: value, "startup_ms": 250,
                   "match_epoch": 2, "load_barrier_checks": 100,
                   "combat_tick": 119, "combat_enemies": 25,
                   "geometry_objects": 806,
                   "projectile_snapshots": 15, "combat_projectiles": 0,
                   "pickup_snapshots": 15, "combat_pickups": 40,
                   "fixture_snapshots": 15, "combat_fixtures": 78,
                   "fighter_snapshots": 15, "combat_fighters": 18,
                   "companion_snapshots": 15, "combat_companions": 7,
                   "hud_snapshots": 15, "hud_players": 4, "hud_overlay_snapshots": 15,
                   "screen_states": 15}
            self.log.write_text(json.dumps(row), encoding="utf-8")
            netplay_test.verify_room_motion(self.log, role)
            for invalid in (None, 0, True, "120"):
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    netplay_test.verify_room_motion(self.log, role)
        self.log.write_text("", encoding="utf-8")
        with self.assertRaises(RuntimeError):
            netplay_test.verify_room_motion(self.log, "room-host")

    def test_room_verdict_requires_complete_combat_state(self):
        for field in ("combat_tick", "combat_enemies"):
            for invalid in (None, 0, True, "119"):
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "combat checkpoint"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_live_projectiles_and_their_final_removal(self):
        for field, invalid_values in (("projectile_snapshots", (None, 0, 4, True, "15")),
                                       ("combat_projectiles", (None, 1, False, "0"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "projectile roster"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_live_pickups(self):
        for field, invalid_values in (("pickup_snapshots", (None, 0, 4, True, "15")),
                                       ("combat_pickups", (None, 39, 41, False, "40"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0,
                       "pickup_snapshots": 15, "combat_pickups": 40}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "pickup roster"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_live_fixtures(self):
        for field, invalid_values in (("fixture_snapshots", (None, 0, 4, True, "15")),
                                       ("combat_fixtures", (None, 77, 79, False, "78"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0,
                       "pickup_snapshots": 15, "combat_pickups": 40,
                       "fixture_snapshots": 15, "combat_fixtures": 78}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "fixture roster"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_dense_geometry(self):
        for invalid in (None, 0, 805, True, "806"):
            row = {"event": "passed", "role": "room-client", "ticks": 120,
                   "match_epoch": 2, "load_barrier_checks": 100,
                   "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                   "projectile_snapshots": 15, "combat_projectiles": 0,
                   "geometry_objects": invalid}
            self.log.write_text(json.dumps(row), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "dense geometry"):
                netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_articulated_fighters(self):
        for field, invalid_values in (("fighter_snapshots", (None, 0, 4, True, "15")),
                                       ("combat_fighters", (None, 17, 19, False, "18"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0,
                       "pickup_snapshots": 15, "combat_pickups": 40,
                       "fixture_snapshots": 15, "combat_fixtures": 78,
                       "fighter_snapshots": 15, "combat_fighters": 18}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "fighter roster"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_match_verdict_requires_shared_epoch_and_loading_barrier(self):
        for field in ("match_epoch", "load_barrier_checks"):
            for invalid in (None, 0, True, "2"):
                row = {"event": "passed", "role": "room-host", "simulation_ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100}
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "loading barrier"):
                    netplay_test.verify_room_motion(self.log, "room-host")

    def test_room_verdict_requires_both_companion_slots(self):
        for field, invalid_values in (("companion_snapshots", (None, 0, 4, True, "15")),
                                      ("combat_companions", (None, 6, 8, False, "7"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0,
                       "pickup_snapshots": 15, "combat_pickups": 40,
                       "fixture_snapshots": 15, "combat_fixtures": 78,
                       "fighter_snapshots": 15, "combat_fighters": 18,
                       "companion_snapshots": 15, "combat_companions": 7,
                       "hud_snapshots": 15, "hud_players": 4, "hud_overlay_snapshots": 15,
                       "screen_states": 15}
                self.log.write_text(json.dumps(row), encoding="utf-8")
                netplay_test.verify_room_motion(self.log, "room-client")
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "companion roster"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_live_hud_updates(self):
        for field, invalid_values in (("hud_snapshots", (None, 0, 4, True, "15")),
                                      ("hud_players", (None, 0, 3, 5, True, "4"))):
            for invalid in invalid_values:
                row = {"event": "passed", "role": "room-client", "ticks": 120,
                       "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                       "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                       "geometry_objects": 806,
                       "projectile_snapshots": 15, "combat_projectiles": 0,
                       "pickup_snapshots": 15, "combat_pickups": 40,
                       "fixture_snapshots": 15, "combat_fixtures": 78,
                       "fighter_snapshots": 15, "combat_fighters": 18,
                       "companion_snapshots": 15, "combat_companions": 7,
                       "hud_snapshots": 15, "hud_players": 4, "hud_overlay_snapshots": 15,
                       "screen_states": 15}
                self.log.write_text(json.dumps(row), encoding="utf-8")
                netplay_test.verify_room_motion(self.log, "room-client")
                row[field] = invalid
                self.log.write_text(json.dumps(row), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "player HUD"):
                    netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_pickup_and_boss_overlays(self):
        for invalid in (None, 0, 4, True, "15"):
            row = {"event": "passed", "role": "room-client", "ticks": 120,
                   "match_epoch": 2, "load_barrier_checks": 100, "startup_ms": 250,
                   "snapshots_received": 20, "combat_tick": 119, "combat_enemies": 25,
                   "geometry_objects": 806,
                   "projectile_snapshots": 15, "combat_projectiles": 0,
                   "pickup_snapshots": 15, "combat_pickups": 40,
                   "fixture_snapshots": 15, "combat_fixtures": 78,
                   "fighter_snapshots": 15, "combat_fighters": 18,
                   "companion_snapshots": 15, "combat_companions": 7,
                   "hud_snapshots": 15, "hud_players": 4, "hud_overlay_snapshots": 15,
                   "screen_states": 15}
            self.log.write_text(json.dumps(row), encoding="utf-8")
            netplay_test.verify_room_motion(self.log, "room-client")
            row["hud_overlay_snapshots"] = invalid
            self.log.write_text(json.dumps(row), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "HUD overlays"):
                netplay_test.verify_room_motion(self.log, "room-client")

    def test_room_verdict_requires_all_scene_overlay_phases(self):
        row = {"event": "passed", "role": "room-client", "snapshots_received": 20,
               "ticks": 120, "startup_ms": 250, "match_epoch": 2, "load_barrier_checks": 1,
               "combat_tick": 119, "combat_enemies": 25, "geometry_objects": 806,
               "projectile_snapshots": 15, "combat_projectiles": 0,
               "pickup_snapshots": 15, "combat_pickups": 40,
               "fixture_snapshots": 15, "combat_fixtures": 78,
               "fighter_snapshots": 15, "combat_fighters": 18,
               "companion_snapshots": 15, "combat_companions": 7,
               "hud_snapshots": 15, "hud_players": 4, "hud_overlay_snapshots": 15,
               "screen_states": 15}
        self.log.write_text(json.dumps(row), encoding="utf-8")
        netplay_test.verify_room_motion(self.log, "room-client")
        for invalid in (None, 0, 7, 11, 13, 14, 31, True, "15"):
            row["screen_states"] = invalid
            self.log.write_text(json.dumps(row), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "scene overlay phase"):
                netplay_test.verify_room_motion(self.log, "room-client")

    def test_port_validation_and_startup_timeout(self):
        process = mock.Mock()
        process.poll.return_value = None
        for port in (0, 65536, True, "1234"):
            self.log.write_text(json.dumps({"event": "listening", "port": port}), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "invalid port"):
                netplay_test.listener_port(process, self.log, float("inf"))
        self.log.write_text('{"event":"listening","port":1234}', encoding="utf-8")
        self.assertEqual(netplay_test.listener_port(process, self.log, float("inf")), 1234)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            netplay_test.listener_port(process, self.log, 0)

    def test_cleanup_terminates_then_kills_only_its_own_process(self):
        process = mock.Mock()
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("netplaycheck", 3), 0]
        netplay_test.stop(process)
        process.terminate.assert_called_once()
        process.kill.assert_called_once()
        self.assertEqual(process.wait.call_count, 2)

    def test_cleanup_does_not_signal_finished_process(self):
        process = mock.Mock()
        process.poll.return_value = 0
        netplay_test.stop(process)
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_build_is_separate_and_does_not_build_the_game(self):
        with mock.patch.object(netplay_test.devenv, "run") as run:
            netplay_test.build(self.directory)
        configure, build = [call.args[0] for call in run.call_args_list]
        self.assertIn("-DGDL_ENABLE_NETPLAY=ON", configure)
        self.assertIn("-DGDL_BUILD_TESTS=OFF", configure)
        self.assertEqual(build[2], str(self.directory))
        self.assertEqual(build[-3:], ["netplaycheck", "roomcheck", "compile_commands"])
        self.assertNotIn("gauntlet", build)

    def test_room_code_validation_and_early_failure(self):
        process = mock.Mock(returncode=9)
        process.poll.return_value = 9
        with self.assertRaisesRegex(RuntimeError, "exited during startup"):
            netplay_test.room_code(process, self.log)
        process.poll.return_value = None
        for code in (None, 1234, "", "AAAAAAAAA", "ABCDEFG0", "abcd2345"):
            self.log.write_text(json.dumps({"event": "room_created", "code": code}), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "invalid room code"):
                netplay_test.room_code(process, self.log)
        self.log.write_text('{"event":"room_created","code":"ABCDEFGH"}', encoding="utf-8")
        self.assertEqual(netplay_test.room_code(process, self.log), "ABCDEFGH")

    def test_repeat_mode_does_not_retry_away_a_failed_run(self):
        binary = self.directory / "bin" / ("netplaycheck.exe" if sys.platform == "win32" else "netplaycheck")
        binary.parent.mkdir()
        binary.touch()
        with mock.patch.object(netplay_test, "run_room_contract") as contract, \
                mock.patch.object(netplay_test, "run_rooms",
                                  side_effect=[None, RuntimeError("failed second run")]) as rooms, \
                mock.patch("builtins.print"):
            result = netplay_test.main(["--build-dir", str(self.directory), "--suite", "rooms",
                                       "--room-runs", "3", "--network-trace"])
        self.assertEqual(result, 1)
        contract.assert_called_once()
        self.assertEqual(rooms.call_count, 2)
        self.assertEqual([call.args[1].name for call in rooms.call_args_list], ["round-1", "round-2"])
        self.assertTrue(all(call.args[2] is True for call in rooms.call_args_list))

    def test_repeat_count_is_bounded(self):
        for value in ("0", "-1", "21"):
            with mock.patch("sys.stderr"), self.assertRaises(SystemExit) as error:
                netplay_test.main(["--room-runs", value])
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
