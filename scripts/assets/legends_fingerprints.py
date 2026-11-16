#!/usr/bin/env python3
"""Write the fingerprints the launcher holds a Dreamcast disc to, from a trusted extraction.

    python scripts/assets/legends_fingerprints.py [--tree <extracted>] [--output <file>]
                                                  [--check | --replace]

The tree is what dreamcast_legends.py extracted from the player's own Gauntlet Legends disc, by
default assets/dreamcast/extracted. A fingerprint is the SHA-256 of a tree path beside the SHA-256
of the file at it: IP.BIN, which is hashed here, and each disc file disc-manifest.json lists, whose
digest the manifest gives, so no disc file is read. The output, by default the launcher's copy in
src/launcher/crates/gdl-extract/fingerprints, holds those digests and the identity of the disc
they are for: no name, no size and no other byte of the disc. --check compares them with the
output instead of writing it, and --replace writes over an output that is there already.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TREE = ROOT / "assets" / "dreamcast" / "extracted"
DEFAULT_OUTPUT = (ROOT / "src" / "launcher" / "crates" / "gdl-extract" / "fingerprints"
                  / "dreamcast-us.json")

# What the output is: its format, the version of that format, and the expansion whose disc it
# fingerprints.
FORMAT = "gdl-legends-fingerprints"
FORMAT_VERSION = 1
EXPANSION = "dreamcast"

# The manifest the Dreamcast extractor writes for the disc, its format, and the most of it read.
DISC_MANIFEST = "disc-manifest.json"
DISC_MANIFEST_FORMAT = "dreamcast-legends-disc-v1"
MAX_MANIFEST_BYTES = 4 * 1024 * 1024

# The folder of the tree that the manifest's paths lie in.
DISC_FOLDER = "disc"

# The disc's boot area, which the extractor writes whole beside the disc's files: sixteen sectors
# of 2,048 bytes, the first beginning with the hardware ID.
IP_BIN = "IP.BIN"
IP_BIN_SIZE = 16 * 2048
HARDWARE_ID = (0, b"SEGA SEGAKATANA ")

# The disc the fingerprints are for, by its fields in IP.BIN: the output's key for each, what the
# field is, and its offset and bytes there. The output gives each without its padding.
IDENTITY = [("product", "product", 64, b"T9710N    "),
            ("productVersion", "version", 74, b"V1.003"),
            ("area", "area", 48, b" U      ")]

SHA256_HEX = re.compile("[0-9a-f]{64}")


class Refused(Exception):
    """A tree the fingerprints cannot be taken from, or an output they cannot be written to."""


def io_refusal(path: Path, error: OSError) -> Refused:
    """The refusal of a failure to read or write `path`, which it names."""
    return Refused(f"{path}: {error.strerror or error}")


def read_bounded(path: Path, most: int) -> bytes:
    """A file's bytes, up to one past `most`, so that a longer file shows without being read
    whole."""
    try:
        with path.open("rb") as stream:
            return stream.read(most + 1)
    except OSError as error:
        raise io_refusal(path, error) from error


def identity() -> dict[str, str]:
    """The identity of the disc the fingerprints are for, by the output's keys."""
    return {key: value.decode("ascii").strip() for key, _, _, value in IDENTITY}


def is_relative_ascii(path) -> bool:
    """Whether `path` is a relative path in ASCII whose components "/" joins: none empty, "." or
    "..", and no backslash in it."""
    return (isinstance(path, str) and path.isascii() and "\\" not in path
            and all(part not in ("", ".", "..") for part in path.split("/")))


def disc_rows(tree: Path) -> list[tuple[str, str]]:
    """The tree path and SHA-256 of each disc file the tree's disc manifest lists. Each row's name
    and size are checked, and neither is kept."""
    path = tree / DISC_MANIFEST
    data = read_bounded(path, MAX_MANIFEST_BYTES)
    if len(data) > MAX_MANIFEST_BYTES:
        raise Refused(f"{path} is over {MAX_MANIFEST_BYTES} bytes")
    try:
        manifest = json.loads(data.decode("utf-8"))
    except ValueError as error:
        raise Refused(f"{path} is not JSON: {error}") from error
    except RecursionError as error:
        # JSON nested deeper than Python's reader descends, which no extraction writes.
        raise Refused(f"{path} nests its JSON too deep to read") from error
    if not isinstance(manifest, dict) or manifest.get("format") != DISC_MANIFEST_FORMAT:
        raise Refused(f"{path} is not a disc manifest of the format {DISC_MANIFEST_FORMAT}")
    product = identity()["product"]
    if manifest.get("product") != product:
        raise Refused(f"{path} is the manifest of another product than {product}")
    files = manifest.get("files")
    if not isinstance(files, list):
        raise Refused(f"{path} has no list of files")
    rows = []
    listed = set()
    for number, row in enumerate(files):
        where = f"{path}, file {number}"
        if not isinstance(row, dict):
            raise Refused(f"{where} is not an object")
        name, size, digest = row.get("path"), row.get("size"), row.get("sha256")
        if not is_relative_ascii(name):
            raise Refused(f"{where} has no relative ASCII path")
        if isinstance(size, bool) or not isinstance(size, int) or size < 0:
            raise Refused(f"{where} has no size of 0 or more")
        if not isinstance(digest, str) or not SHA256_HEX.fullmatch(digest):
            raise Refused(f"{where} has no SHA-256 of 64 lowercase hex digits")
        # Two fingerprints under one key would leave the launcher unable to tell which holds.
        if name in listed:
            raise Refused(f"{where} has a path an earlier file has")
        listed.add(name)
        rows.append((f"{DISC_FOLDER}/{name}", digest))
    return rows


