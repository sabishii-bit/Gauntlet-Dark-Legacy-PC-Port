"""The Dreamcast fingerprints are written from an extraction's disc manifest and IP.BIN alone, as
hashes only; synthetic trees, no game data."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts" / "assets"
sys.path.insert(0, str(SCRIPTS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import legends_fingerprints
from legends_fixtures import FULL_FINGERPRINTS, VECTOR_FOLDER, full_set_tree


# The key of IP.BIN's fingerprint: the SHA-256 of the tree path "IP.BIN".
IP_BIN_KEY = "e60b7f339972edaa3a7a242795ff7dbf098d6c5f565b1e4c4ceed4469aa67814"

# What the output holds before its fingerprints, in its order.
HEADER = {"format": "gdl-legends-fingerprints", "version": 1, "expansion": "dreamcast",
          "product": "T9710N", "productVersion": "V1.003", "area": "U"}

# The fingerprints the script writes for the full set's tree, as committed beside the other
# Dreamcast vectors.
GOLDEN = (VECTOR_FOLDER / FULL_FINGERPRINTS).read_bytes()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def set_fields(**fields):
    """A change to a disc manifest that sets its fields."""
    return lambda manifest: manifest.update(fields)


def set_first_file(**fields):
    """A change to a disc manifest that sets fields of its first file's row, removing those given
    as None."""
    def change(manifest: dict) -> None:
        row = manifest["files"][0]
        for key, value in fields.items():
            if value is None:
                del row[key]
            else:
                row[key] = value
    return change


def add_file(row):
    """A change to a disc manifest that lists one more file, `row`."""
    return lambda manifest: manifest["files"].append(row)


def repeat_first_file(manifest: dict) -> None:
    """Lists a disc manifest's first file a second time."""
    manifest["files"].append(dict(manifest["files"][0]))


# Disc manifests no extraction writes, by what is wrong with each.
MANIFEST_CHANGES = [
    ("another format", set_fields(format="dreamcast-legends-disc-v2")),
    ("no list of files", set_fields(files={})),
    ("a file that is not an object", add_file([])),
    ("no path", set_first_file(path=None)),
    ("an absolute path", set_first_file(path="/DISK.ROM")),
    ("a path out of the disc", set_first_file(path="../IP.BIN")),
    ("a path through its own folder", set_first_file(path="AUDIO/./X.STR")),
    ("an empty component", set_first_file(path="AUDIO//X.STR")),
    ("a separator at the end", set_first_file(path="AUDIO/")),
    ("an empty path", set_first_file(path="")),
    ("a backslash", set_first_file(path="AUDIO\\X.STR")),
    ("a path outside ASCII", set_first_file(path="Xé.STR")),
    ("no size", set_first_file(size=None)),
    ("a negative size", set_first_file(size=-1)),
    ("a size in text", set_first_file(size="3000")),
    ("a size that is true", set_first_file(size=True)),
    ("no digest", set_first_file(sha256=None)),
    ("an uppercase digest", set_first_file(sha256="A" * 64)),
    ("a digest a digit short", set_first_file(sha256="a" * 63)),
    ("a digest of other letters", set_first_file(sha256="g" * 64)),
    ("a path listed twice", repeat_first_file),
]


def write_over_ip_bin(offset: int, data: bytes):
    """A change to a tree that writes `data` over its IP.BIN at `offset`."""
    def change(tree: Path) -> None:
        path = tree / "IP.BIN"
        boot = bytearray(path.read_bytes())
        boot[offset:offset + len(data)] = data
        path.write_bytes(boot)
    return change


def remove(name: str):
    """A change to a tree that removes its file `name`."""
    return lambda tree: (tree / name).unlink()


def replace_with(name: str, data: bytes):
    """A change to a tree that writes `data` in place of its file `name`."""
    return lambda tree: (tree / name).write_bytes(data)


def cut_ip_bin(tree: Path) -> None:
    """Cuts a tree's IP.BIN a byte short."""
    path = tree / "IP.BIN"
    path.write_bytes(path.read_bytes()[:-1])


def grow_ip_bin(tree: Path) -> None:
    """Makes a tree's IP.BIN a byte longer."""
    with (tree / "IP.BIN").open("ab") as stream:
        stream.write(b"\0")


