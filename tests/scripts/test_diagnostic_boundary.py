"""Keep test-only state capture and wire codecs outside production targets."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class DiagnosticBoundaryTests(unittest.TestCase):
    def test_production_includes_do_not_depend_on_test_only_headers(self):
        sources = [
            path for path in (ROOT / "src").rglob("*")
            if path.suffix in (".cpp", ".h")
        ]
        self.assertTrue(sources, "Missing production source tree")
        violations = []
        for source in sources:
            includes = re.findall(
                r'^\s*#include\s+"((?:game|engine|formats)/[^"]+)"',
                source.read_text(encoding="utf-8"), re.MULTILINE,
            )
            for include in includes:
                if not (ROOT / "src" / include).is_file() and (ROOT / "tests" / include).is_file():
                    violations.append(f"{source.relative_to(ROOT)} includes test-only {include}")
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
