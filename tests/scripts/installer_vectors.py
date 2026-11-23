"""The installer's disc and icon cases, kept as vectors the launcher's readers are held to; no game
data.

The vectors in tests/scripts/data/installer are written here by running the installer's disc reader
and icon decoder on the installer tests' own synthetic builders, never by hand. After a change to
any of them, write them again with

    python tests/scripts/installer_vectors.py --write tests/scripts/data/installer

Every case records what the installer did with its input (`python`: "accepted" or "refused", and
its `message`) and what the launcher does: its `rule`, None where it reads the input, and its text,
`detail`. The text is the installer's message where the installer refuses in the launcher's words,
and the one the tables below give where the launcher refuses what the installer reads, or words its
refusal otherwise.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "release"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from installer.disc import CISO_HEADER, DiscImage
from installer.icon import ICON_SIZES, decode_first_frame, icon_resources
from test_installer import synthetic_ciso, synthetic_iso
from test_installer_icon import tpl_fixture

# Where the vectors live; the launcher's tests read the same files.
VECTOR_FOLDER = ROOT / "tests" / "scripts" / "data" / "installer"
GAMECUBE_VECTORS = "gamecube-vectors.json"
ICON_VECTORS = "icon-vectors.json"

# The synthetic disc's icon entry: the size field of the file table's ninth row, and where its
# bytes lie (test_installer.synthetic_iso).
ICON_SIZE_FIELD = 0x800 + 8 * 12 + 8
ICON_DATA_AT = 0x2900

# The icon size whose image the resource vectors hold whole; of the other sizes they hold the
# length and the digest.
WHOLE_IMAGE_SIZE = 16


def u32(at: int, value: int) -> dict:
    return {"op": "u32be", "at": at, "value": value}


def byte(at: int, value: int) -> dict:
    return {"op": "byte", "at": at, "value": value}


def put(at: int, hex_text: str) -> dict:
    return {"op": "bytes", "at": at, "hex": hex_text}


def truncate(to: int) -> dict:
    return {"op": "truncate", "to": to}


def drop_last(count: int) -> dict:
    return {"op": "dropLast", "count": count}


def append(hex_text: str) -> dict:
    return {"op": "append", "hex": hex_text}


def append_zeros(count: int) -> dict:
    return {"op": "appendZeros", "count": count}


def edited(base: bytes, edits: list[dict]) -> bytes:
    """Applies a case's edits to its base in order; an edit that reaches past the bytes it changes
    is refused, as the launcher's tests refuse it."""
    data = bytearray(base)

    def within(at: int, length: int) -> None:
        if at < 0 or at + length > len(data):
            raise ValueError(f"an edit of {length} bytes at {at} reaches past {len(data)} bytes")

    for edit in edits:
        op = edit["op"]
        if op == "u32be":
            within(edit["at"], 4)
            struct.pack_into(">I", data, edit["at"], edit["value"])
        elif op == "byte":
            within(edit["at"], 1)
            data[edit["at"]] = edit["value"]
        elif op == "bytes":
            replacement = bytes.fromhex(edit["hex"])
            within(edit["at"], len(replacement))
            data[edit["at"]:edit["at"] + len(replacement)] = replacement
        elif op == "truncate":
            within(0, edit["to"])
            del data[edit["to"]:]
        elif op == "dropLast":
            within(0, edit["count"])
            del data[len(data) - edit["count"]:]
        elif op == "append":
            data.extend(bytes.fromhex(edit["hex"]))
        elif op == "appendZeros":
            data.extend(bytes(edit["count"]))
        else:
            raise ValueError(f"no edit is called {op!r}")
    return bytes(data)


def iso_with_icon(tpl: bytes) -> bytes:
    """The synthetic disc with `tpl` as its icon, as the installer's Windows icon tests make it."""
    disc = synthetic_iso()
    struct.pack_into(">I", disc, ICON_SIZE_FIELD, len(tpl))
    disc[ICON_DATA_AT:ICON_DATA_AT + len(tpl)] = tpl
    return bytes(disc)


def first_frame_only_tpl() -> bytes:
    """A 5x3 library of two table slots whose first image header was moved past the second slot,
    as the installer's partial-tile test makes it."""
    data = tpl_fixture(width=5, height=3)
    header = bytes(data[20:56])
    data[28:64] = header
    struct.pack_into(">I", data, 4, 2)
    struct.pack_into(">IIII", data, 12, 28, 0, 28, 0)
    return bytes(data)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# The disc cases: name, base image, edits, the launcher's rule (None where it reads the image) and
