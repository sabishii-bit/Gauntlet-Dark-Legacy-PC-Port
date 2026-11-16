"""Synthetic GD-ROM extraction tests, held to the vectors the launcher's tests share; no game data
is required."""

from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts" / "assets"
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dreamcast_legends as dc
from legends_fixtures import (DREAMCAST_EXTRACTIONS, DREAMCAST_SETS, DREAMCAST_VECTORS,
                              NAME_DISK_LEN, VECTOR_FOLDER, cue_tracks, disc_paths, extract_set,
                              file_digests, fixture, iso_identifier, name_fixture, name_sheet,
                              name_table, on_this_system, packed_rows, raw_sector, record,
                              sheet_bytes, tree_listing, vectors, write_vectors)


VECTORS = vectors(DREAMCAST_VECTORS)
SIZES = VECTORS["sizes"]


def check_outcome(test: unittest.TestCase, case: dict, function, *args):
    """Checks that the extractor still makes of a case what the vectors hold for this system: it
    takes the case and gives its result, or refuses it with exactly the vectors' message."""
    python = on_this_system(case["python"])
    if python is None:
        return function(*args)
    with test.assertRaises(ValueError) as caught:
        function(*args)
    test.assertEqual(str(caught.exception), python)
    return None


class DreamcastDiscTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def new_folder(self) -> Path:
        return Path(tempfile.mkdtemp(dir=self.root))

    def test_absolute_extents_and_stored_pregap(self):
        cue = fixture(self.root)
        with dc.Disc(cue) as disc:
            self.assertEqual(disc.entries[0].lba, 45100)
            self.assertEqual(disc.read(45100, 5), b"hello")
            self.assertEqual(disc.tracks[1].source.index, 225)

    def test_high_density_minute_above_ninety_nine(self):
        self.assertEqual(dc.sector_lba(raw_sector(547163, b"")), 547163)

    def test_extraction_is_exact_and_repeatable(self):
        cue = fixture(self.root)
        output = self.root / "out"
        manifest = dc.extract(cue, output)
        self.assertEqual((output / "disc/TEST.WAD").read_bytes(), b"hello")
        self.assertEqual(dc.extract(cue, output), manifest)
        (output / "disc/TEST.WAD").write_bytes(b"other")
        with self.assertRaisesRegex(ValueError, "differs"):
            dc.extract(cue, output)
        self.assertEqual((output / "disc/TEST.WAD").read_bytes(), b"other")

    def test_rejects_extent_in_audio_gap(self):
        cue = fixture(self.root, file_lba=45050)
        with self.assertRaisesRegex(ValueError, "outside data tracks"):
            dc.Disc(cue)

    def test_rejects_unsafe_native_name(self):
        cue = fixture(self.root, name=b"../escape;1")
        with self.assertRaisesRegex(ValueError, "Unsafe disc name"):
            dc.Disc(cue)

    def test_rejects_external_cue_path(self):
        cue = self.root / "bad.cue"
        cue.write_text('FILE "../other.bin" BINARY\nTRACK 03 MODE1/2352\nINDEX 01 00:00:00\n')
        with self.assertRaisesRegex(ValueError, "Unsafe disc name"):
            dc.read_cue(cue)

    def test_rejects_multiextent_files(self):
        cue = fixture(self.root, flags=128)
        with self.assertRaisesRegex(ValueError, "multiextent"):
            dc.Disc(cue)

    def test_rejects_truncated_raw_track(self):
        cue = fixture(self.root)
        with (self.root / "track5.bin").open("ab") as stream:
            stream.write(b"x")
        with self.assertRaisesRegex(ValueError, "Truncated"):
            dc.Disc(cue)

    def test_rejects_inconsistent_sector_address(self):
        cue = fixture(self.root)
        with (self.root / "track3.bin").open("r+b") as stream:
            stream.seek(10 * dc.RAW_SECTOR + 14)
            stream.write(b"\x11")
        with self.assertRaisesRegex(ValueError, "disagrees"):
            dc.Disc(cue)

    def test_sector_addresses_follow_the_shared_vectors(self):
        cases = VECTORS["sectors"]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(header=case["header"]):
                lba = check_outcome(self, case, dc.sector_lba, bytes.fromhex(case["header"]))
                self.assertEqual(lba, case["lba"])

    def test_names_follow_the_shared_vectors(self):
        cases = VECTORS["names"]
        self.assertTrue(cases)
        for case in cases:
            name = case["name"]
            folder = self.new_folder()
            # A reader that takes a name lists it as it is.
            with self.subTest(name=name, reader="track"):
                tracks = check_outcome(self, case["track"], cue_tracks, name_sheet(folder, name))
                if tracks is not None:
                    self.assertEqual([track[1] for track in tracks], [name])
            with self.subTest(name=name, reader="iso"):
                paths = check_outcome(self, case["iso"], disc_paths,
                                      name_fixture(folder, iso_identifier(name)))
                if paths is not None:
                    self.assertEqual(paths, [name])
            with self.subTest(name=name, reader="sizes"):
                files = check_outcome(self, case["sizes"], packed_rows, name_table(name),
                                      NAME_DISK_LEN)
                if files is not None:
                    self.assertEqual([file[0] for file in files], [name])

    def test_iso_paths_follow_the_shared_vectors(self):
        cases = VECTORS["isoPaths"]
        self.assertTrue(cases)
        for case in cases:
            *folders, name = case["path"].split("/")
            with self.subTest(chars=case["chars"]):
                self.assertEqual((len(case["path"]), len(folders)), (case["chars"], case["depth"]))
                sheet = name_fixture(self.new_folder(), iso_identifier(name),
                                     [folder.encode("ascii") for folder in folders])
                paths = check_outcome(self, case, disc_paths, sheet)
                if paths is not None:
                    self.assertEqual(paths, [case["path"]])

    def test_sheets_follow_the_shared_vectors(self):
        cases = VECTORS["sheets"]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(sheet=case["name"]):
                sheet = self.new_folder() / "sheet.cue"
                sheet.write_bytes(sheet_bytes(case))
                tracks = check_outcome(self, case, cue_tracks, sheet)
                self.assertEqual(tracks, on_this_system(case["tracks"]))

    def test_golden_manifests_are_the_extractors_bytes(self):
        self.assertTrue(DREAMCAST_EXTRACTIONS)
        for name, (native, _) in DREAMCAST_EXTRACTIONS.items():
            case = VECTORS["sets"][name]
            with self.subTest(set=name):
                tree = self.new_folder() / "tree"
                extract_set(DREAMCAST_SETS[name](self.new_folder()), tree, native)
                self.assertTrue(case["goldens"])
                for path, golden in case["goldens"].items():
                    self.assertEqual((tree / path).read_bytes(),
                                     (VECTOR_FOLDER / golden).read_bytes())
                self.assertEqual(tree_listing(tree), case["tree"])


