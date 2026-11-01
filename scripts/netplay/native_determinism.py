#!/usr/bin/env python3
"""Compare bounded native PlayScene observations from two independent processes.

    python scripts/netplay/native_determinism.py --ticks 600 --render-every 0 1
    python scripts/netplay/native_determinism.py --mechanic aggressor --workload combat
    python scripts/netplay/native_determinism.py --compare first.jsonl second.jsonl

Runs are sequential, headless, and use the existing nativeprobe executable;
this script never builds it. Reports and process logs are retained. Equal hashes
prove only observed-equal state for these inputs, not complete determinism.
"""

import argparse
import contextlib
import json
import math
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))  # the top-level helpers
import devenv


MAX_TICKS = 36_000
MAX_RENDER_EVERY = 60
MAX_LINE_BYTES = 1024 * 1024
MAX_SUBSYSTEMS = 256
HASH = re.compile(r"[0-9a-f]{64}\Z")
METADATA_FIELDS = {
    "type", "schema", "simulation", "stage", "ticks", "players", "tick_rate",
    "render_every", "workload", "input_version", "initialization",
    "input_delivery", "input_delay",
    "complete_state", "unobserved", "subsystems", "assets_digest", "data_digest", "config_digest",
}
ROLLBACK_METADATA_FIELDS = METADATA_FIELDS | {"agreement_schema", "runtime_digest"}
ROLLBACK_COUNTS = {
    "sampled_inputs", "native_ticks", "replay_ticks", "corrections", "max_replay",
    "stalls", "pacing_idles", "prediction_max_ticks",
}
ROLLBACK_TIMINGS = {
    "input_sample_to_first_native_update_p50_ms", "input_sample_to_first_native_update_p95_ms",
    "correction_p50_ms", "correction_p95_ms", "correction_max_ms",
}
IMPAIRMENT_DEFAULTS = {
    "latency_ms": 0, "jitter_ms": 0, "loss_ppm": 0, "reorder_ppm": 0,
    "reorder_ms": 0, "seed": 1, "outage_tick": 0, "outage_ms": 0, "outage_peer": 0,
}
IMPAIRMENT_COUNTS = {
    "attempted", "enqueued", "transmitted", "loss_dropped", "outage_dropped", "reordered",
    "queue_peak", "queue_pending", "queue_abandoned", "outage_started_tick", "oldest_recovery_datagrams",
}
IMPAIRMENT_TIMINGS = {
    "outage_elapsed_ms", "residence_p50_ms", "residence_p95_ms", "residence_max_ms",
}
OUTAGE_PROGRESS_COUNTS = {
    "updates", "stalls", "pacing_idles", "first_next_tick", "last_next_tick",
    "first_confirmed_tick", "last_confirmed_tick", "prediction_max_ticks",
}
PATH_COUNTS = {"samples", "relay_samples", "direct_samples", "unavailable_samples"}
PATH_TIMINGS = {"rtt_p50_ms", "rtt_p95_ms", "rtt_max_ms"}


class ReportError(ValueError):
    """Missing, malformed, incomplete, or unequal native observations."""


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ReportError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def _nonfinite(value):
    raise ReportError(f"Nonfinite JSON number: {value}")


def _integer(value, minimum, maximum):
    return type(value) is int and minimum <= value <= maximum


