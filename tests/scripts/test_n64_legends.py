"""Native ROMDISK extraction, bounds checks and byte preservation, held to the vectors the
launcher's tests share."""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "assets"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import n64_legends as n64
from legends_fixtures import (JSON_STRINGS, N64_VECTORS, VECTOR_FOLDER, header_input, json_string,
                              paths_rom, reorder, rom, tree_listing, vectors, wad, with_header_word,
                              with_record_word, write_vectors)


VECTORS = vectors(N64_VECTORS)
IMAGES = {name: bytes.fromhex(text) for name, text in VECTORS["images"].items()}


def check_outcome(test: unittest.TestCase, case: dict, function, *args, **kwargs):
    """Checks that the extractor still makes of a case what the vectors hold: it takes the case and
    gives its result, or refuses it with exactly the vectors' message."""
    if case["python"] is None:
        return function(*args, **kwargs)
    with test.assertRaises(n64.FormatError) as caught:
        function(*args, **kwargs)
    test.assertEqual(str(caught.exception), case["python"])
    return None


class RomOrderTests(unittest.TestCase):
    def test_supported_byte_orders_normalize_identically(self):
        image = rom()
        paired = b"".join(image[i:i + 2][::-1] for i in range(0, len(image), 2))
        words = b"".join(image[i:i + 4][::-1] for i in range(0, len(image), 4))
        self.assertEqual(n64.normalize_rom(image), (image, "big-endian"))
        self.assertEqual(n64.normalize_rom(paired), (image, "byte-swapped"))
        self.assertEqual(n64.normalize_rom(words), (image, "little-endian"))

    def test_invalid_headers_fail(self):
        for case in VECTORS["headers"]:
            data = header_input(case, IMAGES)
            with self.subTest(size=len(data)):
                self.assertIsNotNone(case["python"])
                check_outcome(self, case, n64.normalize_rom, data)


class DirectoryTests(unittest.TestCase):
    def test_native_files_and_unknown_word_preserved(self):
        image = rom()
        base, entries = n64.read_directory(image)
        self.assertEqual(base, 64)
        self.assertEqual([entry.path for entry in entries], ["levels/test/worlds.wad", "texture.bin"])
        self.assertEqual([n64.read_entry(image, entry) for entry in entries], [wad(), b"native"])
        self.assertEqual(entries[0].unknown_word, 0x12345678)

    def test_missing_or_duplicate_romdisk_header_fails(self):
        for image in (b"\0" * 512, rom() + n64.ROMDISK_MAGIC):
            with self.assertRaises(n64.FormatError):
                n64.read_directory(image)

    def test_unsafe_paths_fail(self):
        cases = [case for case in VECTORS["paths"] if case["python"] is not None]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(path=case["path"]):
                check_outcome(self, case, n64.read_directory, paths_rom([case["path"]]))

    def test_paths_the_extractor_accepts(self):
        cases = [case for case in VECTORS["paths"] if case["python"] is None]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(path=case["path"]):
                _, entries = n64.read_directory(paths_rom([case["path"]]))
                self.assertEqual([entry.path for entry in entries], [case["path"][1:]])

    def test_case_alias_and_file_directory_collision_fail(self):
        for case in VECTORS["collisions"]:
            with self.subTest(paths=case["paths"]):
                check_outcome(self, case, n64.read_directory, paths_rom(case["paths"]))

    def record_word_cases(self, plain: bool) -> list[dict]:
        """The record word cases on the default image's plain entries, or on its compressed ones."""
        _, entries = n64.read_directory(IMAGES["default"])
        cases = [case for case in VECTORS["recordWords"]
                 if (entries[case["index"]].flags == 0) == plain]
        self.assertTrue(cases)
        return cases

    def test_outside_data_and_unsupported_flags_fail(self):
        for case in self.record_word_cases(plain=False):
            with self.subTest(word=case["word"], value=case["value"]):
                check_outcome(self, case, n64.read_directory,
                              with_record_word(IMAGES["default"], case))

    def test_plain_size_mismatch_fails(self):
        for case in self.record_word_cases(plain=True):
            with self.subTest(word=case["word"], value=case["value"]):
                check_outcome(self, case, n64.read_directory,
                              with_record_word(IMAGES["default"], case))

    def test_total_expansion_limit(self):
        for case in VECTORS["limits"]:
            with self.subTest(limit=case["maxTotal"]), \
                    mock.patch.object(n64, "MAX_TOTAL_SIZE", case["maxTotal"]):
                check_outcome(self, case, n64.read_directory, IMAGES["default"])

    def test_table_bounds_and_overlapping_records(self):
        for case in VECTORS["headerWords"]:
            with self.subTest(offset=case.get("offset"), value=case.get("value"),
                              copy=case.get("copy")):
                check_outcome(self, case, n64.read_directory,
                              with_header_word(IMAGES["default"], case))

    def test_unterminated_name_fails(self):
        image = bytearray(rom([("/x", b"data", False)]))
        image[-2:] = b"xx"
        with self.assertRaises(n64.FormatError):
            n64.read_directory(bytes(image))


