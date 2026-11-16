"""Synthetic N64 ROMs and Dreamcast GD-ROM sets for the Legends extractor tests, and the vectors
those tests share with the launcher's; no game data.

The vectors in tests/scripts/data/legends are written by the builders and the extractors, never by
hand. After a change to either, write them again with

    python tests/scripts/legends_fixtures.py --write tests/scripts/data/legends

The ROM images and the DEFLATE streams the cases are cut from depend on the compressor of the zlib
Python links, so a write carries them over from the vectors it replaces. --refreeze compresses them
anew with this Python's zlib, and every golden made from them follows in the same run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import tempfile
import zlib
from pathlib import Path, PurePosixPath, PureWindowsPath
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "assets"))
import dreamcast_legends as dc
import n64_legends as n64

# Where the shared vectors live; the launcher's tests read the same files.
VECTOR_FOLDER = ROOT / "tests" / "scripts" / "data" / "legends"


def deflate(data: bytes) -> bytes:
    compressor = zlib.compressobj(wbits=-15)
    return compressor.compress(data) + compressor.flush()


def wad(order: str = ">", tag: bytes = b"TEST") -> bytes:
    return struct.pack(order + "2I", 12, 1) + b"data" + struct.pack(order + "4sII", tag, 8, 4)


def rom(files=None) -> bytes:
    if files is None:
        files = [("/levels/test/worlds.wad", wad(), True), ("/texture.bin", b"native", False)]
    result = bytearray(64)
    result[:4] = b"\x80\x37\x12\x40"
    base = len(result)
    result.extend(n64.ROMDISK_MAGIC)
    result.extend(b"\0" * 16)
    metadata = []
    for name, payload, compressed in files:
        stored = deflate(payload) if compressed else payload
        offset = len(result) - base
        result.extend(stored)
        result.extend(b"\0" * (-len(result) % 4))
        metadata.append((name, n64.COMPRESSED if compressed else 0, len(stored), len(payload), offset))
    table = len(result)
    result.extend(b"\0" * (len(files) * 4))
    for index, (name, flags, stored, size, offset) in enumerate(metadata):
        record = len(result)
        struct.pack_into(">I", result, table + index * 4, record - base)
        result.extend(struct.pack(">5I", flags, stored, size, offset, 0x12345678))
        result.extend(name.encode("ascii") + b"\0")
        result.extend(b"\0" * (-len(result) % 4))
    struct.pack_into(">4I", result, base + 32, len(result) - table, table - base, len(files), 0)
    return bytes(result)


def change_entry(image: bytes, word: int, value: int, index: int = 0) -> bytes:
    result = bytearray(image)
    _, entries = n64.read_directory(image)
    struct.pack_into(">I", result, entries[index].record_offset + 4 * word, value)
    return bytes(result)


def both(value, width):
    return value.to_bytes(width, "little") + value.to_bytes(width, "big")


def record(name, lba, size, flags=0):
    length = 33 + len(name) + (len(name) % 2 == 0)
    result = bytearray(length)
    result[0] = length
    result[2:10] = both(lba, 4)
    result[10:18] = both(size, 4)
    result[25] = flags
    result[28:32] = both(1, 2)
    result[32] = len(name)
    result[33:33 + len(name)] = name
    return result


def raw_sector(lba, payload):
    address = lba + 150
    minute, remainder = divmod(address, 60 * 75)
    second, frame = divmod(remainder, 75)
    bcd = lambda n: (n // 10 << 4) | (n % 10)
    result = bytearray(dc.RAW_SECTOR)
    result[:12] = dc.SYNC
    result[12:16] = bytes((bcd(minute), bcd(second), bcd(frame), 1))
    result[16:16 + len(payload)] = payload
    return result


def fixture(root, name=b"TEST.WAD;1", file_lba=45100, flags=0):
    boot = bytearray(dc.SECTOR)
    boot[:16] = b"SEGA SEGAKATANA "
    boot[64:74] = b"T9710N    "
    boot[96:112] = b"1ST_READ.BIN    "
    boot[128:144] = b"GAUNTLET LEGENDS"
    pvd = bytearray(dc.SECTOR)
    pvd[:7] = b"\x01CD001\x01"
    pvd[40:72] = b"TEST".ljust(32, b" ")
    pvd[128:132] = both(dc.SECTOR, 2)
    pvd[156:190] = record(b"\0", 45020, dc.SECTOR, 2)
    directory = record(b"\0", 45020, dc.SECTOR, 2)
    directory += record(b"\1", 45020, dc.SECTOR, 2)
    directory += record(name, file_lba, 5, flags)
    with (root / "track3.bin").open("wb") as stream:
        for n in range(21):
            stream.write(raw_sector(45000 + n,
                                    {0: boot, 16: pvd, 20: directory}.get(n, b"")))
    (root / "track5.bin").write_bytes(b"\0" * dc.RAW_SECTOR * 225 +
                                     raw_sector(45100, b"hello"))
    cue = root / "test.cue"
    # Bytes, not text, so the set is the same on every system: text mode ends lines in CRLF on
    # Windows.
    text = ('REM HIGH-DENSITY AREA\nFILE "track3.bin" BINARY\n'
            'TRACK 03 MODE1/2352\nINDEX 01 00:00:00\n'
            'FILE "track5.bin" BINARY\nTRACK 05 MODE1/2352\n'
            'INDEX 00 00:00:00\nINDEX 01 00:03:00\n')
    cue.write_bytes(text.encode("ascii"))
    return cue


# The reflected polynomial of the error detection code ECMA-130 gives a raw sector, and the bytes
# of the sector it covers: the sync, the header and the user data.
EDC_POLYNOMIAL = 0xD8018001
EDC_SPAN = 16 + dc.SECTOR


def edc_table() -> list[int]:
    """The EDC's value for each byte, from which `edc` runs a byte at a time."""
    table = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = (value >> 1) ^ (EDC_POLYNOMIAL if value & 1 else 0)
        table.append(value)
    return table


EDC_TABLE = edc_table()


def edc(data: bytes) -> int:
    """The EDC of ECMA-130 over `data`: the reflected CRC from nought with no final inversion that a
    raw sector keeps, little-endian, after its user data."""
    value = 0
    for byte in data:
        value = EDC_TABLE[(value ^ byte) & 0xFF] ^ (value >> 8)
    return value


# Where the sets' file systems lie: the root directory sector of the high-density area.
ROOT_LBA = dc.HIGH_DENSITY_LBA + 20


def boot_sector(area: bytes = b"", version: bytes = b"",
                title: bytes = b"GAUNTLET LEGENDS") -> bytearray:
    """The first sector of a set's boot area: the hardware ID, the product and the executable the
    small set's boot area holds, with the area, version and title given and the rest nought."""
    boot = bytearray(dc.SECTOR)
    boot[:16] = b"SEGA SEGAKATANA "
    boot[48:48 + len(area)] = area
    boot[64:74] = b"T9710N    "
    boot[74:74 + len(version)] = version
    boot[96:112] = b"1ST_READ.BIN    "
    boot[128:128 + len(title)] = title
    return boot


def pvd_sector(volume: bytes) -> bytearray:
    """A primary volume descriptor of 2,048-byte blocks naming `volume`, its root at ROOT_LBA."""
    pvd = bytearray(dc.SECTOR)
    pvd[:7] = b"\x01CD001\x01"
    pvd[40:72] = volume.ljust(32, b" ")
    pvd[128:132] = both(dc.SECTOR, 2)
    pvd[156:190] = record(b"\0", ROOT_LBA, dc.SECTOR, 2)
    return pvd


def directory(lba: int, parent: int, entries: bytes) -> bytes:
    """A one-sector directory at `lba`: its own record, its parent's, then `entries`."""
    return bytes(record(b"\0", lba, dc.SECTOR, 2) + record(b"\1", parent, dc.SECTOR, 2) + entries)


