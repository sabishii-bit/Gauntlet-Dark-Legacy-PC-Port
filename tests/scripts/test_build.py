"""The build launcher forwards preview arguments after building the selected preset."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import build


class BuildLaunchTests(unittest.TestCase):
    def test_item_collision_refreshes_only_incomplete_levels_and_verifies_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary = root / "build/release"
            (binary / "bin").mkdir(parents=True)
            (binary / "bin" / f"gdlunpack{build.EXE}").touch()
            (root / "raw/LEVELS").mkdir(parents=True)
            manifest = root / "export/LEVELS/LEVELE1/world.json"
            manifest.parent.mkdir(parents=True)
            stale = {"itemInstances": [{"triangleCount": 1}]}
            manifest.write_text(json.dumps(stale), encoding="utf-8")
            current = {"itemInstances": [{"triangleCount": 1, "collision": [{}]}]}
            args = ["--data", "raw", "--unpacked", "export"]

            def unpack(_command):
                manifest.write_text(json.dumps(current), encoding="utf-8")

            with mock.patch.object(build.devenv, "run", side_effect=unpack) as run:
                build.refresh_item_collision(binary, args, root)
                run.assert_called_once_with([
                    str(binary / "bin" / f"gdlunpack{build.EXE}"), str(root / "raw"),
                    str(root / "export"), "--only", "LEVELE1"])
                run.reset_mock()
                build.refresh_item_collision(binary, args, root)
                run.assert_not_called()
                manifest.write_text(json.dumps(stale), encoding="utf-8")
                run.side_effect = None
                with self.assertRaisesRegex(ValueError, "did not upgrade"):
                    build.refresh_item_collision(binary, args, root)
            with self.assertRaisesRegex(ValueError, "needs re-exporting"):
                build.refresh_item_collision(binary, ["--unpacked", "export"], root)

    def test_legacy_player_exports_refresh_once_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary = root / "build/release"
            (binary / "bin").mkdir(parents=True)
            (binary / "bin" / f"gdlunpack{build.EXE}").touch()
            (root / "assets/GUNE5D/Gauntlet/PLAYERS").mkdir(parents=True)
            manifest = root / "assets/unpacked/PLAYERS/JES/SFXYEL/animations.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text('{"trees":[]}', encoding="utf-8")
            def unpack(_command):
                manifest.write_text(json.dumps({"textureBindingVersion": 1, "trees": []}),
                                    encoding="utf-8")

            with mock.patch.object(build.devenv, "run", side_effect=unpack) as run:
                build.refresh_player_effects(binary, [], root)
                run.assert_called_once_with([
                    str(binary / "bin" / f"gdlunpack{build.EXE}"),
                    str(root / "assets/GUNE5D/Gauntlet"), str(root / "assets/unpacked"),
                    "--only", "PLAYERS"])
                run.reset_mock()
                # Optional node/sequence links may legitimately be absent. The
                # exporter version, not their presence, establishes freshness.
                manifest.write_text(json.dumps({"textureBindingVersion": 1, "trees": []}),
                                    encoding="utf-8")
                build.refresh_player_effects(binary, [], root)
                run.assert_not_called()
                manifest.write_text('{}', encoding="utf-8")
                run.side_effect = None
                with self.assertRaisesRegex(ValueError, "did not upgrade"):
                    build.refresh_player_effects(binary, [], root)

    def test_asset_free_builds_do_not_run_unpacker(self):
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(build.devenv, "run") as run:
            root = pathlib.Path(directory)
            build.refresh_player_effects(root / "build/release", [], root)
            build.refresh_item_collision(root / "build/release", [], root)
            run.assert_not_called()

    def test_custom_asset_paths_are_respected_and_missing_raw_assets_fail_clearly(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            manifest = root / "custom output/PLAYERS/JES/SFXYEL/animations.json"
            manifest.parent.mkdir(parents=True)
            manifest.write_text('{}', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "need re-exporting"):
                build.refresh_player_effects(root / "build/release",
                                             ["--unpacked", "custom output"], root)

    def test_previews_use_release_and_preserve_forwarded_arguments(self):
        with tempfile.TemporaryDirectory(prefix="gdl build ") as directory:
            root = pathlib.Path(directory)
            binary = root / "build" / "release-config"
            binary.mkdir(parents=True)
            (binary / "CMakeCache.txt").touch()
            for flag in ("--demo", "--screensaver"):
                with self.subTest(flag=flag), \
                        mock.patch.object(build, "ROOT", root), \
                        mock.patch.object(build, "build_presets",
                                          return_value={"release": "release-config"}), \
                        mock.patch.object(build.devenv, "release_preset", return_value="release"), \
                        mock.patch.object(build.devenv, "run",
                                          return_value=subprocess.CompletedProcess([], 0)) as run, \
                        mock.patch.object(sys, "argv", ["build.py", "--run", "--", flag,
                                                         "--frames", "120", "--unpacked",
                                                         "a path with spaces"]):
                    self.assertEqual(build.main(), 0)
                    self.assertEqual(run.call_args_list, [
                        mock.call(["cmake", "--build", "--preset", "release"]),
                        mock.call([str(binary / "bin" / f"gauntlet{build.EXE}"), flag,
                                   "--frames", "120", "--unpacked", "a path with spaces"]),
                    ])


if __name__ == "__main__":
    unittest.main()
