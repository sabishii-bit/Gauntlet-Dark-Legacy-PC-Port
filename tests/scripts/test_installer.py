"""Disc extraction, installation transactions and release guards (no proprietary fixtures)."""

import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from installer.disc import CISO_HEADER, DiscImage, safe_component
from installer.install import Cancelled, host_platform, install, payload_inventory
from release import freezer_environment, make_payload, validate_tag, version


def synthetic_iso():
    """Real GC FST layout with one file spanning a sparse CISO block boundary."""
    strings = bytearray(b"\0")
    entries = [(0x01000000, 0, 9)]

    def entry(name, offset, size, directory=False):
        index = len(strings)
        strings.extend(name.encode("ascii") + b"\0")
        entries.append((index | (0x01000000 if directory else 0), offset, size))

    entry("Gauntlet", 0, 7, True)
    entry("PDATA", 1, 4, True)
    entry("WAR.WAD", 0x1FF0, 32)
    entry("WDATA", 1, 6, True)
    entry("TOWER.WAD", 0x2800, 5)
    entry("EMPTY", 1, 7, True)
    entry("carddemo", 0, 9, True)
    entry("icon.tpl", 0x2900, 4)
    fst = b"".join(struct.pack(">III", *record) for record in entries) + strings
    image = bytearray(0x4000)
    image[:6] = b"GUNE5D"
    image[7] = 1
    image[0x1C:0x20] = b"\xc2\x33\x9f\x3d"
    struct.pack_into(">II", image, 0x424, 0x800, len(fst))
    image[0x800:0x800 + len(fst)] = fst
    image[0x1FF0:0x2000] = b"A" * 16  # next block's first 16 bytes are zero
    image[0x2800:0x2805] = b"tower"
    image[0x2900:0x2904] = b"icon"
    return image