def raw_track(first_lba: int, count: int, payloads: dict[int, bytes]) -> bytes:
    """`count` raw sectors from `first_lba`, each holding the payload `payloads` gives for its place
    in the track, or noughts."""
    return b"".join(raw_sector(first_lba + n, payloads.get(n, b"")) for n in range(count))


def sizes_rom(rows, pool: bytes) -> bytes:
    """A SIZES.ROM table: its count, each row's size, DISK.ROM offset and four pool offsets of its
    components (the first negative one ending them), then the pool of names."""
    return (struct.pack("<I", len(rows))
            + b"".join(struct.pack("<Ii4h", size, offset, *components)
                       for size, offset, components in rows)
            + pool)


def one_track_cue(name: str = "t.bin", track: str = "03 MODE1/2352",
                  index: str = "00:00:00") -> str:
    """A cue sheet of one track in the file `name`, the track's number and mode `track` and its
    INDEX 01 `index`: by default a raw data track from the file's start."""
    return f'FILE "{name}" BINARY\nTRACK {track}\nINDEX 01 {index}\n'


# Where the name fixture's file lies, between the volume descriptor and the root directory, and
# what it holds.
NAME_FILE_LBA = dc.HIGH_DENSITY_LBA + 17
NAME_FILE = b"hello"


def name_fixture(root: Path, identifier: bytes, folders=()) -> Path:
    """A one-track disc whose only file has `identifier` as its ISO 9660 identifier, in the folders
    whose identifiers `folders` gives from the root down, and its sheet: 21 sectors, and a sector
    more for each folder. Its boot area is the small set's."""
    start = dc.HIGH_DENSITY_LBA
    lbas = [ROOT_LBA + depth for depth in range(len(folders) + 1)]
    payloads = {0: boot_sector(), 16: pvd_sector(b"TEST"), NAME_FILE_LBA - start: NAME_FILE}
    for depth, lba in enumerate(lbas):
        if depth < len(folders):
            entry = record(folders[depth], lbas[depth + 1], dc.SECTOR, 2)
        else:
            entry = record(identifier, NAME_FILE_LBA, len(NAME_FILE))
        payloads[lba - start] = directory(lba, lbas[max(depth - 1, 0)], entry)
    (root / "track3.bin").write_bytes(raw_track(start, lbas[-1] + 1 - start, payloads))
    sheet = root / "names.cue"
    sheet.write_bytes(one_track_cue("track3.bin").encode("ascii"))
    return sheet


# The full set's SIZES.ROM: levels/a.wad, DISK.ROM's five bytes; levels/b.rom, empty, after them;
# and ext.str, seven bytes outside DISK.ROM.
FULL_SIZES_ROM = sizes_rom([(5, 0, (0, 7, -1, 0)), (0, 5, (0, 13, -1, 0)),
                            (7, -1, (19, -1, 0, 0))], b"levels\0a.wad\0b.rom\0ext.str\0")
FULL_DISK_ROM = b"hello"

# The full set's AUDIO/X.STR, which spans two sectors.
FULL_STREAM = (bytes(range(256)) * 12)[:3000]

# The full set's cue sheet: a byte order mark, CRLF line ends, indents of two and four spaces, and
# an audio track whose file the set leaves out.
FULL_CUE = ('\ufeffREM HIGH-DENSITY AREA\r\nFILE "track3.bin" BINARY\r\n'
            '  TRACK 03 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
            'FILE "track4.bin" BINARY\r\n  TRACK 04 AUDIO\r\n'
            '    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n'
            'FILE "track5.bin" BINARY\r\n  TRACK 05 MODE1/2352\r\n'
            '    INDEX 00 00:00:00\r\n    INDEX 01 00:03:00\r\n')

# The full set's second data track: where it starts, after the audio track, and the sectors of
# the pregap before it that track5.bin stores.
FULL_SECOND_LBA = 45100
FULL_PREGAP_SECTORS = 225

# Where the full set's root lists EMPTY.BIN, a file of no bytes: an LBA no track holds.
FULL_EMPTY_LBA = 999999

# The GDI set's tracks: number, first LBA, type (4 raw data, 0 audio) and file. The audio track's
# file is left out of the set, as the full set leaves out its own.
GDI_DATA, GDI_AUDIO = 4, 0
FULL_GDI_TRACKS = [(1, dc.HIGH_DENSITY_LBA, GDI_DATA, "track03.bin"),
                   (2, 45050, GDI_AUDIO, "track04.raw"),
                   (3, FULL_SECOND_LBA, GDI_DATA, "track05.bin")]


def full_tracks() -> tuple[bytes, bytes]:
    """The full set's two data tracks without the second's pregap. The first is 22 sectors from
    45000: the boot area, the volume descriptor at 16, the root directory at 20 and AUDIO/ at 21.
    The second is the four from FULL_SECOND_LBA: DISK.ROM, SIZES.ROM and the two of AUDIO/X.STR."""
    audio = ROOT_LBA + 1
    disk, sizes, stream = FULL_SECOND_LBA, FULL_SECOND_LBA + 1, FULL_SECOND_LBA + 2
    root = directory(ROOT_LBA, ROOT_LBA,
                     record(b"AUDIO", audio, dc.SECTOR, 2)
                     + record(b"DISK.ROM;1", disk, len(FULL_DISK_ROM))
                     + record(b"EMPTY.BIN;1", FULL_EMPTY_LBA, 0)
                     + record(b"SIZES.ROM;1", sizes, len(FULL_SIZES_ROM)))
    boot = boot_sector(area=b" U      ", version=b"V1.003",
                       title=b"GAUNTLET LEGENDS".ljust(128, b" "))
    first = raw_track(dc.HIGH_DENSITY_LBA, 22, {
        0: boot, 16: pvd_sector(b"GAUNT1"), 20: root,
        21: directory(audio, ROOT_LBA, record(b"X.STR;1", stream, len(FULL_STREAM)))})
    second = raw_track(FULL_SECOND_LBA, 4, {0: FULL_DISK_ROM, 1: FULL_SIZES_ROM,
                                            2: FULL_STREAM[:dc.SECTOR], 3: FULL_STREAM[dc.SECTOR:]})
    return first, second


def full_set(root: Path) -> Path:
    """The end-to-end set: a disc whose SIZES.ROM and DISK.ROM restore native files, a folder, a
    file of no bytes and an external record, behind the cue sheet FULL_CUE. Returns the sheet."""
    first, second = full_tracks()
    (root / "track3.bin").write_bytes(first)
    (root / "track5.bin").write_bytes(b"\0" * dc.RAW_SECTOR * FULL_PREGAP_SECTORS + second)
    sheet = root / "full.cue"
    sheet.write_bytes(FULL_CUE.encode("utf-8"))
    return sheet


def full_gdi_set(root: Path) -> Path:
    """The full set's sectors behind a GDI of FULL_GDI_TRACKS, with CRLF line ends: each data
    track's file holds its sectors from its first LBA, the second's without the pregap. Returns the
    GDI."""
    data = iter(full_tracks())
    lines = [str(len(FULL_GDI_TRACKS))]
    for number, lba, kind, name in FULL_GDI_TRACKS:
        if kind == GDI_DATA:
            (root / name).write_bytes(next(data))
        lines.append(f"{number} {lba} {kind} {dc.RAW_SECTOR} {name} 0")
    sheet = root / "full.gdi"
    sheet.write_bytes("".join(line + "\r\n" for line in lines).encode("ascii"))
    return sheet


# The cases. Beside the script's own outcome, which the generator finds by running it, each holds
# the launcher's: the rule its reader refuses the case by, or None when it takes the case; the name
# rule, where checking a name decides; and, where the launcher decides otherwise than the script,
# the deviation that explains why.

