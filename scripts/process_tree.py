"""Notice when the process a helper answers to is gone, stop a helper's whole tree, and let a
helper's work give way to whoever is using the machine.

A push interrupted at the terminal ends git but not the hook git was running, nor the
lint under that hook, nor the clang-tidy workers under the lint: on Windows the hook
runs behind the shell and batch wrappers of a Python shim and never sees the interrupt.
So a helper watches the process it answers to and takes its own children down with it,
and tethers the children it must not leave behind, which then end with it even when it is
killed outright, as a timeout kills. A helper that keeps every CPU busy for long lowers its
own priority first, and the children it starts inherit it.
"""

from __future__ import annotations

import ctypes
import os
import pathlib
import select
import signal
import subprocess
import sys
import threading
import time

WINDOWS = sys.platform == "win32"
GIT = ("git", "git.exe")
BELOW_NORMAL_PRIORITY_CLASS = 0x4000
IDLE_PRIORITY_CLASS = 0x40
NICENESS_STEP = 10  # how much nicer a POSIX helper makes itself
PR_SET_PDEATHSIG = 1  # prctl: the signal a child gets once the thread that started it ends
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9  # SetInformationJobObject's class for those limits
PROCESS_SET_QUOTA = 0x0100
PROCESS_TERMINATE = 0x0001

_tether_lock = threading.Lock()
_job = None  # Windows: the job tethered children join; 0 once the system refused one
_prctl = None  # Linux: libc's prctl; False where there is none


def table() -> dict[int, tuple[int, str]]:
    """Every running process: id -> (parent id, executable name)."""
    return _windows_table() if WINDOWS else _proc_table()


def nearest_ancestor(processes: dict[int, tuple[int, str]], pid: int,
                     names: tuple[str, ...]) -> int | None:
    """The closest ancestor of `pid` whose executable is one of `names`, if it is running."""
    wanted = {name.lower() for name in names}
    seen = {pid}
    current = processes.get(pid, (None, ""))[0]
    while current in processes and current not in seen:
        seen.add(current)
        parent, name = processes[current]
        if name.lower() in wanted:
            return current
        current = parent
    return None


def wait_for_exit(pid: int) -> bool:
    """Block until the process is gone; False when it cannot be watched at all."""
    if WINDOWS:
        return _windows_wait(pid)
    try:
        descriptor = os.pidfd_open(pid)  # exact: the descriptor is readable once it exits
    except (AttributeError, OSError):
        descriptor = None
    if descriptor is not None:
        try:
            select.select([descriptor], [], [])
        finally:
            os.close(descriptor)
        return True
    while True:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        except PermissionError:
            pass  # alive, owned by someone else
        time.sleep(0.5)


def stop_when_gone(pid: int, callback) -> threading.Thread:
    """Call back on a daemon thread once `pid` has exited."""
    def watch():
        if wait_for_exit(pid):
            callback()
    thread = threading.Thread(target=watch, name=f"watch-{pid}", daemon=True)
    thread.start()
    return thread


def start(command: list[str], **kwargs) -> subprocess.Popen:
    """Start a child whose whole tree `stop` can end."""
    if not WINDOWS:
        kwargs.setdefault("start_new_session", True)
    return subprocess.Popen(command, **kwargs)


def stop(process: subprocess.Popen) -> None:
    """End a child started by `start`, and every process under it."""
    if process.poll() is not None:
        return
    if WINDOWS:
        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                       capture_output=True, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def start_tethered(command: list[str], **kwargs) -> subprocess.Popen:
    """Start a child that ends with this process however this process ends, killed outright
    included. On Windows it joins, as soon as it has started, a job object that kills what
    it holds once its last handle, which only this process has, closes (a kill in the
    instant before it joins misses it); on Linux it asks for SIGKILL once the thread that
    started it ends, so that thread must outlive it. Elsewhere, or where the system
    refuses, it is an ordinary child, which the other ways of ending one still reach."""
    if WINDOWS:
        process = subprocess.Popen(command, **kwargs)
        job = _windows_job()
        if job:
            _join_job(job, process.pid)
        return process
    prctl = _linux_prctl()
    if prctl:
        kwargs["preexec_fn"] = _death_signal_request(prctl, os.getpid(), kwargs.get("preexec_fn"))
    return subprocess.Popen(command, **kwargs)


