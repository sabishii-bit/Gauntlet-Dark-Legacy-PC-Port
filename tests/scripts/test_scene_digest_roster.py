"""Pin the play scene's digest samples, which every netplay peer must hash alike."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCENES = ROOT / "src/game/scenes"

# appendSceneDigest's samples in order; nativeAgreementDigest takes "resources" before them.
# The scene measures the first two and the last itself and walks its owners' roster between.
SAMPLES = [
    "control", "players", "world", "fixtures", "console-objects", "opponents", "attacks",
    "arsenal", "effects", "portals", "transporters", "departure", "arrival", "sumner", "hud",
    "messages", "sumner-visit", "promotion", "relics", "boss-sequence", "audio",
    "challenge-hud", "mechanics", "camera-speech",
]
MEASURED = 3
OWNERS = 21
SCHEMA = "PlayScene.native.10"
HOW = ("A digest sample change alters every peer's stream: bump the schema string, and update "
       "NativeAgreementSceneTests' ordered owners and this list in the same change.")


def function_body(text, signature):
    start = text.find(signature)
    if start < 0:
        return None
    depth = 0
    for i in range(text.find("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


class SceneDigestRosterTests(unittest.TestCase):
    def test_samples_keep_their_names_and_order(self):
        text = (SCENES / "PlaySceneRewind.cpp").read_text(encoding="utf-8")
        body = function_body(text, "bool PlayScene::appendSceneDigest(")
        self.assertIsNotNone(body, "Missing PlayScene::appendSceneDigest")
        roster = function_body(text, "PlayScene::sceneOwners(")
        self.assertIsNotNone(roster, "Missing PlayScene::sceneOwners")
        # clang-format may break a call after its parenthesis.
        measured = [(m.start(), m.group(1))
                    for m in re.finditer(r'\bmeasured\(\s*"([a-z0-9-]+)"', body)]
        owners = re.findall(r'\bsample\(\s*"([a-z0-9-]+)"', roster)
        self.assertEqual(len(measured), MEASURED, HOW)
        self.assertEqual(len(owners), OWNERS, HOW)
        names = [name for _, name in measured]
        self.assertEqual(names[:2] + owners + names[2:], SAMPLES, HOW)
        walk = body.find("eachSample(")
        self.assertTrue(measured[1][0] < walk < measured[2][0],
                        "The owners' roster must be hashed between the players and the camera")
        self.assertEqual(re.findall(r'digest\.string\("(PlayScene\.native\.\d+)"\)', body),
                         [SCHEMA], HOW)

    def test_resources_are_sampled_before_the_scene(self):
        text = (SCENES / "PlaySceneSimulation.cpp").read_text(encoding="utf-8")
        body = function_body(text, "PlayScene::nativeAgreementDigest(")
        self.assertIsNotNone(body, "Missing PlayScene::nativeAgreementDigest")
        resources = body.find('{"resources"')
        self.assertGreaterEqual(resources, 0, HOW)
        self.assertLess(resources, body.find("appendSceneDigest("), HOW)


if __name__ == "__main__":
    unittest.main()