# its text where the installer's message is not it. The synthetic disc's file table lies at 0x800:
# twelve-byte rows for the root, Gauntlet, PDATA, WAR.WAD, WDATA, TOWER.WAD, EMPTY, carddemo and
# icon.tpl, then their names from 0x86C (Gauntlet at 0x86D, WDATA at 0x884, EMPTY at 0x894,
# carddemo at 0x89A, icon.tpl at 0x8A3), 172 bytes in all. A CISO keeps the disc's first block at
# 0x8000.
DISC_CASES = [
    ("empty", "empty", [], "outsideImage", None),
    ("cisoMagic", "empty", [append("4349534f")], "cisoHeader", None),
    ("cisoZeros", "empty", [append("4349534f"), append_zeros(CISO_HEADER)], "cisoBlockSize", None),
    ("notAGame", "empty", [append(b"not a game".hex())], "outsideImage", None),
    # A PSP CSO header: the mark, a header size of 0x18, a total of 0x100000, blocks of 0x800,
    # version 1, alignment 0 and two reserved bytes.
    ("pspCso", "empty", [append("4349534f1800000000001000000000000008000001000000"),
                         append_zeros(CISO_HEADER - 24)], "cisoBlockSize", None),
    ("id", "iso", [put(0, b"GALE01".hex())], "otherGame", None),
    ("europe", "iso", [byte(3, ord("P"))], "otherRegion", None),
    ("europeNkit", "iso", [byte(3, ord("P")), put(0x200, b"NKIT".hex())], "otherRegion", None),
    ("noMark", "iso", [put(0x1C, "00000000")], "notGameCube", None),
    ("cisoNoMark", "ciso", [put(0x8000 + 0x1C, "00000000")], "notGameCube", None),
    ("discNumber", "iso", [byte(6, 1)], "discNumber", None),
    ("revision0", "iso", [byte(7, 0)], None, None),
    ("revision2", "iso", [byte(7, 2)], "revision", "Unsupported disc revision: 2"),
    ("nkit", "iso", [put(0x200, b"NKIT".hex())], "nkit",
     "NKit images are not read: use the disc's plain ISO or CISO dump"),
    ("fst", "iso", [u32(0x424, 0xFFFFFF00)], "outsideImage", None),
    ("fstLow", "iso", [u32(0x424, 0x43F)], "fstExtent", None),
    ("fstSmall", "iso", [u32(0x428, 11)], "fstExtent", None),
    ("count", "iso", [u32(0x808, 0x10000000)], "fstRoot", None),
    ("kind", "iso", [byte(0x80C, 2)], "fstEntry", None),
    ("parent", "iso", [u32(0x810, 9)], "nesting", None),
    ("file", "iso", [u32(0x82C, 0xFFFFFFFF)], "truncatedFile", None),
    ("name", "iso", [byte(0x86D, ord("/"))], "unsafeName",
     'Unsafe disc path component: "/auntlet"'),
    ("caseDuplicate", "iso", [put(0x884, b"PDATa".hex())], "duplicatePath", None),
    ("deleteInName", "iso", [byte(0x896, 0x7F)], "unsafeName",
     'Unsafe disc path component: "EM\\u{7f}TY"'),
    ("nonAscii", "iso", [byte(0x896, 0xE9)], "unsafeName",
     'Unsafe disc path component: "EM�TY"'),
    ("deviceWithSpace", "iso", [put(0x8A3, b"CON .tpl".hex())], "unsafeName",
     'Unsafe disc path component: "CON .tpl"'),
    # The icon's row names a new name after the table's last, and the table grows to hold it.
    ("alphaName", "iso", [u32(0x860, 0x40), put(0x8AC, b"installation.json\0".hex()),
                          u32(0x428, 190)], "unsafeName",
     'Unsafe disc path component: "installation.json"'),
    ("rootFile", "iso", [byte(0x80C, 0)], "rootNotFolder", None),
    ("missingCarddemo", "iso", [byte(0x8A1, ord("x"))], "missingRoots", None),
    ("missingTower", "iso", [byte(0x892, ord("X"))], "missingData", None),
    ("missingIcon", "iso", [byte(0x8AA, ord("x"))], "missingData",
     "Disc is missing required native game data"),
    ("trimmedToLastByte", "iso", [truncate(ICON_DATA_AT + 4)], None, None),
    ("trimmedOneShort", "iso", [truncate(ICON_DATA_AT + 3)], "truncatedFile", None),
    ("cisoMap", "ciso", [byte(8, 2)], "cisoMap", None),
    ("cisoTruncated", "ciso", [drop_last(1)], "cisoData", None),
    ("cisoTrailing", "ciso", [append("00")], None, None),
]

