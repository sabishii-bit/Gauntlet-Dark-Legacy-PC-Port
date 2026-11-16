"""The writer of the Legends vectors records a departure from the extractors exactly where the
launcher decides a case otherwise; no game data."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from legends_fixtures import outcome


# A script's outcome on Windows alone: a refusal there, the case taken on Linux.
REFUSED_ON_WINDOWS = {"windows": "Expected one complete track per BIN file", "linux": None}


class OutcomeTests(unittest.TestCase):
    def test_a_difference_needs_a_deviation(self):
        for python, rule in [(None, "unsafePath"), ("unsafe ROMDISK path", None),
                             (REFUSED_ON_WINDOWS, "trackPerFile")]:
            with self.subTest(python=python, rule=rule):
                with self.assertRaisesRegex(ValueError, "no deviation"):
                    outcome(python, rule)
        self.assertEqual(outcome(None, "unsafePath", "control", 1),
                         {"python": None, "rule": "unsafePath", "nameRule": "control",
                          "deviation": 1})
        # A difference on one system alone is one too.
        self.assertEqual(outcome(REFUSED_ON_WINDOWS, "trackPerFile", deviation=9),
                         {"python": REFUSED_ON_WINDOWS, "rule": "trackPerFile", "deviation": 9})

    def test_a_deviation_needs_a_difference(self):
        refused_everywhere = {"windows": "Expected one complete track per BIN file",
                              "linux": "Expected one complete track per BIN file"}
        for python, rule in [(None, None), ("unsafe ROMDISK path", "unsafePath"),
                             (refused_everywhere, "trackPerFile")]:
            with self.subTest(python=python, rule=rule):
                with self.assertRaisesRegex(ValueError, "agree"):
                    outcome(python, rule, deviation=1)
        self.assertEqual(outcome(None, None), {"python": None, "rule": None})

    def test_a_deviation_is_one_of_those_numbered(self):
        with self.assertRaisesRegex(ValueError, "not one of DEVIATIONS"):
            outcome(None, "unsafePath", deviation=0)


if __name__ == "__main__":
    unittest.main()
