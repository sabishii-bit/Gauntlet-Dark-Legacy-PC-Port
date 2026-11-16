#!/usr/bin/env python3
"""Extract the native ROMDISK filesystem from Gauntlet Legends for Nintendo 64.

The ROMDISK directory uses big-endian offsets relative to its 48-byte header.
Each directory pointer names a flags/stored-size/size/offset/unknown-word record,
followed by a zero-terminated absolute game path. Bit 0x20000000 selects raw
DEFLATE. WADs are preserved byte-for-byte after decompression, not converted to
the different GameCube archive format. The last record word is retained without
assigning an unverified checksum or timestamp meaning.

The US boot table independently records the executable overlays and their load
addresses.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from collections import Counter
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Optional


ROMDISK_MAGIC = b"-==ROMDISK Header - HERE I AM==-\0"
ROMDISK_HEADER_SIZE = 48
COMPRESSED = 0x20000000
MAX_FILE_SIZE = 64 * 1024 * 1024
MAX_TOTAL_SIZE = 512 * 1024 * 1024
US_SHA1 = "0489dcce749c6a5102681d288ed0616a4b94e99d"


class FormatError(ValueError):
    """The supplied image is not a supported, internally consistent ROMDISK."""


@dataclass(frozen=True)
class Entry:
    path: str
    record_offset: int
    flags: int
    stored_size: int
    size: int
    rom_offset: int
    unknown_word: int


def normalize_rom(data: bytes) -> tuple[bytes, str]:
    """Return canonical big-endian ROM bytes without modifying the input file."""
    if len(data) < 64 or len(data) % 4:
        raise FormatError("ROM must contain a complete, word-aligned N64 header")
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data, "big-endian"
    if magic == b"\x37\x80\x40\x12":
        result = bytearray(len(data))
        result[::2], result[1::2] = data[1::2], data[::2]
        return bytes(result), "byte-swapped"
    if magic == b"\x40\x12\x37\x80":
        result = bytearray(len(data))
        for lane in range(4):
            result[lane::4] = data[3 - lane::4]
        return bytes(result), "little-endian"
    raise FormatError("unrecognized Nintendo 64 ROM byte order")


def checked_end(offset: int, size: int, limit: int, label: str) -> int:
    if offset < 0 or size < 0 or offset > limit or size > limit - offset:
        raise FormatError(f"{label} extends outside its containing data")
    return offset + size


def checked_path(native: str) -> str:
    if not native.startswith("/") or native.startswith("//"):
        raise FormatError("ROMDISK paths must have one leading slash")
    parts = native[1:].split("/")
    reserved = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
    reserved.update(f"{prefix}{number}" for prefix in ("COM", "LPT") for number in range(10))
    for part in parts:
        if not part or part in (".", "..") or part[-1] in " .":
            raise FormatError(f"unsafe ROMDISK path: {native!r}")
        if any(ord(char) < 32 or char in '<>:"\\|?*' for char in part):
            raise FormatError(f"unsafe ROMDISK path: {native!r}")
        if part.split(".", 1)[0].upper() in reserved:
            raise FormatError(f"reserved ROMDISK filename: {native!r}")
    if parts[0].casefold() in ("_program", "manifest.json"):
        raise FormatError(f"reserved extractor output path: {native!r}")
    return "/".join(parts)


def read_directory(rom: bytes) -> tuple[int, list[Entry]]:
    base = rom.find(ROMDISK_MAGIC)
    if base < 0 or rom.find(ROMDISK_MAGIC, base + 1) >= 0:
        raise FormatError("expected exactly one native ROMDISK header")
    checked_end(base, ROMDISK_HEADER_SIZE, len(rom), "ROMDISK header")
    metadata_size, table_relative, count, reserved = struct.unpack_from(">4I", rom, base + 32)
    if reserved or not 0 < count <= 65536:
        raise FormatError("unsupported ROMDISK header or directory count")
    table = base + table_relative
    end = checked_end(table, metadata_size, len(rom), "ROMDISK directory")
    records_start = checked_end(table, count * 4, end, "ROMDISK pointer table")
    if table_relative < ROMDISK_HEADER_SIZE:
        raise FormatError("ROMDISK directory overlaps its header")
    entries = []
    names = set()
    total = 0
    prior_record_end = records_start
    for index in range(count):
        relative, = struct.unpack_from(">I", rom, table + index * 4)
        record = base + relative
        if record < prior_record_end or record % 4:
            raise FormatError("ROMDISK records overlap or are not word aligned")
        name_start = checked_end(record, 20, end, "ROMDISK entry")
        name_end = rom.find(b"\0", name_start, end)
        if name_end < 0:
            raise FormatError("unterminated ROMDISK path")
        prior_record_end = (name_end + 4) & ~3
        try:
            path = checked_path(rom[name_start:name_end].decode("ascii"))
        except UnicodeDecodeError as exc:
            raise FormatError("non-ASCII ROMDISK path") from exc
        key = path.casefold()
        if key in names:
            raise FormatError(f"duplicate case-insensitive ROMDISK path: {path}")
        names.add(key)
        flags, stored, size, data_relative, unknown = struct.unpack_from(">5I", rom, record)
        if flags not in (0, COMPRESSED):
            raise FormatError(f"unsupported ROMDISK flags 0x{flags:x}: {path}")
        if size > MAX_FILE_SIZE or (not flags and stored != size):
            raise FormatError(f"invalid ROMDISK file size: {path}")
        data_offset = base + data_relative
        if data_relative < ROMDISK_HEADER_SIZE:
            raise FormatError(f"ROMDISK file overlaps its header: {path}")
        checked_end(data_offset, stored, table, f"ROMDISK file {path}")
        total += size
        if total > MAX_TOTAL_SIZE:
            raise FormatError("ROMDISK expanded size exceeds extraction limit")
        entries.append(Entry(path, record, flags, stored, size, data_offset, unknown))
    for path in names:
        if any(str(parent) in names for parent in PurePosixPath(path).parents if str(parent) != "."):
            raise FormatError(f"ROMDISK file/directory collision: {path}")
    return base, entries


def inflate(data: bytes, size: int, *, padded: bool = False) -> bytes:
    """Bound raw DEFLATE expansion and require the declared stream boundary."""
    decoder = zlib.decompressobj(-15)
    try:
        output = decoder.decompress(data, size + 1)
    except zlib.error as exc:
        raise FormatError(f"invalid raw DEFLATE stream: {exc}") from exc
    if len(output) != size or not decoder.eof or decoder.unconsumed_tail:
        raise FormatError("DEFLATE output size or end marker does not match its directory")
    if decoder.unused_data and not padded:
        raise FormatError("unexpected bytes after ROMDISK DEFLATE stream")
    # Boot table extents are DMA-aligned and retain nonzero stream trailer bytes;
    # ROMDISK file extents, in contrast, end at the raw DEFLATE end marker.
    if padded and len(decoder.unused_data) >= 16:
        raise FormatError("unsupported executable overlay padding")
    return output


def read_entry(rom: bytes, entry: Entry) -> bytes:
    data = rom[entry.rom_offset:entry.rom_offset + entry.stored_size]
    return inflate(data, entry.size) if entry.flags == COMPRESSED else data


def wad_directory(data: bytes) -> list[dict]:
    """Describe native tags; the last field can be byte size or typed row count."""
    if len(data) < 8:
        raise FormatError("truncated WAD header")
    # The shipped losing movie retains a little-endian WAD inside the otherwise
    # big-endian ROM filesystem. Its bytes must not be silently rewritten.
    candidates = [(order, *struct.unpack_from(order + "2I", data)) for order in (">", "<")]
    valid = [(order, directory, count) for order, directory, count in candidates
             if directory >= 8 and directory % 4 == 0 and count <= 65536
             and directory + count * 12 == len(data)]
    if len(valid) != 1:
        raise FormatError("invalid or ambiguous native WAD directory")
    order, directory, count = valid[0]
    result = []
    for index in range(count):
        tag, offset, size_or_count = struct.unpack_from(order + "4sII", data, directory + 12 * index)
        if any(char != 0 and (char < 32 or char > 126) for char in tag) or not 8 <= offset <= directory:
            raise FormatError("invalid native WAD section")
        result.append({"tag": tag.decode("ascii"), "offset": offset, "size_or_count": size_or_count,
                       "byte_order": "big-endian" if order == ">" else "little-endian"})
    return result


def read_overlays(rom: bytes) -> list[tuple[dict, bytes]]:
    """Export only the verified US executable table, including its BSS metadata."""
    if hashlib.sha1(rom).hexdigest() != US_SHA1:
        return []
    result = []
    for index in (0, 1, 2, 4):
        record = 0x12E0 + index * 48
        name = rom[record:record + 16].split(b"\0", 1)[0].decode("ascii")
        offset, stored, address, memory, bss, bss_size, reserved0, reserved1 = struct.unpack_from(">8I", rom, record + 16)
        if reserved0 or reserved1 or bss - address != memory - bss_size:
            raise FormatError("inconsistent executable overlay metadata")
        checked_end(offset, stored, len(rom), "executable overlay")
        output = inflate(rom[offset:offset + stored], bss - address, padded=True)
        result.append(({"path": f"_program/{name}.bin", "rom_offset": offset,
                        "stored_size": stored, "load_address": address, "size": len(output),
                        "memory_size": memory, "bss_address": bss, "bss_size": bss_size,
                        "sha256": hashlib.sha256(output).hexdigest()}, output))
    return result


def sample_directory(data: bytes, sample_data: bytes) -> list[dict]:
    """Inspect native N64 MusyX SDIR rows; do not convert samples to GameCube DSP.

    US game.bin e0088834 advances by 0x1c bytes, not the older 24-byte MusyX
    record. e0088a60 keeps the runtime address at +8 separate from the ROM
    offset at +4. Sample type 3 uses N64 MusyX's 256-byte predictor book and
    40-byte frames containing 64 samples.
    """
    if len(data) < 4 or (len(data) - 4) % 28 or data[-4:] != b"\xff" * 4:
        raise FormatError("invalid native MusyX sample directory")
    result = []
    ids = set()
    for at in range(0, len(data) - 4, 28):
        identity, refs, offset, runtime, pitch_rate, count, loop_start, loop_length = struct.unpack_from(
            ">HH6I", data, at)
        sample_format, samples = count >> 24, count & 0xFFFFFF
        rate = pitch_rate & 0xFFFF
        if identity in ids or identity == 0xFFFF or refs or runtime or not rate or not samples:
            raise FormatError("invalid or duplicate native MusyX sample record")
        if sample_format != 3:
            raise FormatError(f"unsupported native MusyX sample format {sample_format}")
        if loop_length and (loop_start > samples or loop_length > samples - loop_start):
            raise FormatError("native MusyX sample loop exceeds its samples")
        encoded_size = 256 + ((samples + 63) // 64) * 40
        checked_end(offset, encoded_size, len(sample_data), "MusyX encoded sample")
        ids.add(identity)
        result.append({"id": identity, "sample_offset": offset, "encoded_size": encoded_size,
                       "sample_count": samples, "sample_rate": rate, "pitch": pitch_rate >> 24,
                       "loop_start": loop_start, "loop_length": loop_length,
                       "codec": "musyx-n64-vadpcm"})
    return result


def music_groups(data: bytes) -> list[dict]:
    """Native snd_pushgroup (e00842d8) follows relative PROJ group sizes."""
    result = []
    at = 0
    ids = set()
    while True:
        checked_end(at, 4, len(data), "MusyX group header")
        size, = struct.unpack_from(">I", data, at)
        if size == 0xFFFFFFFF:
            if at + 4 != len(data):
                raise FormatError("unexpected data after MusyX project terminator")
            return result
        checked_end(at, 8, len(data), "MusyX group header")
        end = checked_end(at, size, len(data), "MusyX group")
        identity, kind = struct.unpack_from(">2H", data, at + 4)
        if identity in ids or kind not in (0, 1):
            raise FormatError("invalid or duplicate MusyX group")
        count = 8 if kind == 0 else 6
        checked_end(at, 8 + count * 4, end, "MusyX group header")
        for relative in struct.unpack_from(f">{count}I", data, at + 8):
            if relative:
                checked_end(at + 8 + relative, 2, end, "MusyX group table")
        ids.add(identity)
        result.append({"id": identity, "kind": "song" if kind == 0 else "sound-effects",
                       "offset": at, "size": size})
        at = end


def song_header(data: bytes) -> dict:
    """Bound the fixed native sequence indexes without claiming to render a song."""
    checked_end(0, 24, len(data), "MusyX song header")
    tracks, regions, channels, tempos, initial_tempo, loop_start = struct.unpack_from(">6I", data)
    if tracks != 24 or initial_tempo == 0 or initial_tempo & 0x80000000:
        raise FormatError("unsupported native MusyX song header")
    checked_end(tracks, 64 * 4, len(data), "MusyX track index")
    checked_end(regions, 4, len(data), "MusyX region index")
    checked_end(channels, 64, len(data), "MusyX channel map")
    if tempos:
        checked_end(tempos, 8, len(data), "MusyX tempo table")
    active = 0
    for offset in struct.unpack_from(">64I", data, tracks):
        if offset:
            checked_end(offset, 12, len(data), "MusyX track region")
            active += 1
    return {"format": "musyx-sequence", "active_tracks": active,
            "initial_tempo_word": initial_tempo, "loop_start_tick": loop_start,
            "track_index_offset": tracks, "region_index_offset": regions,
            "channel_map_offset": channels, "tempo_table_offset": tempos}


def audio_catalog(rom: bytes) -> dict:
    """Catalog native music and its verified US song IDs, never fabricate WAVs."""
    if hashlib.sha1(rom).hexdigest() != US_SHA1:
        raise FormatError("audio catalog requires the verified US Gauntlet Legends ROM")
    _, entries = read_directory(rom)
    audio = {entry.path: read_entry(rom, entry) for entry in entries if entry.path.startswith("audio/")}
    paths = {name.casefold(): name for name in audio}
    bank_paths = [f"audio/gauntlet.{extension}" for extension in ("proj", "pool", "sdir", "samp")]
    if any(path not in audio for path in bank_paths):
        raise FormatError("native MusyX audio bank is incomplete")
    groups = music_groups(audio[bank_paths[0]])
    group_ids = {group["id"] for group in groups if group["kind"] == "song"}
    samples = sample_directory(audio[bank_paths[2]], audio[bank_paths[3]])
    game = next(data for row, data in read_overlays(rom) if row["path"] == "_program/game.bin")
    songs = []
    # US PlaySong e000c458: pointer table e000ee1c, group IDs e00a8a82.
    for identity in range(39):
        address, = struct.unpack_from(">I", game, 0xEE1C - 0x400 + identity * 4)
        name_start = address - 0xE0000400
        checked_end(name_start, 1, len(game), "native song name")
        name_end = game.find(b"\0", name_start)
        if name_end < 0:
            raise FormatError("unterminated native song name")
        try:
            native_name = game[name_start:name_end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise FormatError("invalid native song name") from exc
        path = paths.get(f"audio/{native_name}".casefold())
        group, = struct.unpack_from(">H", game, 0xA8A82 - 0x400 + identity * 4)
        if path is None or group not in group_ids:
            raise FormatError("native song references missing sequence or group")
        payload = audio[path]
        songs.append({"id": identity, "group_id": group, "path": path,
                      "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                      **song_header(payload)})
    return {"format": "gauntlet-legends-n64-audio-catalog", "version": 1,
            "source_sha256": hashlib.sha256(rom).hexdigest(), "playback_supported": False,
            "rendered_audio": [], "songs": songs, "groups": groups, "samples": samples,
            "bank_files": [{"path": path, "size": len(audio[path]),
                            "sha256": hashlib.sha256(audio[path]).hexdigest()} for path in bank_paths],
            "other_audio": [{"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                            for path, data in audio.items() if not path.endswith(".song") and path not in bank_paths],
            "notes": ["Songs are native MusyX sequences, not waveform recordings.",
                      "This revision combines relative PROJ offsets with 28-byte SDIR records.",
                      "A compatible sequencer and sample-bank adapter are required for soundtrack rendering."]}


def write_audio_catalog(source: Path, destination: Path) -> dict:
    """Write a new standalone catalog, preserving extracted files and manifests."""
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"output already exists: {destination}")
    rom, _ = normalize_rom(source.read_bytes())
    catalog = audio_catalog(rom)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(catalog, output, indent=2)
        output.write("\n")
    return catalog


def extract(source: Path, destination: Path) -> dict:
    """Validate everything first, then create a new tree; never replace a tree."""
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"output already exists: {destination}")
    original = source.read_bytes()
    rom, byte_order = normalize_rom(original)
    base, entries = read_directory(rom)
    decoded = [(entry, read_entry(rom, entry)) for entry in entries]
    manifest = {"format": "gauntlet-legends-n64-romdisk", "version": 1,
                "source": {"name": source.name, "size": len(original), "byte_order": byte_order,
                           "sha256": hashlib.sha256(original).hexdigest(),
                           "canonical_sha256": hashlib.sha256(rom).hexdigest()},
                "romdisk_offset": base, "files": [], "programs": [], "maps": []}
    for entry, data in decoded:
        row = {"path": entry.path, "record_offset": entry.record_offset,
               "rom_offset": entry.rom_offset, "flags": entry.flags,
               "stored_size": entry.stored_size, "size": entry.size,
               "unknown_word": entry.unknown_word,
               "stored_sha256": hashlib.sha256(rom[entry.rom_offset:entry.rom_offset + entry.stored_size]).hexdigest(),
               "sha256": hashlib.sha256(data).hexdigest()}
        if entry.path.lower().endswith(".wad"):
            row["wad_sections"] = wad_directory(data)
        manifest["files"].append(row)
        if entry.path.startswith("levels/") and entry.path.endswith("/worlds.wad"):
            manifest["maps"].append({"name": entry.path.split("/")[1], "world": entry.path,
                                     "objects": str(PurePosixPath(entry.path).with_name("objects.wad")),
                                     "runtime_supported": False})
    overlays = read_overlays(rom)
    manifest["programs"] = [row for row, _ in overlays]
    destination.mkdir(parents=True)
    for path, data in [(entry.path, data) for entry, data in decoded] + [(row["path"], data) for row, data in overlays]:
        target = destination.joinpath(*PurePosixPath(path).parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("xb") as output:
            output.write(data)
    with (destination / "manifest.json").open("x", encoding="utf-8", newline="\n") as output:
        json.dump(manifest, output, indent=2)
        output.write("\n")
    return manifest


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="your Gauntlet Legends N64 ROM")
    parser.add_argument("--output", type=Path, help="new destination directory; existing paths are refused")
    parser.add_argument("--list", action="store_true", help="validate directory and list native files without writing")
    parser.add_argument("--audio-catalog", type=Path, help="new JSON catalog of native songs and sample banks; no WAV conversion")
    args = parser.parse_args(argv)
    if sum((args.list, bool(args.output), bool(args.audio_catalog))) != 1:
        parser.error("select exactly one of --list, --output or --audio-catalog")
    try:
        if args.list:
            rom, _ = normalize_rom(args.rom.read_bytes())
            _, entries = read_directory(rom)
            for entry in entries:
                print(f"{entry.size:9d} {entry.stored_size:9d} {entry.path}")
            print(f"{len(entries)} files; {sum(entry.size for entry in entries)} expanded bytes")
        elif args.audio_catalog:
            catalog = write_audio_catalog(args.rom, args.audio_catalog)
            print(f"Cataloged {len(catalog['songs'])} native MusyX songs and {len(catalog['samples'])} samples "
                  f"to {args.audio_catalog}")
            print("Native song sequences and banks are extracted by --output; this does not render WAV audio.")
        else:
            manifest = extract(args.rom, args.output)
            extensions = Counter(PurePosixPath(row["path"]).suffix.lower() for row in manifest["files"])
            print(f"Extracted {len(manifest['files'])} native files ({extensions['.wad']} WADs), "
                  f"{len(manifest['maps'])} maps and {len(manifest['programs'])} executable overlays to {args.output}")
            print("Native N64 WADs are preserved; loading these maps in the port requires an N64 format adapter.")
    except (OSError, FormatError) as exc:
        print(f"N64 extraction failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
