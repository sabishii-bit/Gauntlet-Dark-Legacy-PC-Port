"""Pin the play scene's digest samples, which every netplay peer must hash alike."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCENES = ROOT / "src/game/scenes"

# appendSceneDigest's samples in order; nativeAgreementDigest takes "resources" before them.
SAMPLES = [
    "control", "players", "world", "fixtures", "console-objects", "opponents", "attacks",
    "arsenal", "effects", "portals", "transporters", "departure", "arrival", "sumner", "hud",
    "messages", "sumner-visit", "promotion", "relics", "boss-sequence", "audio",
    "challenge-hud", "mechanics", "camera-speech",
]
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
        self.assertEqual(re.findall(r'\b(?:measured|child)\("([a-z0-9-]+)"', body), SAMPLES, HOW)
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