def run_tethered(command: list[str], timeout: float | None = None,
                 **kwargs) -> subprocess.CompletedProcess:
    """subprocess.run for a child started by start_tethered and waited for by the thread
    that starts it, as Linux's tether needs; a timeout, or any other error while it runs,
    ends the child before the error is passed on."""
    with start_tethered(command, **kwargs) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            process.kill()
            raise
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def _death_signal_request(prctl, parent: int, previous=None):
    """What a forked child runs before it executes its program, to die with `parent`."""
    def request():
        if previous is not None:
            previous()
        prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)
        if os.getppid() != parent:  # the parent ended before the request took hold
            os._exit(1)
    return request


def _linux_prctl():
    global _prctl
    with _tether_lock:
        if _prctl is None:
            _prctl = False
            if sys.platform.startswith("linux"):
                try:
                    prctl = ctypes.CDLL(None, use_errno=True).prctl
                    prctl.argtypes = [ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong,
                                      ctypes.c_ulong, ctypes.c_ulong]
                    prctl.restype = ctypes.c_int
                    _prctl = prctl
                except (OSError, AttributeError):
                    pass
        return _prctl or None


def _windows_job():
    """The job this process's tethered children join, made once and held until it ends."""
    global _job
    with _tether_lock:
        if _job is None:
            _job = _create_kill_on_close_job() or 0
        return _job or None


def _job_api():
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    kernel32.SetInformationJobObject.restype = wintypes.BOOL
    kernel32.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                                 wintypes.DWORD]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    return kernel32


def _create_kill_on_close_job():
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64),
                    ("PerJobUserTimeLimit", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                    ("MinimumWorkingSetSize", ctypes.c_size_t),
                    ("MaximumWorkingSetSize", ctypes.c_size_t),
                    ("ActiveProcessLimit", wintypes.DWORD), ("Affinity", ctypes.c_size_t),
                    ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_uint64) for name in (
            "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
            "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", IoCounters),
                    ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                    ("PeakProcessMemoryUsed", ctypes.c_size_t),
                    ("PeakJobMemoryUsed", ctypes.c_size_t)]

    try:
        kernel32 = _job_api()
        job = kernel32.CreateJobObjectW(None, None)  # not inheritable: no child keeps it open
        if not job:
            return None
        limits = ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(job, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                                ctypes.byref(limits), ctypes.sizeof(limits)):
            kernel32.CloseHandle(job)
            return None
        return job
    except (OSError, AttributeError):
        return None


def _join_job(job, pid: int) -> bool:
    try:
        kernel32 = _job_api()
        handle = kernel32.OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, False, pid)
        if not handle:
            return False
        try:
            return bool(kernel32.AssignProcessToJobObject(job, handle))
        finally:
            kernel32.CloseHandle(handle)
    except (OSError, AttributeError):
        return False


def lower_priority() -> bool:
    """Lower this process's scheduling priority, which the children it starts afterwards
    inherit: below normal on Windows (never raising a lower class), NICENESS_STEP nicer on
    POSIX, where the nice value is a thread's, so call it before starting any thread.
    Whether it took; a refusal is never an error."""
    try:
        return _lower_windows_priority() if WINDOWS else _lower_posix_priority()
    except (OSError, AttributeError, ValueError):
        return False


def _lower_posix_priority() -> bool:
    os.nice(NICENESS_STEP)
    return True


def _lower_windows_priority() -> bool:
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    kernel32.GetPriorityClass.argtypes = [ctypes.c_void_p]
    kernel32.GetPriorityClass.restype = wintypes.DWORD
    kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    kernel32.SetPriorityClass.restype = wintypes.BOOL
    process = kernel32.GetCurrentProcess()
    if kernel32.GetPriorityClass(process) in (BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS):
        return True
    return bool(kernel32.SetPriorityClass(process, BELOW_NORMAL_PRIORITY_CLASS))


def _proc_table() -> dict[int, tuple[int, str]]:
    processes = {}
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue  # it exited while being listed
        opened, closed = stat.find("("), stat.rfind(")")
        if opened < 0 or closed < 0:
            continue
        parent = int(stat[closed + 1:].split()[1])
        processes[int(entry.name)] = (parent, stat[opened + 1:closed])
    return processes


def _windows_table() -> dict[int, tuple[int, str]]:
    from ctypes import wintypes

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                    ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                    ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_wchar * 260)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    snapshot = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if not snapshot or snapshot == ctypes.c_void_p(-1).value:
        return {}
    processes = {}
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            processes[entry.th32ProcessID] = (entry.th32ParentProcessID, entry.szExeFile)
            more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return processes


def _windows_wait(pid: int) -> bool:
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return ctypes.get_last_error() == 87  # ERROR_INVALID_PARAMETER: no such process
    try:
        kernel32.WaitForSingleObject(handle, 0xFFFFFFFF)
    finally:
        kernel32.CloseHandle(handle)
    return True