def ip_bin_digest(tree: Path) -> str:
    """The SHA-256 of the tree's IP.BIN, once it is the whole boot area of the disc the
    fingerprints are for."""
    path = tree / IP_BIN
    data = read_bounded(path, IP_BIN_SIZE)
    if len(data) != IP_BIN_SIZE:
        raise Refused(f"{path} is not {IP_BIN_SIZE} bytes long")
    offset, hardware = HARDWARE_ID
    if data[offset:offset + len(hardware)] != hardware:
        raise Refused(f"{path} is not a Dreamcast boot area")
    for _, field, offset, value in IDENTITY:
        if data[offset:offset + len(value)] != value:
            raise Refused(f"{path} is the boot area of another {field} than "
                          f"{value.decode('ascii').strip()}")
    return hashlib.sha256(data).hexdigest()


def path_digest(path: str) -> str:
    """The key a file's fingerprint is found by: the SHA-256 of its tree path."""
    return hashlib.sha256(path.encode("ascii")).hexdigest()


def fingerprints_of(tree: Path) -> dict:
    """The output for a tree: its format, the disc it is for, and a fingerprint for IP.BIN and for
    each disc file, in the order of their keys."""
    rows = disc_rows(tree)
    rows.append((IP_BIN, ip_bin_digest(tree)))
    files = sorted(({"pathSha256": path_digest(path), "sha256": digest} for path, digest in rows),
                   key=lambda row: row["pathSha256"])
    return {"format": FORMAT, "version": FORMAT_VERSION, "expansion": EXPANSION, **identity(),
            "files": files}


def rendered(document: dict) -> bytes:
    """The output's bytes: indented JSON in ASCII, ending in a newline, LF on every system."""
    return (json.dumps(document, indent=2) + "\n").encode("ascii")


def matches(output: Path, data: bytes) -> bool:
    """Whether the output holds exactly `data`; an output that is not there does not."""
    try:
        with output.open("rb") as stream:
            return stream.read(len(data) + 1) == data
    except (FileNotFoundError, NotADirectoryError):
        return False
    except OSError as error:
        raise io_refusal(output, error) from error


def make_folder(folder: Path) -> None:
    """Makes the output's folder, and its parents, where they are not there yet."""
    try:
        folder.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        raise io_refusal(folder, error) from error


def create(output: Path, data: bytes) -> bool:
    """Writes `data` into a new output, or returns False where an output is there already. A write
    that fails leaves no output."""
    try:
        stream = output.open("xb")
    except FileExistsError:
        return False
    except OSError as error:
        raise io_refusal(output, error) from error
    try:
        with stream:
            stream.write(data)
    except OSError as error:
        with contextlib.suppress(OSError):
            output.unlink()
        raise io_refusal(output, error) from error
    return True


def replace(output: Path, data: bytes) -> None:
    """Writes `data` into a new file beside the output, which then takes the output's place, so
    that the output is never seen half written."""
    try:
        handle, name = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    except OSError as error:
        raise io_refusal(output.parent, error) from error
    temporary = Path(name)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.replace(temporary, output)
    except OSError as error:
        with contextlib.suppress(OSError):
            temporary.unlink()
        raise io_refusal(output, error) from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--tree", type=Path, default=DEFAULT_TREE,
                        help="what dreamcast_legends.py extracted from a trusted disc "
                             "(default: assets/dreamcast/extracted)")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                        help="the fingerprint file (default: the launcher's copy)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="compare the fingerprints with the output instead of writing it")
    mode.add_argument("--replace", action="store_true",
                      help="write over an output that is there already")
    args = parser.parse_args(argv)
    try:
        document = fingerprints_of(args.tree)
        data = rendered(document)
        count = len(document["files"])
        if args.check:
            if not matches(args.output, data):
                print(f"fingerprints differ: {args.output}", file=sys.stderr)
                return 1
            print(f"The {count} fingerprints in {args.output} are current")
            return 0
        make_folder(args.output.parent)
        if args.replace:
            replace(args.output, data)
        elif not create(args.output, data):
            print(f"fingerprints exist already: {args.output}; pass --replace", file=sys.stderr)
            return 1
    except Refused as error:
        print(f"fingerprints refused: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {count} fingerprints to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
