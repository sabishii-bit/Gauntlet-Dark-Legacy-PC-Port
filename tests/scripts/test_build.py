"""Build/run orchestration never requires exported gameplay assets."""
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
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl build ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        for target, field, value in (
            (build, "ROOT", self.root),
            (build.devenv, "default_preset", lambda: "debug"),
            (build.devenv, "release_preset", lambda: "release"),
        ):
            patch = mock.patch.object(target, field, value)
            patch.start()
            self.addCleanup(patch.stop)
        (self.root / "CMakePresets.json").write_text(json.dumps({"buildPresets": [
            {"name": "debug", "configurePreset": "debug-config"},
            {"name": "release", "configurePreset": "release-config"},
        ]}), encoding="utf-8")

    def configured(self, preset="release-config", text=""):
        binary = self.root / "build" / preset
        binary.mkdir(parents=True, exist_ok=True)
        (binary / "CMakeCache.txt").write_text(text, encoding="utf-8")
        return binary

    def launch(self, args, code=0):
        with mock.patch.object(sys, "argv", ["build.py", *args]), \
                mock.patch.object(build.devenv, "run",
                                  return_value=subprocess.CompletedProcess([], code)) as run:
            result = build.main()
        return result, run.call_args_list

    def test_presets_keep_configure_mapping(self):
        self.assertEqual(build.build_presets(),
                         {"debug": "debug-config", "release": "release-config"})

    def test_first_build_configures_and_uses_debug(self):
        result, calls = self.launch([])
        self.assertEqual(result, 0)
        self.assertEqual(calls, [mock.call(["cmake", "--preset", "debug-config"]),
                                mock.call(["cmake", "--build", "--preset", "debug"])])

    def test_existing_cache_avoids_configuration(self):
        self.configured("debug-config")
        result, calls = self.launch(["--test"])
        self.assertEqual(result, 0)
        self.assertEqual(calls, [mock.call(["cmake", "--build", "--preset", "debug"]),
                                mock.call(["ctest", "--preset", "debug", "-LE", "gpu"])])

    def test_native_previews_preserve_arguments_and_exit_code(self):
        binary = self.configured()
        for flag in ("--demo", "--screensaver", "--title"):
            with self.subTest(flag=flag):
                args = [flag, "--frames", "120", "--assets", "native path with spaces"]
                result, calls = self.launch(["--run", "--", *args], 17)
                self.assertEqual(result, 17)
                self.assertEqual(calls, [
                    mock.call(["cmake", "--build", "--preset", "release"]),
                    mock.call([str(binary / "bin" / f"gauntlet{build.EXE}"), *args]),
                ])

    def test_explicit_debug_run_remains_debug(self):
        binary = self.configured("debug-config")
        _, calls = self.launch(["debug", "--run"])
        self.assertEqual(calls[-1], mock.call([str(binary / "bin" / f"gauntlet{build.EXE}")]))
        self.assertEqual(calls[0], mock.call(["cmake", "--build", "--preset", "debug"]))

    def test_native_launch_ignores_malformed_legacy_exports(self):
        self.configured()
        path = self.root / "assets/unpacked/PLAYERS/JES/SFXGRE/animations.json"
        path.parent.mkdir(parents=True)
        path.write_text("not JSON", encoding="utf-8")
        result, calls = self.launch(["--run"])
        self.assertEqual(result, 0)
        self.assertEqual(len(calls), 2)
        self.assertFalse(any("gdlunpack" in str(call) for call in calls))
        self.assertEqual(path.read_text(encoding="utf-8"), "not JSON")

    def test_explicit_inspection_export_remains_available(self):
        binary = self.configured("debug-config",
                                 "GDL_ASSET_DIR:PATH=original files\n"
                                 "GDL_UNPACKED_DIR:PATH=inspection output\n")
        result, calls = self.launch(["--unpack", "--levels"])
        self.assertEqual(result, 0)
        self.assertEqual(calls, [
            mock.call(["cmake", "--build", "--preset", "debug"]),
            mock.call([str(binary / "bin" / f"gdlunpack{build.EXE}"),
                       "original files", "inspection output", "--levels"]),
        ])

    def test_cache_path_handles_absent_empty_and_supported_types(self):
        fallback = self.root / "default"
        self.assertEqual(build.cache_path(self.root / "missing", "LOCATION", fallback), fallback)
        for kind in ("PATH", "FILEPATH", "STRING"):
            binary = self.configured(text=f"LOCATION:{kind}=some directory\n")
            self.assertEqual(build.cache_path(binary, "LOCATION", fallback),
                             pathlib.Path("some directory"))
        binary = self.configured(text="LOCATION:PATH=\n")
        self.assertEqual(build.cache_path(binary, "LOCATION", fallback), fallback)

    def test_unknown_preset_does_not_run_commands(self):
        with mock.patch.object(build.devenv, "run") as run, \
                mock.patch.object(sys, "argv", ["build.py", "unknown"]):
            with self.assertRaisesRegex(SystemExit, "Unknown build preset"):
                build.main()
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