class DecodeTests(unittest.TestCase):
    def check_streams(self, trailer: str):
        cases = [case for case in VECTORS["deflate"] if case["trailer"] == trailer]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(stream=case["stream"][:32], size=case["size"]):
                output = check_outcome(self, case, n64.inflate, bytes.fromhex(case["stream"]),
                                       case["size"], padded=trailer == "overlayPadding")
                if case["python"] is None:
                    self.assertEqual(len(output), case["size"])
                    self.assertEqual(hashlib.sha256(output).hexdigest(), case["outputSha256"])

    def test_raw_deflate_strict_size_and_end(self):
        self.check_streams("none")

    def test_overlay_padding_is_bounded_and_only_allowed_explicitly(self):
        self.check_streams("overlayPadding")

    def test_native_wad_endian_and_zero_tag_byte(self):
        cases = [case for case in VECTORS["wads"] if case["python"] is None]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(wad=case["hex"]):
                self.assertEqual(n64.wad_directory(bytes.fromhex(case["hex"])), case["sections"])

    def test_native_wad_bounds(self):
        cases = [case for case in VECTORS["wads"] if case["python"] is not None]
        self.assertTrue(cases)
        for case in cases:
            with self.subTest(wad=case["hex"]):
                check_outcome(self, case, n64.wad_directory, bytes.fromhex(case["hex"]))


class ExtractionTests(unittest.TestCase):
    def test_preserves_native_files_source_and_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "source.z64", root / "new"
            image = rom()
            source.write_bytes(image)
            manifest = n64.extract(source, destination)
            self.assertEqual(source.read_bytes(), image)
            self.assertEqual((destination / "levels/test/worlds.wad").read_bytes(), wad())
            self.assertEqual((destination / "texture.bin").read_bytes(), b"native")
            self.assertEqual(manifest, json.loads((destination / "manifest.json").read_text()))
            self.assertEqual(manifest["source"]["sha256"], hashlib.sha256(image).hexdigest())
            self.assertFalse(manifest["maps"][0]["runtime_supported"])
            self.assertEqual(manifest["programs"], [])

    def test_refuses_existing_directory_and_preserves_its_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.z64"
            source.write_bytes(rom())
            with self.assertRaises(FileExistsError):
                n64.extract(source, root)
            self.assertEqual(source.read_bytes(), rom())

    def test_corrupt_stream_does_not_create_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, destination = root / "source.z64", root / "new"
            image = bytearray(rom())
            _, entries = n64.read_directory(image)
            image[entries[0].rom_offset] = 0xFF
            source.write_bytes(image)
            with self.assertRaises(n64.FormatError):
                n64.extract(source, destination)
            self.assertFalse(destination.exists())

    def extract_named(self, root: Path, data: bytes, name: str) -> Path:
        """Extracts `data` given as a file of that name into a new folder of `root`, checks that the
        file is left as it was, and returns the new folder."""
        source, destination = root / name, root / "new"
        source.write_bytes(data)
        n64.extract(source, destination)
        self.assertEqual(source.read_bytes(), data)
        return destination

    def test_extraction_writes_the_golden_trees(self):
        for case in VECTORS["extractions"]:
            with self.subTest(image=case["image"]), tempfile.TemporaryDirectory() as directory:
                tree = self.extract_named(Path(directory), IMAGES[case["image"]], case["name"])
                self.assertEqual((tree / "manifest.json").read_bytes(),
                                 (VECTOR_FOLDER / case["golden"]).read_bytes())
                self.assertEqual(tree_listing(tree), VECTORS["trees"][case["image"]])

    def test_byte_orders_write_their_goldens(self):
        default_golden = next(case["golden"] for case in VECTORS["extractions"]
                              if case["image"] == "default")
        default = json.loads((VECTOR_FOLDER / default_golden).read_text(encoding="utf-8"))
        # Only the source's name, byte order and digest set another order's manifest apart.
        apart = ("name", "byte_order", "sha256")
        for case in VECTORS["byteOrders"]:
            with self.subTest(order=case["byteOrder"]), tempfile.TemporaryDirectory() as directory:
                data = reorder(IMAGES["default"], case["transform"])
                self.assertEqual(n64.normalize_rom(data), (IMAGES["default"], case["byteOrder"]))
                tree = self.extract_named(Path(directory), data, case["name"])
                golden = (VECTOR_FOLDER / case["golden"]).read_bytes()
                self.assertEqual((tree / "manifest.json").read_bytes(), golden)
                manifest = json.loads(golden)
                source = manifest.pop("source")
                digest = hashlib.sha256(data).hexdigest()
                self.assertEqual([source[key] for key in apart],
                                 [case["name"], case["byteOrder"], digest])
                expected = dict(default)
                expected_source = expected.pop("source")
                self.assertEqual(manifest, expected)
                self.assertEqual({key: value for key, value in source.items() if key not in apart},
                                 {key: value for key, value in expected_source.items()
                                  if key not in apart})