# The launcher's departures from the scripts the cases show, by number. Each refuses only what no
# real dump holds.
DEVIATIONS = {
    1: "a delete character in a name, which the launcher refuses as a control character",
    2: "a component of over 200 characters or a path of over 220, which the launcher refuses",
    3: "one of the alpha installer's names at any depth, which the launcher never writes",
    4: "a device name followed by spaces, which Windows opens as the device",
    5: "a port's device name ending in a superscript digit, which Windows opens as the device too",
    6: "a folder spelled in two letter cases, one folder on Windows and two on Linux for the "
       "script, which the launcher refuses so that a tree is the same on both",
    7: "a cue sheet's number written in digits other than ASCII's, which the launcher does not "
       "read as a number",
    8: "a cue sheet's track number past 32 bits, or a time or its count of frames past 64, which "
       "the launcher refuses as too large",
    9: "two track files whose names differ only in letter case, which the script refuses on "
       "Windows alone and the launcher on every system",
}

N64_VECTORS = "n64-vectors.json"
JSON_STRINGS = "json-strings.json"

# The images the N64 vectors freeze, by their files: the builder's default ROM, and one holding a
# compressed little-endian WAD under a zero tag byte, an empty file and a map stored plain.
FROZEN_IMAGES = {
    "default": None,
    "mixed": [("/a/x.WAD", wad("<", b"\0TOC"), True), ("/b.bin", b"", False),
              ("/levels/m/worlds.wad", wad(), False)],
}

# What the frozen DEFLATE streams the inflate cases are cut from hold.
DEFLATED_PAYLOADS = {"nativeBytes": b"original native bytes" * 100, "test": b"test"}

# A ROM of plain files only, which needs no compressor, so that every language builds its bytes.
PLAIN_ROM_FILES = [("/x", b"data", False)]

# (image, file name, golden): a frozen image extracted from a file of that name, and the manifest
# file the extractor writes for it.
N64_EXTRACTIONS = [("default", "source.z64", "n64-default-manifest.json"),
                   ("mixed", "mixed.z64", "n64-mixed-manifest.json")]

# (transform, file name, golden): the default image with every byte pair swapped or every word
# reversed, extracted from a file of that name.
N64_BYTE_ORDERS = [("pairs", "source-byte-swapped.v64", "n64-byte-swapped-manifest.json"),
                   ("words", "source-little-endian.v64", "n64-little-endian-manifest.json")]

# Names the default image is extracted from: one outside ASCII, and one that is no Unicode at all,
# as a system may hand a file name over.
N64_SOURCE_NAMES = ["sourc\u00e9\U0001d11e.z64", "x\udc80.z64"]

# (input, rule): bytes of their own, or a frozen image cut short by `drop` bytes.
N64_HEADER_CASES = [({"hex": ""}, "header"), ({"hex": "00" * 64}, "byteOrder"),
                    ({"image": "default", "drop": 1}, "header")]

# (native path, rule, name rule, deviation), each the only file of a ROM.
N64_PATH_CASES = [
    # The extractor's own refusals.
    ("relative", "leadingSlash", None, None),
    ("//root/file", "leadingSlash", None, None),
    ("/../outside", "unsafePath", "dot", None),
    ("/dir/./file", "unsafePath", "dot", None),
    ("/dir//file", "unsafePath", "empty", None),
    ("/dir\\file", "unsafePath", "forbidden", None),
    ("/C:/file", "unsafePath", "forbidden", None),
    ("/con.txt", "unsafePath", "deviceName", None),
    ("/LPT1/file", "unsafePath", "deviceName", None),
    ("/file. ", "unsafePath", "trailingDotOrSpace", None),
    ("/CONIN$", "unsafePath", "deviceName", None),
    ("/CONOUT$.txt", "unsafePath", "deviceName", None),
    ("/COM0", "unsafePath", "deviceName", None),
    ("/LPT0", "unsafePath", "deviceName", None),
    ("/_program/game.bin", "unsafePath", "reserved", None),
    ("/manifest.json", "unsafePath", "reserved", None),
    ("/bad\x01file", "unsafePath", "control", None),
    # Taken by both.
    ("/a", None, None, None),
    ("/a/b.c", None, None, None),
    ("/COM10", None, None, None),
    ("/x/manifest.json", None, None, None),
    ("/x/_program", None, None, None),
    ("/a b", None, None, None),
    ("/ a", None, None, None),
    ("/.a", None, None, None),
    ("/a..b", None, None, None),
    ("/$", None, None, None),
    ("/a~b", None, None, None),
    ("/a;1", None, None, None),
    # Taken by the extractor, refused by the launcher.
    ("/a\x7fb", "unsafePath", "control", 1),
    ("/CON .txt", "unsafePath", "deviceName", 4),
    ("/x/installation.json", "unsafePath", "alphaName", 3),
    ("/" + "a" * 201, "unsafePath", "componentTooLong", 2),
    ("/" + "a" * 115 + "/" + "b" * 115, "unsafePath", "pathTooLong", 2),
    # Refused by both.
    ("/a.", "unsafePath", "trailingDotOrSpace", None),
    ("/a\tb", "unsafePath", "control", None),
    ("/Con.tar.gz", "unsafePath", "deviceName", None),
    ("/MANIFEST.JSON", "unsafePath", "reserved", None),
    ("/_Program/x", "unsafePath", "reserved", None),
    ("/manifest.json/x", "unsafePath", "reserved", None),
]

# (native paths, rule, deviation), together the files of one ROM.
N64_COLLISION_CASES = [
    (["/file", "/FILE"], "duplicatePath", None),
    (["/folder", "/folder/file"], "collision", None),
    (["/Font/a", "/font/b"], "treeFolderCase", 6),
]

# (lowered expansion limit, rule), on the default image.
N64_LIMIT_CASES = [(2, "expandedSize")]

# A stored block of ten bytes that is not the last, then a block header of the reserved type. An
# inflater that stops one byte past the size it was given reaches the bad header only when the size
# lies past the stored block.
STORED_THEN_BAD_BLOCK = bytes((0,)) + struct.pack("<2H", 10, 0xFFF5) + b"A" * 10 + bytes((0b111,))

# A block whose first code is a match reaching back before the stream begins.
MATCH_BEFORE_THE_START = bytes.fromhex("030200")

# (WAD, rule): a ROMDISK WAD whose directory fits one byte order alone.
N64_WAD_CASES = [
    (wad(">", b"\0TOC"), None),
    (wad("<", b"\0TOC"), None),
    (b"", "wadHeader"),
    (wad()[:-1], "wadDirectory"),
    (wad() + b"junk", "wadDirectory"),
    (struct.pack(">2I", 4, 1) + wad()[8:], "wadDirectory"),
    (struct.pack(">2I", 8, 0), None),
    (wad(">", b"TE\x7fT"), "wadSection"),
    # A section that starts inside the header, and one that starts at the directory itself.
    (struct.pack(">2I", 12, 1) + b"data" + struct.pack(">4sII", b"TEST", 7, 4), "wadSection"),
    (struct.pack(">2I", 12, 1) + b"data" + struct.pack(">4sII", b"TEST", 12, 4), None),
]

# Strings as the manifests' writer renders them: every character outside ASCII and every control
# character escaped. A string that is no Unicode is given by its UTF-16 units.
JSON_STRING_CASES = [
    {"text": 'q"b\\s/'},
    {"text": "\n\r\t\b\f"},
    {"text": "\0\x1f\x7f~ "},
    {"text": "\u00e9"},
    {"text": "\u2028"},
    {"text": "\U0001d11e"},
    {"units": [120, 0xDC80, 46, 122, 54, 52]},
    {"text": "sourc\u00e9\U0001d11e.z64"},
    {"text": ""},
    {"text": "".join(map(chr, range(0x20, 0x7F)))},
]

DREAMCAST_VECTORS = "dreamcast-vectors.json"


def sector_header(address: tuple[int, int, int], mode: int = 1) -> bytes:
    """The 16 bytes a raw sector begins with: the sync, its address as written (the BCD minute,
    second and frame) and its mode."""
    return dc.SYNC + bytes((*address, mode))