def validate_transport(value, path, players):
    if (not isinstance(value, dict) or set(value) != {"local_peer", "links"} or
            not _integer(value["local_peer"], 1, players) or not isinstance(value["links"], list)):
        raise ReportError("Invalid transport path evidence")
    local = value["local_peer"]
    expected = set(range(2, players + 1)) if local == 1 else {1}
    if len(value["links"]) != len(expected):
        raise ReportError("Transport path evidence must cover every active link")
    seen = set()
    for link in value["links"]:
        if (not isinstance(link, dict) or set(link) != PATH_COUNTS | PATH_TIMINGS | {"peer"} or
                not _integer(link["peer"], 1, players) or link["peer"] not in expected or
                link["peer"] in seen):
            raise ReportError("Invalid or duplicate transport link")
        seen.add(link["peer"])
        if (any(not _integer(link[key], 0, 2**64 - 1) for key in PATH_COUNTS) or
                link["samples"] == 0 or link["samples"] != sum(link[key] for key in
                    ("relay_samples", "direct_samples", "unavailable_samples"))):
            raise ReportError("Invalid transport path sample accounting")
        if any(type(link[key]) not in (int, float) or not math.isfinite(link[key]) or
               not 0 <= link[key] <= 2**31 - 1 for key in PATH_TIMINGS):
            raise ReportError("Invalid measured transport RTT")
        if (not link["rtt_p50_ms"] <= link["rtt_p95_ms"] <= link["rtt_max_ms"] or
                (link["relay_samples"] + link["direct_samples"] == 0 and
                 any(link[key] != 0 for key in PATH_TIMINGS))):
            raise ReportError("Inconsistent measured transport RTT")
        if path == "relay" and (link["relay_samples"] == 0 or link["direct_samples"] != 0):
            raise ReportError("Requested relay run lacks actual relay evidence for every active link")
        if path == "local" and link["relay_samples"] != 0:
            raise ReportError("Local-only run reported a relay path")


def validate_impairment(value, players, ticks):
    """Strict test-only profile; never infer an Internet latency from these values."""
    if (not isinstance(value, dict) or
            set(value) != set(IMPAIRMENT_DEFAULTS) | {"mode", "scope", "reliable_impaired"}):
        raise ReportError("Missing or unknown input impairment fields")
    for key, low, high in (("latency_ms", 0, 500), ("jitter_ms", 0, 500),
                           ("loss_ppm", 0, 250_000), ("reorder_ppm", 0, 1_000_000),
                           ("reorder_ms", 0, 500), ("seed", 1, 2**32 - 1),
                           ("outage_tick", 0, ticks), ("outage_ms", 0, 5000),
                           ("outage_peer", 0, players)):
        if not _integer(value[key], low, high):
            raise ReportError(f"Invalid input impairment {key}")
    enabled = any(value[key] for key in
                  ("latency_ms", "jitter_ms", "loss_ppm", "reorder_ppm", "outage_ms"))
    if (value["mode"] != ("simulated-application-input" if enabled else "none") or
            value["scope"] != "outbound-unreliable-input-only" or
            value["reliable_impaired"] is not False or
            (value["reorder_ppm"] == 0) != (value["reorder_ms"] == 0)):
        raise ReportError("Inconsistent input impairment labels or reorder settings")
    if value["outage_ms"]:
        if not (1 <= value["outage_tick"] < ticks - 8 and value["outage_peer"] >= 1):
            raise ReportError("Input outage must leave eight recovery ticks and name a peer")
    elif value["outage_tick"] or value["outage_peer"]:
        raise ReportError("Input outage requires a nonzero duration")


