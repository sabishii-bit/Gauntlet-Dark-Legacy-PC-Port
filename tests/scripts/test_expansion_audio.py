"""The OFF-backend smoke uses an isolated cache and never invokes the game."""

import pathlib
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts" / "checks"))
import expansion_audio


class ExpansionAudioTests(unittest.TestCase):
    def test_fresh_check_targets_only_isolated_headless_audio(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            result = expansion_audio.commands(root, "linux-ninja-debug", 2)
            configure, compile_command, test = result
            self.assertIn("-DGDL_LEGENDS_AUDIO=OFF", configure)
            self.assertIn("-DGDL_ENABLE_NETPLAY=OFF", configure)
            self.assertIn("-DGDL_EMBED_DISC_ICON=OFF", configure)
            self.assertIn("-DGDL_BUILD_TESTS=ON", configure)
            self.assertIn("-DVCPKG_MANIFEST_INSTALL=ON", configure)
            isolated = root / "build/expansion-audio-off"
            self.assertEqual(configure[configure.index("-B") + 1], str(isolated))
            self.assertEqual(compile_command[compile_command.index("--target") + 1],
                             "expansion_audio_tests")
            self.assertEqual(compile_command[-2:], ["--parallel", "2"])
            self.assertIn("^expansion_audio_smoke$", test)
            self.assertIn("--no-tests=error", test)
            self.assertEqual(list(root.iterdir()), [])

    def test_valid_installed_tree_is_reused_without_manifest_install(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            binary = root / "build/linux-ninja-debug"
            installed = binary / "vcpkg_installed"
            (installed / "x64-linux/include").mkdir(parents=True)
            (installed / "vcpkg").mkdir()
            (installed / "vcpkg/status").write_text("Package: catch2\n", encoding="utf-8")
            cache = (f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
                     "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                     "VCPKG_TARGET_TRIPLET:STRING=x64-linux\n"
                     f"VCPKG_INSTALLED_DIR:PATH={installed}\n")
            (binary / "CMakeCache.txt").write_text(cache, encoding="utf-8")
            configure = expansion_audio.commands(root, "linux-ninja-debug", 4)[0]
            self.assertIn(f"-DVCPKG_INSTALLED_DIR={installed}", configure)
            self.assertIn("-DVCPKG_MANIFEST_INSTALL=OFF", configure)
            self.assertEqual((binary / "CMakeCache.txt").read_text(encoding="utf-8"), cache)

    def test_foreign_or_incompatible_cache_is_never_reconfigured(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            valid = {"CMAKE_HOME_DIRECTORY": str(root), "CMAKE_GENERATOR": "Ninja",
                     "CMAKE_BUILD_TYPE": "Debug", "VCPKG_TARGET_TRIPLET": "x64-linux"}
            for changes in ({"CMAKE_HOME_DIRECTORY": str(root / "foreign")},
                            {"CMAKE_GENERATOR": "Visual Studio 17 2022"},
                            {"CMAKE_BUILD_TYPE": "RelWithDebInfo"},
                            {"VCPKG_TARGET_TRIPLET": "x64-windows-static"}):
                with self.subTest(changes=changes), mock.patch.object(
                        expansion_audio, "read_cache", side_effect=[valid, {**valid, **changes}]):
                    with self.assertRaises(ValueError):
                        expansion_audio.commands(root, "linux-ninja-debug", 4)

    def test_jobs_are_bounded(self):
        for jobs in (0, 5, 999):
            with self.subTest(jobs=jobs), self.assertRaises(ValueError):
                expansion_audio.commands(pathlib.Path.cwd(), "linux-ninja-debug", jobs)

    def test_detected_compiler_accepts_cl_alias_but_rejects_different_toolchains(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            ordinary = root / "build/windows-ninja-release"
            isolated = root / "build/expansion-audio-off"
            compiler = root / "MSVC/14.44.35207/bin/cl.exe"

            def write_cache(binary, spelling):
                binary.mkdir(parents=True, exist_ok=True)
                (binary / "CMakeCache.txt").write_text(
                    f"CMAKE_HOME_DIRECTORY:INTERNAL={root}\n"
                    "CMAKE_GENERATOR:INTERNAL=Ninja\n"
                    "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\n"
                    "VCPKG_TARGET_TRIPLET:STRING=x64-windows-static\n"
                    f"CMAKE_CXX_COMPILER:UNINITIALIZED={spelling}\n"
                    "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3\n"
                    "CMAKE_CACHE_MINOR_VERSION:INTERNAL=31\n"
                    "CMAKE_CACHE_PATCH_VERSION:INTERNAL=6\n", encoding="utf-8")

            def write_compiler(binary, executable, version="19.44.35209.0", cmake="3.31.6-msvc6"):
                target = binary / "CMakeFiles" / cmake / "CMakeCXXCompiler.cmake"
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(f'set(CMAKE_CXX_COMPILER "{executable.as_posix()}")\n'
                                  'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                                  f'set(CMAKE_CXX_COMPILER_VERSION "{version}")\n'
                                  'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)\n', encoding="utf-8")

            write_cache(ordinary, "cl")
            write_cache(isolated, compiler.as_posix())
            write_compiler(ordinary, compiler)
            write_compiler(isolated, compiler)
            self.assertEqual(len(expansion_audio.commands(root, "windows-ninja-release", 4)), 3)
            # An old CMake installation's record must not shadow the current one.
            write_compiler(ordinary, root / "old/cl.exe", "19.30", "3.29.1")
            self.assertEqual(len(expansion_audio.commands(root, "windows-ninja-release", 4)), 3)
            # Identical basenames and even identical cache aliases do not admit
            # another toolchain or a compiler upgraded in place.
            write_cache(isolated, "cl")
            for executable, version in ((root / "MSVC/14.43/bin/cl.exe", "19.43"),
                                        (compiler, "19.45")):
                write_compiler(isolated, executable, version)
                with self.subTest(executable=executable, version=version), self.assertRaisesRegex(
                        ValueError, "different detected C\\+\\+ compiler"):
                    expansion_audio.commands(root, "windows-ninja-release", 4)

    def test_unresolved_or_missing_detected_compiler_cannot_prove_equivalence(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory)
            with self.assertRaises(ValueError):
                expansion_audio.compiler_identity(binary, {})
            record = binary / "CMakeFiles/3.31.6/CMakeCXXCompiler.cmake"
            record.parent.mkdir(parents=True)
            record.write_text('set(CMAKE_CXX_COMPILER "cl")\n'
                              'set(CMAKE_CXX_COMPILER_ID "MSVC")\n'
                              'set(CMAKE_CXX_COMPILER_VERSION "19.44")\n', encoding="utf-8")
            with self.assertRaises(ValueError):
                expansion_audio.compiler_identity(binary, {})

    def test_main_runs_all_steps_through_shared_developer_environment(self):
        with mock.patch.object(expansion_audio.build, "build_presets",
                               return_value={"linux-ninja-debug": "linux-ninja-debug"}), \
                mock.patch.object(expansion_audio, "commands", return_value=[["cmake"], ["ctest"]]), \
                mock.patch.object(expansion_audio.devenv, "run") as run:
            self.assertEqual(expansion_audio.main(["--preset", "linux-ninja-debug"]), 0)
            self.assertEqual(run.call_args_list, [mock.call(["cmake"]), mock.call(["ctest"])])


if __name__ == "__main__":
    unittest.main()