# (header, rule): a raw sector's first bytes, read for the LBA their address gives.
DREAMCAST_SECTOR_CASES = [
    (bytes(raw_sector(45000, b"")[:16]), None),
    (bytes(raw_sector(547163, b"")[:16]), None),
    (sector_header((0x00, 0x02, 0x00)), None),
    (sector_header((0x00, 0x00, 0x00)), None),
    # The minute's tens may pass nine, as the high-density area's addresses do.
    (sector_header((0xA0, 0x00, 0x00)), None),
    (sector_header((0xF9, 0x59, 0x74)), None),
    (sector_header((0x0A, 0x00, 0x00)), "sectorBcd"),
    (sector_header((0x00, 0xA0, 0x00)), "sectorBcd"),
    (sector_header((0x00, 0x60, 0x00)), "sectorTime"),
    (sector_header((0x00, 0x00, 0x75)), "sectorTime"),
    (sector_header((0x00, 0x02, 0x00), mode=2), "sectorHeader"),
    (bytes(12) + bytes((0x00, 0x02, 0x00, 1)), "sectorHeader"),
    (sector_header((0x00, 0x02, 0x00))[:15], "sectorHeader"),
]

# The text whose EDC is the check value of ECMA-130's code.
EDC_CHECK_TEXT = "123456789"

# The payloads the EDC cases' sectors hold, by name, each followed by noughts.
EDC_PAYLOADS = {"zero": b"", "bytes0to255x8": bytes(range(256)) * 8, "hello": b"hello"}

# (LBA, payload): a raw sector the builder makes, whose EDC the launcher checks.
DREAMCAST_EDC_CASES = [(0, "zero"), (16, "zero"), (45000, "zero"), (45016, "zero"),
                       (549149, "zero"), (547163, "zero"), (45000, "bytes0to255x8"),
                       (45100, "hello")]

# (name, name rule, deviation): a name given to each reader that takes one from a dump: a cue
# sheet's FILE line, an ISO 9660 identifier with ";1" after it, and a SIZES.ROM component. The name
# rule is what the launcher's check of a name by itself gives it, or None where that check takes
# it; launcher_name_outcomes says what each reader makes of that.
DREAMCAST_NAME_CASES = [
    # Taken by both, or refused by both.
    ("Track (1).bin", None, None),
    ("a", None, None),
    ("a;1", None, None),
    ("a" * 200, None, None),
    ("trak\u00e9.bin", None, None),
    ("../other.bin", "forbidden", None),
    ("../escape", "forbidden", None),
    ("", "empty", None),
    (".", "dot", None),
    ("..", "dot", None),
    ("a.", "trailingDotOrSpace", None),
    ("a ", "trailingDotOrSpace", None),
    ("a/b", "forbidden", None),
    ("A/B", "forbidden", None),
    ("a\\b", "forbidden", None),
    ("a:b", "forbidden", None),
    ("NUL.x.y", "deviceName", None),
    ("con\u0131n$", "deviceName", None),
    # Taken by the extractor, refused by the launcher.
    ("a\x7fb", "control", 1),
    ("a" * 201, "componentTooLong", 2),
    ("INSTALLATION.JSON", "alphaName", 3),
    (".gdl-update", "alphaName", 3),
    ("CON .bin", "deviceName", 4),
    ("CON .X", "deviceName", 4),
    ("COM\u00b9.bin", "deviceName", 5),
]

# The one row of the SIZES.ROM table a name is the one component of: five bytes at the start of a
# DISK.ROM of five.
NAME_TABLE_ROW = (5, 0, (0, -1, 0, 0))
NAME_DISK_LEN = 5

# The folders of the long paths; a path's file takes up what is left of its length.
LONG_PATH_FOLDERS = ("a" * 54, "b" * 54, "c" * 54)


def long_path(chars: int) -> list[str]:
    """The components of a path of `chars` characters, separators included: the folders of
    LONG_PATH_FOLDERS, then a file named to make up the length."""
    used = sum(len(folder) + 1 for folder in LONG_PATH_FOLDERS)
    return [*LONG_PATH_FOLDERS, "d" * (chars - used)]


# (characters, rule, name rule, deviation): a disc's file in the three folders of
# LONG_PATH_FOLDERS whose path has that many characters, as the walk of the disc lists it.
DREAMCAST_ISO_PATH_CASES = [(220, None, None, None), (221, "entryName", "pathTooLong", 2)]

# (name, sheet, rule, name rule, deviation): a cue sheet as text, or as bytes where it is no UTF-8.
DREAMCAST_SHEET_CASES = [
    ("minimal", one_track_cue(), None, None, None),
    ("CRLF line ends, indents and a byte order mark",
     "\ufeff" + one_track_cue().replace("\n", "\r\n").replace("TRACK", "  TRACK"),
     None, None, None),
    ("CR line ends", one_track_cue().replace("\n", "\r"), None, None, None),
    ("file separators between lines", one_track_cue().replace("\n", "\x1c"), None, None, None),
    ("line separators between lines", one_track_cue().replace("\n", "\u2028"), None, None, None),
    ("a unit separator ending a line", one_track_cue().replace("BINARY\n", "BINARY\x1f\n"),
     None, None, None),
    ("a no-break space indent", one_track_cue().replace("TRACK", "\u00a0TRACK"), None, None, None),
    ("REM alone", "REM\n" + one_track_cue(), "cueLine", None, None),
    ("REM and a tab", "REM\tx\n" + one_track_cue(), "cueLine", None, None),
    ("FILE in lower case", one_track_cue().replace("FILE", "file"), "cueLine", None, None),
    ("two spaces after FILE", one_track_cue().replace("FILE ", "FILE  "), "cueLine", None, None),
    ("a MODE2 track", one_track_cue(track="03 MODE2/2352"), "trackMode", None, None),
    ("CATALOG", "CATALOG 0000000000000\n" + one_track_cue(), "cueLine", None, None),
    ("FLAGS", one_track_cue() + "FLAGS DCP\n", "cueLine", None, None),
    ("PREGAP", one_track_cue().replace("INDEX 01", "PREGAP 00:02:00\nINDEX 01"), "cueLine",
     None, None),
    ("INDEX before TRACK", 'FILE "t.bin" BINARY\nINDEX 01 00:00:00\n', "cueLine", None, None),
    ("INDEX 02", one_track_cue() + "INDEX 02 00:01:00\n", "cueLine", None, None),
    ("INDEX 00 after INDEX 01", one_track_cue() + "INDEX 00 00:01:00\n", "cueLine", None, None),
    ("second 60", one_track_cue(index="00:60:00"), "cueTime", None, None),
    ("frame 75", one_track_cue(index="00:00:75"), "cueTime", None, None),
    ("minute 999", one_track_cue(index="999:00:00"), None, None, None),
    ("a track number with leading zeros", one_track_cue(track="000999 MODE1/2352"),
     None, None, None),
    ("two tracks in one file", one_track_cue() + "TRACK 04 AUDIO\nINDEX 01 00:00:00\n",
     "trackPerFile", None, None),
    ("one file named twice", one_track_cue() + one_track_cue(track="04 AUDIO"), "trackPerFile",
     None, None),
    ("FILE without TRACK, then FILE", 'FILE "u.bin" BINARY\n' + one_track_cue(), None, None, None),
    ("FILE without TRACK at the end", one_track_cue() + 'FILE "u.bin" BINARY\n', None, None, None),
    ("TRACK without INDEX 01 at the end", one_track_cue() + 'FILE "u.bin" BINARY\nTRACK 04 AUDIO\n',
     "incompleteSheet", None, None),
    ("FILE before INDEX 01", 'FILE "t.bin" BINARY\nTRACK 03 MODE1/2352\nFILE "u.bin" BINARY\n',
     "missingIndex", None, None),
    ("a track number twice", one_track_cue() + one_track_cue("u.bin", "03 AUDIO"),
     "duplicateTrack", None, None),
    ("an empty sheet", "", "incompleteSheet", None, None),
    ("a file outside the sheet's folder", one_track_cue("../t.bin"), "trackName", "forbidden",
     None),
    ("the extractor's test's file outside the sheet's folder", one_track_cue("../other.bin"),
     "trackName", "forbidden", None),
    ("a file name outside ASCII", one_track_cue("tr\u00e9ck.bin"), None, None, None),
    ("a quote in a file name", one_track_cue('t"x.bin'), "cueLine", None, None),
    ("a NUL ending a line", one_track_cue().replace("BINARY", "BINARY\0"), "cueLine", None, None),
    ("no UTF-8", b"\xff" + one_track_cue().encode("ascii"), "sheetText", None, None),
    # Taken by the extractor, refused by the launcher.
    ("Arabic-Indic digits in a track number", one_track_cue(track="\u0660\u0663 MODE1/2352"),
     "cueLine", None, 7),
    ("fullwidth digits in an index", one_track_cue(index="\uff10\uff10:00:00"), "cueLine", None, 7),
    ("a track number past 32 bits", one_track_cue(track="4294967296 MODE1/2352"),
     "numberTooLarge", None, 8),
    ("a minute past 64 bits", one_track_cue(index="18446744073709551616:00:00"), "numberTooLarge",
     None, 8),
    ("a count of frames past 64 bits", one_track_cue(index="4099276460824345:00:00"),
     "numberTooLarge", None, 8),
    # Refused by the extractor on Windows alone, by the launcher on every system.
    ("one file named in two letter cases", one_track_cue() + one_track_cue("T.BIN", "04 AUDIO"),
     "trackPerFile", None, 9),
]


