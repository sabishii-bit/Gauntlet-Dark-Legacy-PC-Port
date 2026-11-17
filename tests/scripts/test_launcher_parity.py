"""The launcher's headless program writes the Legends trees the extraction scripts write, byte for
byte, from the same synthetic dumps, and refuses the dumps the scripts fail on without writing
anything; no game data is read.

Each case builds its dump with the builders the extractors' own tests use, or from the vectors
they share with the launcher's tests, then extracts it into folders beside it twice: with the
script, in this process, and with the launcher, which holds the dump as the scripts hold one.

The launcher they run is the program GDL_LAUNCHER_BIN names, a path from the repository's root,
which must be there; else the newer of the release and debug builds under build/launcher. Without
either, the tests that run it skip.
"""

from __future__ import annotations

from functools import partial
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "scripts" / "assets"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import legends_fixtures
import n64_legends as n64
import process_tree
from legends_fixtures import (DREAMCAST_VECTORS, FULL_DISK_ROM, N64_VECTORS, extract_set, fixture,
                              full_gdi_set, full_set, iso_identifier, json_string, name_fixture,
                              paths_rom, reorder, rom, vectors)


# The variable that names the headless program to run, as a path from the repository's root.
LAUNCHER_VARIABLE = "GDL_LAUNCHER_BIN"

# The headless program's file, and the folders cargo builds it into.
PROGRAM = "gdl-launcher-headless" + (".exe" if os.name == "nt" else "")
BUILDS = (ROOT / "build" / "launcher" / "release", ROOT / "build" / "launcher" / "debug")

# How long one extraction by the launcher may run, in seconds, before it is stopped.
LAUNCHER_TIMEOUT = 120

# What the launcher exits with once it has extracted a tree, and once it has refused a dump.
EXTRACTED, REFUSED = 0, 1

# The one line the launcher refuses a dump with: the refusal's code, the rule that decides it, and
# what it found.
REFUSAL_LINE = re.compile(r"error: (?P<code>\S+) \((?P<rule>\w+)\): (?P<detail>[^\n]*)\n")

# The flag that starts the launcher, a console program, without a console window of its own on
# Windows.
NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

# What each extraction script's program reports as a failed extraction, exiting 1.
SCRIPT_FAILURES = {"n64": (OSError, n64.FormatError),
                   "dreamcast": (OSError, ValueError, UnicodeError, struct.error)}

N64_CASES = vectors(N64_VECTORS)
DREAMCAST_CASES = vectors(DREAMCAST_VECTORS)
IMAGES = {name: bytes.fromhex(text) for name, text in N64_CASES["images"].items()}

# The name the N64 extractor's tests give a dump, and the manifest the extractor writes.
DUMP_NAME = "source.z64"
N64_MANIFEST = "manifest.json"

# A ROM of plain files under names the N64 extractor takes, though they look like ones it reserves
# or like a disc's: a manifest below the root, a version suffix and a leading space.
PLAIN_FILES = [("/x/manifest.json", b"{}\n", False), ("/a;1", b"one", False),
               ("/ a", b"leading space", False)]

# The byte the corrupt stream puts first in the default image's first stored file: a last block of
# the type raw DEFLATE reserves, which no stream begins with; and the rule the launcher refuses a
# stream its inflater cannot decode by.
NO_STREAM = 0xFF
DEFLATE_RULE = "deflate"

# The vectors' cases the other N64 refusals come from: a path out of the tree, and two paths in two
# letter cases; and where a refused WAD goes in a ROM, which the extractors read as one by its
# suffix.
OUTSIDE_PATH = "/../outside"
ALIAS_PATHS = ["/file", "/FILE"]
BAD_WAD_PATH = "/bad.wad"

# The disc manifest, whose data tracks are a sheet's own.
DISC_MANIFEST = "disc-manifest.json"

# The file of the small and full sets' second data track, which holds the disc's files.
SECOND_TRACK = "track5.bin"

# Where the gap's disc lists the small set's file: in its audio track, between its data tracks.
GAP_LBA = 45050

# The ISO 9660 file flag of a record whose file goes on in another record.
MULTI_EXTENT = 0x80

# The vectors' SIZES.ROM table the full set holds in place of its own for a refusal, which counts
# more rows than it has; and the disc file name the vectors' discs hold that every reader refuses.
BAD_SIZES_ROM = "truncatedTable"
BAD_NAME = "A/B"


