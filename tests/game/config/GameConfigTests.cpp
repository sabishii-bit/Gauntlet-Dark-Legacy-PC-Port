#include <cmath>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
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

TEST_CASE("expanded shop and autosave are independent opt-in settings", "[game][config]") {
    GameConfig config;
    CHECK_FALSE(config.game.expandedShop);
    CHECK_FALSE(config.game.autoSave);
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK_FALSE(config.game.expandedShop);
    CHECK_FALSE(config.game.autoSave);
    config.mergeJson(R"({"game":{"expandedShop":true}})");
    CHECK(config.game.expandedShop);
    CHECK_FALSE(config.game.autoSave);
    config.mergeJson(R"({"game":{"autoSave":true}})");
    config.mergeJson(R"({"display":{"monitor":1}})");
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.game.expandedShop);
    CHECK(restored.game.autoSave);
    CHECK(restored.display.monitor == 1); // the display chosen is kept with the settings
    CHECK(GameConfig{}.display.monitor == -1);
    config.mergeJson(R"({"game":{"expandedShop":false,"autoSave":false}})");
    CHECK_FALSE(config.game.expandedShop);
    CHECK_FALSE(config.game.autoSave);
}

TEST_CASE("Escape cannot be rebound from pause to a gameplay or menu action", "[game][config]") {
    GameConfig config;
    config.mergeJson(R"({"controls":{"keyboard":{"escape":["R"],"select":["Escape","Enter"]},
        "play":{"keyboard":{"attack":["Escape","Space"]}}}})");
    CHECK(config.menu.escape == std::vector{Key::Escape});
    CHECK(config.menu.select == std::vector{Key::Enter});
    CHECK(config.play.attack == std::vector{Key::Space});
}

TEST_CASE("combat is opt-in and movie volume persists independently of other audio",
          "[game][config][combat-settings][movie-volume]") {
    GameConfig config;
    CHECK_FALSE(config.combat.autoMelee);
    CHECK(config.combat.enemyHealthBars);
    CHECK(config.combat.autoActivateItems);
    CHECK(config.combat.autoAim);
    CHECK(config.audio.movieVolume == 1.0f);
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK_FALSE(config.combat.autoMelee);
    CHECK(config.audio.movieVolume == 1.0f);
    config.mergeJson(R"({"game":{"difficulty":"hard"},"audio":{"musicVolume":0.25}})");
    CHECK_FALSE(config.combat.autoMelee); // old settings keep the new feature off
    CHECK(config.audio.movieVolume == 1.0f);
    config.mergeJson(R"({"game":{"combat":{"autoMelee":true}},"audio":{"movieVolume":0.125}})");
    CHECK(config.combat.enemyHealthBars);
    CHECK(config.combat.autoActivateItems);
    config.mergeJson(R"({"game":{"combat":{"enemyHealthBars":false,"autoActivateItems":false}}})");
    CHECK(config.combat.autoAim); // old settings keep aiming on, as the game always did
    config.mergeJson(R"({"game":{"combat":{"autoAim":false}}})");
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.combat.autoMelee);
    CHECK_FALSE(restored.combat.autoAim);
    CHECK_FALSE(restored.combat.enemyHealthBars);
    CHECK_FALSE(restored.combat.autoActivateItems);
    CHECK(restored.audio.movieVolume == 0.125f);
    CHECK(restored.audio.musicVolume == 0.25f);
    CHECK(restored.audio.effectsVolume == AudioConfig{}.effectsVolume);
    CHECK(restored.difficulty.level == "hard");
}

TEST_CASE("every mechanic is off until switched on, and its switch survives a round trip",
          "[game][config][mechanics]") {
    GameConfig config;
    CHECK(config.game.mechanics.empty());
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK(config.game.mechanics.empty());
    config.mergeJson(R"({"game":{"mechanics":{"rage":true,"calm":false}}})");
    REQUIRE(config.game.mechanics.size() == 2);
    CHECK(config.game.mechanics.at("rage"));
    CHECK_FALSE(config.game.mechanics.at("calm"));
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.game.mechanics == config.game.mechanics);
    CHECK_THROWS_AS(restored.mergeJson(R"({"game":{"mechanics":{"rage":1}}})"), FormatError);
    CHECK_THROWS_AS(restored.mergeJson(R"({"game":{"mechanics":["rage"]}})"), FormatError);
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

