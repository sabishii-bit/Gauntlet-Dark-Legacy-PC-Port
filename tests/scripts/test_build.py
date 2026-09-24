"""The build launcher forwards preview arguments after building the selected preset."""
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import build


class BuildLaunchTests(unittest.TestCase):
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