# The disc reads: image, offset, length and the launcher's rule (None where it reads them).
DISC_READS = [
    ("ciso", 0x1FF0, 32, None),
    ("iso", 0x4000 - 1, 2, "outsideImage"),
]

# The icon cases: name, base, edits and the launcher's rule. Every refusal is worded as the
# installer words it.
ICON_CASES = [
    ("magic", "rgb5a3", [u32(0, 0)], "directory"),
    ("count", "rgb5a3", [u32(4, 0)], "directory"),
    ("table", "rgb5a3", [u32(8, 0xFFFFFFFF)], "directory"),
    ("header", "rgb5a3", [u32(12, 0xFFFFFFFF)], "header"),
    ("size", "rgb5a3", [u32(20, 0)], "dimensions"),
    ("format", "rgb5a3", [u32(24, 9)], "format"),
    ("palette", "rgb5a3", [u32(16, 32)], "format"),
    ("pixels", "rgb5a3", [u32(28, 0xFFFFFF00)], "pixels"),
    ("empty", "empty", [], "size"),
    ("short", "rgb5a3", [drop_last(1)], "pixels"),
    ("tooLarge", "empty", [append_zeros(1024 * 1024 + 1)], "size"),
]

# The texels the installer's icon tests look at: library, x and y.
ICON_PIXELS = [
    ("rgb5a3", 0, 0), ("rgb5a3", 4, 0), ("rgb5a3", 0, 4), ("rgb5a3", 4, 4),
    ("rgb565", 0, 0), ("rgba8", 0, 0), ("firstFrameOnly", 4, 0),
]


def launcher_detail(name: str, rule: str | None, python: str, message: str | None,
                    text: str | None) -> str | None:
    """The launcher's text for a case: none where it reads the input, else the table's text or,
    where the table gives none, the installer's own refusal."""
    if rule is None:
        if python != "accepted":
            raise ValueError(f"{name}: the launcher reads what the installer refuses")
        if text is not None:
            raise ValueError(f"{name}: the table words a refusal the launcher does not make")
        return None
    if text is not None:
        if text == message:
            raise ValueError(f"{name}: the table repeats the installer's message")
        return text
    if python != "refused":
        raise ValueError(f"{name}: the launcher refuses what the installer reads, in words the "
                         f"table does not give")
    return message


def outcome(run) -> tuple[str, str | None]:
    """What the installer did: "accepted" with no message, or "refused" with its message."""
    try:
        run()
    except ValueError as error:
        return "refused", str(error)
    return "accepted", None


def opened(path: Path, image: bytes) -> DiscImage:
    path.write_bytes(image)
    return DiscImage(path)


def disc_outcome(path: Path, image: bytes) -> tuple[str, str | None]:
    def run():
        with opened(path, image):
            pass
    return outcome(run)


def gamecube_vectors(scratch: Path) -> dict:
    path = scratch / "game.iso"
    iso = bytes(synthetic_iso())
    ciso = bytes(synthetic_ciso(iso))
    icon_disc = iso_with_icon(bytes(tpl_fixture()))
    bases = {"iso": iso, "ciso": ciso, "empty": b""}

    with opened(path, iso) as disc:
        fst_offset, fst_size = struct.unpack_from(">II", disc.read(0, 0x440), 0x424)
        fst = disc.read(fst_offset, fst_size)
        revision = disc.revision
        entries = disc.entries
        contents = {entry.path: disc.read(entry.offset, entry.size)
                    for entry in entries if not entry.directory}
    with opened(path, ciso) as disc:
        block_size = disc.block_size
        if disc.entries != entries or any(disc.read(entry.offset, entry.size) != contents[entry.path]
                                          for entry in entries if not entry.directory):
            raise ValueError("the synthetic CISO does not give the synthetic ISO's disc")
    with opened(path, icon_disc):
        pass

    listed = []
    for entry in entries:
        if entry.directory:
            listed.append({"path": entry.path.as_posix(), "folder": True})
        else:
            listed.append({"path": entry.path.as_posix(), "folder": False, "offset": entry.offset,
                           "size": entry.size, "sha256": sha256(contents[entry.path])})

    reads = []
    for image, offset, size, rule in DISC_READS:
        read = {}

        def run():
            with opened(path, bases[image]) as disc:
                read["bytes"] = disc.read(offset, size)

        python, message = outcome(run)
        name = f"read {size} at {offset} of {image}"
        reads.append({"image": image, "offset": offset, "size": size,
                      "hex": read["bytes"].hex() if "bytes" in read else None,
                      "python": python, "message": message, "rule": rule,
                      "detail": launcher_detail(name, rule, python, message, None)})

    cases = []
    for name, base, edits, rule, text in DISC_CASES:
        python, message = disc_outcome(path, edited(bases[base], edits))
        cases.append({"name": name, "base": base, "edits": edits, "rule": rule, "python": python,
                      "message": message,
                      "detail": launcher_detail(name, rule, python, message, text)})

    return {
        "format": "gdl-gamecube-vectors",
        "version": 1,
        "images": {
            "iso": {"length": len(iso), "sha256": sha256(iso), "fstHex": fst.hex(),
                    "revision": revision},
            "ciso": {"length": len(ciso), "sha256": sha256(ciso), "blockSize": block_size,
                     "usedBlocks": [index for index, flag in enumerate(ciso[8:CISO_HEADER])
                                    if flag]},
            "isoWithIcon": {"length": len(icon_disc), "sha256": sha256(icon_disc)},
        },
        "entries": listed,
        "reads": reads,
        "cases": cases,
    }