TEST_CASE("shipped and reset attack defaults lead with left click and preserve custom mappings",
          "[game][config][controls]") {
    GameConfig config;
    REQUIRE(config.play.attack.front() == Key::MouseLeft);
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK(config.play.attack == PlayBindings{}.attack);
    config.mergeJson(R"({"controls":{"play":{"keyboard":{"attack":["Q"]}}}})");
    CHECK(config.play.attack == std::vector{Key::Q});
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.play.attack == std::vector{Key::Q});
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
    REQUIRE(config.menu.back == std::vector<Key>{Key::Q});
    REQUIRE(config.menu.escape == std::vector<Key>{Key::Escape});
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
    CHECK_FALSE(config.display.smoothSprites);
    CHECK(config.display.authoredSilhouettes);
    CHECK(config.display.maxFrameRate == 30);
    CHECK(config.timing.gameplayFrameRate == 30);
    for (const u32 samples : {1U, 2U, 4U}) {
        for (const u32 rate : {30U, 60U, 0U}) {
            config.display.vsync = false;
            config.display.sampleCount = samples;
            config.display.depthOfField = rate == 60;
            config.display.bloom = samples == 4;
            config.display.ambientOcclusion = rate == 30;
            config.display.smoothSprites = rate == 0;
            config.display.authoredSilhouettes = samples == 2;
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

TEST_CASE("the renderer is Vulkan unless the settings say OpenGL, and nothing else",
          "[game][config][graphics][renderer]") {
    GameConfig config;
    CHECK(config.display.renderer == GraphicsApi::Vulkan);
    CHECK(config.toJson().find("\"renderer\"") != std::string::npos);
    config.mergeJson(R"({"display": {"renderer": "opengl"}})");
    CHECK(config.display.renderer == GraphicsApi::OpenGl);
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.display.renderer == GraphicsApi::OpenGl);
    CHECK(restored.toJson() == config.toJson());
    config.mergeJson(R"({"display": {"renderer": "vulkan"}})");
    CHECK(config.display.renderer == GraphicsApi::Vulkan);
    CHECK_THROWS_AS(config.mergeJson(R"({"display": {"renderer": "software"}})"), FormatError);
    CHECK_THROWS_AS(config.mergeJson(R"({"display": {"renderer": 1}})"), FormatError);
    CHECK(config.display.renderer == GraphicsApi::Vulkan);
}

TEST_CASE("texture filtering persists and invalid choices fall back to trilinear",
          "[game][config][graphics][mipmaps]") {
    GameConfig config;
    CHECK(config.display.textureFiltering == 8);
    REQUIRE(config.loadFile(test::dataDirectory() / "config.json"));
    CHECK(config.display.textureFiltering == 8);
    config.mergeJson(R"({"display":{"vsync":false}})");
    CHECK(config.display.textureFiltering == 8);
    for (const u32 quality : {0U, 1U, 2U, 4U, 8U, 16U}) {
        config.display.textureFiltering = quality;
        GameConfig restored;
        restored.mergeJson(config.toJson());
        CHECK(restored.display.textureFiltering == quality);
    }
    for (const auto* value : {"-1", "3", "32", "null", R"("8")", "true", "1.5"}) {
        CAPTURE(value);
        config.display.textureFiltering = 8;
        config.mergeJson(R"({"display":{"textureFiltering":)" + std::string(value) + "}}");
        CHECK(config.display.textureFiltering == 1);
    }
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
    REQUIRE_FALSE(roundTrip.play.keyboardMagicGestures);
    GameConfig keyboardGestures;
    keyboardGestures.mergeJson(R"({"controls":{"play":{"keyboardMagicGestures":true}}})");
    REQUIRE(keyboardGestures.play.keyboardMagicGestures);
    GameConfig again;
    again.mergeJson(keyboardGestures.toJson());
    REQUIRE(again.play.keyboardMagicGestures);
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
    // A folder named beyond the system code page, as the settings file holds it (UTF-8).
    config.mergeJson(R"({"save":{"directory":"sävé-日本"}})");
    REQUIRE(config.saveDirectory(game) == game / std::filesystem::path(u8"sävé-日本"));
    // With nothing said, that is beside the running program, never the per-user folder.
    const GameConfig plain;
    REQUIRE(plain.saveDirectory().filename() == "saves");
    REQUIRE(plain.saveDirectory().parent_path() == GameConfig{}.saveDirectory().parent_path());
    REQUIRE(plain.saveDirectory().parent_path() != GameConfig::userSettingsPath().parent_path());
}

TEST_CASE("user settings live beside the executable", "[game][config]") {
    const std::filesystem::path path = GameConfig::userSettingsPath();
    REQUIRE(path.filename() == "settings.json");
    REQUIRE(path == GameConfig{}.saveDirectory().parent_path() / "config" / "settings.json");
}

TEST_CASE("all builds keep settings beside the game regardless of the portable marker",
          "[game][config]") {
    const auto game = test::scratchDirectory("portable-settings");
    const auto original = GameConfig::userSettingsPath(game);
    REQUIRE(original == game / "config" / "settings.json");
    // A directory named portable.flag is not an opt-in marker.
    std::filesystem::create_directory(game / "portable.flag");
    REQUIRE(GameConfig::userSettingsPath(game) == original);
    std::filesystem::remove(game / "portable.flag");
    std::ofstream(game / "portable.flag") << "Portable installation\n";
    REQUIRE(GameConfig::userSettingsPath(game) == game / "config" / "settings.json");
    REQUIRE(GameConfig{}.saveDirectory(game) == game / "saves");
    REQUIRE_FALSE(std::filesystem::exists(game / "config"));
    std::filesystem::remove_all(game);
}

TEST_CASE("legacy settings migrate once without overwriting either existing file",
          "[game][config]") {
    const auto root = test::scratchDirectory("settings-migration");
    const auto game = root / "game";
    const auto legacy = root / "old" / "settings.json";
    GameConfig previous;
    previous.display.maxFrameRate = 60;
    previous.audio.movieVolume = 0.25f;
    previous.saveFile(legacy);
    const auto original = readTextFile(legacy);
    GameConfig loaded;
    REQUIRE(loaded.loadUserSettings(game, legacy));
    CHECK(loaded.toJson() == previous.toJson());
    CHECK(readTextFile(GameConfig::userSettingsPath(game)) == original);
    CHECK(readTextFile(legacy) == original);
    loaded.display.maxFrameRate = 30;
    loaded.saveFile(GameConfig::userSettingsPath(game));
    GameConfig again;
    REQUIRE(again.loadUserSettings(game, legacy));
    CHECK(again.display.maxFrameRate == 30);
    CHECK(again.audio.movieVolume == 0.25f);
    CHECK(readTextFile(legacy) == original);
}

TEST_CASE("failed local settings imports preserve defaults and all existing files",
          "[game][config]") {
    const auto root = test::scratchDirectory("settings-migration-failure");
    const auto game = root / "game";
    const auto legacy = root / "old" / "settings.json";
    GameConfig previous;
    previous.display.maxFrameRate = 60;
    previous.saveFile(legacy);
    const auto local = GameConfig::userSettingsPath(game);
    SECTION("corrupt local settings take precedence over a stale legacy file") {
        std::filesystem::create_directories(local.parent_path());
        writeTextFile(local, "not settings");
    }
    SECTION("a file blocks creation of the config directory") {
        std::filesystem::create_directories(game);
        writeTextFile(local.parent_path(), "keep this file");
    }
    SECTION("legacy settings cannot be parsed") {
        writeTextFile(legacy, "not settings");
    }
    const auto expectedLegacy = readTextFile(legacy);
    GameConfig loaded;
    const auto defaults = loaded.toJson();
    CHECK_FALSE(loaded.loadUserSettings(game, legacy));
    CHECK(loaded.toJson() == defaults);
    CHECK(readTextFile(legacy) == expectedLegacy);
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

TEST_CASE("the online camera rule is kept with the settings, shared unless said otherwise",
          "[game][config][multiplayer]") {
    GameConfig config;
    CHECK_FALSE(config.multiplayer.soloCamera);
    config.mergeJson(R"({"game":{"soloCamera":true}})");
    CHECK(config.multiplayer.soloCamera);
    GameConfig restored;
    restored.mergeJson(config.toJson());
    CHECK(restored.multiplayer.soloCamera);
    CHECK(restored.toJson() == config.toJson());
    config.mergeJson(R"({"game":{"difficulty":"easy"}})"); // nothing said: unchanged
    CHECK(config.multiplayer.soloCamera);
}

} // namespace