def _validate_impairment_metrics(value, config, ticks, rollback):
    if (not isinstance(value, dict) or
            set(value) != IMPAIRMENT_COUNTS | IMPAIRMENT_TIMINGS | {"outage_finished"} |
            ({"outage_progress"} if "outage_progress" in value else set())):
        raise ReportError("Missing or unknown input impairment metrics")
    if any(not _integer(value[key], 0, 2**64 - 1) for key in IMPAIRMENT_COUNTS):
        raise ReportError("Invalid input impairment count")
    if any(type(value[key]) not in (int, float) or not math.isfinite(value[key]) or value[key] < 0
           for key in IMPAIRMENT_TIMINGS):
        raise ReportError("Invalid input impairment timing")
    if (type(value["outage_finished"]) is not bool or
            not value["residence_p50_ms"] <= value["residence_p95_ms"] <= value["residence_max_ms"] or
            value["queue_pending"] > value["queue_peak"] or value["queue_peak"] > 4096 or
            value["reordered"] > value["transmitted"] or
            value["oldest_recovery_datagrams"] > value["transmitted"]):
        raise ReportError("Inconsistent input impairment bounds")
    if config["mode"] == "none":
        if (value["attempted"] != value["transmitted"] or
                any(value[key] for key in IMPAIRMENT_COUNTS - {"attempted", "transmitted"}) or
                any(value[key] for key in IMPAIRMENT_TIMINGS) or value["outage_finished"]):
            raise ReportError("Unimpaired run reported injected input impairment")
    elif (value["attempted"] != value["enqueued"] + value["loss_dropped"] + value["outage_dropped"] or
          value["enqueued"] != value["transmitted"] + value["queue_pending"] + value["queue_abandoned"]):
        raise ReportError("Input impairment accounting does not balance")
    if not config["loss_ppm"] and value["loss_dropped"]:
        raise ReportError("Input loss was not configured")
    started = value["outage_started_tick"]
    if started:
        if (not config["outage_ms"] or not config["outage_tick"] <= started <= ticks + 1 or
                not value["outage_dropped"] or
                (value["outage_finished"] and value["outage_elapsed_ms"] < config["outage_ms"])):
            raise ReportError("Invalid observed input outage")
    elif (value["outage_dropped"] or value["outage_finished"] or value["outage_elapsed_ms"] or
          value["oldest_recovery_datagrams"]):
        raise ReportError("Input outage metrics lack a start")
    if not value["outage_finished"] and value["outage_elapsed_ms"]:
        raise ReportError("Unfinished input outage reported a completed duration")
    # Old reports remain readable, but only this explicit window observation can
    # certify that native pacing/prediction actually waited during an outage.
    if "outage_progress" in value:
        progress = value["outage_progress"]
        if (not isinstance(progress, dict) or set(progress) != OUTAGE_PROGRESS_COUNTS or
                any(not _integer(progress[key], 0, 2**64 - 1) for key in OUTAGE_PROGRESS_COUNTS)):
            raise ReportError("Invalid outage-window progress observation")
        if progress["updates"] == 0:
            if any(progress.values()):
                raise ReportError("Outage progress has no observed native updates")
        elif (not started or
              not started <= progress["first_next_tick"] <= progress["last_next_tick"] <= ticks + 1 or
              not progress["first_confirmed_tick"] <= progress["last_confirmed_tick"] <= ticks or
              progress["first_confirmed_tick"] >= progress["first_next_tick"] or
              progress["last_confirmed_tick"] >= progress["last_next_tick"] or
              progress["last_next_tick"] - progress["first_next_tick"] > progress["updates"] or
              progress["stalls"] + progress["pacing_idles"] > progress["updates"] or
              progress["stalls"] > rollback["stalls"] or
              progress["pacing_idles"] > rollback["pacing_idles"] or
              not max(progress["first_next_tick"] - 1 - progress["first_confirmed_tick"],
                      progress["last_next_tick"] - 1 - progress["last_confirmed_tick"]) <=
                  progress["prediction_max_ticks"] <= rollback["prediction_max_ticks"]):
            raise ReportError("Inconsistent outage-window progress bounds")


def _name(value):
    return isinstance(value, str) and 0 < len(value) <= 128 and not any(
        ord(character) < 32 for character in value)


def _names(value, nonempty=False, ordered=False):
    return (isinstance(value, list) and int(nonempty) <= len(value) <= MAX_SUBSYSTEMS and
            all(_name(name) for name in value) and len(set(value)) == len(value) and
            (not ordered or value == sorted(value)))


def _descriptions(value):
    return (isinstance(value, list) and len(value) <= MAX_SUBSYSTEMS and
            all(isinstance(description, str) and description.strip() and
                len(description) <= 1024 and
                not any(ord(character) < 32 for character in description)
                for description in value) and len(set(value)) == len(value))