class NativeRomTests(unittest.TestCase):
    @unittest.skipUnless((ROOT / "assets/n64/Gauntlet Legends (USA).z64").is_file(), "local N64 image not installed")
    def test_us_rom_entire_native_corpus(self):
        image, _ = n64.normalize_rom((ROOT / "assets/n64/Gauntlet Legends (USA).z64").read_bytes())
        if hashlib.sha1(image).hexdigest() != n64.US_SHA1:
            self.skipTest("different N64 ROM revision")
        _, entries = n64.read_directory(image)
        self.assertEqual(len(entries), 618)
        self.assertEqual(sum(entry.flags == n64.COMPRESSED for entry in entries), 586)
        wads = maps = 0
        for entry in entries:
            payload = n64.read_entry(image, entry)
            self.assertEqual(len(payload), entry.size)
            if entry.path.lower().endswith(".wad"):
                n64.wad_directory(payload)
                wads += 1
            maps += entry.path.startswith("levels/") and entry.path.endswith("/worlds.wad")
        self.assertEqual((wads, maps), (445, 34))
        programs = n64.read_overlays(image)
        self.assertEqual(len(programs), 4)
        self.assertEqual(programs[-1][0]["load_address"], 0xE0000400)
        self.assertEqual(len(programs[-1][1]), 0xACAF0)
        catalog = n64.audio_catalog(image)
        self.assertEqual(len(catalog["songs"]), 39)
        self.assertEqual(len(catalog["groups"]), 40)
        self.assertEqual(len(catalog["samples"]), 240)
        self.assertEqual(catalog["songs"][0]["path"], "audio/mount1.song")
        self.assertEqual(catalog["songs"][17]["path"], "audio/Towerchoon.song")
        self.assertEqual(catalog["songs"][38]["path"], "audio/Pirate.song")
        self.assertFalse(catalog["playback_supported"])
        self.assertEqual(catalog["rendered_audio"], [])
        self.assertEqual(catalog["source_sha256"], hashlib.sha256(image).hexdigest())


