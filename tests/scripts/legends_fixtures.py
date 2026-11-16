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
import struct
import sys
import tempfile
import zlib
from pathlib import Path
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
    6: "a folder spelled in two letter cases, one folder on Windows and two on Linux for the "
       "script, which the launcher refuses so that a tree is the same on both",
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


def script_result(function, *args, **kwargs) -> tuple[object, str | None]:
    """What the N64 extractor gives for a case and None, or None and its message refusing it."""
    try:
        return function(*args, **kwargs), None
    except n64.FormatError as error:
        return None, str(error)


def outcome(python: str | None, rule: str | None, name_rule: str | None = None,
            deviation: int | None = None) -> dict:
    """A case's outcome fields: the script's message and the launcher's rule, each None where it
    takes the case, then the name rule and the deviation where they apply. Where one takes a case
    the other refuses, a deviation must say why."""
    if (python is None) != (rule is None) and deviation is None:
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


# Each set of vectors, by the name write_vectors takes.
VECTOR_SETS = {"n64": write_n64_vectors}


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
