#!/usr/bin/env python3
"""Launch two playable game windows connected through a private local room.

    python scripts/netplay_scene.py
    python scripts/netplay_scene.py --scenario level-g1 --build
    python scripts/netplay_scene.py --seconds 45

The host runs gameplay; the guest renders its checkpoints and sends input. This
supports tower portals and direct boss entrances, not Internet play or the
post-level results/shop flow.
The host accepts keyboard/controller input; the guest uses keyboard/mouse only
so one controller cannot drive both windows. Click a window to steer its player.
Start/Enter pauses either window; only the host resumes. Escape closes the run.
Settings and unsaved test characters are isolated under build/netplay/scene-*.
"""

import argparse
import contextlib
import copy
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time

import devenv
import netplay_rooms
import netplay_test
import scenario

ROOT = devenv.ROOT
PLAYING = "Online scene test - Start pauses"


def asset_digest(root: pathlib.Path) -> str:
    """Hash native paths/content, independent of host filesystem order or casing."""
    if not root.is_dir():
        raise ValueError(f"No assets found under {root}")

    def unreadable(error: OSError) -> None:
        raise error

    entries = {}
    for directory, directories, files in os.walk(root, followlinks=False, onerror=unreadable):
        for name in directories + files:
            path = pathlib.Path(directory) / name
            info = path.lstat()
            if (stat.S_ISLNK(info.st_mode) or
                    getattr(info, "st_file_attributes", 0) &
                    getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
                raise ValueError(f"Linked assets are not supported by this test: {path}")
        for name in files:
            path = pathlib.Path(directory) / name
            key = path.relative_to(root).as_posix().casefold()
            if key in entries:
                raise ValueError(f"Ambiguous asset casing: {key}")
            entries[key] = path
    if not entries:
        raise ValueError(f"No assets found under {root}")
    digest = hashlib.sha256(b"gdl-native-tree-v1\0")
    for name, path in sorted(entries.items()):
        before = path.stat()
        content = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                content.update(block)
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError(f"Asset changed while hashing: {path}")
        encoded = name.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "little"))
        digest.update(encoded)
        digest.update(before.st_size.to_bytes(8, "little"))
        digest.update(content.digest())
    return digest.hexdigest()


def test_scenario(source: dict, guest: bool) -> dict:
    if source.get("screen", "tower") != "tower" or len(source.get("party", [])) != 1:
        raise ValueError("Choose a gameplay scenario containing exactly one local character")
    result = copy.deepcopy(source)
    result["welcome"] = False
    member = result["party"][0]
    member.pop("slot", None)
    member["player"] = 0  # physical input slot; OnlineParty maps this to the room seat
    member["name"] = "GUEST" if guest else "HOST"
    if guest:
        member["color"] = "BLU" if member.get("color") != "BLU" else "RED"
    return result


def test_config(source: dict, guest: bool, fps: int) -> dict:
    result = copy.deepcopy(source)
    result["display"].update(windowWidth=800, windowHeight=560, windowMode="windowed",
                             vsync=True, maxFrameRate=fps, sampleCount=1)
    result["timing"].update(tickRate=60, gameplayFrameRate=fps)
    result["controls"]["players"] = [
        {"device": "keyboard" if guest else "", "rumble": not guest},
        *[{"device": "none"} for _ in range(3)],
    ]
    # The guest audio event renderer is still separate work; do not double music.
    if guest:
        result["audio"]["masterVolume"] = 0
    return result


def build(directory: pathlib.Path, jobs: int) -> None:
    options = ";".join(f"--x-{name}-root={(directory / ('vcpkg-' + name)).as_posix()}"
                       for name in ("buildtrees", "packages"))
    devenv.run(["cmake", "--preset", devenv.release_preset(), "-B", str(directory),
                "-DGDL_ENABLE_NETPLAY=ON", "-DGDL_BUILD_TESTS=OFF",
                f"-DVCPKG_INSTALL_OPTIONS={options}"])
    devenv.run(["cmake", "--build", str(directory), "--target", "gauntlet",
                "--parallel", str(jobs)])


def log_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def healthy(process: subprocess.Popen, path: pathlib.Path) -> str:
    text = log_text(path)
    if (process.poll() is not None or "ERROR" in text or
            "Connection failed" in text or "Online scene stopped" in text):
        raise RuntimeError(f"Game stopped or reported a failure (exit {process.poll()}): {path}")
    return text


def playing(text: str, expected_level: str = "") -> bool:
    if not expected_level:
        return PLAYING in text
    stages = list(re.finditer(r"Online stage loaded: ([A-Z][0-9]+) \(epoch [0-9]+, (?:host|guest)\)",
                              text))
    return bool(stages and stages[-1][1] == expected_level and
                PLAYING in text[stages[-1].end():])


def checkpoint(text: str) -> tuple[int, int] | None:
    """Return only progress from the current stage, never an old scene's ticks."""
    stages = list(re.finditer(r"Online stage loaded: [A-Z][0-9]+ \(epoch ([0-9]+), (?:host|guest)\)",
                              text))
    if not stages:
        return None
    stage = stages[-1]
    samples = list(re.finditer(r"Online checkpoint progress: epoch ([0-9]+) tick ([0-9]+) "
                               r"\((?:host|guest)\)", text[stage.end():]))
    if not samples or int(samples[-1][1]) != int(stage[1]):
        return None
    return int(samples[-1][1]), int(samples[-1][2])


