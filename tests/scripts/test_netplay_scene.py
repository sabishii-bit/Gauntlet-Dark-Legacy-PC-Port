import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import netplay_scene


class PlayableSceneTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="netplay scene ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)

    def test_digest_uses_contents_paths_and_case_independent_order(self):
        one, two = self.root / "one", self.root / "two"
        one.mkdir()
        two.mkdir()
        (one / "B.WAD").write_bytes(b"second")
        (one / "a.ngc").write_bytes(b"first")
        (two / "A.NGC").write_bytes(b"first")
        (two / "b.wad").write_bytes(b"second")
        original = netplay_scene.asset_digest(one)
        self.assertEqual(original, netplay_scene.asset_digest(two))
        (two / "A.NGC").write_bytes(b"other")
        self.assertNotEqual(original, netplay_scene.asset_digest(two))
        (two / "A.NGC").write_bytes(b"first")
        (two / "b.wad").rename(two / "c.wad")
        self.assertNotEqual(original, netplay_scene.asset_digest(two))

    def test_missing_or_empty_asset_tree_cannot_match(self):
        for path in (self.root, self.root / "missing"):
            with self.assertRaisesRegex(ValueError, "No assets"):
                netplay_scene.asset_digest(path)

    def test_unreadable_asset_directory_does_not_produce_a_partial_identity(self):
        def broken_walk(root, *, followlinks, onerror):
            onerror(PermissionError("cannot enumerate native assets"))
            return iter(())

        with mock.patch.object(netplay_scene.os, "walk", side_effect=broken_walk):
            with self.assertRaisesRegex(PermissionError, "cannot enumerate"):
                netplay_scene.asset_digest(self.root)

    def test_test_characters_never_retain_disk_save_authority(self):
        original = {"level": "G1", "party": [{"player": 3, "slot": 2, "color": "BLU"}]}
        saved = copy.deepcopy(original)
        host = netplay_scene.test_scenario(original, False)
        guest = netplay_scene.test_scenario(original, True)
        self.assertEqual(original, saved)
        for selection in (host, guest):
            self.assertEqual(selection["party"][0]["player"], 0)
            self.assertNotIn("slot", selection["party"][0])
        self.assertNotEqual(host["party"][0]["color"], guest["party"][0]["color"])
        for invalid in ({}, {"party": []}, {"party": [{}, {}]},
                        {"screen": "ending", "party": [{}]}):
            with self.assertRaises(ValueError):
                netplay_scene.test_scenario(invalid, False)

    def test_default_scenario_carries_valid_native_potion_types(self):
        source = json.loads((netplay_scene.ROOT / "tests/scenarios/netplay-g1.json").read_text())
        for guest in (False, True):
            selection = netplay_scene.test_scenario(source, guest)
            player = selection["party"][0]
            self.assertEqual(player["level"], 60)
            self.assertEqual(len(player["potions"]), 9)
            self.assertTrue(all(1 <= kind <= 4 for kind in player["potions"]))

    def test_two_windows_have_safe_isolated_display_and_device_defaults(self):
        defaults = json.loads((netplay_scene.ROOT / "data/config.json").read_text(encoding="utf-8"))
        original = copy.deepcopy(defaults)
        for guest in (False, True):
            config = netplay_scene.test_config(defaults, guest, 30)
            self.assertTrue(config["display"]["vsync"])
            self.assertEqual(config["display"]["windowMode"], "windowed")
            self.assertEqual(config["timing"]["tickRate"], 60)
            self.assertEqual(config["display"]["maxFrameRate"], 30)
            self.assertEqual(config["controls"]["players"][0]["device"], "keyboard" if guest else "")
            self.assertTrue(all(p["device"] == "none" for p in config["controls"]["players"][1:]))
        self.assertEqual(defaults, original)

    def test_runtime_errors_and_crashes_are_not_successful_visual_checks(self):
        log = self.root / "game.log"
        child = mock.Mock()
        child.poll.return_value = None
        log.write_text("still loading", encoding="utf-8")
        self.assertNotIn(netplay_scene.PLAYING, netplay_scene.healthy(child, log))
        for line in ("ERROR ASSERTION FAILED", "Connection failed (3)", "Online scene stopped (1)"):
            log.write_text(line, encoding="utf-8")
            with self.assertRaises(RuntimeError):
                netplay_scene.healthy(child, log)
        log.write_text(netplay_scene.PLAYING, encoding="utf-8")
        child.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            netplay_scene.healthy(child, log)

    def test_expected_level_requires_its_own_completed_loading_barrier(self):
        first = "Online stage loaded: L1 (epoch 2, host)\n" + netplay_scene.PLAYING
        self.assertTrue(netplay_scene.playing(first))
        self.assertTrue(netplay_scene.playing(first, "L1"))
        self.assertFalse(netplay_scene.playing(first, "G1"))
        pending = first + "\nOnline stage loaded: G1 (epoch 3, host)\n"
        self.assertFalse(netplay_scene.playing(pending, "G1"))
        final = pending + netplay_scene.PLAYING
        self.assertTrue(netplay_scene.playing(final, "G1"))
        self.assertFalse(netplay_scene.playing(final, "L1"))

    def test_smoke_test_requires_advancing_checkpoints_in_the_loaded_stage(self):
        first = ("Online stage loaded: L1 (epoch 2, host)\n"
                 "Online checkpoint progress: epoch 2 tick 600 (host)\n")
        self.assertEqual(netplay_scene.checkpoint(first), (2, 600))
        pending = first + "Online stage loaded: G1 (epoch 3, host)\n"
        self.assertIsNone(netplay_scene.checkpoint(pending))
        self.assertIsNone(netplay_scene.checkpoint(
            pending + "Online checkpoint progress: epoch 2 tick 900 (host)\n"))
        started = pending + "Online checkpoint progress: epoch 3 tick 0 (host)\n"
        self.assertEqual(netplay_scene.checkpoint(started), (3, 0))
        final = started + "Online checkpoint progress: epoch 3 tick 300 (host)\n"
        self.assertEqual(netplay_scene.checkpoint(final), (3, 300))
        self.assertTrue(netplay_scene.advancing((3, 0), netplay_scene.checkpoint(final)))
        for original, latest in ((None, (3, 300)), ((3, 0), None), ((3, 300), (3, 300)),
                                 ((3, 300), (2, 600)), ((3, 300), (3, 100))):
            self.assertFalse(netplay_scene.advancing(original, latest))


if __name__ == "__main__":
    unittest.main()