class AudioCatalogTests(unittest.TestCase):
    @staticmethod
    def samples() -> tuple[bytes, bytes]:
        row = struct.pack(">HH6I", 1, 0, 0, 0, (60 << 24) | 11025, (3 << 24) | 64, 5, 32)
        return row + b"\xff" * 4, b"\0" * 296

    @staticmethod
    def song() -> bytes:
        data = bytearray(384)
        struct.pack_into(">6I", data, 0, 24, 304, 320, 0, 120, 0)
        struct.pack_into(">I", data, 24, 280)
        return bytes(data)

    def test_sample_metadata_uses_28_byte_rows_and_native_codec(self):
        directory, data = self.samples()
        rows = n64.sample_directory(directory, data)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["sample_count"], 64)
        self.assertEqual(rows[0]["sample_rate"], 11025)
        self.assertEqual(rows[0]["loop_length"], 32)
        self.assertEqual(rows[0]["codec"], "musyx-n64-vadpcm")
        self.assertEqual(rows[0]["encoded_size"], 296)

    def test_invalid_samples_rejected(self):
        directory, data = self.samples()
        bad = [directory[:-1], directory[:-4] + b"bad!", directory[:-4] * 2 + b"\xff" * 4]
        for offset, value in ((4, 300), (8, 1), (12, 0), (16, 0x04000040), (20, 60), (24, 65)):
            changed = bytearray(directory)
            struct.pack_into(">I", changed, offset, value)
            bad.append(bytes(changed))
        for changed in bad:
            with self.subTest(directory=changed.hex()), self.assertRaises(n64.FormatError):
                n64.sample_directory(changed, data)
        with self.assertRaises(n64.FormatError):
            n64.sample_directory(directory, data[:-1])

    def test_relative_project_groups_and_distinct_sfx_header(self):
        project = struct.pack(">IHH8I", 40, 1, 0, *([0] * 8))
        project += struct.pack(">IHH6I", 32, 2, 1, *([0] * 6)) + b"\xff" * 4
        groups = n64.music_groups(project)
        self.assertEqual([group["offset"] for group in groups], [0, 40])
        self.assertEqual([group["kind"] for group in groups], ["song", "sound-effects"])
        for changed in (project[:-1], project + b"junk", b"\0" * 40,
                        struct.pack(">IHH8I", 40, 1, 0, 100, *([0] * 7)) + b"\xff" * 4):
            with self.assertRaises(n64.FormatError):
                n64.music_groups(changed)

    def test_sequence_catalog_does_not_claim_a_waveform(self):
        row = n64.song_header(self.song())
        self.assertEqual(row["format"], "musyx-sequence")
        self.assertEqual(row["active_tracks"], 1)
        self.assertEqual(row["initial_tempo_word"], 120)
        for offset, value in ((0, 88), (4, 1000), (8, 380), (12, 1000), (16, 0), (24, 380)):
            changed = bytearray(self.song())
            struct.pack_into(">I", changed, offset, value)
            with self.subTest(offset=offset), self.assertRaises(n64.FormatError):
                n64.song_header(changed)

    def test_unknown_revision_does_not_use_fixed_song_table_addresses(self):
        with self.assertRaisesRegex(n64.FormatError, "verified US"):
            n64.audio_catalog(rom())

    def test_catalog_output_never_replaces_existing_files(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing.json"
            path.write_text("existing", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                n64.write_audio_catalog(Path(directory) / "not-read.z64", path)
            self.assertEqual(path.read_text(encoding="utf-8"), "existing")

    def test_catalog_is_written_separately(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.z64"
            source.write_bytes(rom())
            native = root / "native"
            native.mkdir()
            (native / "manifest.json").write_text("unchanged", encoding="utf-8")
            result = {"format": "synthetic-catalog", "songs": []}
            with mock.patch.object(n64, "audio_catalog", return_value=result):
                self.assertEqual(n64.write_audio_catalog(source, native / "audio-catalog.json"), result)
            self.assertEqual(json.loads((native / "audio-catalog.json").read_text()), result)
            self.assertEqual((native / "manifest.json").read_text(), "unchanged")


class ManifestJsonTests(unittest.TestCase):
    def test_strings_render_as_the_shared_vectors_pin(self):
        cases = vectors(JSON_STRINGS)["cases"]
        self.assertTrue(cases)
        for case in cases:
            text = json_string(case)
            with self.subTest(rendered=case["rendered"]):
                self.assertEqual(json.dumps(text), case["rendered"])
                # The manifests are written indented, which renders a string the same way.
                self.assertEqual(json.dumps({"name": text}, indent=2),
                                 '{\n  "name": ' + case["rendered"] + "\n}")


class VectorTests(unittest.TestCase):
    def test_builder_writes_the_shared_plain_image(self):
        plain = VECTORS["plainRom"]
        files = [(path, bytes.fromhex(payload), compressed)
                 for path, payload, compressed in plain["files"]]
        self.assertEqual(rom(files).hex(), plain["hex"])

    def test_vectors_are_current(self):
        self.maxDiff = None
        committed = sorted([path.name for path in VECTOR_FOLDER.glob("n64-*.json")]
                           + [JSON_STRINGS])
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            # The frozen images and streams come over from the committed vectors.
            shutil.copyfile(VECTOR_FOLDER / N64_VECTORS, folder / N64_VECTORS)
            written = write_vectors(folder, "n64")
            self.assertEqual(sorted(written), committed)
            for name in written:
                with self.subTest(name=name):
                    # Latin-1 keeps every byte as one character, so a difference shows as text.
                    self.assertEqual((folder / name).read_bytes().decode("latin-1"),
                                     (VECTOR_FOLDER / name).read_bytes().decode("latin-1"))


if __name__ == "__main__":
    unittest.main()