def levels_table(offset: int = 0, size: int = 5, component: int = 7) -> bytes:
    """The extractor's tests' SIZES.ROM table: one row naming levels/worlds.rom, `size` bytes at
    `offset` of DISK.ROM, its second component at `component` of the pool."""
    return sizes_rom([(size, offset, (0, component, -1, 0))], b"levels\0worlds.rom\0")


def name_table(name: str) -> bytes:
    """A SIZES.ROM table whose one row, NAME_TABLE_ROW, has `name` as its one component."""
    return sizes_rom([NAME_TABLE_ROW], name.encode("utf-8") + b"\0")


def path_table(components: list[str]) -> bytes:
    """A SIZES.ROM table whose one row is NAME_TABLE_ROW's file named by four components."""
    offsets = [sum(len(component) + 1 for component in components[:n])
               for n in range(len(components))]
    size, offset, _ = NAME_TABLE_ROW
    return sizes_rom([(size, offset, tuple(offsets))],
                     b"".join(component.encode("ascii") + b"\0" for component in components))


# SIZES.ROM tables, by the extractor's test that reads each: (table, DISK.ROM's length, rule).
DREAMCAST_SIZES_CASES = {
    "firstNegativeComponent": (levels_table(), 5, None),
    "externalFile": (levels_table(offset=-1), 0, None),
    "outOfBoundsPayload": (levels_table(size=6), 5, "sizesExtent"),
    "outOfBoundsName": (levels_table(component=100), 5, "sizesName"),
    "truncatedTable": (struct.pack("<I", 1000), 5, "sizesTable"),
    "duplicateNames": (sizes_rom([NAME_TABLE_ROW] * 2, b"file\0"), 5, "sizesDuplicate"),
    "fileDirectoryConflict": (sizes_rom([NAME_TABLE_ROW, (5, 0, (0, 5, -1, 0))], b"file\0child\0"),
                              5, "sizesConflict"),
}

# The extractor's unpack test: a table of a file in DISK.ROM and an external one, DISK.ROM, and
# the golden of the manifest unpack_native writes for them.
UNPACK_SIZES_ROM = sizes_rom([(5, 0, (0, -1, 0, 0)), (9, -1, (9, -1, 0, 0))],
                             b"test.wad\0outside.rom\0")
UNPACK_DISK_ROM = b"hello"
UNPACK_GOLDEN = "dreamcast-unpack-native-manifest.json"

# More SIZES.ROM tables: (name, table, DISK.ROM's length, rule, name rule, deviation).
DREAMCAST_SIZES_EXTRAS = [
    ("countZero", struct.pack("<I", 0), 5, "sizesTable", None, None),
    ("headerCutShort", bytes(3), 5, "sizesHeader", None, None),
    ("offsetBelowExternal", levels_table(offset=-2), 5, "sizesExtent", None, None),
    ("noComponent", sizes_rom([(5, 0, (-1, 0, 0, 0))], b"x\0"), 5, "sizesNoName", None, None),
    ("emptyComponent", sizes_rom([(5, 0, (1, -1, 0, 0))], b"x\0"), 5, "sizesNameUnsafe", "empty",
     None),
    ("externalOfAnySize", levels_table(offset=-1, size=0xFFFFFFFF), 0, None, None, None),
    ("separatorInComponent", name_table("a/b"), 5, "sizesNameUnsafe", "forbidden", None),
    ("pathOf221Characters", path_table(long_path(221)), 5, "sizesNameUnsafe", "pathTooLong", 2),
]

# (identifier, LBA, size, flags): directory records the builder writes, for files and folders,
# with identifiers of even length, which a pad byte follows, and of odd.
DREAMCAST_RECORD_CASES = [(b"TEST.WAD;1", 45100, 5, 0), (b"\0", ROOT_LBA, dc.SECTOR, 2),
                          (b"AUDIO", ROOT_LBA + 1, dc.SECTOR, 2)]

# The GD-ROM sets, by their names in the vectors, and the builder of each.
DREAMCAST_SETS = {"small": fixture, "full": full_set, "fullGdi": full_gdi_set}

# The sets the extractor reads, by name: whether each holds a native table for unpack_native to
# restore, and the goldens of its tree's manifests, by their paths in the tree. It cannot read the
# GDI set.
DREAMCAST_EXTRACTIONS = {
    "small": (False, {"disc-manifest.json": "dreamcast-small-disc-manifest.json"}),
    "full": (True, {"disc-manifest.json": "dreamcast-full-disc-manifest.json",
                    "native-manifest.json": "dreamcast-full-native-manifest.json"}),
}

# The systems the launcher runs on, by the path rules their file systems follow: Windows compares
# names without letter case, Linux with it.
SYSTEMS = {"windows": PureWindowsPath, "linux": PurePosixPath}

# The system that runs this, by its name in SYSTEMS.
THIS_SYSTEM = "windows" if os.name == "nt" else "linux"


def n64_record_word_cases(image: bytes) -> list[tuple[int, int, int, str]]:
    """(entry, word, value, rule): one word of an entry's record in `image` changed."""
    return [(0, 0, 1, "flags"), (0, 1, len(image), "outsideData"),
            (0, 2, n64.MAX_FILE_SIZE + 1, "fileSize"), (0, 3, 0, "fileOverlap"),
            (0, 3, len(image), "outsideData"), (1, 2, 5, "fileSize")]


def n64_header_word_cases(image: bytes) -> list[tuple[dict, str]]:
    """(change, rule): a word of the ROMDISK header or pointer table in `image` set, or the second
    pointer made a copy of the first ("copy" is [to, from])."""
    base, _ = n64.read_directory(image)
    table = base + struct.unpack_from(">I", image, base + 36)[0]
    return [({"offset": base + 32, "value": len(image)}, "outsideData"),
            ({"offset": base + 40, "value": 65537}, "directoryCount"),
            ({"offset": base + 44, "value": 1}, "directoryCount"),
            ({"offset": table, "value": 48}, "recordPlace"),
            ({"offset": table, "value": len(image)}, "outsideData"),
            ({"copy": [table + 4, table]}, "recordPlace")]


