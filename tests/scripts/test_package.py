"""Packaging copies original files unchanged and cannot overwrite its inputs or an old package."""
import contextlib
import io
import pathlib
import stat
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import package


class PackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl package ")
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.executable = self.root / "build/bin/gauntlet.exe"
        self.assets = self.root / "original/gAuNtLeT"
        self.data = self.root / "defaults"
        self.output = self.root / "distribution/game"
        for name, content in {
            "build/bin/gauntlet.exe": b"executable",
            "build/bin/glfw3.dll": b"runtime",
            "build/bin/tests.exe": b"not shipped",
            "build/bin/shaders/immediate.vert.spv": b"vertex shader",
            "build/bin/shaders/immediate.frag.spv": b"fragment shader",
            "original/gAuNtLeT/PDATA/WAR.WAD": b"original player table\x00\xff",
            "original/gAuNtLeT/unknown/file.bin": b"preserve unrecognized files",
            "original/CARDDEMO/icon.tpl": b"original icon",
            "defaults/config.json": b"{}",
            "defaults/text/en.json": b"{}",
        }.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        (self.assets / "EMPTY").mkdir()

    def plan(self, output=None, runtime_dirs=()):
        return package.make_plan(self.executable, self.assets, self.data,
                                  output or self.output, runtime_dirs)

    def test_stages_original_tree_bytes_casing_empty_directories_and_runtime(self):
        plan = self.plan()
        originals = {source: source.read_bytes() for source, _ in plan.files}
        package.stage(plan)
        for source, relative in plan.files:
            self.assertEqual((self.output / relative).read_bytes(), originals[source])
            self.assertEqual(source.read_bytes(), originals[source])
        self.assertTrue((self.output / "gAuNtLeT/EMPTY").is_dir())
        self.assertTrue((self.output / "CARDDEMO/icon.tpl").is_file())
        self.assertTrue((self.output / "glfw3.dll").is_file())
        self.assertFalse((self.output / "tests.exe").exists())
        self.assertFalse((self.output / "unpacked").exists())

    def test_planning_does_not_create_output(self):
        self.assertGreater(len(self.plan().files), 0)
        self.assertFalse(self.output.exists())

    def test_existing_outputs_are_refused_without_changing_them(self):
        self.output.mkdir(parents=True)
        marker = self.output / "save.json"
        marker.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.plan()
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_outputs_inside_source_trees_are_refused(self):
        for parent in (self.assets, self.assets.parent, self.data, self.executable.parent):
            with self.subTest(parent=parent), self.assertRaisesRegex(ValueError, "inside"):
                self.plan(parent / "new-package")

    def test_stage_rechecks_that_output_did_not_appear_since_planning(self):
        plan = self.plan()
        self.output.mkdir(parents=True)
        with self.assertRaises(FileExistsError):
            package.stage(plan)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_missing_inputs_are_rejected_before_writing(self):
        for relative in ("CARDDEMO", "../build/bin/shaders/immediate.vert.spv",
                         "../defaults/config.json"):
            path = self.assets.parent / relative
            renamed = path.with_name(path.name + "-held")
            path.rename(renamed)
            try:
                with self.assertRaises(ValueError):
                    self.plan()
                self.assertFalse(self.output.exists())
            finally:
                renamed.rename(path)

    def test_extra_runtime_libraries_and_conflicts(self):
        runtime = self.root / "CRT"
        runtime.mkdir()
        (runtime / "msvcp140.dll").write_bytes(b"CRT")
        plan = self.plan(runtime_dirs=(runtime,))
        self.assertIn(pathlib.Path("msvcp140.dll"), [dest for _, dest in plan.files])
        (runtime / "GLFW3.DLL").write_bytes(b"wrong runtime")
        with self.assertRaisesRegex(ValueError, "Conflicting"):
            self.plan(runtime_dirs=(runtime,))

    def test_dry_run_validates_and_reports_without_copying(self):
        with mock.patch.object(package.devenv, "WINDOWS", False), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(package.main([
                "--executable", str(self.executable), "--assets", str(self.assets),
                "--data", str(self.data), "--output", str(self.output), "--dry-run"]), 0)
        self.assertIn("Would stage", output.getvalue())
        self.assertIn("WAR.WAD", output.getvalue())
        self.assertFalse(self.output.exists())

    def test_linked_entries_in_asset_trees_are_refused(self):
        linked = self.assets / "unknown"
        original_lstat = pathlib.Path.lstat

        def lstat(path):
            if path == linked:
                return SimpleNamespace(st_mode=stat.S_IFLNK)
            return original_lstat(path)

        with mock.patch.object(pathlib.Path, "lstat", lstat), \
                self.assertRaisesRegex(ValueError, "Linked entry"):
            self.plan()
        self.assertFalse(self.output.exists())

    def test_windows_junction_is_rejected_without_pathlib_is_junction(self):
        junction = self.assets / "unknown"
        original_lstat = pathlib.Path.lstat

        def lstat(path):
            if path == junction:
                return SimpleNamespace(st_mode=stat.S_IFDIR,
                                       st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
            return original_lstat(path)

        # A junction is a directory, not a symlink; pre-3.12 pathlib has no
        # is_junction method. The lstat reparse flag must stop traversal itself.
        with mock.patch.object(pathlib.Path, "lstat", lstat), \
                self.assertRaisesRegex(ValueError, "Linked entry"):
            self.plan()
        self.assertFalse(self.output.exists())

    def test_ordinary_file_and_directory_are_not_links(self):
        self.assertFalse(package.linked_entry(self.assets))
        self.assertFalse(package.linked_entry(self.assets / "PDATA/WAR.WAD"))

    def test_msvc_crt_comes_from_the_developer_redist_directory(self):
        redist = self.root / "redist"
        crt = redist / "x64/Microsoft.VC143.CRT"
        crt.mkdir(parents=True)
        with mock.patch.object(package.devenv, "environment",
                               return_value={"VCToolsRedistDir": str(redist)}):
            self.assertEqual(package.msvc_runtime_directory(), crt)
        with mock.patch.object(package.devenv, "environment", return_value={}), \
                self.assertRaisesRegex(ValueError, "--runtime-dir"):
            package.msvc_runtime_directory()


if __name__ == "__main__":
    unittest.main()
