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

TEST_CASE("video window modes and resolution survive settings round trips",
          "[game][config][graphics]") {
    for (const auto mode :
         {WindowMode::Windowed, WindowMode::Fullscreen, WindowMode::BorderlessFullscreen}) {
        GameConfig config;
        config.display.windowMode = mode;
        config.display.windowWidth = 1920;
        config.display.windowHeight = 1080;
        GameConfig restored;
        restored.mergeJson(config.toJson());
        CHECK(restored.display.windowMode == mode);
        CHECK(restored.display.windowWidth == 1920);
        CHECK(restored.display.windowHeight == 1080);
        restored.mergeJson(R"({"display":{"vsync":false}})");
        CHECK(restored.display.windowMode == mode);
    }
    GameConfig config;
    config.mergeJson(R"({"display":{"windowWidth":1280}})");
    CHECK(config.display.windowMode == WindowMode::Windowed);
    CHECK_THROWS_AS(config.mergeJson(R"({"display":{"windowMode":"invalid"}})"), FormatError);
    CHECK_THROWS_AS(config.mergeJson(R"({"display":{"windowWidth":4294967295}})"), FormatError);
}

TEST_CASE("video application compares actual window dimensions rather than a stale saved size",
          "[game][config][graphics]") {
    DisplayConfig defaults;
    CHECK(defaults.matchesWindow(WindowMode::Windowed, {1280, 896}));
    CHECK_FALSE(defaults.matchesWindow(WindowMode::Windowed, {1600, 1000}));
    CHECK_FALSE(defaults.matchesWindow(WindowMode::Fullscreen, {1280, 896}));
    defaults.windowMode = WindowMode::BorderlessFullscreen;
    CHECK(defaults.matchesWindow(WindowMode::BorderlessFullscreen, {1920, 1080}));
    CHECK_FALSE(defaults.matchesWindow(WindowMode::Windowed, {1920, 1080}));
}

TEST_CASE("shipped defaults use thirty fps without changing the simulation clock",
          "[game][config][graphics]") {
    GameConfig config;
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK(config.display.maxFrameRate == 30);
    CHECK(config.timing.gameplayFrameRate == 30);
    CHECK(config.timing.tickRate == 60);
    config.mergeJson(R"({"display":{"maxFrameRate":60},"timing":{"gameplayFrameRate":60}})");
    CHECK(config.display.maxFrameRate == 60);
    CHECK(config.timing.gameplayFrameRate == 60);
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
    REQUIRE_THROWS_AS(config.mergeJson(R"({"controls":{"play":{"magicHoldSeconds":0}}})"),
                      FormatError);
    REQUIRE_FALSE(config.loadFile(test::scratchDirectory("config-missing") / "none.json"));
}

TEST_CASE("graphics settings round-trip with thirty fps presentation defaults",
          "[game][config][graphics]") {
    GameConfig config;
    CHECK(config.display.vsync);
    CHECK(config.display.sampleCount == 1);
    CHECK_FALSE(config.display.depthOfField);
    CHECK_FALSE(config.display.bloom);
    CHECK_FALSE(config.display.ambientOcclusion);
    CHECK(config.display.maxFrameRate == 30);
    CHECK(config.timing.gameplayFrameRate == 30);
    for (const u32 samples : {1U, 2U, 4U}) {
        for (const u32 rate : {30U, 60U, 0U}) {
            config.display.vsync = false;
            config.display.sampleCount = samples;
            config.display.depthOfField = rate == 60;
            config.display.bloom = samples == 4;
            config.display.ambientOcclusion = rate == 30;
            config.display.maxFrameRate = rate;
            config.timing.gameplayFrameRate = rate;
            GameConfig restored;
            restored.mergeJson(config.toJson());
            CHECK(restored.toJson() == config.toJson());
        }
    }
    const auto file = test::scratchDirectory("config-graphics") / "settings.json";
    config.saveFile(file);
    GameConfig restored;
    REQUIRE(restored.loadFile(file));
    CHECK(restored.toJson() == config.toJson());
}

TEST_CASE("invalid graphics sample counts fall back to off and invalid rates retain safe values",
          "[game][config][graphics]") {
    for (const auto* value : {"-1", "0", "3", "8", "null", R"("4")", "true", "1.5"}) {
        GameConfig config;
        config.display.sampleCount = 4;
        config.mergeJson(R"({"display":{"sampleCount":)" + std::string(value) + "}}");
        CHECK(config.display.sampleCount == 1);
    }
    for (const auto* value : {"-1", "4294967296", "null", R"("60")", "true", "30.5"}) {
        GameConfig config;
        config.mergeJson(R"({"display":{"maxFrameRate":)" + std::string(value) +
                         R"(},"timing":{"gameplayFrameRate":)" + std::string(value) + "}}");
        CHECK(config.display.maxFrameRate == 30);
        CHECK(config.timing.gameplayFrameRate == 30);
    }
}