class _Reader:
    def __init__(self, path, stream):
        self.path = path
        self.stream = stream
        self.line = 0

    def fail(self, message):
        raise ReportError(f"{self.path}:{self.line}: {message}")

    def row(self):
        line = self.stream.readline(MAX_LINE_BYTES + 1)
        self.line += 1
        if not line:
            self.fail("Missing row (empty or truncated report)")
        if len(line) > MAX_LINE_BYTES:
            self.fail("Report line exceeds byte bound")
        if not line.endswith(b"\n"):
            self.fail("Unterminated JSONL row (possibly truncated report)")
        try:
            row = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object,
                             parse_constant=_nonfinite)
        except (ValueError, UnicodeError, RecursionError) as error:
            self.fail(f"Invalid JSON: {error}")
        if not isinstance(row, dict):
            self.fail("Expected a JSON object")
        return row

    def metadata(self, expected):
        row = self.row()
        rollback = type(row.get("schema")) is int and row["schema"] == 2
        self.impairment = row.get("impairment") if rollback else None
        self.network_path = row.get("network_path") if rollback else None
        fields = ROLLBACK_METADATA_FIELDS if rollback else METADATA_FIELDS
        if rollback and "impairment" in row:
            fields = fields | {"impairment"}
        if rollback and "network_path" in row:
            fields = fields | {"network_path"}
            if self.network_path not in ("local", "relay"):
                self.fail("Invalid network path policy")
        if set(row) != fields:
            self.fail("Missing or unknown metadata fields")
        exact = {"type": "metadata", "simulation": "NativeRollbackDriver" if rollback else "PlayScene::update",
                 "initialization": "native-defaults-v1", "complete_state": False}
        if rollback:
            exact.update(input_delivery="iroh-rollback", agreement_schema="native-scene-and-control-v1")
        if any(type(row[key]) is not type(value) or row[key] != value
               for key, value in exact.items()):
            self.fail("Unsupported simulation, initialization, or observation scope")
        for key, low, high in (("schema", 2 if rollback else 1, 2 if rollback else 1), ("input_version", 1, 1),
                               ("tick_rate", 60, 60), ("ticks", 1, MAX_TICKS),
                               ("players", 1, 4), ("render_every", 0, MAX_RENDER_EVERY)):
            if not _integer(row[key], low, high):
                self.fail(f"Invalid metadata {key}")
        deliveries = ("iroh-rollback",) if rollback else ("local", "iroh-lockstep")
        if row["input_delivery"] not in deliveries:
            self.fail("Invalid native input delivery")
        if (not _integer(row["input_delay"], 0, 2 if rollback else 6) or
                (row["input_delivery"] == "local" and row["input_delay"] != 0) or
                (row["input_delivery"] != "local" and row["players"] < 2)):
            self.fail("Invalid native input delivery or delay")
        if (not _name(row["stage"]) or row["workload"] not in ("idle", "combat") or
                not _names(row["subsystems"], nonempty=True, ordered=True) or
                not _descriptions(row["unobserved"])):
            self.fail("Invalid stage, workload, or observation roster")
        if set(row["subsystems"]) & set(row["unobserved"]):
            self.fail("Observed and unobserved rosters overlap")
        if any(not isinstance(row[key], str) or not HASH.fullmatch(row[key])
               for key in (("assets_digest", "data_digest", "config_digest", "runtime_digest")
                           if rollback else ("assets_digest", "data_digest", "config_digest"))):
            self.fail("Invalid assets, data, or config digest")
        if rollback and "impairment" in row:
            validate_impairment(self.impairment, row["players"], row["ticks"])
        for key, value in (expected or {}).items():
            if key not in row or type(row[key]) is not type(value) or row[key] != value:
                self.fail(f"Requested {key} differs from report")
        return row

    def rollback_tick(self, number):
        row = self.row()
        fields = {"type", "tick", "input_digest", "event_digest", "audio_digest"}
        if (set(row) != fields or row["type"] != "tick" or
                not _integer(row["tick"], number, number)):
            self.fail(f"Expected contiguous confirmed tick {number}")
        for key in ("input_digest", "event_digest", "audio_digest"):
            if not isinstance(row[key], str) or not HASH.fullmatch(row[key]):
                self.fail(f"Invalid {key} at tick {number}")
        return row

    def confirmed(self, number):
        row = self.row()
        if (set(row) != {"type", "tick", "digest"} or row["type"] != "confirmed" or
                not _integer(row["tick"], number, number) or
                not isinstance(row["digest"], str) or not HASH.fullmatch(row["digest"])):
            self.fail(f"Missing or invalid native agreement at tick {number}")
        return row["digest"]

    def rollback_finish(self, ticks, players):
        row = self.row()
        if (set(row) != {"type", "ticks", "last_tick", "native_digest", "metrics"} or
                row["type"] != "complete" or not _integer(row["ticks"], ticks, ticks) or
                not _integer(row["last_tick"], ticks, ticks) or
                not isinstance(row["native_digest"], str) or not HASH.fullmatch(row["native_digest"])):
            self.fail("Missing or inconsistent rollback completion footer")
        metrics = row["metrics"]
        if (not isinstance(metrics, dict) or
                set(metrics) != ROLLBACK_COUNTS | ROLLBACK_TIMINGS | {"link_rtt_ms"} |
                ({"transport"} if self.network_path is not None else set()) |
                ({"impairment"} if self.impairment is not None else set())):
            self.fail("Missing or unknown rollback metrics")
        for key in ROLLBACK_COUNTS:
            if not _integer(metrics[key], 0, 2**64 - 1):
                self.fail(f"Invalid rollback count {key}")
        for key in ROLLBACK_TIMINGS:
            if (type(metrics[key]) not in (int, float) or not math.isfinite(metrics[key]) or
                    metrics[key] < 0):
                self.fail(f"Invalid rollback timing {key}")
        if (metrics["max_replay"] > 8 or metrics["prediction_max_ticks"] > 8 or
                metrics["native_ticks"] != ticks + metrics["replay_ticks"] or
                metrics["corrections"] > metrics["replay_ticks"] or
                metrics["input_sample_to_first_native_update_p50_ms"] >
                metrics["input_sample_to_first_native_update_p95_ms"] or
                not metrics["correction_p50_ms"] <= metrics["correction_p95_ms"] <= metrics["correction_max_ms"]):
            self.fail("Inconsistent rollback metric bounds")
        rtt = metrics["link_rtt_ms"]
        if (not isinstance(rtt, list) or not 1 <= len(rtt) <= players - 1 or
                any(not _integer(value, -1, 2**31 - 1) for value in rtt)):
            self.fail("Invalid transport RTT samples")
        if self.impairment is not None:
            _validate_impairment_metrics(metrics["impairment"], self.impairment, ticks, metrics)
        if self.network_path is not None:
            validate_transport(metrics["transport"], self.network_path, players)
            if len(rtt) != len(metrics["transport"]["links"]):
                self.fail("Transport RTT footer does not cover the observed links")
        if self.stream.read(1):
            self.fail("Unexpected data after completion footer")
        return row

    def tick(self, number, names):
        row = self.row()
        if (set(row) != {"type", "tick", "input_digest", "subsystems", "unavailable"} or
                row["type"] != "tick" or not _integer(row["tick"], number, number)):
            self.fail(f"Expected contiguous tick {number}")
        digest, hashes, unavailable = row["input_digest"], row["subsystems"], row["unavailable"]
        if not isinstance(digest, str) or not HASH.fullmatch(digest):
            self.fail(f"Invalid input digest at tick {number}")
        if not isinstance(hashes, dict) or not isinstance(unavailable, dict):
            self.fail(f"Invalid subsystem map at tick {number}")
        if (set(hashes) & set(unavailable) or set(hashes) | set(unavailable) != set(names) or
                any(not isinstance(value, str) or not HASH.fullmatch(value)
                    for value in hashes.values()) or
                any(not isinstance(reason, str) or not reason.strip() or len(reason) > 4096
                    for reason in unavailable.values())):
            self.fail(f"Invalid subsystem roster or digest at tick {number}")
        if unavailable:
            name = sorted(unavailable)[0]
            self.fail(f"Unavailable subsystem {name} at tick {number}: {unavailable[name]}")
        return row

    def finish(self, ticks):
        row = self.row()
        if (set(row) != {"type", "ticks", "last_tick"} or row["type"] != "complete" or
                not _integer(row["ticks"], ticks, ticks) or
                not _integer(row["last_tick"], ticks, ticks)):
            self.fail("Missing or inconsistent completion footer")
        if self.stream.read(1):
            self.fail("Unexpected data after completion footer")


