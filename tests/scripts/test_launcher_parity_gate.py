"""The parity gate holds the launcher's Legends trees to the extraction scripts' and to the
reference trees, runs its steps in order with their timeouts, and reports what it found. Its trees
are synthetic and every child it starts has a stand-in: nothing is built or run, and no game data
is read."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "scripts" / "checks"))
import launcher  # noqa: E402  the paths above make both importable
import launcher_parity as gate  # noqa: E402


# The synthetic trees' files: the N64 tree's ROMDISK files and overlays, and the Dreamcast tree's
# disc files and the native files packed in them.
N64_FILES = ("texture.bin", "levels/a/worlds.wad")
N64_PROGRAMS = ("_program/boot.bin", "_program/audio.bin")
DISC_FILES = ("SIZES.ROM", "DATA/MAP.BIN")
NATIVE_FILES = ("levels/a.wad", "levels/b.wad")

# What the research tools leave in the reference trees, which no extraction writes.
EXTRAS = {"n64": {"audio-catalog.json": b"{}\n"},
          "dreamcast": {"audio-wav/one.wav": b"RIFF one", "audio-wav/two.wav": b"RIFF two"}}

# The environment launcher.environment gives cargo in these tests.
CARGO_ENVIRONMENT = {"PATH": "where cargo is"}

# The variables that tell the launcher's ignored tests where the dumps and the reference trees are.
VARIABLES = ("GDL_LEGENDS_N64_ROM", "GDL_LEGENDS_DREAMCAST_DISC", "GDL_LEGENDS_N64_TREE",
             "GDL_LEGENDS_DREAMCAST_TREE")

# Every step of a run that passes, in its order.
STEPS = ["lower priority", "stop on signals", "watch the caller", "cargo build", "cargo test",
         "script n64", "launcher n64", "script dreamcast", "launcher dreamcast", "fingerprints"]


def contents(path: str) -> bytes:
    """A synthetic file's bytes, which name it."""
    return f"the file {path}\n".encode("ascii")


def manifest(document: dict) -> bytes:
    """A manifest's bytes, as the extraction scripts write one."""
    return (json.dumps(document, indent=2) + "\n").encode("ascii")


def n64_tree() -> dict[str, bytes]:
    """A tree as the N64 extractor writes one: its files, its overlays and the manifest that lists
    both."""
    tree = {path: contents(path) for path in (*N64_FILES, *N64_PROGRAMS)}
    tree[gate.N64_MANIFEST] = manifest({
        "format": "gauntlet-legends-n64-romdisk", "version": 1,
        "files": [{"path": path} for path in N64_FILES],
        "programs": [{"path": path} for path in N64_PROGRAMS]})
    return tree


def dreamcast_tree() -> dict[str, bytes]:
    """A tree as the Dreamcast extractor writes one: the disc's files, its boot area, the native
    files packed in it, and the two manifests that list them."""
    tree = {f"disc/{path}": contents(path) for path in DISC_FILES}
    tree.update({f"native/{path}": contents(path) for path in NATIVE_FILES})
    tree[gate.IP_BIN] = bytes(gate.IP_BIN_SIZE)
    tree[gate.DISC_MANIFEST] = manifest({"format": "dreamcast-legends-disc-v1",
                                         "files": [{"path": path} for path in DISC_FILES]})
    tree[gate.NATIVE_MANIFEST] = manifest({"format": "dreamcast-legends-native-v1",
                                           "files": [{"path": path} for path in NATIVE_FILES]})
    return tree


TREES = {"n64": n64_tree, "dreamcast": dreamcast_tree}


def write(root: Path, tree: dict[str, bytes]) -> Path:
    """Writes the files of `tree` under `root`, folders included; returns the root."""
    for path, data in tree.items():
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    return root


def held(data: bytes) -> str:
    """How the gate names a file holding `data`."""
    return f"{len(data)} bytes (SHA-256 {hashlib.sha256(data).hexdigest()})"


def synthetic_counts():
    """The counts the gate holds a tree to, set to the synthetic trees'."""
    return mock.patch.multiple(gate, N64_FILES=len(N64_FILES), N64_PROGRAMS=len(N64_PROGRAMS),
                               DREAMCAST_DISC_FILES=len(DISC_FILES),
                               DREAMCAST_NATIVE_FILES=len(NATIVE_FILES))