TEST_CASE("GameCube controls and rebindable triggers and gestures round-trip", "[game][config]") {
    GameConfig config;
    REQUIRE(config.play.padAttack == std::vector<PadButton>{PadButton::A});
    REQUIRE(config.play.padStrongAttack == std::vector<PadButton>{PadButton::Y});
    REQUIRE(config.play.padUsePotion == std::vector<PadButton>{PadButton::X});
    REQUIRE(config.play.padTurbo == std::vector<PadButton>{PadButton::B});
    REQUIRE(config.play.padCharge == std::vector<PadButton>{PadButton::LeftTrigger});
    REQUIRE(config.play.padStrafe == std::vector<PadButton>{PadButton::RightTrigger});
    REQUIRE(config.play.padCombo == std::vector<PadButton>{PadButton::RightBumper});
    REQUIRE(config.play.combo == std::vector<Key>{Key::V});
    config.mergeJson(R"({"controls":{"play":{
        "keyboard":{"combo":["G"]},
        "pad":{"attack":["RightTrigger"],"turbo":["LeftTrigger"],"usePotion":["Y"],"charge":[],
               "combo":["LeftBumper"]},
        "magicHoldSeconds":0.4,"magicDoubleTapSeconds":0.3,"actionChords":false,"padMagicGestures":false
    }}})");
    GameConfig roundTrip;
    roundTrip.mergeJson(config.toJson());
    REQUIRE(roundTrip.toJson() == config.toJson());
    REQUIRE(roundTrip.play.padAttack == std::vector<PadButton>{PadButton::RightTrigger});
    REQUIRE(roundTrip.play.padCharge.empty());
    REQUIRE(roundTrip.play.padCombo == std::vector<PadButton>{PadButton::LeftBumper});
    REQUIRE(roundTrip.play.combo == std::vector<Key>{Key::G});
    REQUIRE_FALSE(roundTrip.play.actionChords);
    REQUIRE_FALSE(roundTrip.play.padMagicGestures);
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

TEST_CASE("stick menu bindings are ordinary rebindable configuration names", "[game][config]") {
    GameConfig config;
    config.mergeJson(R"({"controls":{"pad":{"up":["LeftStickRight"],"right":[]}}})");
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.menu.padUp == std::vector<PadButton>{PadButton::LeftStickRight});
    CHECK(restored.menu.padRight.empty());
    CHECK(restored.play.padRight == std::vector<PadButton>{PadButton::LeftStickRight});
    CHECK(restored.play.padSelectorUp == std::vector<PadButton>{PadButton::DpadUp});
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

TEST_CASE("characters are saved beside the game unless the settings say where", "[game][config]") {
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
    REQUIRE(plain.saveDirectory().parent_path() == GameConfig{}.saveDirectory().parent_path());
    REQUIRE(plain.saveDirectory().parent_path() != GameConfig::userSettingsPath().parent_path());
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

TEST_CASE("multiplayer modes round-trip and invalid modes leave the selection unchanged",
          "[game][config][multiplayer]") {
    GameConfig config;
    CHECK(config.multiplayer.mode == MultiplayerMode::Normal);
    for (const auto mode :
         {MultiplayerMode::Normal, MultiplayerMode::Stun, MultiplayerMode::Hurt}) {
        config.multiplayer.mode = mode;
        GameConfig restored;
        restored.mergeJson(config.toJson());
        CHECK(restored.multiplayer.mode == mode);
        CHECK(restored.toJson() == config.toJson());
    }
    config.mergeJson(R"({"game":{"difficulty":"easy"}})");
    CHECK(config.multiplayer.mode == MultiplayerMode::Hurt);
    for (const auto* invalid :
         {R"({"game":{"multiplayer":"friendly"}})", R"({"game":{"multiplayer":3}})",
          R"({"game":{"multiplayer":null}})"}) {
        CHECK_THROWS_AS(config.mergeJson(invalid), FormatError);
        CHECK(config.multiplayer.mode == MultiplayerMode::Hurt);
    }
    const auto file = test::scratchDirectory("config-multiplayer") / "settings.json";
    config.saveFile(file);
    GameConfig restored;
    REQUIRE(restored.loadFile(file));
    CHECK(restored.multiplayer.mode == MultiplayerMode::Hurt);
}

} // namespace