def n64_deflate_cases(deflated: dict[str, bytes]) -> list[tuple[bytes, int, str, str | None]]:
    """(stream, size, trailer, rule): a raw DEFLATE stream inflated to the size given, its trailer
    "none" for a ROMDISK file and "overlayPadding" for an executable overlay."""
    native, test = deflated["nativeBytes"], deflated["test"]
    size = len(DEFLATED_PAYLOADS["nativeBytes"])
    return [(native, size, "none", None),
            (native[:-1], size, "none", "deflateEnd"),
            (native + b"x", size, "none", "deflateTrailer"),
            (native, size - 1, "none", "deflateEnd"),
            (native, size + 1, "none", "deflateEnd"),
            (b"garbage", size, "none", "deflate"),
            (test + b"trailer", 4, "overlayPadding", None),
            (test + b"x" * 16, 4, "overlayPadding", "overlayPadding"),
            (test + b"x" * 15, 4, "overlayPadding", None),
            (test + b"trailer", 4, "none", "deflateTrailer"),
            (MATCH_BEFORE_THE_START, 3, "none", "deflate"),
            (STORED_THEN_BAD_BLOCK, 5, "none", "deflateEnd"),
            (STORED_THEN_BAD_BLOCK, 10, "none", "deflate")]


def paths_rom(paths) -> bytes:
    """A ROM whose ROMDISK holds a plain file of one byte at each native path."""
    return rom([(path, b"x", False) for path in paths])


def header_input(case: dict, images: dict[str, bytes]) -> bytes:
    """What a header case hands the extractor: bytes of its own, or a frozen image cut short."""
    if "hex" in case:
        return bytes.fromhex(case["hex"])
    image = images[case["image"]]
    return image[:len(image) - case["drop"]]


def with_record_word(image: bytes, case: dict) -> bytes:
    """`image` with the record word a record word case names set to its value."""
    return change_entry(image, case["word"], case["value"], case["index"])


def with_header_word(image: bytes, case: dict) -> bytes:
    """`image` with the change a header word case names."""
    result = bytearray(image)
    if "copy" in case:
        to, start = case["copy"]
        result[to:to + 4] = result[start:start + 4]
    else:
        struct.pack_into(">I", result, case["offset"], case["value"])
    return bytes(result)


def reorder(image: bytes, transform: str) -> bytes:
    """A big-endian image in another byte order: every byte pair swapped, or every word reversed."""
    width = {"pairs": 2, "words": 4}[transform]
    return b"".join(image[i:i + width][::-1] for i in range(0, len(image), width))


def utf16_units(text: str) -> list[int]:
    """A string's UTF-16 code units, a lone surrogate kept as itself."""
    data = text.encode("utf-16-le", "surrogatepass")
    return list(struct.unpack(f"<{len(data) // 2}H", data))


def json_string(case: dict) -> str:
    """The string a JSON string case holds."""
    if "text" in case:
        return case["text"]
    units = case["units"]
    return struct.pack(f"<{len(units)}H", *units).decode("utf-16-le", "surrogatepass")


def tree_listing(folder: Path) -> list[list]:
    """Every file under `folder` as [path, size, SHA-256], in the order of their paths."""
    rows = []
    for path in folder.rglob("*"):
        if path.is_file():
            data = path.read_bytes()
            rows.append([path.relative_to(folder).as_posix(), len(data),
                         hashlib.sha256(data).hexdigest()])
    return sorted(rows)


def vectors(name: str):
    """A shared vector file, read."""
    return json.loads((VECTOR_FOLDER / name).read_text(encoding="utf-8"))


def script_result(function, *args, refusal: type = n64.FormatError,
                  **kwargs) -> tuple[object, str | None]:
    """What an extractor gives for a case and None, or None and its message refusing it with
    `refusal`: FormatError for the N64 extractor, ValueError for the Dreamcast extractor."""
    try:
        return function(*args, **kwargs), None
    except refusal as error:
        return None, str(error)


def outcome(python: str | dict | None, rule: str | None, name_rule: str | None = None,
            deviation: int | None = None) -> dict:
    """A case's outcome fields: the script's message and the launcher's rule, each None where it
    takes the case, then the name rule and the deviation where they apply. The script's message is
    given by system where the systems differ. Where one takes a case the other refuses, on any
    system, a deviation must say why."""
    messages = python.values() if isinstance(python, dict) else [python]
    if any((message is None) != (rule is None) for message in messages) and deviation is None:
        raise ValueError(f"the script and the launcher disagree with no deviation: {python!r}, "
                         f"{rule!r}")
    fields = {"python": python, "rule": rule}
    if name_rule is not None:
        fields["nameRule"] = name_rule
    if deviation is not None:
        if deviation not in DEVIATIONS:
            raise ValueError(f"deviation {deviation} is not one of DEVIATIONS")
        fields["deviation"] = deviation
    return fields


def romdisk_files(image: bytes) -> list[tuple]:
    """What a ROM's ROMDISK holds, whatever compressed it: each file's path, flags, size, last
    record word and bytes."""
    _, entries = n64.read_directory(image)
    return [(entry.path, entry.flags, entry.size, entry.unknown_word, n64.read_entry(image, entry))
            for entry in entries]


def n64_frozen(path: Path, refreeze: bool) -> tuple[dict[str, bytes], dict[str, bytes]]:
    """The images and DEFLATE streams the N64 vectors freeze: compressed anew by this Python's zlib,
    or carried over from the vectors at `path`, which must still hold what the builders make."""
    if refreeze:
        return ({name: rom(files) for name, files in FROZEN_IMAGES.items()},
                {name: deflate(payload) for name, payload in DEFLATED_PAYLOADS.items()})
    if not path.is_file():
        raise FileNotFoundError(f"no vectors at {path} to carry the frozen images and streams over "
                                "from; write with --refreeze to make them")
    document = json.loads(path.read_text(encoding="utf-8"))
    images = {name: bytes.fromhex(document["images"][name]) for name in FROZEN_IMAGES}
    deflated = {name: bytes.fromhex(document["deflated"][name]) for name in DEFLATED_PAYLOADS}
    for name, files in FROZEN_IMAGES.items():
        if romdisk_files(images[name]) != romdisk_files(rom(files)):
            raise ValueError(f"the frozen {name} image no longer holds the files the builder puts "
                             "in it; write with --refreeze")
    for name, payload in DEFLATED_PAYLOADS.items():
        if n64.inflate(deflated[name], len(payload)) != payload:
            raise ValueError(f"the frozen {name} stream no longer holds its payload; write with "
                             "--refreeze")
    return images, deflated


def extracted(scratch: Path, image: bytes, name: str) -> tuple[bytes, list[list]]:
    """What the N64 extractor writes for `image` given as a file of that name, in a new folder of
    `scratch`: the manifest's bytes and the tree's listing."""
    folder = Path(tempfile.mkdtemp(dir=scratch))
    source, tree = folder / name, folder / "tree"
    source.write_bytes(image)
    n64.extract(source, tree)
    return (tree / "manifest.json").read_bytes(), tree_listing(tree)


def write_document(path: Path, document) -> None:
    """Writes a vector file: indented JSON in ASCII, ending in a newline, LF on every system."""
    path.write_bytes((json.dumps(document, indent=2) + "\n").encode("ascii"))


