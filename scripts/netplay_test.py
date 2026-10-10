#!/usr/bin/env python3
"""Run a headless host and three clients over loopback UDP, clean and with lag/loss.

    python scripts/netplay_test.py --build
    python scripts/netplay_test.py

The opt-in build lives in build/netplay; normal game presets are unchanged. Uses
the pinned vcpkg feature, no extra Python packages, game assets, accounts, Docker,
router changes or public listeners. Logs survive failures under the build folder.
This validates transport, room admission, authoritative movement snapshots and
local ICE connections, not a complete online game or internet NAT traversal.
Room tests use local ICE candidates;
they do not contact a public coordinator, STUN or TURN server.
"""

import argparse
import contextlib
import copy
import http.server
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time

import devenv
import netplay_rooms

ROOT = pathlib.Path(__file__).resolve().parents[1]


def records(path: pathlib.Path) -> list[dict]:
    """Library diagnostic text and partial lines are not harness records."""
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
    """Only stop our own child, and always reap it before releasing its log."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def listener_port(process: subprocess.Popen, log: pathlib.Path, deadline: float) -> int:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Host exited during startup (exit {process.returncode}): {log}")
        for record in records(log):
            if record.get("event") == "listening":
                port = record.get("port")
                if type(port) is not int or not 1 <= port <= 65535:
                    raise RuntimeError(f"Host reported an invalid port: {log}")
                return port
        time.sleep(0.02)
    raise RuntimeError(f"Host startup timed out: {log}")


def verify_log(log: pathlib.Path, role: str, returncode: int) -> None:
    if any(marker in log.read_text(encoding="utf-8", errors="replace")
           for marker in ("pwd_frag:", "private_key:", '"token":')):
        raise RuntimeError(f"Network diagnostics exposed private handshake material: {log}")
    entries = records(log)
    if returncode != 0 or any(entry.get("event") == "failed" for entry in entries):
        raise RuntimeError(f"{role} failed (exit {returncode}): {log}")
    if not any(entry.get("event") == "passed" and entry.get("role") == role for entry in entries):
        raise RuntimeError(f"{role} exited without a passing verdict: {log}")


def verify_room_motion(log: pathlib.Path, role: str) -> None:
    """Require evidence of simulation and live output, not just a successful handshake."""
    passed = [row for row in records(log) if row.get("event") == "passed" and row.get("role") == role]
    if len(passed) != 1:
        raise RuntimeError(f"Missing unique motion verdict: {log}")
    row = passed[0]
    if (type(row.get("match_epoch")) is not int or row["match_epoch"] != 2
            or type(row.get("load_barrier_checks")) is not int or row["load_barrier_checks"] < 1):
        raise RuntimeError(f"Match did not exercise the shared loading barrier: {log}")
    if role == "room-host":
        if type(row.get("simulation_ticks")) is not int or row["simulation_ticks"] != 120:
            raise RuntimeError(f"Host did not advance the simulation: {log}")
    elif role == "room-client":
        if (type(row.get("snapshots_received")) is not int or row["snapshots_received"] < 10
                or type(row.get("ticks")) is not int or row["ticks"] != 120):
            raise RuntimeError(f"Client did not exchange live movement: {log}")
        if (type(row.get("combat_tick")) is not int or row["combat_tick"] != 119
                or type(row.get("combat_enemies")) is not int or row["combat_enemies"] != 25):
            raise RuntimeError(f"Client did not converge to the combat checkpoint: {log}")
        if type(row.get("geometry_objects")) is not int or row["geometry_objects"] != 806:
            raise RuntimeError(f"Client did not converge to the dense geometry checkpoint: {log}")
        if (type(row.get("projectile_snapshots")) is not int or row["projectile_snapshots"] < 5
                or type(row.get("combat_projectiles")) is not int or row["combat_projectiles"] != 0):
            raise RuntimeError(f"Client did not display and retire the projectile roster: {log}")
        if (type(row.get("pickup_snapshots")) is not int or row["pickup_snapshots"] < 5
                or type(row.get("combat_pickups")) is not int or row["combat_pickups"] != 40):
            raise RuntimeError(f"Client did not converge to the live pickup roster: {log}")
        if (type(row.get("fixture_snapshots")) is not int or row["fixture_snapshots"] < 5
                or type(row.get("combat_fixtures")) is not int or row["combat_fixtures"] != 78):
            raise RuntimeError(f"Client did not converge to the live fixture roster: {log}")
        if (type(row.get("fighter_snapshots")) is not int or row["fighter_snapshots"] < 5
                or type(row.get("combat_fighters")) is not int or row["combat_fighters"] != 18):
            raise RuntimeError(f"Client did not converge to the articulated fighter roster: {log}")
        if (type(row.get("companion_snapshots")) is not int or row["companion_snapshots"] < 5
                or type(row.get("combat_companions")) is not int or row["combat_companions"] != 7):
            raise RuntimeError(f"Client did not converge to the companion roster: {log}")
        if (type(row.get("hud_snapshots")) is not int or row["hud_snapshots"] < 5
                or type(row.get("hud_players")) is not int or row["hud_players"] != 4):
            raise RuntimeError(f"Client did not converge to the player HUD: {log}")
        if (type(row.get("hud_overlay_snapshots")) is not int
                or row["hud_overlay_snapshots"] < 5):
            raise RuntimeError(f"Client did not receive pickup and boss HUD overlays: {log}")
        if type(row.get("screen_states")) is not int or row["screen_states"] != 15:
            raise RuntimeError(f"Client did not receive every scene overlay phase: {log}")
    else:
        raise RuntimeError(f"Invalid room role: {role}")
    # Deliberately generous for loaded CI machines. Normal local setup takes
    # less than a second; previously unacknowledged Expect headers caused every
    # handshake message to stall for one second and still eventually pass.
    if type(row.get("startup_ms")) is not int or not 0 < row["startup_ms"] <= 10000:
        raise RuntimeError(f"Room setup stalled before simulation: {log}")


def run_case(executable: pathlib.Path, directory: pathlib.Path, clients: int,
             lag_ms: int, loss_percent: int) -> None:
    directory.mkdir()
    children = []
    with contextlib.ExitStack() as stack:
        def launch(name, args):
            log = directory / f"{name}.log"
            stream = stack.enter_context(log.open("w", encoding="utf-8"))
            process = subprocess.Popen([str(executable), *args], cwd=ROOT,
                                       stdin=subprocess.DEVNULL, stdout=stream,
                                       stderr=subprocess.STDOUT)
            stack.callback(stop, process)
            children.append((process, log, "host" if name == "host" else "client"))
            return process, log

        host, log = launch("host", ["host", str(clients), str(lag_ms), str(loss_percent)])
        port = listener_port(host, log, time.monotonic() + 10)
        for index in range(clients):
            launch(f"client-{index}", ["client", str(port), str(lag_ms), str(loss_percent)])
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            for process, log, role in children:
                if process.poll() is not None and process.returncode != 0:
                    verify_log(log, role, process.returncode)
            if all(process.poll() is not None for process, _, _ in children):
                break
            time.sleep(0.02)
        else:
            raise RuntimeError(f"Network run timed out: {directory}")
        for process, log, role in children:
            verify_log(log, role, process.returncode)
        peers = [row for row in records(directory / "host.log") if row.get("event") == "peer_done"]
        if len(peers) != clients or {row.get("seat") for row in peers} != set(range(clients)):
            raise RuntimeError(f"Host did not verify each assigned seat: {directory}")
        print(f"PASS: {clients} clients, {lag_ms} ms send lag, {loss_percent}% packet loss; "
              f"accepted inputs {[row['accepted'] for row in peers]}", flush=True)


def build(directory: pathlib.Path) -> None:
    # Keep dependency intermediate files beside this build, not on the drive that
    # happens to contain vcpkg. This also isolates parallel game/network builds.
    options = ";".join(f"--x-{name}-root={(directory / ('vcpkg-' + name)).as_posix()}"
                       for name in ("buildtrees", "packages"))
    devenv.run(["cmake", "--preset", devenv.release_preset(), "-B", str(directory),
                "-DGDL_ENABLE_NETPLAY=ON", "-DGDL_BUILD_TESTS=OFF",
                f"-DVCPKG_INSTALL_OPTIONS={options}"])
    devenv.run(["cmake", "--build", str(directory), "--target", "netplaycheck", "roomcheck",
                "compile_commands"])


def room_code(process: subprocess.Popen, log: pathlib.Path) -> str:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Room host exited during startup: {log}")
        for record in records(log):
            if record.get("event") == "room_created":
                code = record.get("code")
                if (not isinstance(code, str) or len(code) != 8
                        or any(c not in netplay_rooms.Directory.CODE_ALPHABET for c in code)):
                    raise RuntimeError(f"Host reported an invalid room code: {log}")
                return code
        time.sleep(0.02)
    raise RuntimeError(f"Room host startup timed out: {log}")


def run_room_case(executable: pathlib.Path, directory: pathlib.Path, endpoint: str,
                  host_players: int, guest_players: list[int], lag: int, loss: int,
                  trace: bool = False, session: bool = False) -> None:
    directory.mkdir()
    prefix = "session" if session else "room"
    host_role = f"{prefix}-host"
    guest_role = f"{prefix}-client"
    total_players = host_players + sum(guest_players)
    with contextlib.ExitStack() as stack:
        children = []

        def launch(name, args, role):
            log = directory / f"{name}.log"
            stream = stack.enter_context(log.open("w", encoding="utf-8"))
            process = subprocess.Popen([str(executable), *args, *(["--trace"] if trace else [])], cwd=ROOT,
                                       stdin=subprocess.DEVNULL, stdout=stream,
                                       stderr=subprocess.STDOUT)
            stack.callback(stop, process)
            children.append((process, log, role))
            return process, log

        host, log = launch("host", ["session-host" if session else "host", endpoint,
                                    str(host_players), str(total_players if session else len(guest_players)),
                                    str(lag), str(loss)], host_role)
        code = room_code(host, log)
        for index, players in enumerate(guest_players):
            launch(f"client-{index}", ["session-client" if session else "client", endpoint,
                                       str(players), code, str(lag), str(loss)], guest_role)
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            for process, log, role in children:
                if process.poll() is not None and process.returncode != 0:
                    verify_log(log, role, process.returncode)
            if all(process.poll() is not None for process, _, _ in children):
                break
            time.sleep(0.02)
        else:
            raise RuntimeError(f"Room/ICE test timed out: {directory}")
        for process, log, role in children:
            verify_log(log, role, process.returncode)
            if session:
                verify_session(log, role, total_players)
            else:
                verify_room_motion(log, role)
        if not session:
            seats = [r["seat"] for r in records(directory / "host.log") if r.get("event") == "seat_done"]
            if sorted(seats) != list(range(host_players, 4)):
                raise RuntimeError(f"Not all room-assigned seats verified: {directory}")
        startup = [row["startup_ms"] for _, log, _ in children for row in records(log)
                   if row.get("event") == "passed"]
        print(f"PASS: {prefix} code + ICE + motion/combat snapshots, local players {[host_players, *guest_players]}, "
              f"{lag} ms send lag, {loss}% loss; startup {startup} ms", flush=True)


def verify_session(log: pathlib.Path, role: str, players: int) -> None:
    """The application session must load, travel, pause/resume and move every seat."""
    passed = [row for row in records(log) if row.get("event") == "passed" and row.get("role") == role]
    if len(passed) != 1 or role not in ("session-host", "session-client"):
        raise RuntimeError(f"Missing unique online-session verdict: {log}")
    row = passed[0]
    for field, expected in (("epochs", 4), ("barriers", 4), ("players", players)):
        if type(row.get(field)) is not int or row[field] != expected:
            raise RuntimeError(f"Online session has incomplete {field}: {log}")
    for field, minimum, maximum in (("inputs", 120, 10000), ("startup_ms", 1, 10000)):
        if type(row.get(field)) is not int or not minimum <= row[field] <= maximum:
            raise RuntimeError(f"Online session has invalid {field}: {log}")
    if role == "session-client":
        for field, minimum in (("snapshots", 20), ("third_epoch_tick", 90), ("interpolated", 20)):
            if type(row.get(field)) is not int or row[field] < minimum:
                raise RuntimeError(f"Online session has insufficient {field}: {log}")
    elif row.get("paused") is not True:
        raise RuntimeError(f"Online session never paused: {log}")
    elif row.get("stalled") is not True:
        raise RuntimeError(f"Online session never stalled: {log}")


def run_rooms(executable: pathlib.Path, logs: pathlib.Path, trace: bool = False,
              sessions_only: bool = False) -> None:
    server = netplay_rooms.make_server()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        endpoint = f"http://127.0.0.1:{server.server_port}"
        if not sessions_only:
            run_room_case(executable, logs / "rooms-clean", endpoint, 1, [1, 1, 1], 0, 0, trace)
            run_room_case(executable, logs / "rooms-impaired", endpoint, 1, [1, 1, 1], 35, 10, trace)
            run_room_case(executable, logs / "rooms-mixed-local", endpoint, 2, [2], 35, 10, trace)
        for name, host, guests, lag, loss in (
                ("sessions-two", 1, [1], 0, 0),
                ("sessions-three", 1, [1, 1], 35, 10),
                ("sessions-four", 1, [1, 1, 1], 35, 10),
                ("sessions-mixed", 2, [2], 35, 10)):
            run_room_case(executable, logs / name, endpoint, host, guests, lag, loss, trace, session=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def run_room_contract(executable: pathlib.Path, logs: pathlib.Path) -> None:
    """Feed malformed responses to the real C++ parser, not a Python imitation."""
    peer = "a" * 32
    valid = {"peer": peer, "token": "b" * 64,
             "room": {"code": "ABCDEFGH", "host": peer, "revision": 1, "started": False,
                      "members": [{"peer": peer, "seats": [0], "ready": False}]}}
    bad = ["null", "{}", "{", "[" * 20 + "0" + "]" * 20, " " * 300001]
    for field, value in (("peer", "x"), ("token", "short")):
        response = copy.deepcopy(valid)
        response[field] = value
        bad.append(json.dumps(response))
    for field, value in (("revision", -1), ("revision", 0), ("revision", True),
                         ("started", True), ("code", "../rooms"), ("host", "0" * 32),
                         ("members", [])):
        response = copy.deepcopy(valid)
        response["room"][field] = value
        bad.append(json.dumps(response))
    for seats in ([256], [0, 0], [], [True], [0, 1]):
        response = copy.deepcopy(valid)
        response["room"]["members"][0]["seats"] = seats
        bad.append(json.dumps(response))
    response = copy.deepcopy(valid)
    response["room"]["members"].append(copy.deepcopy(response["room"]["members"][0]))
    bad.append(json.dumps(response))
    responses = queue.Queue()
    served = queue.Queue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, _format, *args):
            pass

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            data, delay = responses.get(timeout=3)
            data = data.encode("utf-8")
            time.sleep(delay)
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            try:
                self.wfile.write(data)
            except OSError:
                pass  # The client deliberately rejects oversized responses early.
            served.put(True)

    server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        for index, response in enumerate(bad):
            responses.put((response, 0))
            log = logs / f"room-contract-{index}.log"
            with log.open("w", encoding="utf-8") as stream:
                result = subprocess.run([str(executable), "reject-response",
                                         f"http://127.0.0.1:{server.server_port}"],
                                        cwd=ROOT, stdin=subprocess.DEVNULL, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=6, check=False)
            verify_log(log, "room-contract", result.returncode)
            served.get(timeout=3)  # An unreachable service is not a successful parser test.
        print(f"PASS: C++ room client rejects {len(bad)} malformed replies and unsafe endpoints", flush=True)
        responses.put(("{}", 1))
        log = logs / "room-worker.log"
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run([str(executable), "worker-test",
                                     f"http://127.0.0.1:{server.server_port}"],
                                    cwd=ROOT, stdin=subprocess.DEVNULL, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=6, check=False)
        verify_log(log, "room-worker", result.returncode)
        served.get(timeout=3)
        print("PASS: stalled service does not block polling; worker queues are bounded", flush=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=3)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", action="store_true", help="configure and build the optional transport")
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build/netplay")
    parser.add_argument("--clients", type=int, choices=(1, 2, 3), default=3)
    parser.add_argument("--suite", choices=("all", "transport", "rooms", "sessions"), default="all")
    parser.add_argument("--network-trace", action="store_true",
                        help="trace room ICE connections; logs include network addresses")
    parser.add_argument("--room-runs", type=int, choices=range(1, 21), default=1,
                        help="repeat all room profiles; any failed run fails the suite")
    args = parser.parse_args(argv)
    directory = args.build_dir.resolve()
    executable = directory / "bin" / ("netplaycheck.exe" if sys.platform == "win32" else "netplaycheck")
    try:
        if args.build:
            build(directory)
        if not executable.is_file():
            parser.error(f"{executable} not found; run with --build first")
        logs = pathlib.Path(tempfile.mkdtemp(prefix="network-test-", dir=directory))
        print(f"Network test logs: {logs}", flush=True)
        if args.suite in ("all", "transport"):
            log = logs / "self-test.log"
            with log.open("w", encoding="utf-8") as stream:
                result = subprocess.run([str(executable), "self-test"], cwd=ROOT,
                                        stdin=subprocess.DEVNULL, stdout=stream,
                                        stderr=subprocess.STDOUT, timeout=15, check=False)
            verify_log(log, "self-test", result.returncode)
            run_case(executable, logs / "clean", args.clients, 0, 0)
            run_case(executable, logs / "impaired", args.clients, 35, 10)
        if args.suite in ("all", "rooms", "sessions"):
            room_executable = directory / "bin" / ("roomcheck.exe" if sys.platform == "win32" else "roomcheck")
            if args.suite != "sessions":
                run_room_contract(room_executable, logs)
            for index in range(args.room_runs):
                round_logs = logs / f"round-{index + 1}"
                round_logs.mkdir()
                run_rooms(room_executable, round_logs, args.network_trace, args.suite == "sessions")
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired, queue.Empty) as error:
        print(f"Network test failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
