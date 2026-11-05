"""Keep test-only code out of the production sources: no include of it, and no friend of it."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]

# A friend in src that names a class or struct, with or without its class key.
FRIEND = re.compile(r"\bfriend\s+(?:(?:class|struct|union)\s+)?(?:::)?(?:\w+::)*(\w+)\s*;")
# The head of a class, struct or union definition: its name, then a base list or a body.
DEFINITION = re.compile(r"\b(?:class|struct|union)\s+(\w+)(?:\s+final)?\s*(?:\{|:(?!:))")
# Test classes that src may still befriend, each until its owner gives the tests a public seam.
ALLOWED_TEST_FRIENDS = {"PlayerAttacksTestPeer"}


def sources_under(folder):
    return [path for path in (ROOT / folder).rglob("*") if path.suffix in (".cpp", ".h")]


def classes_defined_under(folder):
    names = set()
    for source in sources_under(folder):
        names.update(DEFINITION.findall(source.read_text(encoding="utf-8")))
    return names


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

    def test_production_friends_are_not_test_only_classes(self):
        production = classes_defined_under("src")
        tests = classes_defined_under("tests")
        self.assertTrue(production, "Missing production class definitions")
        violations = []
        allowed = set()
        for source in sources_under("src"):
            for name in FRIEND.findall(source.read_text(encoding="utf-8")):
                if name in production or name not in tests:
                    continue
                if name in ALLOWED_TEST_FRIENDS:
                    allowed.add(name)
                else:
                    violations.append(f"{source.relative_to(ROOT)} befriends test-only {name}")
        self.assertEqual(violations, [])
        self.assertEqual(allowed, ALLOWED_TEST_FRIENDS,
                         "An allowed test friend is no longer befriended; drop it from the list")


if __name__ == "__main__":
    unittest.main()
