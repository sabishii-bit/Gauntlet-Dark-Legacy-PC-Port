"""A helper notices the process it answers to is gone, and ends its own tree."""
import ctypes
import os
import pathlib
import signal
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


def running(pid):
    """Whether `pid` still runs: listed, and on Linux not a zombie awaiting a reaper (an
    orphan's goes to init, which a container may not have)."""
    if process_tree.WINDOWS:
        return pid in process_tree.table()
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return stat[stat.rfind(")") + 2:][:1] not in ("Z", "X")


def settles(pid, expected, seconds=10):
    """`running(pid)`, polled until it matches `expected`."""
    deadline = time.monotonic() + seconds
    while True:
        state = running(pid)
        if state == expected or time.monotonic() >= deadline:
            return state
        time.sleep(0.1)


def end(pid):
    try:
        os.kill(pid, signal.SIGTERM)  # TerminateProcess on Windows
    except OSError:
        pass  # already gone


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


TETHERED = process_tree.WINDOWS or sys.platform.startswith("linux")


class TetherTests(unittest.TestCase):
    @unittest.skipUnless(TETHERED, "children are tethered on Windows and Linux")
    def test_a_tethered_child_ends_when_its_parent_is_killed_outright(self):
        script = (f"import subprocess, sys, time\n"
                  f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
                  f"import process_tree\n"
                  f"tethered = process_tree.start_tethered({SLEEPER!r})\n"
                  f"loose = subprocess.Popen({SLEEPER!r})\n"
                  f"print(tethered.pid, loose.pid, flush=True)\n"
                  f"time.sleep(60)\n")
        helper = subprocess.Popen([sys.executable, "-c", script], stdout=subprocess.PIPE,
                                  text=True)
        self.addCleanup(helper.wait)
        self.addCleanup(helper.kill)
        with helper.stdout:
            tethered, loose = (int(pid) for pid in helper.stdout.readline().split())
        self.addCleanup(end, tethered)
        self.addCleanup(end, loose)
        self.assertTrue(settles(tethered, True))
        helper.kill()  # TerminateProcess or SIGKILL: nothing of the helper's own code runs
        helper.wait()
        self.assertFalse(settles(tethered, False))
        # An ordinary child outlives it, which is what the tether is for.
        self.assertTrue(running(loose))

    @unittest.skipUnless(process_tree.WINDOWS, "Windows tethers through a job object")
    def test_a_tethered_child_joins_this_process_job(self):
        from ctypes import wintypes
        child = process_tree.start_tethered(SLEEPER)
        self.addCleanup(child.wait)
        self.addCleanup(child.kill)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.IsProcessInJob.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                            ctypes.POINTER(wintypes.BOOL)]
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel32.OpenProcess(0x1000, False, child.pid)  # QUERY_LIMITED_INFORMATION
        self.assertTrue(handle)
        try:
            inside = wintypes.BOOL()
            self.assertTrue(kernel32.IsProcessInJob(handle, process_tree._windows_job(),
                                                    ctypes.byref(inside)))
            self.assertTrue(inside.value)
        finally:
            kernel32.CloseHandle(handle)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux tethers by prctl")
    def test_a_tethered_child_asks_for_sigkill_once_its_parent_thread_ends(self):
        report = ("import ctypes; signal = ctypes.c_int(); "
                  "ctypes.CDLL(None).prctl(2, ctypes.byref(signal), 0, 0, 0); "  # PR_GET_PDEATHSIG
                  "print(signal.value)")
        # A request of the caller's own still runs first.
        child = process_tree.start_tethered([sys.executable, "-c", report],
                                            stdout=subprocess.PIPE, text=True,
                                            preexec_fn=lambda: os.write(1, b"first\n"))
        output, _ = child.communicate(timeout=30)
        self.assertEqual(output.split(), ["first", str(int(signal.SIGKILL))])

    def test_where_the_system_refuses_a_tether_the_child_is_ordinary(self):
        tether = "_windows_job" if process_tree.WINDOWS else "_linux_prctl"
        with mock.patch.object(process_tree, tether, return_value=None):
            child = process_tree.start_tethered([sys.executable, "-c", "print('ran')"],
                                                stdout=subprocess.PIPE, text=True)
            output, _ = child.communicate(timeout=30)
        self.assertEqual(output.strip(), "ran")


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