class Children:
    """Stands in for what a run starts, and records each in the order it starts: the calls that
    ready the run; cargo's runs, which launcher.run would start; and the extraction scripts, the
    launcher and the fingerprints' check, which run_tethered would. Each exits with what `codes`
    gives its step, else 0, and an extraction that exits 0 writes its platform's synthetic tree,
    the launcher the one `made` gives in its place."""

    def __init__(self, test: unittest.TestCase, codes: dict[str, int] | None = None,
                 made: dict[str, dict[str, bytes]] | None = None):
        self.test = test
        self.codes = codes or {}
        self.made = made or {}
        self.started: list[tuple[str, list[str] | None, dict]] = []

    def record(self, step: str, command: list[str] | None = None, **keywords) -> int:
        self.started.append((step, command, keywords))
        return self.codes.get(step, 0)

    def steps(self) -> list[str]:
        return [step for step, _, _ in self.started]

    def find(self, step: str) -> tuple[list[str] | None, dict]:
        """The command and keywords of `step`, which ran once."""
        found = [(command, keywords) for name, command, keywords in self.started if name == step]
        self.test.assertEqual(len(found), 1, step)
        return found[0]

    def lower_priority(self) -> bool:
        self.record("lower priority")
        return True

    def stop_on_signals(self) -> None:
        self.record("stop on signals")

    def watch(self, pid: int, callback) -> None:
        self.record("watch the caller", pid=pid)

    def cargo(self, command: list[str], env: dict[str, str], **keywords) -> int:
        return self.record(f"cargo {command[1]}", command, env=env, **keywords)

    def tethered(self, command: list[str], **keywords) -> subprocess.CompletedProcess:
        if command[:3] == [sys.executable, "-I", str(gate.FINGERPRINTS)]:
            code = self.record("fingerprints", command, **keywords)
            return subprocess.CompletedProcess(command, code)
        if command[0] == sys.executable:
            platform = next(name for name, script in gate.SCRIPTS.items()
                            if command[2] == str(script))
            step, tree = f"script {platform}", TREES[platform]()
        else:
            platform = command[2]
            step, tree = f"launcher {platform}", self.made.get(platform) or TREES[platform]()
        output = Path(command[command.index("--output") + 1])
        # Each extraction writes a new folder, and the launcher refuses one whose parent is not
        # there.
        self.test.assertTrue(output.parent.is_dir(), step)
        self.test.assertFalse(os.path.lexists(output), step)
        code = self.record(step, command, **keywords)
        if code == 0:
            write(output, tree)
        return subprocess.CompletedProcess(command, code)


