"""The installer's disc and icon vectors, which the launcher's readers are held to, are what the
installer's disc reader and icon decoder give today."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))
from installer_vectors import VECTOR_FOLDER, vectors


class InstallerVectorTests(unittest.TestCase):
    def test_vectors_are_current(self):
        # A change to the installer's reader, decoder or test builders fails here until the vectors
        # are written again and the launcher follows them.
        self.maxDiff = None
        written = vectors()
        committed = sorted(path.name for path in VECTOR_FOLDER.glob("*.json"))
        self.assertEqual(sorted(written), committed)
        for name, data in written.items():
            with self.subTest(name=name):
                # Latin-1 keeps every byte as one character, so a difference shows as text.
                self.assertEqual(data.decode("latin-1"),
                                 (VECTOR_FOLDER / name).read_bytes().decode("latin-1"))


if __name__ == "__main__":
    unittest.main()