def write_n64_vectors(folder: Path, refreeze: bool) -> list[str]:
    """Writes the N64 vectors, the golden manifests and the JSON strings into `folder`."""
    images, deflated = n64_frozen(folder / N64_VECTORS, refreeze)
    default = images["default"]
    goldens = {}
    trees = {}
    extractions = []
    byte_orders = []
    source_names = []
    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        for image, name, golden in N64_EXTRACTIONS:
            goldens[golden], trees[image] = extracted(scratch, images[image], name)
            extractions.append({"image": image, "name": name, "golden": golden})
        for transform, name, golden in N64_BYTE_ORDERS:
            source = reorder(default, transform)
            canonical, byte_order = n64.normalize_rom(source)
            if canonical != default:
                raise ValueError(f"the {transform} order does not read back as the default image")
            goldens[golden], _ = extracted(scratch, source, name)
            byte_orders.append({"transform": transform, "byteOrder": byte_order, "name": name,
                                "golden": golden})
        for name in N64_SOURCE_NAMES:
            manifest, _ = extracted(scratch, default, name)
            lines = manifest.decode("ascii").split("\n")
            source_names.append({"units": utf16_units(name),
                                 "line": lines[lines.index('  "source": {') + 1]})
    headers = []
    for case, rule in N64_HEADER_CASES:
        _, python = script_result(n64.normalize_rom, header_input(case, images))
        headers.append({**case, **outcome(python, rule)})
    paths = []
    for path, rule, name_rule, deviation in N64_PATH_CASES:
        _, python = script_result(n64.read_directory, paths_rom([path]))
        paths.append({"path": path, **outcome(python, rule, name_rule, deviation)})
    collisions = []
    for native, rule, deviation in N64_COLLISION_CASES:
        _, python = script_result(n64.read_directory, paths_rom(native))
        collisions.append({"paths": native, **outcome(python, rule, deviation=deviation)})
    record_words = []
    for index, word, value, rule in n64_record_word_cases(default):
        case = {"index": index, "word": word, "value": value}
        _, python = script_result(n64.read_directory, with_record_word(default, case))
        record_words.append({**case, **outcome(python, rule)})
    header_words = []
    for case, rule in n64_header_word_cases(default):
        _, python = script_result(n64.read_directory, with_header_word(default, case))
        header_words.append({**case, **outcome(python, rule)})
    limits = []
    for most, rule in N64_LIMIT_CASES:
        with mock.patch.object(n64, "MAX_TOTAL_SIZE", most):
            _, python = script_result(n64.read_directory, default)
        limits.append({"maxTotal": most, **outcome(python, rule)})
    streams = []
    for stream, size, trailer, rule in n64_deflate_cases(deflated):
        padded = trailer == "overlayPadding"
        output, python = script_result(n64.inflate, stream, size, padded=padded)
        digest = None if output is None else hashlib.sha256(output).hexdigest()
        streams.append({"stream": stream.hex(), "size": size, "trailer": trailer,
                        "outputSha256": digest, **outcome(python, rule)})
    wads = []
    for data, rule in N64_WAD_CASES:
        sections, python = script_result(n64.wad_directory, data)
        wads.append({"hex": data.hex(), "sections": sections, **outcome(python, rule)})
    document = {
        "images": {name: image.hex() for name, image in images.items()},
        "deflated": {name: stream.hex() for name, stream in deflated.items()},
        "plainRom": {"files": [[path, payload.hex(), compressed]
                               for path, payload, compressed in PLAIN_ROM_FILES],
                     "hex": rom(PLAIN_ROM_FILES).hex()},
        "extractions": extractions,
        "byteOrders": byte_orders,
        "headers": headers,
        "paths": paths,
        "collisions": collisions,
        "recordWords": record_words,
        "headerWords": header_words,
        "limits": limits,
        "deflate": streams,
        "wads": wads,
        "sourceNames": source_names,
        "trees": trees,
    }
    strings = {"cases": [{**case, "rendered": json.dumps(json_string(case))}
                         for case in JSON_STRING_CASES]}
    write_document(folder / N64_VECTORS, document)
    for name, data in goldens.items():
        (folder / name).write_bytes(data)
    write_document(folder / JSON_STRINGS, strings)
    return [N64_VECTORS, *goldens, JSON_STRINGS]


def on_this_system(value):
    """A vector's value for the system that runs this: one value where every system gives the same,
    else the value by system."""
    return value[THIS_SYSTEM] if isinstance(value, dict) else value


class SheetOnSystem:
    """A cue sheet's path that read_cue reads the sheet through and joins the track files it names
    to, by one system's path rules whichever system runs it: Windows takes two names that differ
    only in letter case for one file."""

    def __init__(self, sheet: Path, system: str):
        self.sheet = sheet
        self.parent = SYSTEMS[system]("sheets")

    def read_text(self, encoding: str) -> str:
        return self.sheet.read_text(encoding=encoding)


def cue_tracks(sheet) -> list[list]:
    """The tracks the Dreamcast extractor reads from a cue sheet: each one's number, file name, mode
    and index."""
    return [[track.number, track.path.name, track.mode, track.index]
            for track in dc.read_cue(sheet)]


def disc_paths(sheet: Path) -> list[str]:
    """The paths of the files the Dreamcast extractor's walk of a disc's file system lists."""
    with dc.Disc(sheet) as disc:
        return [entry.path.as_posix() for entry in disc.entries]


def packed_rows(table: bytes, disk_len: int) -> list[list]:
    """The files the Dreamcast extractor reads from a SIZES.ROM table: each one's path, DISK.ROM
    offset (-1 for one outside DISK.ROM) and size."""
    return [[file.path.as_posix(), file.offset, file.size]
            for file in dc.packed_files(table, disk_len)]


def name_sheet(folder: Path, name: str) -> Path:
    """A cue sheet in `folder` whose one track lies in the file `name`."""
    sheet = folder / "track.cue"
    sheet.write_bytes(one_track_cue(name).encode("utf-8"))
    return sheet


def iso_identifier(name: str) -> bytes:
    """The ISO 9660 identifier a disc holds the file `name` by: its UTF-8 bytes, then version 1."""
    return (name + ";1").encode("utf-8")


def sheet_bytes(case: dict) -> bytes:
    """What the file of a sheet case holds."""
    return bytes.fromhex(case["bytes"]) if "bytes" in case else case["text"].encode("utf-8")


def file_digests(folder: Path) -> dict:
    """Every file in `folder`, by name, with its size and SHA-256."""
    files = {}
    for path in sorted(folder.iterdir(), key=lambda path: path.name):
        data = path.read_bytes()
        files[path.name] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return files