def compare_reports(left, right, expected_left=None, expected_right=None):
    """Validate complete reports in bounded memory and identify the first mismatch."""
    paths = [pathlib.Path(left), pathlib.Path(right)]
    if paths[0].resolve() == paths[1].resolve():
        raise ReportError("Two distinct reports are required")
    with contextlib.ExitStack() as stack:
        readers = [_Reader(path, stack.enter_context(path.open("rb"))) for path in paths]
        metadata = [reader.metadata(expected) for reader, expected in
                    zip(readers, (expected_left, expected_right))]
        if set(metadata[0]) != set(metadata[1]):
            raise ReportError("Metadata differs: schema")
        for key in sorted(set(metadata[0]) - {"render_every"}):
            if metadata[0][key] != metadata[1][key]:
                raise ReportError(f"Metadata differs: {key}")
        first_difference = None
        names = metadata[0]["subsystems"]
        ticks = metadata[0]["ticks"]
        rollback = metadata[0]["schema"] == 2
        for number in range(1, ticks + 1):
            if rollback:
                a, b = (reader.rollback_tick(number) for reader in readers)
                for key in ("input_digest", "event_digest", "audio_digest"):
                    if a[key] != b[key] and first_difference is None:
                        first_difference = f"First difference at tick {number}, {key}"
                if number % 30 == 0:
                    lhs, rhs = (reader.confirmed(number) for reader in readers)
                    if lhs != rhs and first_difference is None:
                        first_difference = f"First difference at tick {number}, native agreement"
                continue
            a, b = (reader.tick(number, names) for reader in readers)
            if a["input_digest"] != b["input_digest"] and first_difference is None:
                first_difference = f"First difference at tick {number}, input_digest"
            for name in names:
                lhs, rhs = a["subsystems"][name], b["subsystems"][name]
                if lhs != rhs and first_difference is None:
                    first_difference = (f"First difference at tick {number}, subsystem {name}: "
                                        f"{lhs} != {rhs}")
        metrics = []
        if rollback:
            footers = [reader.rollback_finish(ticks, metadata[0]["players"]) for reader in readers]
            metrics = [footer["metrics"] for footer in footers]
            if footers[0]["native_digest"] != footers[1]["native_digest"] and first_difference is None:
                first_difference = "Final native agreement differs"
        else:
            for reader in readers:
                reader.finish(ticks)
        if first_difference:
            raise ReportError(first_difference)
    return {"status": "observed-equal", "complete_state": False,
            **({"metrics": metrics} if rollback else {}),
            "metadata": {key: value for key, value in metadata[0].items()
                         if key not in ("type", "render_every")},
            "render_every": [value["render_every"] for value in metadata],
            "reports": [str(path.resolve()) for path in paths]}