# Trees no extraction writes, by what is wrong with each, with the file the refusal names.
TREE_CHANGES = [
    ("no disc manifest", "disc-manifest.json", remove("disc-manifest.json")),
    ("a disc manifest that is not JSON", "disc-manifest.json",
     replace_with("disc-manifest.json", b"{\n")),
    ("no IP.BIN", "IP.BIN", remove("IP.BIN")),
    ("an IP.BIN a byte short", "IP.BIN", cut_ip_bin),
    ("an IP.BIN a byte long", "IP.BIN", grow_ip_bin),
    ("another hardware ID", "IP.BIN", write_over_ip_bin(0, b"SEGA SEGAKATANB")),
]


class FingerprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.tree = full_set_tree(self.root)
        # In a folder of its own, which the first write makes.
        self.output = self.root / "fingerprints" / "dreamcast.json"

    def run_script(self, *arguments: str, tree: Path | None = None,
                   output: Path | None = None) -> tuple[int, str, str]:
        """Runs the script on a tree and an output, the test's own unless given, with `arguments`
        after them: its exit code, and what it printed to stdout and to stderr."""
        printed, errors = io.StringIO(), io.StringIO()
        arguments = ["--tree", str(tree or self.tree), "--output", str(output or self.output),
                     *arguments]
        with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(errors):
            code = legends_fingerprints.main(arguments)
        return code, printed.getvalue(), errors.getvalue()

    def assert_refused(self, result: tuple[int, str, str], *named: str) -> None:
        """Checks that a run refused, naming what it is given, and wrote nothing."""
        code, printed, errors = result
        self.assertEqual((code, printed), (1, ""))
        self.assertTrue(errors.startswith("fingerprints refused: "), errors)
        for text in named:
            self.assertIn(text, errors)
        self.assertFalse(self.output.parent.exists())

    def test_writes_ip_bin_and_the_disc_rows_of_a_tree(self):
        self.assertEqual(self.run_script(), (0, f"Wrote 5 fingerprints to {self.output}\n", ""))
        document = json.loads(self.output.read_text(encoding="ascii"))
        self.assertEqual(list(document), [*HEADER, "files"])
        self.assertEqual({key: document[key] for key in HEADER}, HEADER)
        # IP.BIN, and every disc file the manifest lists, each hashed as it lies in the tree.
        manifest = json.loads((self.tree / "disc-manifest.json").read_text(encoding="utf-8"))
        paths = ["IP.BIN", *(f"disc/{row['path']}" for row in manifest["files"])]
        self.assertEqual(len(paths), 5)
        expected = sorted((sha256(path.encode("ascii")), sha256((self.tree / path).read_bytes()))
                          for path in paths)
        rows = document["files"]
        self.assertEqual([list(row) for row in rows], [["pathSha256", "sha256"]] * len(rows))
        self.assertEqual(sorted((row["pathSha256"], row["sha256"]) for row in rows), expected)
        keys = [row["pathSha256"] for row in rows]
        self.assertEqual(keys, sorted(keys))
        ip_bin = sha256((self.tree / "IP.BIN").read_bytes())
        self.assertIn({"pathSha256": IP_BIN_KEY, "sha256": ip_bin}, rows)
        self.assertEqual(self.output.read_bytes(), GOLDEN)

    def test_writes_hashes_only(self):
        self.assertEqual(self.run_script()[0], 0)
        text = self.output.read_text(encoding="ascii")
        paths = [path.relative_to(self.tree).as_posix() for path in self.tree.rglob("*")]
        self.assertIn("disc/AUDIO/X.STR", paths)
        for name in {*paths, *(part for path in paths for part in path.split("/"))}:
            with self.subTest(name=name):
                self.assertNotIn(name, text)
        self.assertNotIn('"size"', text)
        rows = json.loads(text)["files"]
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(set(row), {"pathSha256", "sha256"})
            for digest in row.values():
                self.assertRegex(digest, "^[0-9a-f]{64}$")

    def test_reads_no_disc_file(self):
        shutil.rmtree(self.tree / "disc")
        self.assertEqual(self.run_script()[0], 0)
        self.assertEqual(self.output.read_bytes(), GOLDEN)

    def test_refuses_another_product_version_or_area(self):
        for field, offset, value in [("product", 64, b"T9710P    "), ("version", 74, b"V1.002"),
                                     ("area", 48, b" E      ")]:
            with self.subTest(field=field):
                tree = full_set_tree(self.root)
                write_over_ip_bin(offset, value)(tree)
                self.assert_refused(self.run_script(tree=tree), str(tree / "IP.BIN"),
                                    f"another {field} than")
        with self.subTest(field="the manifest's product"):
            tree = full_set_tree(self.root)
            path = tree / "disc-manifest.json"
            manifest = json.loads(path.read_text(encoding="utf-8"))
            manifest["product"] = "T9710P"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            self.assert_refused(self.run_script(tree=tree), str(path),
                                "another product than T9710N")

    def test_refuses_a_tree_no_extraction_writes(self):
        for name, change in MANIFEST_CHANGES:
            with self.subTest(manifest=name):
                tree = full_set_tree(self.root)
                path = tree / "disc-manifest.json"
                manifest = json.loads(path.read_text(encoding="utf-8"))
                change(manifest)
                path.write_text(json.dumps(manifest), encoding="utf-8")
                self.assert_refused(self.run_script(tree=tree), str(path))
        for name, named, change in TREE_CHANGES:
            with self.subTest(tree=name):
                tree = full_set_tree(self.root)
                change(tree)
                self.assert_refused(self.run_script(tree=tree), str(tree / named))
        with self.subTest(tree="a disc manifest past the bound"):
            size = (self.tree / "disc-manifest.json").stat().st_size
            with mock.patch.object(legends_fingerprints, "MAX_MANIFEST_BYTES", size - 1):
                self.assert_refused(self.run_script(), f"is over {size - 1} bytes")
            # One of exactly the bound is read.
            with mock.patch.object(legends_fingerprints, "MAX_MANIFEST_BYTES", size):
                self.assertEqual(self.run_script()[0], 0)
            self.assertEqual(self.output.read_bytes(), GOLDEN)

    def test_refuses_an_existing_file_unless_replace(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b"older\n")
        self.assertEqual(self.run_script(),
                         (1, "", f"fingerprints exist already: {self.output}; pass --replace\n"))
        self.assertEqual(self.output.read_bytes(), b"older\n")
        self.assertEqual(self.run_script("--replace"),
                         (0, f"Wrote 5 fingerprints to {self.output}\n", ""))
        self.assertEqual(self.output.read_bytes(), GOLDEN)
        # Nothing is left of the file the new fingerprints were written into first.
        self.assertEqual(list(self.output.parent.iterdir()), [self.output])
        self.assertEqual(self.run_script("--replace")[0], 0)
        self.assertEqual(self.output.read_bytes(), GOLDEN)

    def test_check_reports_equal_and_different(self):
        differ = (1, "", f"fingerprints differ: {self.output}\n")
        self.assertEqual(self.run_script("--check"), differ)
        # A check writes nothing, not even the output's folder.
        self.assertFalse(self.output.parent.exists())
        self.assertEqual(self.run_script()[0], 0)
        self.assertEqual(self.run_script("--check"),
                         (0, f"The 5 fingerprints in {self.output} are current\n", ""))
        changed = bytearray(GOLDEN)
        changed[GOLDEN.index(IP_BIN_KEY.encode("ascii"))] ^= 1
        for name, data in [("a changed byte", bytes(changed)), ("a byte more", GOLDEN + b"\n"),
                           ("a byte less", GOLDEN[:-1]), ("nothing", b"")]:
            with self.subTest(output=name):
                self.output.write_bytes(data)
                self.assertEqual(self.run_script("--check"), differ)
                self.assertEqual(self.output.read_bytes(), data)
        with self.subTest(output="under a file"):
            output = self.output / "dreamcast.json"
            self.assertEqual(self.run_script("--check", output=output),
                             (1, "", f"fingerprints differ: {output}\n"))

    def test_makes_the_output_folder(self):
        output = self.root / "a" / "b" / "fingerprints.json"
        self.assertEqual(self.run_script(output=output),
                         (0, f"Wrote 5 fingerprints to {output}\n", ""))
        self.assertEqual(output.read_bytes(), GOLDEN)

    def test_a_failed_write_is_a_refusal(self):
        folder = self.root / "taken"
        folder.write_bytes(b"")
        output = folder / "fingerprints.json"
        for arguments in [(), ("--replace",)]:
            with self.subTest(arguments=arguments):
                code, printed, errors = self.run_script(*arguments, output=output)
                self.assertEqual((code, printed), (1, ""))
                self.assertTrue(errors.startswith(f"fingerprints refused: {folder}: "), errors)
                self.assertEqual(folder.read_bytes(), b"")

    def test_output_is_lf_ascii_json_ending_in_a_newline(self):
        self.assertEqual(self.run_script()[0], 0)
        data = self.output.read_bytes()
        self.assertTrue(data.isascii())
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.endswith(b"}\n"))
        self.assertFalse(data.endswith(b"\n\n"))
        self.assertEqual(data, (json.dumps(json.loads(data), indent=2) + "\n").encode("ascii"))


if __name__ == "__main__":
    unittest.main()
