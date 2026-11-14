"""The helper scripts and their tests stay runnable on Python 3.9, the floor setup.py accepts."""
import ast
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FLOOR = (3, 9)


def sources():
    for folder in (ROOT / "scripts", ROOT / "tests" / "scripts"):
        for path in sorted(folder.rglob("*.py")):
            if "__pycache__" not in path.parts:
                yield path


def postpones_annotations(tree: ast.Module) -> bool:
    """Whether the module has `from __future__ import annotations`, which leaves every annotation
    unevaluated."""
    return any(isinstance(node, ast.ImportFrom) and node.module == "__future__"
               and any(alias.name == "annotations" for alias in node.names)
               for node in tree.body)


def evaluated_annotations(tree: ast.Module):
    """The annotations Python evaluates as a module runs: every signature's, and those of variables
    at module or class level. A variable's annotation inside a function body is never evaluated."""
    def walk(node, in_function):
        for child in ast.iter_child_nodes(node):
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                arguments = child.args
                for argument in (*arguments.posonlyargs, *arguments.args, *arguments.kwonlyargs,
                                 arguments.vararg, arguments.kwarg):
                    if argument is not None and argument.annotation is not None:
                        yield argument.annotation
                if child.returns is not None:
                    yield child.returns
                yield from walk(child, True)
            elif isinstance(child, ast.AnnAssign) and not in_function:
                yield child.annotation
            elif isinstance(child, ast.ClassDef):
                yield from walk(child, False)
            else:
                yield from walk(child, in_function)
    yield from walk(tree, False)


class PythonFloorTests(unittest.TestCase):
    def test_no_syntax_newer_than_the_floor(self):
        for path in sources():
            with self.subTest(path=path.relative_to(ROOT).as_posix()):
                ast.parse(path.read_text(encoding="utf-8"), feature_version=FLOOR)

    def test_no_union_operator_in_an_annotation_the_floor_evaluates(self):
        # On 3.9 `int | None` in a signature raises TypeError while the module loads, unless the
        # module postpones its annotations.
        for path in sources():
            tree = ast.parse(path.read_text(encoding="utf-8"))
            if postpones_annotations(tree):
                continue
            for annotation in evaluated_annotations(tree):
                unions = [node for node in ast.walk(annotation)
                          if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr)]
                with self.subTest(path=path.relative_to(ROOT).as_posix(), line=annotation.lineno):
                    self.assertEqual(unions, [])


if __name__ == "__main__":
    unittest.main()