def built_launcher(environment=os.environ, root: Path = ROOT, builds=BUILDS) -> Path | None:
    """The headless program to run: the one LAUNCHER_VARIABLE names in `environment`, a path from
    `root` that must be there, since one named but missing fails rather than skips; else the newer,
    by its time of change, of those built in `builds`; or None where neither is."""
    if LAUNCHER_VARIABLE in environment:
        named = root / environment[LAUNCHER_VARIABLE]
        if not named.is_file():
            raise AssertionError(f"{LAUNCHER_VARIABLE} names no program: {named}")
        return named
    built = [folder / PROGRAM for folder in builds if (folder / PROGRAM).is_file()]
    return max(built, key=lambda program: program.stat().st_mtime, default=None)


def text(stream: bytes) -> str:
    """What the launcher wrote to one of its streams, as text, a byte that is no UTF-8 shown as its
    escape."""
    return stream.decode("utf-8", "backslashreplace")


def is_link(entry: os.DirEntry) -> bool:
    """Whether a folder's entry is a link of any kind: a symbolic link, or on Windows a junction or
    another reparse point."""
    attributes = getattr(entry.stat(follow_symlinks=False), "st_file_attributes", 0)
    return entry.is_symlink() or bool(attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def extract_with_script(platform: str, dump: Path, output: Path) -> None:
    """Extracts `dump` into `output` as the platform's extraction script's program does: the N64
    tree, or the Dreamcast disc's tree and then the native files restored from it."""
    if platform == "n64":
        n64.extract(dump, output)
    else:
        extract_set(dump, output, native=True)


def truncated_small_set(folder: Path) -> Path:
    """The small set in `folder` with a byte past its second track's last whole sector, as the
    extractor's test cuts it; returns its sheet."""
    sheet = fixture(folder)
    with (folder / SECOND_TRACK).open("ab") as track:
        track.write(b"x")
    return sheet


def full_set_holding(table: bytes):
    """A builder of the full set with `table` as its SIZES.ROM in place of its own."""
    def build(folder: Path) -> Path:
        with mock.patch.object(legends_fixtures, "FULL_SIZES_ROM", table):
            return full_set(folder)
    return build


def rendered(value) -> str:
    """A JSON value as the extraction scripts write their manifests: indented by two spaces."""
    return json.dumps(value, indent=2)


class Case:
    """The folders of one case, side by side in a new folder: those its dumps are built in, and the
    folders the script and the launcher write their trees into, which are not there before."""

    def __init__(self, scratch: Path):
        self.folder = Path(tempfile.mkdtemp(dir=scratch))
        self.script = self.folder / "script"
        self.launcher = self.folder / "launcher"

    def dumps(self, name: str = "dump") -> Path:
        """A new folder of the case's to build a dump in."""
        folder = self.folder / name
        folder.mkdir()
        return folder


class BuiltLauncherTests(unittest.TestCase):
    """Which program the parity tests run."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.builds = (self.root / "release", self.root / "debug")

    def built(self, folder: Path, age: float) -> Path:
        """A program file in the new folder `folder`, last changed `age` seconds ago."""
        folder.mkdir()
        program = folder / PROGRAM
        program.write_bytes(b"")
        changed = time.time() - age
        os.utime(program, (changed, changed))
        return program

    def test_the_newer_build_is_run(self):
        self.assertIsNone(built_launcher({}, self.root, self.builds))
        release = self.built(self.builds[0], 60)
        self.assertEqual(built_launcher({}, self.root, self.builds), release)
        debug = self.built(self.builds[1], 0)
        self.assertEqual(built_launcher({}, self.root, self.builds), debug)

    def test_a_named_program_comes_from_the_repositorys_root(self):
        self.built(self.builds[0], 0)
        named = self.built(self.root / "named", 60)
        environment = {LAUNCHER_VARIABLE: f"named/{PROGRAM}"}
        self.assertEqual(built_launcher(environment, self.root, self.builds), named)

    def test_a_named_program_that_is_missing_fails_rather_than_skips(self):
        self.built(self.builds[0], 0)
        for name in (f"missing/{PROGRAM}", "release", ""):
            with self.subTest(name=name), self.assertRaises(AssertionError):
                built_launcher({LAUNCHER_VARIABLE: name}, self.root, self.builds)


class ParityTests(unittest.TestCase):
    """What the parity tests share: the launcher they run, a scratch folder for their cases, and the
    checks of a tree the launcher wrote against the script's and of a dump both refuse."""

    program: Path

    @classmethod
    def setUpClass(cls):
        program = built_launcher()
        if program is None:
            raise unittest.SkipTest("no built launcher; run python scripts/launcher.py build")
        print(f"{cls.__name__} runs {program}", file=sys.stderr, flush=True)
        cls.program = program

    def setUp(self):
        self.maxDiff = None
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.scratch = Path(temporary.name)

    def launch(self, platform: str, dump: Path, output: Path) -> subprocess.CompletedProcess:
        """Runs the launcher's extraction of the `platform` expansion's tree from `dump` into
        `output`, the dump held as the extraction scripts hold one: tethered to this process, and
        stopped once it runs past LAUNCHER_TIMEOUT."""
        command = [str(self.program), "--extract", platform, str(dump), "--output", str(output),
                   "--strict", "script"]
        return process_tree.run_tethered(command, timeout=LAUNCHER_TIMEOUT,
                                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, creationflags=NO_WINDOW)

    def assert_extracted(self, platform: str, dump: Path, output: Path) -> None:
        """Runs the launcher's extraction of `dump` into `output`, and checks that it extracted."""
        done = self.launch(platform, dump, output)
        self.assertEqual(done.returncode, EXTRACTED, text(done.stdout + done.stderr))
        self.assertEqual(text(done.stderr), "")

    def listing(self, root: Path) -> list[list]:
        """Every file of the tree at `root` as [path, size, SHA-256], its path in the letters the
        system stores, sorted by the path's text as the shared vectors list a tree. A link of any
        kind, which is never followed, anything else that is neither a folder nor a regular file,
        and a folder that holds nothing fail the test: an extraction makes no folder but those
        above its files."""
        rows = []
        folders = [(root, "")]
        while folders:
            folder, above = folders.pop()
            with os.scandir(folder) as found:
                entries = list(found)
            self.assertTrue(entries, f"the folder {folder} holds nothing")
            for entry in entries:
                path = above + entry.name
                self.assertFalse(is_link(entry), f"{path} is a link")
                if entry.is_dir(follow_symlinks=False):
                    folders.append((Path(entry.path), path + "/"))
                    continue
                self.assertTrue(entry.is_file(follow_symlinks=False), f"{path} is no file")
                data = Path(entry.path).read_bytes()
                rows.append([path, len(data), hashlib.sha256(data).hexdigest()])
        return sorted(rows, key=lambda row: row[0])

    def assert_same_tree(self, made: Path, written: Path, apart: tuple[str, ...] = ()) -> None:
        """Checks that the tree the launcher made at `made` is the one the script wrote at
        `written`, but for the paths `apart` names: the same paths in the same letters, and the
        same bytes in every file. A JSON file that differs, such as a manifest, shows how."""
        made_rows = [row for row in self.listing(made) if row[0] not in apart]
        written_rows = [row for row in self.listing(written) if row[0] not in apart]
        self.assertTrue(written_rows)
        self.assertEqual([row[0] for row in made_rows], [row[0] for row in written_rows])
        for path, _, _ in written_rows:
            if path.endswith(".json"):
                # Latin-1 keeps every byte as one character, so a difference shows as text.
                self.assertEqual((made / path).read_bytes().decode("latin-1"),
                                 (written / path).read_bytes().decode("latin-1"), path)
        self.assertEqual(made_rows, written_rows)

    def assert_extracts_alike(self, platform: str, dump: Path, case: Case) -> Path:
        """Extracts `dump` with the script and with the launcher into the case's folders, checks
        that both wrote the same tree, and gives the launcher's."""
        extract_with_script(platform, dump, case.script)
        self.assert_extracted(platform, dump, case.launcher)
        self.assert_same_tree(case.launcher, case.script)
        return case.launcher

    def assert_refused_alike(self, platform: str, dump: Path, case: Case, rule: str,
                             same_reason: bool = False) -> BaseException:
        """Checks that the script's extraction of `dump` fails as its program reports a failed
        one, and that the launcher refuses the dump by `rule`, exiting 1 with its one line, and
        writes nothing: not its output, nor anything else beside it. The two messages are each
        program's own; with `same_reason` they end in the same reason. Gives the script's error."""
        with self.assertRaises(SCRIPT_FAILURES[platform]) as failed:
            extract_with_script(platform, dump, case.script)
        before = sorted(os.listdir(case.folder))
        done = self.launch(platform, dump, case.launcher)
        self.assertEqual(done.returncode, REFUSED, text(done.stdout + done.stderr))
        self.assertEqual(text(done.stdout), "")
        refusal = REFUSAL_LINE.fullmatch(text(done.stderr))
        self.assertIsNotNone(refusal, text(done.stderr))
        self.assertEqual(refusal["rule"], rule, refusal.group(0))
        if same_reason:
            self.assertEqual(refusal["detail"].rpartition(": ")[2],
                             str(failed.exception).rpartition(": ")[2])
        self.assertFalse(os.path.lexists(case.launcher))
        self.assertEqual(sorted(os.listdir(case.folder)), before)
        return failed.exception


class N64ParityTests(ParityTests):
    def assert_image_extracts_alike(self, image: bytes, name: str = DUMP_NAME) -> Path:
        """Writes `image` as the dump file `name` of a new case, extracts it with the script and
        with the launcher, checks that both wrote the same tree, and gives the launcher's."""
        case = Case(self.scratch)
        source = case.dumps() / name
        source.write_bytes(image)
        return self.assert_extracts_alike("n64", source, case)

    def assert_frozen_image_extracts_alike(self, image: str) -> None:
        """Checks the frozen image `image` under the name the vectors extract it from."""
        found = [case for case in N64_CASES["extractions"] if case["image"] == image]
        self.assertEqual(len(found), 1)
        self.assert_image_extracts_alike(IMAGES[image], found[0]["name"])

    def test_n64_default_image_trees_match(self):
        self.assert_frozen_image_extracts_alike("default")

    def test_n64_mixed_image_trees_match(self):
        self.assert_frozen_image_extracts_alike("mixed")

    def test_n64_built_images_trees_match(self):
        # Compressed afresh by this Python's zlib, and of plain files alone.
        for label, files in (("default", None), ("plain", PLAIN_FILES)):
            with self.subTest(image=label):
                self.assert_image_extracts_alike(rom(files))

    def test_n64_byte_orders_trees_match(self):
        cases = N64_CASES["byteOrders"]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(order=case["byteOrder"]):
                self.assert_image_extracts_alike(reorder(IMAGES["default"], case["transform"]),
                                                 case["name"])

    def test_n64_source_names_match(self):
        cases = N64_CASES["sourceNames"]
        self.assertTrue(cases)
        for case in cases:
            name = json_string({"units": case["units"]})
            with self.subTest(name=ascii(name)):
                folders = Case(self.scratch)
                source = folders.dumps() / name
                try:
                    source.write_bytes(IMAGES["default"])
                except (OSError, UnicodeError) as error:
                    self.skipTest(f"this file system takes no file named {ascii(name)}: {error}")
                made = self.assert_extracts_alike("n64", source, folders)
                # Both wrote the name as the vectors pin it.
                lines = (made / N64_MANIFEST).read_bytes().decode("ascii").split("\n")
                self.assertEqual(lines[lines.index('  "source": {') + 1], case["line"])

    def test_n64_refusals_match(self):
        default = IMAGES["default"]
        corrupt = bytearray(default)
        _, entries = n64.read_directory(default)
        corrupt[entries[0].rom_offset] = NO_STREAM
        outside = [case for case in N64_CASES["paths"] if case["path"] == OUTSIDE_PATH]
        alias = [case for case in N64_CASES["collisions"] if case["paths"] == ALIAS_PATHS]
        wads = [case for case in N64_CASES["wads"] if case["rule"] is not None]
        self.assertEqual((len(outside), len(alias)), (1, 1))
        self.assertTrue(wads)
        for case in (*outside, *alias, wads[0]):
            self.assertIsNotNone(case["python"])
        cases = [
            # Both inflaters fail on the stream for the same reason, in words of their own.
            ("a corrupt stream", bytes(corrupt), DEFLATE_RULE, True),
            ("a path out of the tree", paths_rom([OUTSIDE_PATH]), outside[0]["rule"], False),
            ("a case alias", paths_rom(ALIAS_PATHS), alias[0]["rule"], False),
            ("a bad WAD", rom([(BAD_WAD_PATH, bytes.fromhex(wads[0]["hex"]), False)]),
             wads[0]["rule"], False),
        ]
        for label, image, rule, same_reason in cases:
            with self.subTest(dump=label):
                case = Case(self.scratch)
                source = case.dumps() / DUMP_NAME
                source.write_bytes(image)
                self.assert_refused_alike("n64", source, case, rule, same_reason)


class DreamcastParityTests(ParityTests):
    def test_dreamcast_small_set_is_refused_by_both(self):
        # The small set's disc's root holds no SIZES.ROM. The extractor's program writes the disc's
        # tree and then fails restoring the native files; the launcher refuses the disc before it
        # writes anything. What the extractor wrote is the disc's tree alone.
        small = DREAMCAST_CASES["sets"]["small"]
        case = Case(self.scratch)
        failed = self.assert_refused_alike("dreamcast", fixture(case.dumps()), case, small["rule"])
        self.assertIsInstance(failed, FileNotFoundError)
        self.assertEqual(self.listing(case.script), small["tree"])

    def test_dreamcast_full_set_trees_match(self):
        case = Case(self.scratch)
        sheet = full_set(case.dumps())
        extract_with_script("dreamcast", sheet, case.script)
        # The sheet, the folder that holds it, and a track's file that it names.
        forms = {"sheet": sheet, "folder": sheet.parent, "track": sheet.parent / SECOND_TRACK}
        for form, dump in forms.items():
            with self.subTest(form=form):
                made = case.folder / form
                self.assert_extracted("dreamcast", dump, made)
                self.assert_same_tree(made, case.script)

    def test_dreamcast_gdi_tree_matches_the_cue_tree(self):
        # The full set's sectors behind its cue sheet for the script, and behind a GDI for the
        # launcher: every file the same but the disc manifest, whose data tracks are the GDI's.
        case = Case(self.scratch)
        extract_with_script("dreamcast", full_set(case.dumps("cue")), case.script)
        self.assert_extracted("dreamcast", full_gdi_set(case.dumps("gdi")), case.launcher)
        self.assert_same_tree(case.launcher, case.script, apart=(DISC_MANIFEST,))
        made_bytes = (case.launcher / DISC_MANIFEST).read_bytes()
        made = json.loads(made_bytes)
        written = json.loads((case.script / DISC_MANIFEST).read_bytes())
        self.assertEqual(list(made), list(written))
        for key in written:
            if key != "data_tracks":
                self.assertEqual(made[key], written[key], key)
        self.assertEqual(rendered(made["files"]), rendered(written["files"]))
        # The GDI's own tracks, in the fields and order the extractor writes a sheet's.
        fields = list(written["data_tracks"][0])
        rows = DREAMCAST_CASES["sets"]["fullGdi"]["dataTracks"]
        tracks = [dict(zip(fields, row)) for row in rows]
        self.assertEqual(made["data_tracks"], tracks)
        expected = rendered(dict(written, data_tracks=tracks)) + "\n"
        self.assertEqual(made_bytes.decode("latin-1"), expected)

    def test_dreamcast_refusals_match(self):
        sizes = DREAMCAST_CASES["sizes"][BAD_SIZES_ROM]
        names = [case["iso"] for case in DREAMCAST_CASES["names"] if case["name"] == BAD_NAME]
        self.assertEqual(len(names), 1)
        for refused in (sizes, names[0]):
            self.assertIsNotNone(refused["python"])
        # The table's rule holds for a DISK.ROM of its length, which is the full set's.
        self.assertEqual(sizes["diskLen"], len(FULL_DISK_ROM))
        cases = [
            # The extractor's tests' discs: the small set with its file listed in its audio track,
            # or flagged as going on in another record, or with its second track cut; each with
            # the rule the launcher refuses it by.
            ("an extent in the gap", partial(fixture, file_lba=GAP_LBA), "extentOutside"),
            ("a multiextent record", partial(fixture, flags=MULTI_EXTENT), "recordUnsupported"),
            ("a truncated track", truncated_small_set, "truncatedTrack"),
            ("a bad SIZES.ROM", full_set_holding(bytes.fromhex(sizes["hex"])), sizes["rule"]),
            ("an identifier A/B;1", partial(name_fixture, identifier=iso_identifier(BAD_NAME)),
             names[0]["rule"]),
        ]
        for label, build, rule in cases:
            with self.subTest(dump=label):
                case = Case(self.scratch)
                self.assert_refused_alike("dreamcast", build(case.dumps()), case, rule)


if __name__ == "__main__":
    unittest.main()
