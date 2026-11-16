"""Synthetic N64 ROMs and Dreamcast GD-ROM sets for the Legends extractor tests; no game data."""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "assets"))
import dreamcast_legends as dc
import n64_legends as n64


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
