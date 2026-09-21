#include <cmath>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/platform/Input.h"

#include "TestSupport.h"
#include "game/config/GameConfig.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the defaults describe the original's screen and clock", "[game][config]") {
    const GameConfig config;
    REQUIRE(config.display.virtualWidth == 512);
    REQUIRE(config.display.virtualHeight == 384);
    REQUIRE(config.display.frameWidth == 640);
    REQUIRE(config.display.frameHeight == 448);
    REQUIRE(config.timing.tickRate == 60);
    REQUIRE(config.timing.gameplayFrameRate == 30);
    REQUIRE(config.text.language == "en");
    REQUIRE(std::abs(config.horizontalFovRadians() - 1.0471976f) < 1e-5f);
    REQUIRE(config.menu.select.size() == 2);
    REQUIRE(config.menu.padStart.front() == PadButton::Start);
}

TEST_CASE("JSON merges over the defaults and leaves the rest alone", "[game][config]") {
    GameConfig config;
    config.mergeJson(R"({
  "display": {"windowWidth": 800, "vsync": false, "maxFrameRate": 24},
  "timing": {"tickRate": 120},
  "audio": {"musicVolume": 0.25},
  "text": {"language": "fr"},
  "controls": {"keyboard": {"back": ["escape", "Q"]}, "pad": {"select": ["a", "x"]}}
})");
    REQUIRE(config.display.windowWidth == 800);
    REQUIRE(config.display.windowHeight == 896);
    REQUIRE_FALSE(config.display.vsync);
    REQUIRE(config.display.maxFrameRate == 24);
    REQUIRE(config.timing.tickRate == 120);
    REQUIRE(config.timing.gameplayFrameRate == 30);
    REQUIRE(config.audio.musicVolume == 0.25f);
    REQUIRE(config.audio.masterVolume == 1.0f);
    REQUIRE(config.text.language == "fr");
    REQUIRE(config.menu.back == std::vector<Key>{Key::Escape, Key::Q});
    REQUIRE(config.menu.padSelect == std::vector<PadButton>{PadButton::A, PadButton::X});
    REQUIRE(config.menu.up == std::vector<Key>{Key::Up, Key::W});
}

TEST_CASE("bad configuration is rejected", "[game][config]") {
    GameConfig config;
    REQUIRE_THROWS_AS(config.mergeJson("{not json"), FormatError);
    REQUIRE_THROWS_AS(config.mergeJson(R"({"display": {"virtualWidth": 0}})"), FormatError);
    REQUIRE_THROWS_AS(config.mergeJson(R"({"timing": {"tickRate": 0}})"), FormatError);
    REQUIRE_FALSE(config.loadFile(test::scratchDirectory("config-missing") / "none.json"));
}

TEST_CASE("the configuration round-trips through JSON files", "[game][config]") {
    GameConfig config;
    config.display.windowWidth = 1024;
    config.audio.effectsVolume = 0.5f;
    config.menu.start = {Key::Space};
    config.menu.padBack = {PadButton::Y};
    const std::filesystem::path file = test::scratchDirectory("config-roundtrip") / "settings.json";
    config.saveFile(file);

    GameConfig loaded;
    REQUIRE(loaded.loadFile(file));
    REQUIRE(loaded.display.windowWidth == 1024);
    REQUIRE(loaded.audio.effectsVolume == 0.5f);
    REQUIRE(loaded.menu.start == std::vector<Key>{Key::Space});
    REQUIRE(loaded.menu.padBack == std::vector<PadButton>{PadButton::Y});
    REQUIRE(loaded.toJson() == config.toJson());
}

TEST_CASE("the shipped defaults file matches the built-in defaults", "[game][config]") {
    const std::filesystem::path file = test::dataDirectory() / "config.json";
    if (!std::filesystem::exists(file)) {
        SKIP("data/config.json is not available");
    }
    GameConfig loaded;
    REQUIRE(loaded.loadFile(file));
    REQUIRE(loaded.toJson() == GameConfig{}.toJson());
}

TEST_CASE("characters are saved beside the game unless the settings say where",
          "[game][config]") {
    GameConfig config;
    const std::filesystem::path game = std::filesystem::path("somewhere") / "bin";
    REQUIRE(config.saveDirectory(game) == game / "saves");
    config.save.directory = "my-saves";
    REQUIRE(config.saveDirectory(game) == game / "my-saves");
    const std::filesystem::path elsewhere = std::filesystem::absolute("elsewhere");
    config.save.directory = elsewhere.string();
    REQUIRE(config.saveDirectory(game) == elsewhere);
    // With nothing said, that is beside the running program, never the per-user folder.
    const GameConfig plain;
    REQUIRE(plain.saveDirectory().filename() == "saves");
    REQUIRE(plain.saveDirectory().parent_path() ==
            GameConfig{}.saveDirectory().parent_path());
    REQUIRE(plain.saveDirectory().parent_path() !=
            GameConfig::userSettingsPath().parent_path());
}

TEST_CASE("user settings live in a per-user folder", "[game][config]") {
    const std::filesystem::path path = GameConfig::userSettingsPath();
    REQUIRE(path.filename() == "settings.json");
    REQUIRE(path.parent_path().filename() == "GauntletDarkLegacy");
}

TEST_CASE("the difficulty names a gain on the levels' own scales", "[game][config]") {
    GameConfig config;
    REQUIRE(config.difficulty.level == "normal");
    REQUIRE(config.difficulty.gain() == 1.0f);
    config.mergeJson(R"({"game": {"difficulty": "hard"}})");
    REQUIRE(config.difficulty.gain() == 1.5f);
    config.difficulty.level = "easy";
    REQUIRE(config.difficulty.gain() == 0.667f);
    config.difficulty.level = "nightmare";
    REQUIRE(config.difficulty.gain() == 1.0f);
    GameConfig again;
    again.mergeJson(config.toJson());
    REQUIRE(again.difficulty.level == "nightmare");
}

} // namespace
