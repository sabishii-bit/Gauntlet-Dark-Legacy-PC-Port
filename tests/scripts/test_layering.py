"""The layering check finds every include that climbs the module order, and only those."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts" / "checks"))
import layering


def tree(root: pathlib.Path, files: dict[str, str]) -> None:
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


def run(*argv: str) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = layering.main(list(argv))
    return code, out.getvalue(), err.getvalue()


class RuleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def found(self, files: dict[str, str]) -> set[str]:
        tree(self.root, files)
        return layering.scan(self.root)

    def test_engine_includes_only_its_own_lower_modules(self):
        self.assertEqual(self.found({
            "src/engine/io/File.cpp": '#include "engine/core/Types.h"\n#include "engine/platform/Window.h"\n',
            "src/engine/render/Mesh.h": '#include "engine/render/Vertex.h"\n#include "formats/WorldFile.h"\n',
        }), {"src/engine/io/File.cpp -> engine/platform"})

    def test_neither_engine_nor_readers_include_the_game_or_an_expansion(self):
        self.assertEqual(self.found({
            "src/engine/app/Application.cpp": '#include "game/app/Gauntlet.h"\n',
            "src/formats/WorldFile.cpp": '#include "expansions/n64/formats/Map.h"\n',
        }), {"src/engine/app/Application.cpp -> game/app",
             "src/formats/WorldFile.cpp -> expansions/n64"})

    def test_readers_reach_only_the_engine_value_layers(self):
        self.assertEqual(self.found({
            "src/formats/SoundBank.cpp": ('#include "engine/core/Types.h"\n'
                                          '#include "engine/io/ByteReader.h"\n'
                                          '#include "engine/audio/AudioDevice.h"\n'),
        }), {"src/formats/SoundBank.cpp -> engine/audio"})

    def test_game_modules_include_only_lower_ones_and_never_an_expansion(self):
        self.assertEqual(self.found({
            "src/game/level/Chests.cpp": ('#include "engine/world/WorldScene.h"\n'
                                          '#include "game/level/LevelWorld.h"\n'
                                          '#include "game/characters/Inventory.h"\n'
                                          '#include "game/party/PlayerRuntime.h"\n'),
            "src/game/app/Gauntlet.cpp": ('#include "game/scenes/PlayScene.h"\n'
                                          '#include "expansions/shared/scenes/LegendsLevel.h"\n'),
        }), {"src/game/level/Chests.cpp -> game/party",
             "src/game/app/Gauntlet.cpp -> expansions/shared"})

    def test_a_mod_builds_on_the_engine_and_the_game_alone(self):
        self.assertEqual(self.found({
            "src/mods/rage/RageMode.cpp": ('#include "mods/rage/RageMode.h"\n'
                                           '#include "engine/render/RenderDevice.h"\n'
                                           '#include "game/scenes/PlayScene.h"\n'
                                           '#include "mods/calm/CalmMode.h"\n'
                                           '#include "expansions/n64/level/Map.h"\n'),
            "src/game/scenes/PlayScene.cpp": '#include "mods/rage/RageMode.h"\n',
            "src/engine/app/Application.cpp": '#include "mods/rage/RageMode.h"\n',
        }), {"src/mods/rage/RageMode.cpp -> mods/calm",
             "src/mods/rage/RageMode.cpp -> expansions/n64",
             "src/game/scenes/PlayScene.cpp -> mods/rage",
             "src/engine/app/Application.cpp -> mods/rage"})

    def test_expansions_top_level_files_and_unknown_modules_are_not_judged(self):
        self.assertEqual(self.found({
            "src/expansions/n64/level/Map.cpp": '#include "game/app/Gauntlet.h"\n',
            "src/game/main.cpp": '#include "game/app/Gauntlet.h"\n',
            "src/game/sandbox/Try.cpp": '#include "game/app/Gauntlet.h"\n',
        }), set())

    def test_commented_and_angled_includes_do_not_count(self):
        self.assertEqual(self.found({
            "src/game/config/GameConfig.cpp": ('// #include "game/app/Gauntlet.h"\n'
                                               '#include <game/app/Gauntlet.h>\n'),
        }), set())


class BaselineTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.baseline = self.root / "baseline.txt"
        tree(self.root, {
            "src/game/level/Chests.cpp": '#include "game/party/PlayerRuntime.h"\n',
            "src/game/hud/PartyHud.cpp": '#include "game/menu/ScrollBox.h"\n',
        })

    def tearDown(self):
        self.directory.cleanup()

    def check(self, *extra: str) -> tuple[int, str, str]:
        return run("--root", str(self.root), "--baseline", str(self.baseline), *extra)

    def test_an_unlisted_break_fails(self):
        self.baseline.write_text("src/game/level/Chests.cpp -> game/party\n", encoding="utf-8")
        code, _, err = self.check()
        self.assertEqual(code, 1)
        self.assertIn("new break src/game/hud/PartyHud.cpp -> game/menu", err)

    def test_listed_breaks_pass_and_comments_are_ignored(self):
        self.baseline.write_text("# header\n\nsrc/game/level/Chests.cpp -> game/party  # why\n"
                                 "src/game/hud/PartyHud.cpp -> game/menu\n", encoding="utf-8")
        self.assertEqual(self.check()[0], 0)

    def test_a_fixed_break_is_reported_and_tighten_drops_only_it(self):
        self.baseline.write_text("src/game/level/Chests.cpp -> game/party\n"
                                 "src/game/hud/PartyHud.cpp -> game/menu\n"
                                 "src/game/level/Traps.cpp -> game/party\n", encoding="utf-8")
        code, out, _ = self.check()
        self.assertEqual(code, 0)
        self.assertIn("drop from the baseline: src/game/level/Traps.cpp -> game/party", out)
        self.assertEqual(self.check("--tighten")[0], 0)
        self.assertEqual(layering.read_baseline(self.baseline), {
            "src/game/level/Chests.cpp -> game/party", "src/game/hud/PartyHud.cpp -> game/menu"})

    def test_tighten_never_adds_a_new_break(self):
        self.baseline.write_text("src/game/level/Chests.cpp -> game/party\n"
                                 "src/game/level/Traps.cpp -> game/party\n", encoding="utf-8")
        self.assertEqual(self.check("--tighten")[0], 1)
        self.assertEqual(layering.read_baseline(self.baseline),
                         {"src/game/level/Chests.cpp -> game/party"})


class RepositoryTests(unittest.TestCase):
    def test_the_tree_adds_no_break_to_the_baseline(self):
        code, _, err = run("--root", str(REPO))
        self.assertEqual(code, 0, err)


if __name__ == "__main__":
    unittest.main()
