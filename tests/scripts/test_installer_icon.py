"""Disc-icon decoding and real Windows resource/install/update tests, using synthetic art."""

import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from installer.icon import ICON_SIZES, decode_first_frame, embed_icon, icon_resources
from installer.install import apply_disc_icon, install, payload_inventory
from installer.update import apply_update, file_digest, installed_info
from release import make_payload
from test_installer import fixture_payload, synthetic_iso


def tpl_fixture(encoding=5, width=8, height=8):
    data = bytearray(64)
    struct.pack_into(">III", data, 0, 0x0020AF30, 1, 12)
    struct.pack_into(">II", data, 12, 20, 0)
    struct.pack_into(">HHII", data, 20, height, width, encoding, 64)
    tile_count = ((width + 3) // 4) * ((height + 3) // 4)
    for tile in range(tile_count):
        if encoding == 6:
            data.extend(bytes((0x44, 0x88)) * 16 + bytes((0x22, 0x11)) * 16)
        else:
            # Each 4x4 tile is a different colour; includes partial and zero alpha.
            value = (0xFC00, 0x83E0, 0x3123, 0x0456)[tile % 4]
            data.extend(struct.pack(">H", value) * 16)
    return data


def resource_bytes(executable, kind, identity, language=0):
    """Read the PE as data, never execute it or load any of its dependencies."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
    kernel.LoadLibraryExW.restype = wintypes.HMODULE
    kernel.FindResourceExW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD)
    kernel.FindResourceExW.restype = wintypes.HANDLE
    kernel.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HANDLE)
    kernel.SizeofResource.restype = wintypes.DWORD
    kernel.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HANDLE)
    kernel.LoadResource.restype = wintypes.HANDLE
    kernel.LockResource.argtypes = (wintypes.HANDLE,)
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = (wintypes.HMODULE,)
    handle = kernel.LoadLibraryExW(str(executable), None, 0x22)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        found = kernel.FindResourceExW(handle, kind, identity, language)
        if not found:
            return None
        size = kernel.SizeofResource(handle, found)
        memory = kernel.LoadResource(handle, found)
        address = kernel.LockResource(memory)
        if not size or not address:
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.string_at(address, size)
    finally:
        kernel.FreeLibrary(handle)


def code_sections(data):
    pe, = struct.unpack_from("<I", data, 0x3C)
    count, = struct.unpack_from("<H", data, pe + 6)
    optional_size, = struct.unpack_from("<H", data, pe + 20)
    result = {}
    for index in range(count):
        offset = pe + 24 + optional_size + index * 40
        name = data[offset:offset + 8].rstrip(b"\0")
        size, start = struct.unpack_from("<II", data, offset + 16)
        flags, = struct.unpack_from("<I", data, offset + 36)
        if flags & 0x20:
            result[name] = data[start:start + size]
    if not result:
        raise AssertionError("PE test fixture has no code sections")
    return result


class IconFormatTests(unittest.TestCase):
    def test_rgb5a3_tiles_and_alpha_match_window_decoder(self):
        width, height, rgba = decode_first_frame(tpl_fixture())
        self.assertEqual((width, height), (8, 8))
        for x, y, expected in ((0, 0, (255, 0, 0, 255)), (4, 0, (0, 255, 0, 255)),
                               (0, 4, (17, 34, 51, 109)), (4, 4, (68, 85, 102, 0))):
            at = (y * width + x) * 4
            self.assertEqual(rgba[at:at + 4], bytes(expected))

    def test_rgb565_and_planar_rgba8_match_window_decoder(self):
        _, _, rgba = decode_first_frame(tpl_fixture(4))
        self.assertEqual(rgba[:4], bytes((255, 130, 0, 255)))
        _, _, rgba = decode_first_frame(tpl_fixture(6))
        self.assertEqual(rgba[:4], bytes((136, 34, 17, 68)))

    def test_partial_tiles_and_first_frame_only(self):
        data = tpl_fixture(width=5, height=3)
        # A second table slot fits before the relocated first image header.
        header = bytes(data[20:56])
        data[28:64] = header
        struct.pack_into(">I", data, 4, 2)
        struct.pack_into(">IIII", data, 12, 28, 0, 28, 0)
        width, height, rgba = decode_first_frame(data)
        self.assertEqual((width, height, len(rgba)), (5, 3, 60))
        self.assertEqual(rgba[16:20], bytes((0, 255, 0, 255)))

    def test_malformed_tpl_is_rejected_before_resource_edit(self):
        for mutation in ("magic", "count", "table", "header", "size", "format", "palette", "pixels"):
            data = tpl_fixture()
            offset, value = {"magic": (0, 0), "count": (4, 0), "table": (8, 0xFFFFFFFF),
                             "header": (12, 0xFFFFFFFF), "size": (20, 0), "format": (24, 9),
                             "palette": (16, 32), "pixels": (28, 0xFFFFFF00)}[mutation]
            struct.pack_into(">I", data, offset, value)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                decode_first_frame(data)
        for data in (b"", bytes(tpl_fixture()[:-1]), bytes(1024 * 1024 + 1)):
            with self.assertRaises(ValueError):
                decode_first_frame(data)

    def test_group_sizes_bgra_orientation_and_transparent_mask(self):
        group, images = icon_resources(*decode_first_frame(tpl_fixture()))
        self.assertEqual(struct.unpack_from("<HHH", group), (0, 1, len(ICON_SIZES)))
        for index, (size, bitmap) in enumerate(zip(ICON_SIZES, images)):
            self.assertEqual(struct.unpack_from("<BBBBHHIH", group, 6 + index * 14),
                             (size % 256, size % 256, 0, 0, 1, 32, len(bitmap), index + 1))
            self.assertEqual(struct.unpack_from("<IiiHH", bitmap), (40, size, size * 2, 1, 32))
            self.assertEqual(bitmap[40:44], bytes((51, 34, 17, 109)))
            top = 40 + (size - 1) * size * 4
            self.assertEqual(bitmap[top:top + 4], bytes((0, 0, 255, 255)))
            mask = 40 + size * size * 4
            self.assertEqual(bitmap[mask + size // 16:mask + size // 8], b"\xff" * (size // 16))

    def test_linux_leaves_executable_and_metadata_unchanged(self):
        metadata = {"platform": "linux-x64"}
        with mock.patch("installer.install.embed_icon") as embed:
            apply_disc_icon(Path("unused"), Path("unused"), metadata)
        embed.assert_not_called()
        self.assertEqual(metadata, {"platform": "linux-x64"})


@unittest.skipUnless(os.name == "nt", "Windows PE resources")
class WindowsIconTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="gdl icon tests ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        # Python's signed/unsigned launcher is only a PE fixture, never shipped.
        # Copy it, verify code/manifest preservation, and never run modified code.
        self.original = Path(getattr(sys, "_base_executable", sys.executable)).read_bytes()
        self.image = self.root / "game.iso"
        disc = synthetic_iso()
        self.tpl = bytes(tpl_fixture())
        struct.pack_into(">I", disc, 0x800 + 8 * 12 + 8, len(self.tpl))
        disc[0x2900:0x2900 + len(self.tpl)] = self.tpl
        self.image.write_bytes(disc)
        self.destination = self.root / "game"
        self.payload = self.make_runtime(self.root / "initial", "0.1.0-alpha.1")

    def make_runtime(self, root, version):
        root.mkdir()
        payload = fixture_payload(root)
        metadata, _ = payload_inventory(payload)
        (root / "inputs/gauntlet.exe").write_bytes(self.original)
        (root / "inputs/VERSION").write_text(version + "\n")
        payload.unlink()
        make_payload(payload, [(root / "inputs" / name, name) for name in metadata["files"]],
                     version, "icon-test", "windows-x64", "gauntlet.exe")
        return payload

    def assert_icon(self, executable):
        group, images = icon_resources(*decode_first_frame(self.tpl))
        self.assertEqual(resource_bytes(executable, 14, 1), group)
        for index, bitmap in enumerate(images, 1):
            self.assertEqual(resource_bytes(executable, 3, index), bitmap)
        self.assertEqual(code_sections(executable.read_bytes()), code_sections(self.original))

    def test_install_embeds_first_frame_keeps_native_tpl_and_tracks_local_hash(self):
        payload_before = self.payload.read_bytes()
        def notified(executable):
            self.assert_icon(executable)
            self.assertEqual(installed_info(self.destination)[0]["files"]["gauntlet.exe"], file_digest(executable))
        with mock.patch("installer.install.refresh_shell_icon", side_effect=notified) as notify:
            executable = install(self.image, self.destination, self.payload)
        notify.assert_called_once_with(executable)
        self.assert_icon(executable)
        self.assertEqual((self.destination / "carddemo/icon.tpl").read_bytes(), self.tpl)
        self.assertEqual(self.payload.read_bytes(), payload_before)
        metadata, _ = installed_info(self.destination)
        self.assertEqual(metadata["files"]["gauntlet.exe"], file_digest(executable))
        self.assertEqual(metadata["discIcon"]["releaseExecutableSha256"], hashlib.sha256(self.original).hexdigest())
        self.assertEqual(metadata["discIcon"]["sha256"], hashlib.sha256(self.tpl).hexdigest())
        original = self.root / "original.exe"
        original.write_bytes(self.original)
        # Preserve the existing application manifest, including DPI/elevation flags.
        for language in (0, 1033):
            self.assertEqual(resource_bytes(original, 24, 1, language), resource_bytes(executable, 24, 1, language))

    def test_updates_keep_icon_and_integrity_checks_across_successive_versions(self):
        executable = install(self.image, self.destination, self.payload)
        for version in ("0.1.0-alpha.2", "0.1.0"):
            payload = self.make_runtime(self.root / version, version)
            with mock.patch("installer.update.refresh_shell_icon", side_effect=self.assert_icon) as notify:
                apply_update(self.destination, payload)
            notify.assert_called_once_with(executable)
            self.assert_icon(executable)
            metadata, _ = installed_info(self.destination)
            self.assertEqual(metadata["version"], version)
            self.assertEqual(metadata["files"]["gauntlet.exe"], file_digest(executable))
        executable.write_bytes(executable.read_bytes() + b"tampering")
        with self.assertRaisesRegex(ValueError, "modified"):
            apply_update(self.destination, self.make_runtime(self.root / "newer", "0.1.1"))

    def test_legacy_installation_gets_icon_on_its_next_update(self):
        with mock.patch("installer.install.apply_disc_icon"):
            install(self.image, self.destination, self.payload)
        self.assertNotIn("discIcon", installed_info(self.destination)[0])
        executable = apply_update(self.destination, self.make_runtime(self.root / "next", "0.1.0-alpha.2"))
        self.assert_icon(executable)

    def test_icon_failure_aborts_install_and_preserves_existing_update(self):
        with mock.patch("installer.install.embed_icon", side_effect=OSError("resource failure")):
            with self.assertRaisesRegex(OSError, "resource failure"):
                install(self.image, self.destination, self.payload)
        self.assertFalse(self.destination.exists())
        install(self.image, self.destination, self.payload)
        before = {p.relative_to(self.destination): p.read_bytes() for p in self.destination.rglob("*") if p.is_file()}
        with mock.patch("installer.install.embed_icon", side_effect=OSError("resource failure")):
            with self.assertRaisesRegex(OSError, "resource failure"):
                apply_update(self.destination, self.make_runtime(self.root / "next", "0.1.0-alpha.2"))
        after = {p.relative_to(self.destination): p.read_bytes() for p in self.destination.rglob("*")
                 if p.is_file() and p.name != ".gdl-update.lock"}
        self.assertEqual(after, before)

    def test_invalid_executable_is_rejected(self):
        executable = self.root / "invalid.exe"
        executable.write_bytes(b"not a PE")
        with self.assertRaises(OSError):
            embed_icon(executable, *icon_resources(*decode_first_frame(self.tpl)))


if __name__ == "__main__":
    unittest.main()
