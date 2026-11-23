"""The parity gate holds the launcher's Legends trees to the extraction scripts' and to the
reference trees, runs its steps in order with their timeouts, and reports what it found. Its trees
are synthetic and every child it starts has a stand-in: nothing is built or run, and no game data
is read."""

from __future__ import annotations

import contextlib
import errno
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
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

# The bytes the gate hashes at a time in these tests, so that every synthetic file spans several
# blocks, as the files of the dumps' trees span many.
TEST_HASH_BLOCK = 5

# The variables that tell the launcher's ignored tests where the dumps, the reference trees and the
# extracted GameCube disc are.
VARIABLES = ("GDL_LEGENDS_N64_ROM", "GDL_LEGENDS_DREAMCAST_DISC", "GDL_LEGENDS_N64_TREE",
             "GDL_LEGENDS_DREAMCAST_TREE", "GDL_GAMECUBE_TREE")

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
    the launcher the one `made` gives in its place. A step `raises` names raises what it gives
    instead, once recorded, and so does the making of cargo's environment, as "cargo
    environment"."""

    def __init__(self, test: unittest.TestCase, codes: dict[str, int] | None = None,
                 made: dict[str, dict[str, bytes]] | None = None,
                 raises: dict[str, BaseException] | None = None):
        self.test = test
        self.codes = codes or {}
        self.made = made or {}
        self.raises = raises or {}
        self.started: list[tuple[str, list[str] | None, dict]] = []

    def record(self, step: str, command: list[str] | None = None, **keywords) -> int:
        self.started.append((step, command, keywords))
        if step in self.raises:
            raise self.raises[step]
        return self.codes.get(step, 0)

    def environment(self) -> dict[str, str]:
        if "cargo environment" in self.raises:
            raise self.raises["cargo environment"]
        return dict(CARGO_ENVIRONMENT)

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
                            if str(script) in command)
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


class Clock:
    """Stands in for the gate's clock, whose time moves only as the gate sleeps, and records each
    sleep."""

    START = 100.0  # the time the clock reads before the gate first sleeps

    def __init__(self):
        self.now = self.START
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


class Hold:
    """Stands in for shutil.rmtree on folders that a passing hold on a file refuses: each folder's
    first `refusals` removals raise `error`, and the next removes it. Records every removal tried,
    with the time `clock` reads as it is tried."""

    def __init__(self, error: OSError, refusals: int, clock: Clock):
        self.error = error
        self.refusals = refusals
        self.clock = clock
        self.remove = shutil.rmtree  # taken before the stand-in replaces it
        self.attempts: list[tuple[Path, float]] = []

    def __call__(self, path, *args, **keywords) -> None:
        folder = Path(path)
        self.attempts.append((folder, self.clock.monotonic()))
        if sum(1 for tried, _ in self.attempts if tried == folder) <= self.refusals:
            raise self.error
        self.remove(path, *args, **keywords)

    @contextlib.contextmanager
    def in_place(self, windows: bool = True):
        """Stands in for the gate's removals, and its clock for the gate's, while the block runs,
        as on Windows or on another system as `windows` says."""
        with mock.patch.object(gate.process_tree, "WINDOWS", windows), \
                mock.patch.object(gate, "time", self.clock), \
                mock.patch.object(gate.shutil, "rmtree", side_effect=self):
            yield


