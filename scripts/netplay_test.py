#!/usr/bin/env python3
"""Exercise iroh in separate host/guest processes without graphics or assets.

    python scripts/netplay_test.py --build

Local tests never contact a relay. --internet explicitly enables public relays;
a same-machine run still does not prove cross-router connectivity or relay use.
"""
import argparse
import contextlib
import json
import pathlib
import subprocess
import sys
import tempfile
import time

import devenv

ROOT = devenv.ROOT


def records(path: pathlib.Path) -> list[dict]:
    result = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            result.append(value)
    return result


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def verify_log(path: pathlib.Path, role: str, code: int) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    rows = records(path)
    passes = [row for row in rows if row.get("event") == "passed" and row.get("role") == role]
    if code != 0 or len(passes) != 1 or "gdl1-" in text:
        raise RuntimeError(f"{role} failed or leaked its invitation: {path}")
    if role == "client" and (passes[0].get("ticks") != 120 or passes[0].get("ordered_controls") != 15):
        raise RuntimeError(f"Incomplete input/control exchange: {path}")
    if role == "host":
        peers = [row for row in rows if row.get("event") == "peer_done"]
        count = passes[0].get("clients")
        if (count not in (1, 2, 3) or len(peers) != count or
                len({row.get("seat") for row in peers}) != count or
                any(type(row.get("accepted")) is not int or row["accepted"] < 60 for row in peers)):
            raise RuntimeError(f"Missing peer input evidence: {path}")


def wait_invitation(process: subprocess.Popen, path: pathlib.Path, log: pathlib.Path) -> None:
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Host exited before inviting guests: {log}")
        if path.is_file():
            text = path.read_text(encoding="utf-8").strip()
            if text.startswith("gdl1-") and 6 < len(text) <= 2048:
                return
        time.sleep(0.02)
    raise RuntimeError(f"Host invitation timed out: {log}")


def build(directory: pathlib.Path, jobs: int = 2) -> None:
    options = ";".join(f"--x-{name}-root={(directory / ('vcpkg-' + name)).as_posix()}"
                       for name in ("buildtrees", "packages"))
    devenv.run(["cmake", "--preset", devenv.release_preset(), "-B", str(directory),
                "-DGDL_ENABLE_NETPLAY=ON", "-DGDL_BUILD_TESTS=ON", "-DVCPKG_MANIFEST_FEATURES=",
                "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF",
                f"-DVCPKG_INSTALL_OPTIONS={options}"])
    devenv.run(["cmake", "--build", str(directory), "--target", "netplaycheck", "netplaytests", "compile_commands",
                "--parallel", str(jobs)])


def run(executable: pathlib.Path, directory: pathlib.Path, clients: int, internet: bool) -> None:
    with contextlib.ExitStack() as stack:
        invitation = directory / "invitation.txt"
        stack.callback(invitation.unlink, missing_ok=True)

        def launch(name, *args):
            log = directory / f"{name}.log"
            stream = stack.enter_context(log.open("w", encoding="utf-8"))
            process = subprocess.Popen([str(executable), *args], cwd=ROOT, stdin=subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW if devenv.WINDOWS else 0)
            stack.callback(stop, process)
            return process, log

        suffix = ["internet"] if internet else []
        host, host_log = launch("host", "host", str(invitation), str(clients), *suffix)
        wait_invitation(host, invitation, host_log)
        guests = [launch(f"guest-{i}", "client", str(invitation), "unused", *suffix)
                  for i in range(clients)]
        for process, log in guests:
            verify_log(log, "client", process.wait(timeout=40))
        verify_log(host_log, "host", host.wait(timeout=10))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build/netplay")
    parser.add_argument("--runs", type=int, choices=range(1, 21), default=1)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=2)
    parser.add_argument("--internet", action="store_true")
    args = parser.parse_args(argv)
    try:
        directory = args.build_dir.resolve()
        if args.build:
            build(directory, args.jobs)
        executable = directory / "bin" / ("netplaycheck.exe" if devenv.WINDOWS else "netplaycheck")
        if not executable.is_file():
            raise ValueError(f"{executable} is missing; add --build")
        checks = directory / "bin" / ("netplaytests.exe" if devenv.WINDOWS else "netplaytests")
        subprocess.run([str(checks)], cwd=ROOT, timeout=60, check=True)
        logs = pathlib.Path(tempfile.mkdtemp(prefix="network-test-", dir=directory))
        print(f"Network diagnostics: {logs}", flush=True)
        with (logs / "self-test.log").open("w", encoding="utf-8") as stream:
            result = subprocess.run([str(executable), "self-test"], cwd=ROOT, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=30, check=False)
        verify_log(logs / "self-test.log", "self-test", result.returncode)
        for attempt in range(args.runs):
            for clients in (1, 3):
                target = logs / f"run-{attempt}-{clients}"
                target.mkdir()
                run(executable, target, clients, args.internet)
        print("Passed real-process input delivery, reliable ordering, backpressure and teardown.")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Netplay test failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
