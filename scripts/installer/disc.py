"""Read GUNE5D ISO and sparse GameCube CISO images without mounting or conversion.

Format references: Dolphin DiscIO CISOBlob.h (0x8000-byte sparse block map)
and FileSystemGCWii (big-endian, 12-byte FST entries). This is an independent
reader: it copies native assets, not the console executable or system area.
"""

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import struct

DISC_SIZE = 1_459_978_240
CISO_HEADER = 0x8000
MAX_FST = 16 * 1024 * 1024
CHUNK = 1024 * 1024
RESERVED = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} | {
    f"{prefix}{n}" for prefix in ("COM", "LPT") for n in range(10)}


def safe_component(name: str) -> str:
    """Reject ambiguous or escaping names on both supported filesystems."""
    if (not name or name in (".", "..") or name[-1] in " ." or
            len(name) > 200 or any(ord(c) < 32 or c in '/\\:<>"|?*' for c in name) or
            name.split(".")[0].upper() in RESERVED):
        raise ValueError(f"Unsafe disc path component: {name!r}")
    return name


@dataclass(frozen=True)
class Entry:
    path: PurePosixPath
    offset: int
    size: int
    directory: bool = False


class DiscImage:
    """Random access to an ISO or sparse CISO; all extents are checked before copying."""

    def __init__(self, path: Path):
        self.file = path.open("rb")
        try:
            self.file.seek(0, 2)
            self.physical_size = self.file.tell()
            self.file.seek(0)
            magic = self.file.read(4)
            self.blocks = None
            self.block_size = 0
            if magic == b"CISO":
                self.file.seek(0)
                header = self.file.read(CISO_HEADER)
                if len(header) != CISO_HEADER:
                    raise ValueError("Truncated CISO header")
                self.block_size = struct.unpack_from("<I", header, 4)[0]
                if not 512 <= self.block_size <= 16 * 1024 * 1024:
                    raise ValueError("Unsupported GameCube CISO block size (not PSP CSO)")
                self.blocks = []
                used = 0
                for flag in header[8:]:
                    if flag not in (0, 1):
                        raise ValueError("Invalid CISO block map")
                    self.blocks.append(used if flag else None)
                    used += flag
                if CISO_HEADER + used * self.block_size > self.physical_size:
                    raise ValueError("Truncated CISO data")
                self.size = min(DISC_SIZE, len(self.blocks) * self.block_size)
            else:
                self.size = min(DISC_SIZE, self.physical_size)
            header = self.read(0, 0x440)
            if header[:6] != b"GUNE5D" or header[0x1C:0x20] != b"\xc2\x33\x9f\x3d":
                raise ValueError("Select a USA GameCube Gauntlet Dark Legacy disc (GUNE5D)")
            if header[6] != 0:
                raise ValueError("Unsupported disc number")
            self.revision = header[7]
            fst_offset, fst_size = struct.unpack_from(">II", header, 0x424)
            if fst_offset < 0x440 or not 12 <= fst_size <= MAX_FST:
                raise ValueError("Invalid GameCube filesystem extent")
            self.entries = self._directory(self.read(fst_offset, fst_size))
        except BaseException:
            self.file.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.file.close()

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ValueError("Disc file extends beyond the image")
        if self.blocks is None:
            self.file.seek(offset)
            data = self.file.read(size)
            if len(data) != size:
                raise ValueError("Truncated ISO data")
            return data
        result = bytearray()
        while size:
            block, within = divmod(offset, self.block_size)
            count = min(size, self.block_size - within)
            physical = self.blocks[block]
            if physical is None:
                result.extend(bytes(count))
            else:
                self.file.seek(CISO_HEADER + physical * self.block_size + within)
                data = self.file.read(count)
                if len(data) != count:
                    raise ValueError("Truncated CISO data")
                result.extend(data)
            size -= count
            offset += count
        return bytes(result)

    def _directory(self, fst: bytes) -> tuple[Entry, ...]:
        kind, parent, count = struct.unpack_from(">III", fst)
        if kind != 0x01000000 or parent != 0 or not 1 <= count <= len(fst) // 12:
            raise ValueError("Invalid GameCube filesystem root")
        strings = count * 12
        stack = [(0, count, PurePosixPath())]
        seen = set()
        entries = []
        total = 0
        for index in range(1, count):
            while index >= stack[-1][1]:
                stack.pop()
            field, offset, size = struct.unpack_from(">III", fst, index * 12)
            kind = field >> 24
            name_at = strings + (field & 0xFFFFFF)
            end = fst.find(b"\0", name_at)
            if kind not in (0, 1) or name_at >= len(fst) or end < 0:
                raise ValueError("Invalid GameCube filesystem entry")
            name = safe_component(fst[name_at:end].decode("ascii"))
            path = stack[-1][2] / name
            key = path.as_posix().casefold()
            if key in seen or len(path.as_posix()) > 220:
                raise ValueError(f"Duplicate or overlong disc path: {path}")
            seen.add(key)
            if kind:
                if offset != stack[-1][0] or not index < size <= stack[-1][1]:
                    raise ValueError("Invalid filesystem directory nesting")
                stack.append((index, size, path))
            elif offset + size > self.size:
                raise ValueError(f"Truncated disc file: {path}")
            if path.parts[0].casefold() in ("gauntlet", "carddemo"):
                if len(path.parts) == 1 and not kind:
                    raise ValueError("Asset root must be a directory")
                entries.append(Entry(path, offset, 0 if kind else size, bool(kind)))
                total += 0 if kind else size
        if total > DISC_SIZE:
            raise ValueError("Implausible total extracted size")
        roots = {e.path.as_posix().casefold() for e in entries if e.directory}
        if not {"gauntlet", "carddemo"} <= roots:
            raise ValueError("Disc is missing Gauntlet or carddemo assets")
        files = {e.path.as_posix().casefold() for e in entries if not e.directory}
        if not {"gauntlet/pdata/war.wad", "gauntlet/wdata/tower.wad"} <= files:
            raise ValueError("Disc is missing required native game data")
        return tuple(entries)