class CompareTests(unittest.TestCase):
    """The launcher's tree against the scripts' and against the reference tree."""

    def setUp(self):
        self.maxDiff = None
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.scratch = Path(temporary.name)
        for patch in (synthetic_counts(), mock.patch.object(gate, "HASH_BLOCK", TEST_HASH_BLOCK)):
            patch.start()
            self.addCleanup(patch.stop)

    def folder(self) -> Path:
        return Path(tempfile.mkdtemp(dir=self.scratch))

    def pair(self) -> tuple[Path, Path, Path]:
        """The launcher's, the scripts' and the reference N64 trees, each the synthetic tree."""
        return tuple(write(self.folder(), n64_tree()) for _ in range(3))

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

    def test_every_block_of_a_file_is_hashed(self):
        data = contents("texture.bin") * 3
        self.assertGreater(len(data), 2 * gate.HASH_BLOCK)
        path = write(self.folder(), {"texture.bin": data}) / "texture.bin"
        self.assertEqual(gate.fingerprint(path), (len(data), hashlib.sha256(data).hexdigest()))
        # Its last byte changed, past the first block, a file is a difference.
        changed = data[:-1] + b"!"
        tree = {**n64_tree(), "texture.bin": data}
        result = self.compare("n64", made={**tree, "texture.bin": changed}, written=tree,
                              reference=tree)
        self.assertEqual(result["differences"], [
            f"texture.bin: the launcher's tree holds {held(changed)}, the scripts' {held(data)}",
            f"texture.bin: the launcher's tree holds {held(changed)}, the reference tree "
            f"{held(data)}"])

    def test_what_is_no_regular_file_differs_even_from_the_same(self):
        # An extraction writes regular files alone, so the same empty folder in both trees is a
        # difference too.
        made, written, reference = self.pair()
        for root in (made, written):
            (root / "empty").mkdir()
        self.assertEqual(gate.compare_trees("n64", made, written, reference)["differences"],
                         ["empty/: the launcher's tree holds an empty folder, the scripts' an "
                          "empty folder"])

    def test_a_link_is_never_followed(self):
        # A link in both trees, to a folder that holds the file a manifest names, is a link in
        # each, not a file of the tree.
        made, written, reference = self.pair()
        target = write(self.folder(), {"worlds.wad": contents("levels/a/worlds.wad")})
        for root in (made, written):
            shutil.rmtree(root / "levels")
            try:
                os.symlink(target, root / "levels", target_is_directory=True)
            except (OSError, NotImplementedError) as error:
                self.skipTest(f"this system makes no symbolic link here: {error}")
        self.assertEqual(gate.compare_trees("n64", made, written, reference)["differences"], [
            "levels: the launcher's tree holds a link, the scripts' a link",
            "levels/a/worlds.wad: the launcher's tree holds nothing, the reference tree "
            f"{held(contents('levels/a/worlds.wad'))}"])

    @unittest.skipUnless(os.name == "nt", "junctions are Windows' own links")
    def test_a_junction_is_a_link(self):
        import _winapi  # Windows' own API module, which makes junctions without privileges

        made, written, reference = self.pair()
        target = write(self.folder(), {"a/worlds.wad": contents("levels/a/worlds.wad")})
        for root in (made, written):
            shutil.rmtree(root / "levels")
            _winapi.CreateJunction(str(target), str(root / "levels"))
        self.assertEqual(gate.compare_trees("n64", made, written, reference)["differences"], [
            "levels: the launcher's tree holds a link, the scripts' a link",
            "levels/a/worlds.wad: the launcher's tree holds nothing, the reference tree "
            f"{held(contents('levels/a/worlds.wad'))}"])
        # Removing the trees removes the junction, never what it leads to.
        shutil.rmtree(made)
        self.assertTrue((target / "a" / "worlds.wad").is_file())

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
        # The extracted disc's icon is a stand-in: the gate only looks that it is there.
        self.disc_tree = write(self.scratch / "gamecube", {"carddemo/icon.tpl": b"an icon"})
        self.environment = {"GDL_LEGENDS_N64_TREE": str(self.references["n64"]),
                            "GDL_LEGENDS_DREAMCAST_TREE": str(self.references["dreamcast"]),
                            "GDL_GAMECUBE_TREE": str(self.disc_tree)}
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
                mock.patch.object(gate.launcher, "environment", side_effect=children.environment), \
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
                    [sys.executable, "-I", "-X", "utf8", str(gate.SCRIPTS[platform]), str(dump),
                     "--output", str(python / platform)], {"timeout": gate.EXTRACTION_TIMEOUT}))
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
            "GDL_LEGENDS_DREAMCAST_TREE": str(self.references["dreamcast"]),
            "GDL_GAMECUBE_TREE": str(self.disc_tree)})
        self.assertEqual(set(VARIABLES) & set(children.find("cargo build")[1]["env"]), set())
        # The tests read both dumps, both trees and the disc's icon, though only the N64 trees
        # are compared.
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
        self.assertEqual((gate.GAMECUBE_TREE, gate.GAMECUBE_ICON),
                         (ROOT / "assets" / "GUNE5D", Path("carddemo", "icon.tpl")))
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
                script = children.find("script n64")[0]
                self.assertEqual(script[script.index(str(gate.SCRIPTS["n64"])) + 1], str(rom))
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
            # So do they the extracted disc's icon, which no platform's comparison needs.
            (self.argv, {**self.environment, "GDL_GAMECUBE_TREE": str(missing)},
             "launcher parity: the GameCube disc's icon is not at "
             f"{missing / 'carddemo' / 'icon.tpl'}; set GDL_GAMECUBE_TREE"),
            ([*self.argv, "--launcher", str(missing)], self.environment,
             f"launcher parity: there is no launcher at {missing}"),
        ]
        for argv, environment, line in cases:
            with self.subTest(line=line):
                # What an earlier run reported never stands for this one.
                write(self.output, {"report.json": b'{"ignoredTests": "passed"}\n'})
                children = Children(self)
                code, out, err = self.run_gate(children, argv, environment)
                self.assertEqual((code, out, err), (gate.FAILED, "", line + "\n"))
                self.assertEqual(children.started, [])
                # Nothing is made but the report, which names what the run lacks.
                self.assertEqual(os.listdir(self.output), ["report.json"])
                report = self.report()
                self.assertEqual(report["failure"], line.replace("launcher parity: ", "", 1))
                self.assertEqual((report["ignoredTests"], report["fingerprints"],
                                  report["platforms"]), ("notRun", "notChecked", {}))
        # Each input the run lacks has its line, and the report names them all.
        lines = [line for _, _, line in cases[:2]]
        code, out, err = self.run_gate(Children(self), ["--verify", "--n64-rom", str(missing)],
                                       {**self.environment,
                                        "GDL_LEGENDS_DREAMCAST_DISC": str(missing)})
        self.assertEqual((code, out, err),
                         (gate.FAILED, "", "".join(f"{line}\n" for line in lines)))
        self.assertEqual(self.report()["failure"],
                         "; ".join(line.replace("launcher parity: ", "", 1) for line in lines))

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

    def test_a_removal_waits_out_a_passing_hold_on_windows(self):
        # A virus scanner or an indexer that opens a file just written holds off its removal for
        # a moment: the file's own, or its folder's while the file goes only once the hold lets
        # go. The run tries again and passes.
        python, made = self.output / "python", self.output / "launcher"
        for error in (PermissionError("held"), OSError(errno.ENOTEMPTY, "not empty")):
            with self.subTest(error=error):
                clock = Clock()
                hold = Hold(error, 1, clock)
                with hold.in_place():
                    code, _, err = self.run_gate(Children(self))
                self.assertEqual((code, err), (gate.PASSED, ""))
                self.assertNotIn("failure", self.report())
                self.assertFalse(os.path.lexists(python))
                self.assertFalse(os.path.lexists(made))
                # Each tree is refused once, then removed after a pause.
                self.assertEqual([folder for folder, _ in hold.attempts],
                                 [python, python, made, made])
                self.assertEqual(clock.sleeps, [gate.REMOVAL_INTERVAL] * 2)

    def test_a_removal_is_tried_again_only_for_a_hold_on_windows(self):
        # Elsewhere no scanner holds a file, and on Windows another error is no hold: either
        # fails the run at once, though trying again would have worked.
        python, made = self.output / "python", self.output / "launcher"
        cases = [(False, PermissionError("held")), (True, OSError(errno.EIO, "a disk error"))]
        for windows, error in cases:
            with self.subTest(windows=windows, error=error):
                clock = Clock()
                hold = Hold(error, 1, clock)
                with hold.in_place(windows):
                    code, _, _ = self.run_gate(Children(self))
                self.assertEqual(code, gate.FAILED)
                self.assertEqual(self.report()["failure"],
                                 f"the trees could not be removed: {error}")
                self.assertEqual(len(hold.attempts), 1)
                self.assertEqual(clock.sleeps, [])
                # What the run kept goes at the start of the next, before that run's own trees
                # are made and, once it passes, removed.
                remove = Hold(error, 0, clock)
                with mock.patch.object(gate.shutil, "rmtree", side_effect=remove):
                    code, _, err = self.run_gate(Children(self))
                self.assertEqual(code, gate.PASSED, err)
                self.assertEqual([folder for folder, _ in remove.attempts],
                                 [python, made, python, made])

    def test_trees_that_cannot_be_removed_fail_the_run(self):
        # A hold that outlasts the time a removal is tried for fails the run, and its report says
        # why rather than stand as a pass beside the trees it kept.
        clock = Clock()
        hold = Hold(PermissionError("held"), sys.maxsize, clock)
        with hold.in_place():
            code, out, err = self.run_gate(Children(self))
        self.assertEqual(code, gate.FAILED)
        # Tried at once, then after every pause until the time is up, and not after it.
        self.assertEqual({folder for folder, _ in hold.attempts}, {self.output / "python"})
        times = [at for _, at in hold.attempts]
        self.assertEqual(times[0], Clock.START)
        self.assertLess(times[-2], Clock.START + gate.REMOVAL_WINDOW)
        self.assertGreaterEqual(times[-1], Clock.START + gate.REMOVAL_WINDOW)
        self.assertEqual(clock.sleeps, [gate.REMOVAL_INTERVAL] * (len(times) - 1))
        report = self.report()
        self.assertEqual(report["failure"], "the trees could not be removed: held")
        self.assertEqual((report["ignoredTests"], report["fingerprints"]), ("passed", "current"))
        self.assertEqual([record["identical"] for record in report["platforms"].values()],
                         [True, True])
        for folder in ("python", "launcher"):
            self.assertEqual(sorted(os.listdir(self.output / folder)), ["dreamcast", "n64"])
        self.assertIn("dreamcast: identical (", out)
        self.assertEqual(err, "launcher parity: the trees could not be removed: held; the report "
                              f"is {self.output / 'report.json'}, and what the run extracted "
                              f"stays in {self.output}\n")

    def test_a_run_that_ends_before_its_report_leaves_none(self):
        # An interrupt, as at the terminal or from a caller that is gone, and a developer
        # environment that cannot be made, end the run before it writes its report; no earlier
        # run's report is left to stand for it.
        cases = [("cargo environment", SystemExit("VsDevCmd.bat failed")),
                 ("cargo test", KeyboardInterrupt()),
                 ("launcher dreamcast", KeyboardInterrupt())]
        for step, error in cases:
            with self.subTest(step=step):
                code, _, err = self.run_gate(Children(self))
                self.assertEqual(code, gate.PASSED, err)
                self.assertTrue((self.output / "report.json").is_file())
                with self.assertRaises(type(error)):
                    self.run_gate(Children(self, raises={step: error}))
                self.assertFalse(os.path.lexists(self.output / "report.json"))

    def test_an_extraction_that_never_finishes_is_its_platforms_difference(self):
        # One stopped at its timeout, as a hung extraction is, or one that cannot start fails its
        # platform; the run still ends with its report, and nothing is compared.
        cases = [("launcher dreamcast",
                  subprocess.TimeoutExpired("headless", gate.EXTRACTION_TIMEOUT), "dreamcast",
                  f"the launcher ran longer than {gate.EXTRACTION_TIMEOUT} s and was stopped"),
                 ("script n64", OSError("no such program"), "n64",
                  "the extraction script did not start: no such program")]
        for step, error, platform, line in cases:
            with self.subTest(step=step):
                children = Children(self, raises={step: error})
                code, out, _ = self.run_gate(children)
                self.assertEqual(code, gate.FAILED)
                self.assertEqual(children.steps(), STEPS[:STEPS.index(step) + 1])
                report = self.report()
                self.assertEqual(report["failure"], f"the {platform} trees did not pass")
                record = report["platforms"][platform]
                self.assertEqual((record["identical"], record["reference"], record["differences"]),
                                 (False, "notCompared", [line]))
                self.assertIn(f"{platform}: 1 difference; ", out)
                self.assertIn(f"\n  {line}\n", out)

    def test_a_fingerprints_check_that_never_finishes_checks_nothing(self):
        cases = [(subprocess.TimeoutExpired("check", gate.FINGERPRINTS_TIMEOUT),
                  f"the fingerprints' check ran longer than {gate.FINGERPRINTS_TIMEOUT} s and was "
                  "stopped"),
                 (OSError("no such program"),
                  "the fingerprints' check did not start: no such program")]
        for error, problem in cases:
            with self.subTest(problem=problem):
                code, _, err = self.run_gate(Children(self, raises={"fingerprints": error}))
                self.assertEqual(code, gate.FAILED)
                report = self.report()
                self.assertEqual(report["fingerprints"], "notChecked")
                failure = ("the Dreamcast fingerprints the launcher carries could not be checked: "
                           + problem)
                self.assertEqual(report["failure"], failure)
                self.assertTrue(err.startswith(f"launcher parity: {failure}; "), err)

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