def gdi_data_tracks(folder: Path) -> list[list]:
    """The GDI set's data tracks in `folder` as the launcher's disc manifest lists them: each one's
    file, index 0, first LBA, and sectors by its file's length."""
    return [[name, 0, lba, (folder / name).stat().st_size // dc.RAW_SECTOR]
            for _, lba, kind, name in FULL_GDI_TRACKS if kind == GDI_DATA]


def extract_set(sheet: Path, output: Path, native: bool) -> None:
    """Extracts a set into `output` as the Dreamcast extractor's program does: the disc, then, where
    the set holds a native table, the native files."""
    dc.extract(sheet, output)
    if native:
        dc.unpack_native(output)


def launcher_name_outcomes(name: str, name_rule: str | None) -> dict:
    """The launcher's (rule, name rule) for a name in each reader that takes one from a dump, or
    None where the reader takes it. A cue sheet's FILE line holds no empty name and no quote, and
    the walk of a disc's file system and SIZES.ROM's table read names in ASCII alone: each reader
    refuses those first, then judges the name by itself as `name_rule` says."""
    def judged(rule: str):
        return None if name_rule is None else (rule, name_rule)
    in_ascii = name.isascii()
    return {"track": ("cueLine", None) if not name or '"' in name else judged("trackName"),
            "iso": judged("entryName") if in_ascii else ("entryName", None),
            "sizes": judged("sizesNameUnsafe") if in_ascii else ("sizesName", None)}


def by_system(results: dict) -> tuple:
    """A (result, message) pair found on each system: the one pair where every system found the
    same, else the results by system and the messages by system."""
    first, *others = results.values()
    if all(result == first for result in others):
        return first
    return ({system: result for system, (result, _) in results.items()},
            {system: message for system, (_, message) in results.items()})


def new_folder(scratch: Path) -> Path:
    """A new, empty folder in `scratch`."""
    return Path(tempfile.mkdtemp(dir=scratch))


def dreamcast_sectors() -> list[dict]:
    """The sector header cases, with the LBA the extractor reads from each."""
    cases = []
    for header, rule in DREAMCAST_SECTOR_CASES:
        lba, python = script_result(dc.sector_lba, header, refusal=ValueError)
        cases.append({"header": header.hex(), "lba": lba, **outcome(python, rule)})
    return cases


def dreamcast_edc() -> dict:
    """The EDC's check value, and the EDC of each EDC case's sector."""
    sectors = [{"lba": lba, "payload": payload,
                "edc": f"{edc(raw_sector(lba, EDC_PAYLOADS[payload])[:EDC_SPAN]):08x}"}
               for lba, payload in DREAMCAST_EDC_CASES]
    return {"check": {"ascii": EDC_CHECK_TEXT, "edc": f"{edc(EDC_CHECK_TEXT.encode('ascii')):08x}"},
            "payloads": {name: payload.hex() for name, payload in EDC_PAYLOADS.items()},
            "sectors": sectors}


def dreamcast_names(scratch: Path) -> list[dict]:
    """The name cases, with what each reader of the extractor makes of each name."""
    cases = []
    for name, name_rule, deviation in DREAMCAST_NAME_CASES:
        folder = new_folder(scratch)
        found = {
            "track": script_result(cue_tracks, name_sheet(folder, name), refusal=ValueError),
            "iso": script_result(disc_paths, name_fixture(folder, iso_identifier(name)),
                                 refusal=ValueError),
            "sizes": script_result(packed_rows, name_table(name), NAME_DISK_LEN,
                                   refusal=ValueError),
        }
        case = {"name": name}
        explained = False
        for reader, expected in launcher_name_outcomes(name, name_rule).items():
            _, python = found[reader]
            rule, reader_name_rule = expected or (None, None)
            # The deviation goes on the readers that decide the name otherwise than the script.
            differs = (python is None) != (rule is None)
            explained = explained or differs
            case[reader] = outcome(python, rule, reader_name_rule, deviation if differs else None)
        if deviation is not None and not explained:
            raise ValueError(f"the name {name!r} cites deviation {deviation}, but every reader "
                             "decides it as the script does")
        cases.append(case)
    return cases


def dreamcast_iso_paths(scratch: Path) -> list[dict]:
    """The long path cases, with what the extractor's walk makes of each disc."""
    cases = []
    for chars, rule, name_rule, deviation in DREAMCAST_ISO_PATH_CASES:
        components = long_path(chars)
        folders = [component.encode("ascii") for component in components[:-1]]
        sheet = name_fixture(new_folder(scratch), iso_identifier(components[-1]), folders)
        _, python = script_result(disc_paths, sheet, refusal=ValueError)
        cases.append({"depth": len(folders), "chars": chars, "path": "/".join(components),
                      **outcome(python, rule, name_rule, deviation)})
    return cases


def dreamcast_sheets(scratch: Path) -> list[dict]:
    """The cue sheet cases, with the tracks the extractor reads from each on each system."""
    cases = []
    for name, sheet, rule, name_rule, deviation in DREAMCAST_SHEET_CASES:
        data = sheet if isinstance(sheet, bytes) else sheet.encode("utf-8")
        path = new_folder(scratch) / "sheet.cue"
        path.write_bytes(data)
        tracks, python = by_system({
            system: script_result(cue_tracks, SheetOnSystem(path, system), refusal=ValueError)
            for system in SYSTEMS})
        written = {"bytes": data.hex()} if isinstance(sheet, bytes) else {"text": sheet}
        cases.append({"name": name, **written, "tracks": tracks,
                      **outcome(python, rule, name_rule, deviation)})
    return cases


def table_case(table: bytes, disk_len: int, rule: str | None, name_rule: str | None = None,
               deviation: int | None = None) -> dict:
    """A SIZES.ROM table case, with the files the extractor reads from the table."""
    files, python = script_result(packed_rows, table, disk_len, refusal=ValueError)
    return {"hex": table.hex(), "diskLen": disk_len, "files": files,
            **outcome(python, rule, name_rule, deviation)}


def dreamcast_sizes(scratch: Path, goldens: dict[str, bytes]) -> dict:
    """The SIZES.ROM table cases, and the unpack case, whose golden goes into `goldens`."""
    sizes = {name: table_case(table, disk_len, rule)
             for name, (table, disk_len, rule) in DREAMCAST_SIZES_CASES.items()}
    output = new_folder(scratch)
    (output / "disc").mkdir()
    (output / "disc" / "SIZES.ROM").write_bytes(UNPACK_SIZES_ROM)
    (output / "disc" / "DISK.ROM").write_bytes(UNPACK_DISK_ROM)
    dc.unpack_native(output)
    goldens[UNPACK_GOLDEN] = (output / "native-manifest.json").read_bytes()
    sizes["unpack"] = {**table_case(UNPACK_SIZES_ROM, len(UNPACK_DISK_ROM), None),
                       "disk": UNPACK_DISK_ROM.hex(), "golden": UNPACK_GOLDEN,
                       "native": tree_listing(output / "native")}
    sizes["extras"] = [{"name": name, **table_case(table, disk_len, rule, name_rule, deviation)}
                       for name, table, disk_len, rule, name_rule, deviation
                       in DREAMCAST_SIZES_EXTRAS]
    return sizes


def dreamcast_records() -> list[dict]:
    """The directory record cases, with the bytes the builder writes for each."""
    return [{"name": identifier.decode("ascii"), "lba": lba, "size": size, "flags": flags,
             "hex": record(identifier, lba, size, flags).hex()}
            for identifier, lba, size, flags in DREAMCAST_RECORD_CASES]


def dreamcast_sets(scratch: Path, goldens: dict[str, bytes]) -> dict:
    """Each set's files, and the tree the extractor writes for it, whose goldens go into
    `goldens`; or, for the GDI set, which the extractor cannot read, its data tracks."""
    sets = {}
    for name, build in DREAMCAST_SETS.items():
        folder = new_folder(scratch)
        sheet = build(folder)
        entry = {"sheet": sheet.name, "files": file_digests(folder)}
        if name in DREAMCAST_EXTRACTIONS:
            native, tree_goldens = DREAMCAST_EXTRACTIONS[name]
            tree = new_folder(scratch) / "tree"
            extract_set(sheet, tree, native)
            for path, golden in tree_goldens.items():
                goldens[golden] = (tree / path).read_bytes()
            entry.update(goldens=tree_goldens, tree=tree_listing(tree))
        else:
            entry["dataTracks"] = gdi_data_tracks(folder)
        sets[name] = entry
    return sets


def write_dreamcast_vectors(folder: Path, refreeze: bool) -> list[str]:
    """Writes the Dreamcast vectors and the golden manifests into `folder`. No Dreamcast vector
    depends on a compressor, so nothing is carried over and `refreeze` changes nothing."""
    goldens = {}
    with tempfile.TemporaryDirectory() as temporary:
        scratch = Path(temporary)
        document = {
            "sectors": dreamcast_sectors(),
            "edc": dreamcast_edc(),
            "names": dreamcast_names(scratch),
            "isoPaths": dreamcast_iso_paths(scratch),
            "sheets": dreamcast_sheets(scratch),
            "sizes": dreamcast_sizes(scratch, goldens),
            "records": dreamcast_records(),
            "sets": dreamcast_sets(scratch, goldens),
        }
    write_document(folder / DREAMCAST_VECTORS, document)
    for name, data in goldens.items():
        (folder / name).write_bytes(data)
    return [DREAMCAST_VECTORS, *goldens]


# Each set of vectors, by the name write_vectors takes.
VECTOR_SETS = {"n64": write_n64_vectors, "dreamcast": write_dreamcast_vectors}


def write_vectors(folder, which: str, refreeze: bool = False) -> list[str]:
    """Writes every vector and golden of one set of VECTOR_SETS into `folder` by running the
    builders and the extractors, and returns the names of the files written."""
    if which not in VECTOR_SETS:
        raise ValueError(f"no set of vectors is called {which!r}")
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    return VECTOR_SETS[which](folder, refreeze)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write the Legends extractors' shared vectors and "
                                                 "goldens again.")
    parser.add_argument("--write", type=Path, required=True, metavar="FOLDER",
                        help="the folder to write them into: tests/scripts/data/legends")
    parser.add_argument("--refreeze", action="store_true",
                        help="compress the frozen ROM images and DEFLATE streams anew with this "
                             "Python's zlib instead of carrying them over")
    args = parser.parse_args(argv)
    try:
        for which in VECTOR_SETS:
            for name in write_vectors(args.write, which, refreeze=args.refreeze):
                print(args.write / name)
    except (OSError, ValueError) as error:
        print(f"writing the vectors failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
