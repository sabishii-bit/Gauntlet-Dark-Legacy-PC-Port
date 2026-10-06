"""Native gates cannot mistake skipped, missing or empty evidence for a pass."""
import pathlib
import json
import sys
import tempfile
import unittest
from unittest import mock
from subprocess import CompletedProcess

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "scripts"))
import asset_audit


class AssetAuditTests(unittest.TestCase):
    def test_scan_requires_current_nonempty_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            report = pathlib.Path(directory) / "audit.json"
            with self.assertRaises(FileNotFoundError):
                asset_audit.require_complete_scan(report)
            for contents, valid in (
                ({"schema": 1, "archives": [{"directory": "MONSTERS/DEM"}], "errors": 0}, True),
                ({"schema": 1, "archives": [], "errors": 0}, False),
                ({"schema": 1, "archives": [{}], "errors": 1}, False),
                ({"schema": 1, "archives": [{}]}, False),
                ({}, False),
            ):
                with self.subTest(contents=contents):
                    report.write_text(json.dumps(contents), encoding="utf-8")
                    if valid:
                        asset_audit.require_complete_scan(report)
                    else:
                        with self.assertRaises(ValueError):
                            asset_audit.require_complete_scan(report)

    def test_completion_requires_executed_passing_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            report = pathlib.Path(directory) / "results.xml"
            for contents, valid in (
                ('<testsuites><testsuite><testcase name="native"/></testsuite></testsuites>', True),
                ('<testsuites/>', False),
                ('<testsuite><testcase><skipped/></testcase></testsuite>', False),
                ('<testsuite><testcase><failure/></testcase></testsuite>', False),
                ('<testsuite><testcase><error/></testcase></testsuite>', False),
            ):
                with self.subTest(contents=contents):
                    report.write_text(contents, encoding="utf-8")
                    if valid:
                        asset_audit.require_complete_tests(report)
                    else:
                        with self.assertRaises(ValueError):
                            asset_audit.require_complete_tests(report)

    def test_archive_context_cannot_escape_the_asset_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            self.assertEqual(asset_audit.beneath(root, "MONSTERS/DEM"), root / "MONSTERS/DEM")
            with self.assertRaises(ValueError):
                asset_audit.beneath(root, "../outside")

    def test_archive_invocation_keeps_lender_order_and_propagates_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            binary = root / "build" / "test-preset"
            binary.mkdir(parents=True)
            args = ["asset_audit.py", "test-preset", "--assets", str(root),
                    "--archive", "MONSTERS/DEM", "--lender", "LEVELS/LEVELB4",
                    "--lender", "ITEMS/LEVELB"]
            with mock.patch.object(asset_audit.devenv, "ROOT", root), \
                    mock.patch.object(asset_audit.build, "build_presets", return_value={"test-preset": "test-preset"}), \
                    mock.patch.object(asset_audit.devenv, "run"), \
                    mock.patch.object(asset_audit.subprocess, "run", return_value=CompletedProcess([], 1)) as run, \
                    mock.patch.object(sys, "argv", args):
                self.assertEqual(asset_audit.main(), 1)
                command = run.call_args.args[0]
                self.assertNotIn("--decode-only", command)
                self.assertEqual(command[-4:], ["--lender", str(root / "LEVELS/LEVELB4"),
                                                "--lender", str(root / "ITEMS/LEVELB")])

    def test_zero_exit_cannot_reuse_a_stale_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            binary = root / "build" / "test-preset"
            binary.mkdir(parents=True)
            report = binary / "asset-audit.json"
            report.write_text('{"schema":1,"archives":[{}],"errors":0}', encoding="utf-8")
            with mock.patch.object(asset_audit.devenv, "ROOT", root), \
                    mock.patch.object(asset_audit.build, "build_presets", return_value={"test-preset": "test-preset"}), \
                    mock.patch.object(asset_audit.devenv, "run"), \
                    mock.patch.object(asset_audit.subprocess, "run", return_value=CompletedProcess([], 0)), \
                    mock.patch.object(sys, "argv", ["asset_audit.py", "test-preset", "--assets", str(root)]):
                self.assertEqual(asset_audit.main(), 2)
                self.assertFalse(report.exists())


if __name__ == "__main__":
    unittest.main()