def run_pair(probe, output, *, ticks, players, stage, workload, render_every,
             timeout, assets=None, data=None, mechanics=()):
    """Run two independent probes sequentially; never reuse an existing report."""
    probe, output = pathlib.Path(probe).resolve(), pathlib.Path(output).resolve()
    if len(render_every) != 2:
        raise ReportError("Exactly two render cadences are required")
    reports, expectations = [], []
    for index, cadence in enumerate(render_every, start=1):
        report = output / f"run-{index}.jsonl"
        log = output / f"run-{index}.log"
        if report.exists():
            raise ReportError(f"Refusing an existing report: {report}")
        expected = {"ticks": ticks, "players": players, "stage": stage,
                    "workload": workload, "render_every": cadence}
        command = [str(probe), "--report", str(report), "--stage", stage,
                   "--ticks", str(ticks), "--players", str(players),
                   "--render-every", str(cadence), "--workload", workload]
        for flag, path in (("assets", assets), ("data", data)):
            if path is not None:
                command.extend([f"--{flag}", str(pathlib.Path(path).resolve())])
        for mechanic in mechanics:
            command.extend(["--mechanic", mechanic])
        with log.open("x", encoding="utf-8") as stream:
            result = subprocess.run(command, cwd=devenv.ROOT, stdin=subprocess.DEVNULL,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=timeout,
                                    check=False, creationflags=(subprocess.CREATE_NO_WINDOW
                                                               if devenv.WINDOWS else 0))
        if result.returncode != 0:
            raise ReportError(f"Probe {index} failed (exit {result.returncode}); inspect {log}")
        if not report.is_file() or report.stat().st_size == 0:
            raise ReportError(f"Probe {index} produced no observations; inspect {log}")
        reports.append(report)
        expectations.append(expected)
    return compare_reports(*reports, *expectations)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--compare", type=pathlib.Path, nargs=2, metavar=("LEFT", "RIGHT"))
    mode.add_argument("--probe", type=pathlib.Path, help="Existing nativeprobe executable")
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, help="New directory for retained reports/logs")
    parser.add_argument("--assets", type=pathlib.Path)
    parser.add_argument("--data", type=pathlib.Path)
    parser.add_argument("--stage")
    parser.add_argument("--ticks", type=int)
    parser.add_argument("--players", type=int)
    parser.add_argument("--workload", choices=("idle", "combat"))
    parser.add_argument("--mechanic", action="append", metavar="ID",
                        help="Switch on a mechanic the build carries, by id (repeatable)")
    parser.add_argument("--render-every", type=int, nargs=2, metavar=("FIRST", "SECOND"),
                        help="Draw every K simulation ticks (0 disables rendering; default: 0 1)")
    parser.add_argument("--timeout", type=float, help="Per-process timeout seconds (default: 300)")
    args = parser.parse_args(argv)
    try:
        if args.compare:
            if any(value is not None for key, value in vars(args).items()
                   if key not in ("compare", "probe")):
                parser.error("--compare does not accept process-launch options")
            result = compare_reports(*args.compare)
        else:
            ticks = 600 if args.ticks is None else args.ticks
            players = 2 if args.players is None else args.players
            cadence = [0, 1] if args.render_every is None else args.render_every
            timeout = 300 if args.timeout is None else args.timeout
            stage = "G1" if args.stage is None else args.stage
            mechanics = args.mechanic or []
            if (not _integer(ticks, 1, MAX_TICKS) or not _integer(players, 1, 4) or
                    any(not _integer(value, 0, MAX_RENDER_EVERY) for value in cadence) or
                    not math.isfinite(timeout) or timeout <= 0 or not _name(stage) or
                    not all(re.fullmatch(r"[a-z0-9-]{1,32}", mechanic) for mechanic in mechanics)):
                parser.error("Invalid ticks, players, render cadence, timeout, stage or mechanic")
            directory = (args.build_dir or devenv.ROOT / "build" / devenv.release_preset()).resolve()
            probe = (args.probe or directory / "bin" /
                     ("nativeprobe.exe" if devenv.WINDOWS else "nativeprobe")).resolve()
            if not probe.is_file():
                raise ReportError(f"nativeprobe is missing; build it first: {probe}")
            if args.output is not None:
                output = args.output.resolve()
                output.mkdir(parents=True, exist_ok=False)
            else:
                output = pathlib.Path(tempfile.mkdtemp(prefix="native-determinism-", dir=directory))
            print(f"Reports: {output}", flush=True)
            result = run_pair(probe, output, ticks=ticks, players=players, stage=stage,
                              workload=args.workload or "idle", render_every=cadence,
                              timeout=timeout, assets=args.assets, data=args.data,
                              mechanics=mechanics)
        print(json.dumps(result, indent=2, allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Native observation comparison failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