def advancing(first: tuple[int, int] | None, latest: tuple[int, int] | None) -> bool:
    # A later scene still has to render enough snapshots to produce a progress
    # entry; merely emitting a "stage loaded" line cannot satisfy this check.
    return first is not None and latest is not None and latest > first


def wait_room(process: subprocess.Popen, path: pathlib.Path) -> str:
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        match = re.search(r"Online run: Test room ([A-HJ-NP-Z2-9]{8}) -", healthy(process, path))
        if match:
            return match[1]
        time.sleep(0.1)
    raise RuntimeError(f"Room creation timed out: {path}")


def run(executable: pathlib.Path, assets: pathlib.Path, directory: pathlib.Path,
        source: dict, content: str, fps: int, seconds: int, validation: bool,
        expected_level: str = "") -> None:
    defaults = json.loads((ROOT / "data/config.json").read_text(encoding="utf-8"))
    with contextlib.ExitStack() as stack:
        server = netplay_rooms.make_server()
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        stack.callback(worker.join, 3)
        stack.callback(server.server_close)
        stack.callback(server.shutdown)
        endpoint = f"http://127.0.0.1:{server.server_port}"

        def launch(guest: bool, code: str = ""):
            role = "guest" if guest else "host"
            local = directory / role
            local.mkdir()
            data = local / "data"
            data.mkdir()
            shutil.copytree(ROOT / "data/text", data / "text")
            (data / "config.json").write_text(
                json.dumps(test_config(defaults, guest, fps)), encoding="utf-8")
            selection = local / "scenario.json"
            selection.write_text(json.dumps(test_scenario(source, guest)), encoding="utf-8")
            path = local / "game.log"
            stream = stack.enter_context(path.open("w", encoding="utf-8"))
            command = [str(executable), "--assets", str(assets), "--data", str(data),
                       "--scenario", str(selection), "--netplay-test", endpoint,
                       "--netplay-content", content, "--netplay-auto-start"]
            if code:
                command.extend(["--netplay-room", code])
            if validation:
                command.append("--validation")
            process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW
                                       if sys.platform == "win32" else 0)
            stack.callback(netplay_test.stop, process)
            print(f"{role.title()} PID {process.pid}; log: {path}", flush=True)
            return process, path

        host = launch(False)
        code = wait_room(*host)
        guest = launch(True, code)
        print(f"Local room {code}. Waiting for both stages to load...", flush=True)
        deadline = time.monotonic() + 90
        while True:
            texts = [healthy(*child) for child in (host, guest)]
            if all(playing(text, expected_level) and checkpoint(text) is not None for text in texts):
                initial = [checkpoint(text) for text in texts]
                break
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Stage loading timed out: {directory}")
            time.sleep(0.1)
        print("Both windows are playing. Enter pauses; Enter in HOST resumes. Escape exits.\n"
              "Tower portals and direct boss entrances work; results/shop still stop the test.\n"
              "No character saves are written.", flush=True)
        deadline = time.monotonic() + seconds if seconds else None
        while deadline is None or time.monotonic() < deadline:
            if any(child[0].poll() is not None for child in (host, guest)):
                if seconds:
                    raise RuntimeError(f"Game exited before the {seconds}s check completed: {directory}")
                for process, path in (host, guest):
                    if process.returncode not in (None, 0):
                        raise RuntimeError(f"Game exited with {process.returncode}: {path}")
                return
            for child in (host, guest):
                healthy(*child)
            time.sleep(0.1)
        final = [checkpoint(healthy(*child)) for child in (host, guest)]
        if not all(advancing(first, latest) for first, latest in zip(initial, final)):
            raise RuntimeError(f"Host/guest checkpoints did not advance: {initial} -> {final}")
        print(f"Completed {seconds}s of live host/guest presentation; checkpoints {initial} -> {final}.",
              flush=True)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scenario", default="netplay-g1")
    parser.add_argument("--assets", type=pathlib.Path, default=ROOT / "assets/GUNE5D/Gauntlet")
    parser.add_argument("--build-dir", type=pathlib.Path, default=ROOT / "build/netplay")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=1)
    parser.add_argument("--fps", type=int, choices=(30, 60), default=30)
    parser.add_argument("--seconds", type=int, default=0, help="stop after N playable seconds (0: manual)")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--expect-level", default="",
                        help="require both windows to enter this level before timing the test")
    args = parser.parse_args(argv)
    try:
        if args.seconds != 0 and args.seconds < 10:
            raise ValueError("--seconds must be 0 (manual) or at least 10 for progress checks")
        expected_level = args.expect_level.upper()
        if expected_level and not re.fullmatch(r"[A-Z][0-9]{1,2}", expected_level):
            raise ValueError("--expect-level must be a native level name such as G1")
        directory = args.build_dir.resolve()
        if args.build:
            build(directory, args.jobs)
        executable = directory / "bin" / ("gauntlet.exe" if sys.platform == "win32" else "gauntlet")
        if not executable.is_file():
            raise ValueError(f"{executable} is missing; add --build")
        source = json.loads(scenario.resolve_scenario(args.scenario).read_text(encoding="utf-8"))
        test_scenario(source, False)
        assets = args.assets.resolve()
        print(f"Checking native asset identity: {assets}", flush=True)
        content = asset_digest(assets)
        logs = pathlib.Path(tempfile.mkdtemp(prefix="scene-", dir=directory))
        print(f"Test files: {logs}", flush=True)
        run(executable, assets, logs, source, content, args.fps, args.seconds, args.validation,
            expected_level)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Netplay scene test failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