def synthetic_ciso(image):
    block_size = 512
    header = bytearray(CISO_HEADER)
    header[:4] = b"CISO"
    struct.pack_into("<I", header, 4, block_size)
    content = bytearray()
    for index in range(len(image) // block_size):
        block = image[index * block_size:(index + 1) * block_size]
        if any(block):
            header[8 + index] = 1
            content.extend(block)
    return header + content


def fixture_payload(root):
    executable = "gauntlet.exe" if os.name == "nt" else "gauntlet"
    system = "windows-x64" if os.name == "nt" else "linux-x64"
    files = []
    for name, data in {executable: b"not a real executable", "VERSION": b"0.1.0-alpha.1\n",
                       "portable.flag": b"", "data/config.json": b"{}",
                       "data/text/en.json": b"{}", "shaders/test.spv": b"shader"}.items():
        path = root / "inputs" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        files.append((path, name))
    payload = root / "runtime.zip"
    make_payload(payload, files, "0.1.0-alpha.1", "test-commit", system, executable)
    return payload


class InstallerTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl installer tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.image = self.root / "game.iso"
        self.image.write_bytes(synthetic_iso())
        self.payload = fixture_payload(self.root)
        self.destination = self.root / "installed game"

    def test_iso_and_sparse_ciso_are_identical(self):
        ciso = self.root / "game.ciso"
        ciso.write_bytes(synthetic_ciso(synthetic_iso()))
        with DiscImage(self.image) as iso, DiscImage(ciso) as sparse:
            self.assertEqual(iso.entries, sparse.entries)
            self.assertEqual(iso.revision, 1)
            for entry in iso.entries:
                if not entry.directory:
                    self.assertEqual(iso.read(entry.offset, entry.size),
                                     sparse.read(entry.offset, entry.size))
            self.assertEqual(sparse.read(0x1FF0, 32), b"A" * 16 + bytes(16))
            with self.assertRaises(ValueError):
                sparse.read(-1, 3)

    def test_transaction_preserves_native_bytes_and_installer(self):
        self.destination.mkdir()
        own_installer = self.destination / "setup.exe"
        own_installer.write_bytes(b"installer")
        updates = []
        executable = install(self.image, self.destination, self.payload,
                             lambda done, total, name: updates.append((done, total, name)))
        self.assertTrue(executable.is_file())
        self.assertEqual(own_installer.read_bytes(), b"installer")
        self.assertEqual((self.destination / "Gauntlet/PDATA/WAR.WAD").read_bytes(),
                         b"A" * 16 + bytes(16))
        for folder in ("Gauntlet/EMPTY", "carddemo", "config", "saves"):
            self.assertTrue((self.destination / folder).is_dir())
        receipt = json.loads((self.destination / "installation.json").read_text())
        self.assertEqual(receipt["discRevision"], 1)
        self.assertEqual(receipt["version"], "0.1.0-alpha.1")
        self.assertEqual(updates[-1][0], updates[-1][1])
        self.assertFalse((self.destination / ".gdl-install.lock").exists())

    def test_ciso_installation(self):
        self.image.write_bytes(synthetic_ciso(synthetic_iso()))
        install(self.image, self.destination, self.payload)
        self.assertEqual((self.destination / "Gauntlet/WDATA/TOWER.WAD").read_bytes(), b"tower")

    def test_refuses_existing_game_saves_or_config(self):
        for name in ("Gauntlet", "saves", "config", "DATA"):
            with self.subTest(name=name):
                self.destination.mkdir(exist_ok=True)
                path = self.destination / name
                path.write_bytes(b"original")
                with self.assertRaisesRegex(ValueError, "overwritten"):
                    install(self.image, self.destination, self.payload)
                self.assertEqual(path.read_bytes(), b"original")
                path.unlink()

    def test_cancellation_cleans_staging_not_preexisting_files(self):
        self.destination.mkdir()
        keep = self.destination / "user.txt"
        keep.write_text("keep")
        cancelled = False

        def progress(*_):
            nonlocal cancelled
            cancelled = True

        with self.assertRaises(Cancelled):
            install(self.image, self.destination, self.payload, progress, lambda: cancelled)
        self.assertEqual(list(self.destination.iterdir()), [keep])

    def test_failure_during_commit_rolls_back_owned_entries(self):
        original = Path.rename
        moves = 0

        def fail_second(path, target):
            nonlocal moves
            moves += 1
            if moves == 2:
                raise OSError("simulated failed commit")
            return original(path, target)

        with mock.patch.object(Path, "rename", fail_second):
            with self.assertRaisesRegex(OSError, "simulated"):
                install(self.image, self.destination, self.payload)
        self.assertFalse(self.destination.exists())

    def test_digest_corruption_rolls_back(self):
        with zipfile.ZipFile(self.payload) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        files["data/config.json"] = b"damaged"
        with zipfile.ZipFile(self.payload, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, data)
        with self.assertRaisesRegex(ValueError, "Damaged"):
            install(self.image, self.destination, self.payload)
        self.assertFalse(self.destination.exists())

    def test_simultaneous_installers_do_not_remove_anothers_lock(self):
        self.destination.mkdir()
        lock = self.destination / ".gdl-install.lock"
        lock.write_text("other process")
        with self.assertRaises(FileExistsError):
            install(self.image, self.destination, self.payload)
        self.assertEqual(lock.read_text(), "other process")

    def test_invalid_images_write_nothing(self):
        for data in (b"", b"CISO", b"CISO" + bytes(CISO_HEADER), b"not a game"):
            with self.subTest(data=data[:8]):
                self.image.write_bytes(data)
                with self.assertRaises(ValueError):
                    install(self.image, self.destination, self.payload)
                self.assertFalse(self.destination.exists())

    def test_wrong_game_and_bad_extents(self):
        for mutation in ("id", "fst", "parent", "count", "file", "name", "kind"):
            with self.subTest(mutation=mutation):
                data = synthetic_iso()
                if mutation == "id":
                    data[:6] = b"GALE01"
                elif mutation == "fst":
                    struct.pack_into(">I", data, 0x424, 0xFFFFFF00)
                elif mutation == "parent":
                    struct.pack_into(">I", data, 0x800 + 12 + 4, 9)
                elif mutation == "count":
                    struct.pack_into(">I", data, 0x808, 0x10000000)
                elif mutation == "file":
                    struct.pack_into(">I", data, 0x800 + 36 + 8, 0xFFFFFFFF)
                elif mutation == "name":
                    data[0x800 + 9 * 12 + 1] = ord('/')
                else:
                    data[0x800 + 12] = 2
                self.image.write_bytes(data)
                with self.assertRaises(ValueError):
                    DiscImage(self.image)

    def test_ciso_map_and_truncation_rejected(self):
        for mutation in ("map", "truncated"):
            data = synthetic_ciso(synthetic_iso())
            if mutation == "map":
                data[8] = 2
            else:
                data = data[:-1]
            self.image.write_bytes(data)
            with self.assertRaises(ValueError):
                DiscImage(self.image)

    def test_paths_cannot_escape_or_use_windows_device_names(self):
        for name in ("..", ".", "", "a/b", "a\\b", "C:", "CON.txt", "LPT1", "foo.", "foo ", "x\0y"):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    safe_component(name)

    def test_public_payload_rejects_assets_and_path_traversal(self):
        for path in ("Gauntlet/PDATA/WAR.WAD", "carddemo/icon.tpl", "../escaped", "data/../escaped"):
            bad = self.root / "bad.zip"
            with zipfile.ZipFile(bad, "w") as archive:
                archive.writestr(path, "not allowed")
            with self.assertRaises(ValueError):
                payload_inventory(bad)

    def test_wrong_platform_rejected(self):
        with zipfile.ZipFile(self.payload) as archive:
            contents = {name: archive.read(name) for name in archive.namelist()}
        metadata = json.loads(contents["build-info.json"])
        metadata["platform"] = "another-os"
        contents["build-info.json"] = json.dumps(metadata)
        with zipfile.ZipFile(self.payload, "w") as archive:
            for name, content in contents.items():
                archive.writestr(name, content)
        with self.assertRaises(ValueError):
            install(self.image, self.destination, self.payload)
        self.assertFalse(self.destination.exists())

    def test_version_and_tag_are_semver_and_identical(self):
        path = self.root / "VERSION"
        path.write_text("0.1.0-alpha.1\n")
        self.assertEqual(version(self.root), "0.1.0-alpha.1")
        self.assertEqual(validate_tag("v0.1.0-alpha.1", self.root), "v0.1.0-alpha.1")
        with self.assertRaises(ValueError):
            validate_tag("v0.1.0-alpha.2", self.root)
        for text in ("0.1.0", "0.1.0-alpha.0", "1.0.0-beta.2", "1.0.0-rc.1", "1.0.0+build.123"):
            path.write_text(text)
            self.assertEqual(version(self.root), text)
            self.assertEqual(validate_tag("v" + text, self.root), "v" + text)
        for text in ("v0.1.0-alpha.1", "00.1.0-alpha.1", "0.1.0-alpha.01", "1.0.0+", "1.0"):
            path.write_text(text)
            with self.assertRaises(ValueError):
                version(self.root)

    def test_architecture_support_is_explicit(self):
        with mock.patch.dict(os.environ, {"PROCESSOR_ARCHITEW6432": "AMD64"}):
            with mock.patch("installer.install.platform.system", return_value="Windows"), mock.patch(
                    "installer.install.platform.machine", return_value="AMD64"):
                self.assertEqual(host_platform(), "windows-x64")
            with mock.patch.dict(os.environ, {"PROCESSOR_ARCHITEW6432": "ARM64"}), mock.patch(
                    "installer.install.platform.system", return_value="Windows"), mock.patch(
                    "installer.install.platform.machine", return_value="AMD64"):
                with self.assertRaisesRegex(ValueError, "ARM64"):
                    host_platform()
        for system, machine in (("Linux", "aarch64"), ("Darwin", "arm64"), ("Windows", "x86")):
            with mock.patch.dict(os.environ, {}, clear=True), mock.patch(
                    "installer.install.platform.system", return_value=system), mock.patch(
                    "installer.install.platform.machine", return_value=machine):
                with self.assertRaises(ValueError):
                    host_platform()

    def test_freezer_cannot_collect_libraries_from_unrelated_tools(self):
        with mock.patch.dict(os.environ, {"PATH": "unrelated-poppler-toolkit",
                "PYTHONPATH": "unrelated-python", "QT_PLUGIN_PATH": "unrelated-qt",
                "QML2_IMPORT_PATH": "unrelated-qml"}):
            environment = freezer_environment()
        self.assertNotIn("unrelated", environment["PATH"])
        self.assertNotIn("PYTHONPATH", environment)
        self.assertNotIn("QT_PLUGIN_PATH", environment)
        self.assertNotIn("QML2_IMPORT_PATH", environment)
        self.assertEqual(environment["PYTHONNOUSERSITE"], "1")
        self.assertIn(str(Path(sys.executable).parent), environment["PATH"])


if __name__ == "__main__":
    unittest.main()
