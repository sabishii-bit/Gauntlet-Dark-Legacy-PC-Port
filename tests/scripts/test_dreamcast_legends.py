"""Synthetic GD-ROM extraction tests; no game data is required."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts" / "assets"
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dreamcast_legends as dc
from legends_fixtures import fixture, raw_sector


class DreamcastDiscTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

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


class PackedFileTests(unittest.TestCase):
    @staticmethod
    def table(offset=0, size=5, component=7):
        return (struct.pack("<IIi4h", 1, size, offset, 0, component, -1, 0) +
                b"levels\0worlds.rom\0")

    def test_path_terminates_at_first_negative_component(self):
        files = dc.packed_files(self.table(), 5)
        self.assertEqual(files[0].path.as_posix(), "levels/worlds.rom")
        self.assertEqual(files[0].size, 5)

    def test_external_file_is_not_fabricated_from_disk(self):
        files = dc.packed_files(self.table(offset=-1), 0)
        self.assertEqual(files[0].offset, -1)

    def test_refuses_out_of_bounds_payload(self):
        with self.assertRaisesRegex(ValueError, "extent"):
            dc.packed_files(self.table(size=6), 5)

    def test_refuses_out_of_bounds_name(self):
        with self.assertRaisesRegex(ValueError, "string pool"):
            dc.packed_files(self.table(component=100), 5)

    def test_refuses_truncated_table(self):
        with self.assertRaisesRegex(ValueError, "table"):
            dc.packed_files(struct.pack("<I", 1000), 5)

    def test_refuses_duplicate_names(self):
        row = struct.pack("<Ii4h", 5, 0, 0, -1, 0, 0)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            dc.packed_files(struct.pack("<I", 2) + row * 2 + b"file\0", 5)

    def test_refuses_file_directory_conflict(self):
        row1 = struct.pack("<Ii4h", 5, 0, 0, -1, 0, 0)
        row2 = struct.pack("<Ii4h", 5, 0, 0, 5, -1, 0)
        with self.assertRaisesRegex(ValueError, "path conflict"):
            dc.packed_files(struct.pack("<I", 2) + row1 + row2 + b"file\0child\0", 5)

    def test_unpack_preserves_native_bytes_and_external_records(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            (output / "disc").mkdir()
            row1 = struct.pack("<Ii4h", 5, 0, 0, -1, 0, 0)
            row2 = struct.pack("<Ii4h", 9, -1, 9, -1, 0, 0)
            (output / "disc/SIZES.ROM").write_bytes(
                struct.pack("<I", 2) + row1 + row2 + b"test.wad\0outside.rom\0")
            (output / "disc/DISK.ROM").write_bytes(b"hello")
            result = dc.unpack_native(output)
            self.assertEqual((output / "native/test.wad").read_bytes(), b"hello")
            self.assertFalse((output / "native/outside.rom").exists())
            self.assertEqual(len(result["external"]), 1)
            self.assertEqual(dc.unpack_native(output), result)


if __name__ == "__main__":
    unittest.main()