class PackedFileTests(unittest.TestCase):
    def check_table(self, case: dict):
        files = check_outcome(self, case, packed_rows, bytes.fromhex(case["hex"]), case["diskLen"])
        self.assertEqual(files, case["files"])

    def test_path_terminates_at_first_negative_component(self):
        self.check_table(SIZES["firstNegativeComponent"])

    def test_external_file_is_not_fabricated_from_disk(self):
        self.check_table(SIZES["externalFile"])

    def test_refuses_out_of_bounds_payload(self):
        self.check_table(SIZES["outOfBoundsPayload"])

    def test_refuses_out_of_bounds_name(self):
        self.check_table(SIZES["outOfBoundsName"])

    def test_refuses_truncated_table(self):
        self.check_table(SIZES["truncatedTable"])

    def test_refuses_duplicate_names(self):
        self.check_table(SIZES["duplicateNames"])

    def test_refuses_file_directory_conflict(self):
        self.check_table(SIZES["fileDirectoryConflict"])

    def test_unpack_preserves_native_bytes_and_external_records(self):
        case = SIZES["unpack"]
        self.check_table(case)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "disc").mkdir()
            (output / "disc/SIZES.ROM").write_bytes(bytes.fromhex(case["hex"]))
            (output / "disc/DISK.ROM").write_bytes(bytes.fromhex(case["disk"]))
            result = dc.unpack_native(output)
            self.assertEqual((output / "native/test.wad").read_bytes(), b"hello")
            self.assertFalse((output / "native/outside.rom").exists())
            self.assertEqual(len(result["external"]), 1)
            self.assertEqual((output / "native-manifest.json").read_bytes(),
                             (VECTOR_FOLDER / case["golden"]).read_bytes())
            self.assertEqual(tree_listing(output / "native"), case["native"])
            self.assertEqual(dc.unpack_native(output), result)

    def test_other_tables_follow_the_shared_vectors(self):
        cases = SIZES["extras"]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(table=case["name"]):
                self.check_table(case)


class VectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_builders_write_the_shared_sets(self):
        sets = VECTORS["sets"]
        folders = {}
        for name, build in DREAMCAST_SETS.items():
            with self.subTest(set=name):
                folders[name] = Path(tempfile.mkdtemp(dir=self.root))
                self.assertEqual(build(folders[name]).name, sets[name]["sheet"])
                self.assertEqual(file_digests(folders[name]), sets[name]["files"])
        # The GDI's data tracks hold the full set's, without the stored pregap, and start at the
        # LBAs the GDI gives.
        full = json.loads((VECTOR_FOLDER / sets["full"]["goldens"]["disc-manifest.json"])
                          .read_text(encoding="utf-8"))
        rows = sets["fullGdi"]["dataTracks"]
        self.assertEqual(len(rows), len(full["data_tracks"]))
        for (file, index, first_lba, sectors), track in zip(rows, full["data_tracks"]):
            with self.subTest(track=file):
                data = (folders["fullGdi"] / file).read_bytes()
                self.assertEqual(data, (folders["full"] / track["file"]).read_bytes()
                                 [track["index"] * dc.RAW_SECTOR:])
                self.assertEqual([index, first_lba, sectors],
                                 [0, dc.sector_lba(data[:16]), len(data) // dc.RAW_SECTOR])
                self.assertEqual([first_lba, sectors], [track["first_lba"], track["sectors"]])

    def test_sets_record_the_launchers_outcome(self):
        # The launcher refuses a disc whose root lacks SIZES.ROM or DISK.ROM, in exactly those
        # letters, before it writes anything. The extractor writes such a disc's tree, and its
        # program then fails restoring the native files.
        sets = VECTORS["sets"]
        self.assertTrue(DREAMCAST_EXTRACTIONS)
        for name in DREAMCAST_EXTRACTIONS:
            case = sets[name]
            with self.subTest(set=name):
                tree = Path(tempfile.mkdtemp(dir=self.root)) / "tree"
                dc.extract(DREAMCAST_SETS[name](Path(tempfile.mkdtemp(dir=self.root))), tree)
                names = {path.name for path in (tree / "disc").iterdir()}
                if {"SIZES.ROM", "DISK.ROM"} <= names:
                    self.assertEqual((case["python"], case["rule"]), (None, None))
                    self.assertNotIn("deviation", case)
                    dc.unpack_native(tree)
                else:
                    self.assertEqual((case["python"], case["rule"], case["deviation"]),
                                     (None, "nativeTableMissing", 12))
                    with self.assertRaises(OSError):
                        dc.unpack_native(tree)
        # The GDI set holds the full set's sectors, so the launcher decides it alike.
        self.assertEqual(sets["fullGdi"]["rule"], sets["full"]["rule"])

    def test_records_are_built_as_the_vectors_pin(self):
        cases = VECTORS["records"]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(record=case["name"]):
                built = record(case["name"].encode("ascii"), case["lba"], case["size"],
                               case["flags"])
                self.assertEqual(built.hex(), case["hex"])

    def test_vectors_are_current(self):
        self.maxDiff = None
        committed = sorted(path.name for path in VECTOR_FOLDER.glob("dreamcast-*.json"))
        folder = self.root / "vectors"
        written = write_vectors(folder, "dreamcast")
        self.assertEqual(sorted(written), committed)
        for name in written:
            with self.subTest(name=name):
                # Latin-1 keeps every byte as one character, so a difference shows as text.
                self.assertEqual((folder / name).read_bytes().decode("latin-1"),
                                 (VECTOR_FOLDER / name).read_bytes().decode("latin-1"))


if __name__ == "__main__":
    unittest.main()