def icon_vectors() -> dict:
    built = {"rgb5a3": (5, 8, 8), "rgb565": (4, 8, 8), "rgba8": (6, 8, 8)}
    tpls = {name: bytes(tpl_fixture(encoding, width, height))
            for name, (encoding, width, height) in built.items()}
    tpls["firstFrameOnly"] = first_frame_only_tpl()
    bases = {**tpls, "empty": b""}

    listed = {name: {"encoding": encoding, "width": width, "height": height,
                     "hex": tpls[name].hex(), "sha256": sha256(tpls[name])}
              for name, (encoding, width, height) in built.items()}
    listed["firstFrameOnly"] = {"hex": tpls["firstFrameOnly"].hex()}

    frames = {}
    decoded = {}
    for name, tpl in tpls.items():
        width, height, rgba = decode_first_frame(tpl)
        decoded[name] = (width, height, rgba)
        frames[name] = {"width": width, "height": height, "rgbaHex": rgba.hex(),
                        "sha256": sha256(rgba)}

    pixels = []
    for name, x, y in ICON_PIXELS:
        width, _, rgba = decoded[name]
        at = (y * width + x) * 4
        pixels.append({"tpl": name, "x": x, "y": y, "rgba": list(rgba[at:at + 4])})

    cases = []
    for name, base, edits, rule in ICON_CASES:
        python, message = outcome(lambda: decode_first_frame(edited(bases[base], edits)))
        cases.append({"name": name, "base": base, "edits": edits, "rule": rule, "python": python,
                      "message": message,
                      "detail": launcher_detail(name, rule, python, message, None)})

    resources = {}
    for name, frame in decoded.items():
        group, images = icon_resources(*frame)
        listed_images = []
        for size, image in zip(ICON_SIZES, images):
            row = {"size": size, "length": len(image), "sha256": sha256(image)}
            if size == WHOLE_IMAGE_SIZE:
                row["hex"] = image.hex()
            listed_images.append(row)
        resources[name] = {"groupHex": group.hex(), "images": listed_images,
                           "allSha256": sha256(group + b"".join(images))}

    return {
        "format": "gdl-icon-vectors",
        "version": 1,
        "tpls": listed,
        "frames": frames,
        "pixels": pixels,
        "cases": cases,
        "resources": resources,
    }


def rendered(document) -> bytes:
    """A vector file: indented JSON in UTF-8, ending in a newline, LF on every system."""
    return (json.dumps(document, indent=2) + "\n").encode("utf-8")


def vectors() -> dict[str, bytes]:
    """Every vector file, by name, as the installer's reader and decoder give it today."""
    with tempfile.TemporaryDirectory(prefix="gdl installer vectors ") as scratch:
        gamecube = gamecube_vectors(Path(scratch))
    return {GAMECUBE_VECTORS: rendered(gamecube), ICON_VECTORS: rendered(icon_vectors())}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write the installer's disc and icon vectors "
                                                 "again.")
    parser.add_argument("--write", type=Path, required=True, metavar="FOLDER",
                        help="the folder to write them into: tests/scripts/data/installer")
    args = parser.parse_args(argv)
    try:
        written = vectors()
        args.write.mkdir(parents=True, exist_ok=True)
        for name, data in written.items():
            (args.write / name).write_bytes(data)
            print(args.write / name)
    except (OSError, ValueError) as error:
        print(f"writing the vectors failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