class CompareTests(unittest.TestCase):
    """The launcher's tree against the scripts' and against the reference tree."""

    def setUp(self):
        self.maxDiff = None
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.scratch = Path(temporary.name)
        counts = synthetic_counts()
        counts.start()
        self.addCleanup(counts.stop)

    def folder(self) -> Path:
        return Path(tempfile.mkdtemp(dir=self.scratch))

    def compare(self, platform: str, made=None, written=None, reference=None) -> dict:
        """Compares trees of `platform` as the gate does, `made` as the launcher's, `written` as
        the scripts' and `reference` as the reference tree, each the platform's synthetic tree
        unless given."""
        roots = [write(self.folder(), TREES[platform]() if tree is None else tree)
                 for tree in (made, written, reference)]
        return gate.compare_trees(platform, *roots)

    def test_identical_trees_compare_equal(self):
        n64 = self.compare("n64")
        self.assertEqual(list(n64), ["files", "programs", "identical", "reference",
                                     "referenceExtras", "differences"])
        self.assertEqual(n64, {"files": 2, "programs": 2, "identical": True,
                               "reference": "identical", "referenceExtras": [],
                               "differences": []})
        dreamcast = self.compare("dreamcast")
        self.assertEqual(list(dreamcast), ["discFiles", "nativeFiles", "identical", "reference",
                                           "referenceExtras", "differences"])
        self.assertEqual(dreamcast, {"discFiles": 2, "nativeFiles": 2, "identical": True,
                                     "reference": "identical", "referenceExtras": [],
                                     "differences": []})

    def test_a_changed_byte_a_missing_file_and_a_letter_case_are_differences(self):
        written = n64_tree()
        made = dict(written)
        made["texture.bin"] = b"T" + written["texture.bin"][1:]
        del made["_program/boot.bin"]
        made["Levels/a/worlds.wad"] = made.pop("levels/a/worlds.wad")
        result = self.compare("n64", made=made, written=written)
        texture, boot, world = (written[path] for path in
                                ("texture.bin", "_program/boot.bin", "levels/a/worlds.wad"))
        self.assertFalse(result["identical"])
        self.assertEqual(result["reference"], "differs")
        self.assertEqual(result["differences"], [
            # Against the scripts' tree, every path either tree holds, in the letters it has.
            f"Levels/a/worlds.wad: the launcher's tree holds {held(world)}, the scripts' nothing",
            f"_program/boot.bin: the launcher's tree holds nothing, the scripts' {held(boot)}",
            f"levels/a/worlds.wad: the launcher's tree holds nothing, the scripts' {held(world)}",
            f"texture.bin: the launcher's tree holds {held(made['texture.bin'])}, the scripts' "
            f"{held(texture)}",
            # Against the reference tree, every path the launcher's manifest names.
            f"_program/boot.bin: the launcher's tree holds nothing, the reference tree "
            f"{held(boot)}",
            f"levels/a/worlds.wad: the launcher's tree holds nothing, the reference tree "
            f"{held(world)}",
            f"texture.bin: the launcher's tree holds {held(made['texture.bin'])}, the reference "
            f"tree {held(texture)}",
        ])

    def test_reference_extras_are_reported_and_do_not_fail(self):
        cases = (("n64", ["audio-catalog.json"], "audio-catalog.json"),
                 ("dreamcast", ["audio-wav/one.wav", "audio-wav/two.wav"],
                  "audio-wav/ (2 files)"))
        for platform, extras, named in cases:
            with self.subTest(platform=platform):
                result = self.compare(platform,
                                      reference={**TREES[platform](), **EXTRAS[platform]})
                self.assertTrue(result["identical"])
                self.assertEqual(result["reference"], "identical")
                self.assertEqual(result["referenceExtras"], extras)
                self.assertEqual(result["differences"], [])
                self.assertEqual(gate.extras_named(result["referenceExtras"]), named)
        # A folder that holds nothing is an extra too, and so is a file at the root.
        reference = write(self.folder(), {**dreamcast_tree(), **EXTRAS["dreamcast"],
                                          "notes.txt": b"notes"})
        (reference / "empty").mkdir()
        result = gate.compare_trees("dreamcast", write(self.folder(), dreamcast_tree()),
                                    write(self.folder(), dreamcast_tree()), reference)
        self.assertEqual(result["referenceExtras"],
                         ["audio-wav/one.wav", "audio-wav/two.wav", "empty/", "notes.txt"])
        self.assertEqual(result["differences"], [])
        self.assertEqual(gate.extras_named(result["referenceExtras"]),
                         "audio-wav/ (2 files), empty/ (0 files), notes.txt")
        self.assertEqual(gate.extras_named([]), "none")

    def test_extras_against_the_scripts_fail(self):
        stray = {"stray.bin": b"stray"}
        for made, written, line in (
                ({**n64_tree(), **stray}, None,
                 f"stray.bin: the launcher's tree holds {held(b'stray')}, the scripts' nothing"),
                (None, {**n64_tree(), **stray},
                 f"stray.bin: the launcher's tree holds nothing, the scripts' {held(b'stray')}")):
            with self.subTest(line=line):
                result = self.compare("n64", made=made, written=written)
                self.assertFalse(result["identical"])
                # No manifest names the file, so the reference tree need not hold it.
                self.assertEqual(result["reference"], "identical")
                self.assertEqual(result["differences"], [line])
        # A folder the launcher made that holds nothing is one too.
        made = write(self.folder(), n64_tree())
        (made / "empty").mkdir()
        result = gate.compare_trees("n64", made, write(self.folder(), n64_tree()),
                                    write(self.folder(), n64_tree()))
        self.assertEqual(result["differences"],
                         ["empty/: the launcher's tree holds an empty folder, the scripts' "
                          "nothing"])

    def test_expected_paths_come_from_the_manifests(self):
        paths, counts, wrong = gate.expected("n64", gate.Tree(write(self.folder(), n64_tree())))
        self.assertEqual(sorted(paths), sorted(["manifest.json", *N64_FILES, *N64_PROGRAMS]))
        self.assertEqual((counts, wrong), ({"files": 2, "programs": 2}, []))
        made = gate.Tree(write(self.folder(), dreamcast_tree()))
        paths, counts, wrong = gate.expected("dreamcast", made)
        self.assertEqual(sorted(paths), sorted(["IP.BIN", "disc-manifest.json",
                                                "native-manifest.json",
                                                *(f"disc/{path}" for path in DISC_FILES),
                                                *(f"native/{path}" for path in NATIVE_FILES)]))
        self.assertEqual((counts, wrong), ({"discFiles": 2, "nativeFiles": 2}, []))
        # A file both extractions wrote that no manifest names need not be in the reference tree;
        # one a manifest names must be.
        noted = {**dreamcast_tree(), "notes.txt": b"notes"}
        self.assertEqual(self.compare("dreamcast", made=noted, written=noted)["differences"], [])
        reference = dreamcast_tree()
        dropped = reference.pop("native/levels/b.wad")
        result = self.compare("dreamcast", reference=reference)
        self.assertEqual(result["reference"], "differs")
        self.assertEqual(result["differences"],
                         [f"native/levels/b.wad: the launcher's tree holds {held(dropped)}, the "
                          "reference tree nothing"])

    def test_a_tree_unlike_the_players_dump_differs(self):
        # The counts the player's dumps give, and the boot area's size.
        with mock.patch.multiple(gate, N64_FILES=618, N64_PROGRAMS=4):
            self.assertEqual(self.compare("n64")["differences"],
                             ["manifest.json lists 2 files, not 618",
                              "manifest.json lists 2 programs, not 4"])
        with mock.patch.multiple(gate, DREAMCAST_DISC_FILES=156, DREAMCAST_NATIVE_FILES=1361):
            self.assertEqual(self.compare("dreamcast")["differences"],
                             ["disc-manifest.json lists 2 files, not 156",
                              "native-manifest.json lists 2 files, not 1361"])
        short = {**dreamcast_tree(), "IP.BIN": bytes(16)}
        self.assertEqual(self.compare("dreamcast", short, short, short)["differences"],
                         ["IP.BIN holds 16 bytes, not 32768"])
        # A manifest that names no paths leaves the reference tree uncompared.
        for broken, line in (({**n64_tree(), "manifest.json": b"{}\n"},
                              "manifest.json lists no files"),
                             ({path: data for path, data in n64_tree().items()
                               if path != "manifest.json"},
                              "the launcher's tree holds no manifest.json")):
            with self.subTest(line=line):
                self.assertEqual(self.compare("n64", made=broken, written=broken),
                                 {"files": None, "programs": None, "identical": True,
                                  "reference": "notCompared", "referenceExtras": [],
                                  "differences": [line]})


