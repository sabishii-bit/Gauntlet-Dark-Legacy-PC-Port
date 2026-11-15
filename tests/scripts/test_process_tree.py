"""A helper notices the process it answers to is gone, and ends its own tree."""
import os
import pathlib
import subprocess
import sys
import threading
import time
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import process_tree  # noqa: E402

SLEEPER = [sys.executable, "-c", "import time; time.sleep(60)"]
# What a process prints of its own scheduling priority: its class, or its nice value.
PRIORITY = ("import ctypes\n"
            "kernel32 = ctypes.WinDLL('kernel32')\n"
            "kernel32.GetCurrentProcess.restype = ctypes.c_void_p\n"
            "kernel32.GetPriorityClass.argtypes = [ctypes.c_void_p]\n"
            "print(kernel32.GetPriorityClass(kernel32.GetCurrentProcess()))\n"
            if process_tree.WINDOWS else "import os; print(os.nice(0))\n")


def listed(pid, expected, seconds=5):
    """Whether the process table lists `pid`, polled until it matches `expected`."""
    deadline = time.monotonic() + seconds
    while True:
        present = pid in process_tree.table()
        if present == expected or time.monotonic() >= deadline:
            return present
        time.sleep(0.1)


class AncestorTests(unittest.TestCase):
    TABLE = {1: (0, "init"), 10: (1, "Git.exe"), 20: (10, "sh"), 30: (20, "cmd.exe"),
             40: (30, "python")}

    def test_the_nearest_named_ancestor_is_found_through_wrappers(self):
        self.assertEqual(process_tree.nearest_ancestor(self.TABLE, 40, process_tree.GIT), 10)

    def test_a_process_without_such_an_ancestor_has_none(self):
        self.assertIsNone(process_tree.nearest_ancestor(self.TABLE, 10, process_tree.GIT))
        self.assertIsNone(process_tree.nearest_ancestor(self.TABLE, 99, process_tree.GIT))

    def test_a_reused_parent_id_cannot_loop_forever(self):
        self.assertIsNone(process_tree.nearest_ancestor({5: (6, "a"), 6: (5, "b")}, 5, ("git",)))

    def test_the_live_table_lists_this_process_under_its_parent(self):
        table = process_tree.table()
        self.assertIn(os.getpid(), table)
        parent, name = table[os.getpid()]
        self.assertEqual(parent, os.getppid())
        self.assertIn("python", name.lower())


class ExitTests(unittest.TestCase):
    def test_the_callback_runs_once_the_process_exits(self):
        child = subprocess.Popen(SLEEPER)
        gone = threading.Event()
        process_tree.stop_when_gone(child.pid, gone.set)
        self.assertFalse(gone.wait(0.5))
        child.kill()
        child.wait()
        self.assertTrue(gone.wait(5))

    def test_an_exited_process_is_already_gone(self):
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait()
        started = time.monotonic()
        self.assertTrue(process_tree.wait_for_exit(child.pid))
        self.assertLess(time.monotonic() - started, 5)


class TreeTests(unittest.TestCase):
    def test_stop_ends_the_child_and_the_process_under_it(self):
        script = ("import subprocess, sys, time\n"
                  "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                  "print(child.pid, flush=True)\n"
                  "time.sleep(60)\n")
        process = process_tree.start([sys.executable, "-c", script], stdout=subprocess.PIPE,
                                     text=True)
        grandchild = int(process.stdout.readline())
        self.assertTrue(listed(grandchild, True))
        process_tree.stop(process)
        self.assertIsNotNone(process.poll())
        self.assertFalse(listed(grandchild, False))

    def test_stopping_an_exited_child_is_harmless(self):
        process = process_tree.start([sys.executable, "-c", "pass"])
        process.wait()
        process_tree.stop(process)
        self.assertEqual(process.returncode, 0)


class PriorityTests(unittest.TestCase):
    def test_a_helper_lowers_its_priority_and_its_children_inherit_it(self):
        # In a helper of its own, started at normal priority: this runner's is left alone.
        helper = (f"import subprocess, sys\n"
                  f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
                  f"import process_tree\n"
                  f"print(process_tree.lower_priority())\n"
                  f"print(subprocess.run([sys.executable, '-c', {PRIORITY!r}], capture_output=True,"
                  f" text=True, check=True).stdout.strip())\n")
        options = {}
        if process_tree.WINDOWS:
            options["creationflags"] = subprocess.NORMAL_PRIORITY_CLASS
        result = subprocess.run([sys.executable, "-c", helper], capture_output=True, text=True,
                                check=True, timeout=60, **options)
        took, child = result.stdout.split()
        self.assertEqual(took, "True")
        if process_tree.WINDOWS:
            self.assertEqual(int(child), process_tree.BELOW_NORMAL_PRIORITY_CLASS)
        else:
            self.assertEqual(int(child), min(19, os.nice(0) + process_tree.NICENESS_STEP))

    def test_a_refusal_is_not_an_error(self):
        lowering = "_lower_windows_priority" if process_tree.WINDOWS else "_lower_posix_priority"
        for error in (OSError("refused"), AttributeError("no such call")):
            with self.subTest(error=error), \
                 mock.patch.object(process_tree, lowering, side_effect=error):
                self.assertFalse(process_tree.lower_priority())


if __name__ == "__main__":
    unittest.main()
