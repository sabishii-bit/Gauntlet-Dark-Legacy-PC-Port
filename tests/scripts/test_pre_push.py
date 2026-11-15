"""The pre-push hook lints what is pushed and dies with the git that ran it."""
import contextlib
import importlib.machinery
import importlib.util
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
HOOK = ROOT / "scripts" / "hooks" / "pre-push"


def load_hook():
    """The hook as a module, leaving no __pycache__ beside it for setup.py to trip over."""
    loader = importlib.machinery.SourceFileLoader("pre_push", str(HOOK))
    spec = importlib.util.spec_from_loader("pre_push", loader)
    module = importlib.util.module_from_spec(spec)
    previous = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        loader.exec_module(module)
    finally:
        sys.dont_write_bytecode = previous
    return module


class RangeTests(unittest.TestCase):
    def test_updated_and_new_refs_are_linted_and_deletions_skipped(self):
        hook = load_hook()
        zero = "0" * 40
        lines = ["refs/heads/main aaa refs/heads/main bbb",
                 "refs/heads/new ccc refs/heads/new " + zero,
                 f"(delete) {zero} refs/heads/old ddd",
                 "malformed line"]
        self.assertEqual(hook.pushed_ranges(lines), [("bbb", "aaa"), (None, "ccc")])


class GateTests(unittest.TestCase):
    def test_a_push_is_linted_by_the_lint_defaults(self):
        hook = load_hook()
        pushed = io.StringIO("refs/heads/main bbb refs/heads/main aaa\n")
        with mock.patch.dict(os.environ, {"GDL_SKIP_LINT": ""}), \
             mock.patch.object(hook.sys, "stdin", pushed), \
             mock.patch.object(hook, "base_for", return_value="aaa"), \
             mock.patch.object(hook, "run_lint", return_value=0) as run, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(hook.main(), 0)
        arguments = run.call_args.args[0]
        self.assertEqual(arguments, ["--changed", "aaa..bbb"])

        # Those arguments are the gate: one unit for a changed header, its own .cpp, with
        # the checks .clang-tidy enables.
        import lint
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            files = {"src/a/Thing.h": "", "src/a/Thing.cpp": '#include "a/Thing.h"\n',
                     "src/b/User.cpp": '#include "a/Thing.h"\n',
                     "tests/a/ThingTests.cpp": '#include "a/Thing.h"\n'}
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text, encoding="utf-8")
            roster = sorted(root / name for name in files if name.endswith(".cpp"))
            with mock.patch.object(lint, "ROOT", root), \
                 mock.patch.object(lint.process_tree, "lower_priority", return_value=True), \
                 mock.patch.object(lint, "find_clang_tidy", return_value="clang-tidy"), \
                 mock.patch.object(lint, "translation_units", return_value=roster), \
                 mock.patch.object(lint, "changed_files", return_value=[root / "src/a/Thing.h"]), \
                 mock.patch.object(lint, "run_one", return_value=[]) as tidy, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(lint.main(arguments), 0)
            self.assertEqual([call.args[1:] for call in tidy.call_args_list],
                             [(root / "src/a/Thing.cpp", "fast")])


class LintLifetimeTests(unittest.TestCase):
    def test_the_lint_ends_once_the_watched_git_is_gone(self):
        hook = load_hook()
        git = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        self.addCleanup(git.wait)
        self.addCleanup(git.kill)
        with tempfile.TemporaryDirectory() as directory:
            sleeper = pathlib.Path(directory) / "lint.py"
            sleeper.write_text("import time\ntime.sleep(60)\n", encoding="utf-8")
            outcome = {}
            with mock.patch.object(hook, "LINT", sleeper):
                thread = threading.Thread(
                    target=lambda: outcome.update(code=hook.run_lint([], git.pid)))
                thread.start()
                time.sleep(0.5)
                self.assertTrue(thread.is_alive(), "the lint should still be running")
                git.kill()
                git.wait()
                thread.join(timeout=15)
            self.assertFalse(thread.is_alive(), "the lint should have been stopped")
            self.assertNotEqual(outcome["code"], 0)

    def test_a_lint_that_finishes_reports_its_own_exit_code(self):
        hook = load_hook()
        with tempfile.TemporaryDirectory() as directory:
            script = pathlib.Path(directory) / "lint.py"
            script.write_text("import sys\nsys.exit(3)\n", encoding="utf-8")
            with mock.patch.object(hook, "LINT", script):
                self.assertEqual(hook.run_lint([]), 3)


if __name__ == "__main__":
    unittest.main()