class GateTests(unittest.TestCase):
    """Runs of the gate, with stand-ins for every child it starts."""

    def setUp(self):
        self.maxDiff = None
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.scratch = Path(temporary.name)
        self.output = self.scratch / "build" / "launcher-parity"
        dumps = self.scratch / "dumps"
        self.rom = write(dumps, {"rom.z64": b"a ROM"}) / "rom.z64"
        self.disc = write(dumps, {"disc/disc.cue": b"a cue sheet"}) / "disc" / "disc.cue"
        self.references = {platform: write(self.scratch / "reference" / platform,
                                           {**tree(), **EXTRAS[platform]})
                           for platform, tree in TREES.items()}
        self.environment = {"GDL_LEGENDS_N64_TREE": str(self.references["n64"]),
                            "GDL_LEGENDS_DREAMCAST_TREE": str(self.references["dreamcast"])}
        self.argv = ["--verify", "--n64-rom", str(self.rom), "--dreamcast-disc", str(self.disc)]
        counts = synthetic_counts()
        counts.start()
        self.addCleanup(counts.stop)

    def run_gate(self, children: Children, argv: list[str] | None = None,
                 environment: dict[str, str] | None = None) -> tuple[int, str, str]:
        """Runs the gate on `argv` and `environment`, by default the case's, with `children`
        standing in for what it starts: its exit code, and what it wrote to stdout and stderr."""
        out, err = io.StringIO(), io.StringIO()
        processes = gate.process_tree
        with mock.patch.object(processes, "lower_priority", side_effect=children.lower_priority), \
                mock.patch.object(processes, "stop_when_gone", side_effect=children.watch), \
                mock.patch.object(processes, "run_tethered", side_effect=children.tethered), \
                mock.patch.object(gate.launcher, "run", side_effect=children.cargo), \
                mock.patch.object(gate.launcher, "environment",
                                  side_effect=lambda: dict(CARGO_ENVIRONMENT)), \
                mock.patch.object(gate.launcher, "stop_on_signals",
                                  side_effect=children.stop_on_signals), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gate.main(self.argv if argv is None else argv,
                             self.environment if environment is None else environment,
                             self.output)
        return code, out.getvalue(), err.getvalue()

    def report(self) -> dict:
        return json.loads((self.output / "report.json").read_bytes())

    def test_verify_runs_its_steps_in_order_and_tethers_its_children(self):
        children = Children(self)
        code, _, err = self.run_gate(children)
        self.assertEqual((code, err), (gate.PASSED, ""))
        # The priority is lowered before any thread starts, the caller's watch first among them.
        self.assertEqual(children.steps(), STEPS)
        self.assertEqual(children.find("watch the caller")[1], {"pid": os.getppid()})
        python, made = self.output / "python", self.output / "launcher"
        for platform, dump in (("n64", self.rom), ("dreamcast", self.disc)):
            with self.subTest(platform=platform):
                self.assertEqual(children.find(f"script {platform}"), (
                    [sys.executable, "-I", str(gate.SCRIPTS[platform]), str(dump), "--output",
                     str(python / platform)], {"timeout": gate.EXTRACTION_TIMEOUT}))
                self.assertEqual(children.find(f"launcher {platform}"), (
                    [str(gate.BUILT_PROGRAM), "--extract", platform, str(dump), "--output",
                     str(made / platform)], {"timeout": gate.EXTRACTION_TIMEOUT}))
        self.assertEqual(children.find("fingerprints"), (
            [sys.executable, "-I", str(gate.FINGERPRINTS), "--tree", str(python / "dreamcast"),
             "--check"], {"timeout": gate.FINGERPRINTS_TIMEOUT}))
        assets = ROOT / "scripts" / "assets"
        self.assertEqual(gate.SCRIPTS, {"n64": assets / "n64_legends.py",
                                        "dreamcast": assets / "dreamcast_legends.py"})
        self.assertEqual(gate.FINGERPRINTS, assets / "legends_fingerprints.py")
        program = "gdl-launcher-headless" + (".exe" if os.name == "nt" else "")
        self.assertEqual(gate.BUILT_PROGRAM, ROOT / "build" / "launcher" / "release" / program)

    def test_cargo_runs_in_process_with_a_timeout(self):
        children = Children(self)
        code, _, err = self.run_gate(children, [*self.argv, "--jobs", "3"])
        self.assertEqual(code, gate.PASSED, err)
        build, build_keywords = children.find("cargo build")
        tests, test_keywords = children.find("cargo test")
        self.assertEqual(build, ["cargo", "build", "--locked", "--release", "--package",
                                 "gdl-launcher-headless"])
        self.assertEqual(tests[:2], ["cargo", "test"])
        self.assertEqual((build_keywords["timeout"], test_keywords["timeout"]),
                         (gate.BUILD_TIMEOUT, gate.TEST_TIMEOUT))
        for keywords in (build_keywords, test_keywords):
            # From the workspace, where launcher.run starts cargo when given no folder, in cargo's
            # environment with its jobs capped.
            self.assertNotIn("cwd", keywords)
            self.assertEqual(keywords["env"]["PATH"], CARGO_ENVIRONMENT["PATH"])
            self.assertEqual(keywords["env"]["CARGO_BUILD_JOBS"], "3")
        tethered = [command for step, command, _ in children.started
                    if command and not step.startswith("cargo")]
        self.assertEqual(len(tethered), 5)
        self.assertNotIn("cargo", [command[0] for command in tethered])
        self.assertEqual(gate.default_jobs(), max(1, min(12, (os.cpu_count() or 1) // 2)))
        # A program --launcher names runs as it is, and nothing is built.
        given = write(self.scratch / "elsewhere", {"headless": b"a program"}) / "headless"
        children = Children(self)
        code, out, err = self.run_gate(children, [*self.argv, "--launcher", str(given),
                                                  "--strict", "script"])
        self.assertEqual(code, gate.PASSED, err)
        self.assertEqual(children.steps(), [step for step in STEPS if step != "cargo build"])
        self.assertEqual(children.find("launcher n64")[0],
                         [str(given), "--extract", "n64", str(self.rom), "--output",
                          str(self.output / "launcher" / "n64"), "--strict", "script"])
        self.assertEqual((self.report()["launcher"], self.report()["strictness"]),
                         (str(given), "script"))
        self.assertIn("(script); reference extras", out)
        # The gate runs only when asked to, and on a cap of one job or more.
        for argv in (["--n64-rom", str(self.rom)], ["--verify", "--jobs", "0"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit) as refused:
                gate.main(argv, self.environment, self.output)
            self.assertEqual(refused.exception.code, 2)

    def test_the_ignored_tests_get_the_dumps_and_the_reference_trees(self):
        children = Children(self)
        code, _, err = self.run_gate(children, [*self.argv, "--platform", "n64"])
        self.assertEqual(code, gate.PASSED, err)
        command, keywords = children.find("cargo test")
        self.assertEqual(command, ["cargo", "test", "--locked", "--package", "gdl-media",
                                   "--package", "gdl-extract", "--", "--ignored",
                                   "--test-threads", "2"])
        self.assertEqual({name: keywords["env"].get(name) for name in VARIABLES}, {
            "GDL_LEGENDS_N64_ROM": str(self.rom), "GDL_LEGENDS_DREAMCAST_DISC": str(self.disc),
            "GDL_LEGENDS_N64_TREE": str(self.references["n64"]),
            "GDL_LEGENDS_DREAMCAST_TREE": str(self.references["dreamcast"])})
        self.assertEqual(set(VARIABLES) & set(children.find("cargo build")[1]["env"]), set())
        # The tests read both dumps and both trees, though only the N64 trees are compared.
        self.assertEqual(children.steps(), STEPS[:7])
        report = self.report()
        self.assertEqual(list(report["platforms"]), ["n64"])
        self.assertEqual((report["ignoredTests"], report["fingerprints"]),
                         ("passed", "notChecked"))

    def test_dumps_resolve_from_arguments_then_environment_then_assets(self):
        self.assertEqual(gate.DEFAULT_N64_ROM,
                         ROOT / "assets" / "n64" / "Gauntlet Legends (USA).z64")
        self.assertEqual(gate.DEFAULT_DREAMCAST_DISC,
                         ROOT / "assets" / "dreamcast" / "Gauntlet Legends (USA)"
                         / "Gauntlet Legends (USA).cue")
        self.assertEqual(gate.REFERENCE, {"n64": ROOT / "assets" / "n64" / "extracted",
                                          "dreamcast": ROOT / "assets" / "dreamcast" / "extracted"})
        variable, default = "GDL_LEGENDS_N64_ROM", gate.DEFAULT_N64_ROM
        named = {variable: os.path.join("named", "rom.z64")}
        given = Path("given", "rom.z64")
        self.assertEqual(gate.resolved(given, variable, default, named),
                         Path(os.path.abspath(given)))
        self.assertEqual(gate.resolved(None, variable, default, named),
                         Path(os.path.abspath(named[variable])))
        for unnamed in ({}, {variable: ""}):
            self.assertEqual(gate.resolved(None, variable, default, unnamed), default)
        # A run takes each dump from the variable where no option names one, and an option over
        # the variable; the extractions and the tests alike.
        other = write(self.scratch / "other", {"rom.z64": b"another ROM"}) / "rom.z64"
        environment = {**self.environment, "GDL_LEGENDS_N64_ROM": str(other),
                       "GDL_LEGENDS_DREAMCAST_DISC": str(self.disc)}
        for argv, rom in ((["--verify"], other), (["--verify", "--n64-rom", str(self.rom)],
                                                  self.rom)):
            with self.subTest(argv=argv):
                children = Children(self)
                code, _, err = self.run_gate(children, argv, environment)
                self.assertEqual(code, gate.PASSED, err)
                self.assertEqual(children.find("script n64")[0][3], str(rom))
                self.assertEqual(children.find("launcher n64")[0][3], str(rom))
                self.assertEqual(children.find("launcher dreamcast")[0][3], str(self.disc))
                tests = children.find("cargo test")[1]["env"]
                self.assertEqual(
                    (tests["GDL_LEGENDS_N64_ROM"], tests["GDL_LEGENDS_DREAMCAST_DISC"]),
                    (str(rom), str(self.disc)))
                self.assertEqual(self.report()["platforms"]["n64"]["dump"], str(rom))

    def test_a_missing_dump_fails_and_is_never_skipped(self):
        missing = self.scratch / "dumps" / "absent"
        cases = [
            (["--verify", "--n64-rom", str(missing), "--dreamcast-disc", str(self.disc)],
             self.environment,
             f"launcher parity: the n64 dump is not at {missing}; pass --n64-rom or set "
             "GDL_LEGENDS_N64_ROM"),
            # The launcher's tests read the disc even where only the N64 trees are compared.
            (["--verify", "--platform", "n64", "--n64-rom", str(self.rom)],
             {**self.environment, "GDL_LEGENDS_DREAMCAST_DISC": str(missing)},
             f"launcher parity: the dreamcast dump is not at {missing}; pass --dreamcast-disc or "
             "set GDL_LEGENDS_DREAMCAST_DISC"),
            (self.argv, {**self.environment, "GDL_LEGENDS_N64_TREE": str(missing)},
             f"launcher parity: the n64 reference tree is not at {missing}; set "
             "GDL_LEGENDS_N64_TREE"),
            ([*self.argv, "--launcher", str(missing)], self.environment,
             f"launcher parity: there is no launcher at {missing}"),
        ]
        for argv, environment, line in cases:
            with self.subTest(line=line):
                children = Children(self)
                code, out, err = self.run_gate(children, argv, environment)
                self.assertEqual((code, out, err), (gate.FAILED, "", line + "\n"))
                self.assertEqual(children.started, [])
                self.assertFalse(self.output.exists())

    def test_the_report_records_the_times(self):
        children = Children(self)
        # When each extraction starts and ends: the scripts' and the launcher's for N64, then for
        # Dreamcast.
        moments = [10.0, 12.34, 20.0, 20.56, 30.0, 320.44, 400.0, 458.1]
        with mock.patch.object(gate, "time") as clock:
            clock.monotonic.side_effect = moments
            code, out, err = self.run_gate(children)
        self.assertEqual(code, gate.PASSED, err)
        expected = {
            "schema": 1, "launcher": str(gate.BUILT_PROGRAM), "strictness": "install",
            "ignoredTests": "passed", "fingerprints": "current",
            "platforms": {
                "n64": {"dump": str(self.rom), "scriptSeconds": 2.3, "launcherSeconds": 0.6,
                        "files": 2, "programs": 2, "identical": True, "reference": "identical",
                        "referenceExtras": ["audio-catalog.json"], "differences": []},
                "dreamcast": {"dump": str(self.disc), "scriptSeconds": 290.4,
                              "launcherSeconds": 58.1, "discFiles": 2, "nativeFiles": 2,
                              "identical": True, "reference": "identical",
                              "referenceExtras": ["audio-wav/one.wav", "audio-wav/two.wav"],
                              "differences": []}}}
        # Indented JSON in the order above, ending in a newline.
        self.assertEqual((self.output / "report.json").read_bytes(),
                         (json.dumps(expected, indent=2) + "\n").encode("ascii"))
        self.assertIn("n64: identical (2 files, 2 programs, manifest); scripts 2.3 s, launcher "
                      "0.6 s (install); reference extras: audio-catalog.json\n", out)
        self.assertIn("dreamcast: identical (2 disc files, IP.BIN, 2 native files, 2 manifests); "
                      "scripts 290.4 s, launcher 58.1 s (install); reference extras: audio-wav/ "
                      "(2 files)\n", out)
        self.assertTrue(out.endswith(f"launcher parity: passed; the report is "
                                     f"{self.output / 'report.json'}\n"))

    def test_a_passing_run_removes_its_trees_and_a_failing_one_keeps_them(self):
        python, made = self.output / "python", self.output / "launcher"
        code, _, err = self.run_gate(Children(self))
        self.assertEqual(code, gate.PASSED, err)
        self.assertFalse(os.path.lexists(python))
        self.assertFalse(os.path.lexists(made))
        self.assertTrue((self.output / "report.json").is_file())
        # Kept when asked; and the next run, which writes its trees anew, removes them first.
        code, _, err = self.run_gate(Children(self), [*self.argv, "--keep"])
        self.assertEqual(code, gate.PASSED, err)
        for folder in (python, made):
            self.assertEqual(sorted(os.listdir(folder)), ["dreamcast", "n64"])
        changed = dreamcast_tree()
        changed["native/levels/a.wad"] = b"changed"
        children = Children(self, made={"dreamcast": changed})
        code, out, err = self.run_gate(children)
        self.assertEqual(code, gate.FAILED)
        for folder in (python, made):
            self.assertEqual(sorted(os.listdir(folder)), ["dreamcast", "n64"])
        report = self.report()
        self.assertEqual(report["failure"], "the dreamcast trees did not pass")
        self.assertFalse(report["platforms"]["dreamcast"]["identical"])
        self.assertEqual(report["fingerprints"], "notChecked")
        self.assertNotIn("fingerprints", children.steps())
        self.assertIn("dreamcast: 2 differences; ", out)
        self.assertEqual(err, "launcher parity: the dreamcast trees did not pass; the report is "
                              f"{self.output / 'report.json'}, and what the run extracted stays "
                              f"in {self.output}\n")

    def test_a_failing_step_ends_the_run_and_the_report_names_it(self):
        cases = [
            ("cargo build", 101, "the launcher's build exited 101", "notRun"),
            ("cargo build", launcher.TIMED_OUT,
             f"the launcher's build ran longer than {gate.BUILD_TIMEOUT} s and was stopped",
             "notRun"),
            ("cargo test", 101, "the launcher's tests on the dumps exited 101", "failed"),
            ("script n64", 1, "the n64 trees did not pass", "passed"),
            ("launcher dreamcast", 1, "the dreamcast trees did not pass", "passed"),
            ("fingerprints", 1, "the Dreamcast fingerprints the launcher carries are not those of "
                                "the scripts' tree: the fingerprints' check exited 1", "passed"),
        ]
        for step, exit_code, failure, tests in cases:
            with self.subTest(step=step, code=exit_code):
                children = Children(self, codes={step: exit_code})
                code, _, err = self.run_gate(children)
                self.assertEqual(code, gate.FAILED)
                # Nothing runs after the step that failed.
                self.assertEqual(children.steps(), STEPS[:STEPS.index(step) + 1])
                report = self.report()
                self.assertEqual((report["failure"], report["ignoredTests"]), (failure, tests))
                self.assertTrue(err.startswith(f"launcher parity: {failure}; "), err)
        self.assertEqual(report["fingerprints"], "notCurrent")
        # An extraction that failed is the platform's difference, and nothing is compared.
        for step, ran, line in (
                ("script n64", "launcher not run", "the extraction script exited 1"),
                ("launcher n64", "launcher 0.0 s", "the launcher exited 1")):
            with self.subTest(step=step):
                with mock.patch.object(gate, "time") as clock:
                    clock.monotonic.return_value = 5.0
                    code, out, _ = self.run_gate(Children(self, codes={step: 1}))
                self.assertEqual(code, gate.FAILED)
                record = self.report()["platforms"]["n64"]
                self.assertEqual((record["files"], record["identical"], record["reference"],
                                  record["differences"]), (None, False, "notCompared", [line]))
                self.assertIn(f"n64: 1 difference; scripts 0.0 s, {ran} (install); all of them "
                              f"in {self.output / 'report.json'}\n  {line}\n", out)


class CallerTests(unittest.TestCase):
    def test_a_gone_caller_interrupts_the_run_then_ends_it(self):
        with mock.patch.object(gate.process_tree, "stop_when_gone") as watch:
            gate.end_with_caller()
        (pid, end), = [call.args for call in watch.call_args_list]
        self.assertEqual(pid, os.getppid())
        done = []
        with mock.patch.object(gate._thread, "interrupt_main",
                               side_effect=lambda: done.append("interrupt")), \
                mock.patch.object(gate.time, "sleep",
                                  side_effect=lambda seconds: done.append(("sleep", seconds))), \
                mock.patch.object(gate.os, "_exit",
                                  side_effect=lambda code: done.append(("exit", code))):
            end()
        self.assertEqual(done, ["interrupt", ("sleep", gate.STOP_GRACE), ("exit", gate.FAILED)])


if __name__ == "__main__":
    unittest.main()
