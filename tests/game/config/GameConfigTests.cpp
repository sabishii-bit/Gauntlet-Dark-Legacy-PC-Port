#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/platform/DisplayMode.h"
#include "engine/platform/Input.h"
#include "engine/render/RenderTypes.h"

#include "TestSupport.h"
#include "game/config/GameConfig.h"
#include "game/config/MultiplayerMode.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace gdl;
using namespace gdl::game;

/** Closes a stream the tests opened. */
struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
};

// The C runtime's calls that point the error stream, where the log warns, at another file.
#if defined(_WIN32)
std::FILE* openForWriting(const std::filesystem::path& file) {
    return _wfopen(file.c_str(), L"w");
}
int descriptorOf(std::FILE* stream) {
    return _fileno(stream);
}
int duplicateDescriptor(int descriptor) {
    return _dup(descriptor);
}
int redirectDescriptor(int from, int onto) {
    return _dup2(from, onto);
}
int closeDescriptor(int descriptor) {
    return _close(descriptor);
}
#else
std::FILE* openForWriting(const std::filesystem::path& file) {
    return std::fopen(file.c_str(), "w");
}
int descriptorOf(std::FILE* stream) {
    return fileno(stream);
}
int duplicateDescriptor(int descriptor) {
    return dup(descriptor);
}
int redirectDescriptor(int from, int onto) {
    return dup2(from, onto);
}
int closeDescriptor(int descriptor) {
    return close(descriptor);
}
#endif

/** What a merge did besides changing the settings: what the log warned of, and what it threw. */
struct MergeOutcome {
    std::vector<std::string> warnings; ///< each warning's message, without its time stamp
    std::exception_ptr failure;        ///< null when the merge returned
};

/** Merges `json` into `config` with the error stream sent to a scratch file, then reads the
 * log's warnings back from it. What the merge throws is kept, not thrown on. */
MergeOutcome mergeLogged(GameConfig& config, std::string_view json) {
    const auto file = test::scratchDirectory("config-warnings") / "errors.txt";
    MergeOutcome outcome;
    {
        const std::unique_ptr<std::FILE, FileCloser> sink{openForWriting(file)};
        REQUIRE(sink != nullptr);
        std::fflush(stderr);
        const int errors = descriptorOf(stderr);
        const int saved = duplicateDescriptor(errors);
        REQUIRE(saved >= 0);
        REQUIRE(redirectDescriptor(descriptorOf(sink.get()), errors) >= 0);
        try {
            config.mergeJson(json);
        } catch (...) {
            outcome.failure = std::current_exception();
        }
        std::fflush(stderr);
        const bool restored = redirectDescriptor(saved, errors) >= 0;
        const bool closed = closeDescriptor(saved) == 0;
        REQUIRE(restored);
        REQUIRE(closed);
    }
    constexpr std::string_view kWarningTag = "] WARN  ";
    const std::string text = readTextFile(file);
    const std::string_view lines{text};
    for (usize start = 0; start < lines.size();) {
        const usize end = std::min(lines.find('\n', start), lines.size());
        std::string_view line = lines.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (const usize tag = line.find(kWarningTag); tag != std::string_view::npos) {
            outcome.warnings.emplace_back(line.substr(tag + kWarningTag.size()));
        }
        start = end + 1;
    }
    return outcome;
}

/** The message of what the merge threw when it threw a `Failure`, else empty. */
template <class Failure> std::string failureOf(const MergeOutcome& outcome) {
    if (outcome.failure == nullptr) {
        return {};
    }
    try {
        std::rethrow_exception(outcome.failure);
    } catch (const Failure& failure) {
        return failure.what();
    } catch (...) {
        return {};
    }
}

/** Whether the merge threw nlohmann's type error of the id given (302: a value of the wrong
 * type; 306: a lookup in what is not an object). */
bool threwTypeError(const MergeOutcome& outcome, std::string_view id) {
    return failureOf<nlohmann::json::type_error>(outcome).starts_with(
        "[json.exception.type_error." + std::string(id) + "]");
}

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

TEST_CASE("bad configuration is rejected", "[game][config][settings-file]") {
    // Each on its own configuration: a refused merge keeps what it read, which would refuse
    // the next merge for the earlier value instead of its own.
    REQUIRE_THROWS_AS(GameConfig{}.mergeJson("{not json"), FormatError);
    REQUIRE_THROWS_AS(GameConfig{}.mergeJson(R"({"display": {"virtualWidth": 0}})"), FormatError);
    REQUIRE_THROWS_AS(GameConfig{}.mergeJson(R"({"timing": {"tickRate": 0}})"), FormatError);
    REQUIRE_THROWS_AS(GameConfig{}.mergeJson(R"({"controls":{"play":{"magicHoldSeconds":0}}})"),
                      FormatError);
    REQUIRE_FALSE(GameConfig{}.loadFile(test::scratchDirectory("config-missing") / "none.json"));
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

/** The settings file the defaults write. */
constexpr std::string_view kDefaultSettingsFile = R"json({
  "audio": {
    "effectsVolume": 0.501960813999176,
    "masterVolume": 1.0,
    "movieVolume": 1.0,
    "musicVolume": 0.501960813999176,
    "stereo": true
  },
  "camera": {
    "compass": false,
    "horizontalFovDegrees": 60.0
  },
  "controls": {
    "explicitMovement": true,
    "keyboard": {
      "back": [
        "Backspace"
      ],
      "console": [
        "Grave"
      ],
      "down": [
        "Down",
        "S"
      ],
      "escape": [
        "Escape"
      ],
      "left": [
        "Left",
        "A"
      ],
      "right": [
        "Right",
        "D"
      ],
      "select": [
        "Enter",
        "Space"
      ],
      "shopExit": [
        "Escape"
      ],
      "shopSell": [
        "Backspace"
      ],
      "start": [
        "Enter"
      ],
      "up": [
        "Up",
        "W"
      ]
    },
    "pad": {
      "back": [
        "Y"
      ],
      "down": [
        "DpadDown",
        "LeftStickDown"
      ],
      "left": [
        "DpadLeft",
        "LeftStickLeft"
      ],
      "right": [
        "DpadRight",
        "LeftStickRight"
      ],
      "select": [
        "A"
      ],
      "shopExit": [
        "B"
      ],
      "shopSell": [
        "X"
      ],
      "start": [
        "Start"
      ],
      "up": [
        "DpadUp",
        "LeftStickUp"
      ]
    },
    "play": {
      "actionChords": true,
      "keyboard": {
        "attack": [
          "MouseLeft",
          "Space"
        ],
        "charge": [
          "F"
        ],
        "combo": [
          "V"
        ],
        "down": [
          "Down",
          "S"
        ],
        "left": [
          "Left",
          "A"
        ],
        "right": [
          "Right",
          "D"
        ],
        "selectorDown": [
          "K"
        ],
        "selectorLeft": [
          "J"
        ],
        "selectorRight": [
          "L"
        ],
        "selectorUp": [
          "I"
        ],
        "shieldPotion": [
          "C"
        ],
        "strafe": [
          "LeftControl"
        ],
        "strongAttack": [
          "R",
          "MouseRight"
        ],
        "throwPotion": [
          "Q"
        ],
        "turbo": [
          "LeftShift"
        ],
        "up": [
          "Up",
          "W"
        ],
        "usePotion": [
          "E"
        ]
      },
      "keyboardMagicGestures": false,
      "magicDoubleTapSeconds": 0.25,
      "magicHoldSeconds": 0.25,
      "pad": {
        "attack": [
          "A"
        ],
        "charge": [
          "LeftTrigger"
        ],
        "combo": [
          "RightBumper"
        ],
        "down": [
          "LeftStickDown"
        ],
        "left": [
          "LeftStickLeft"
        ],
        "right": [
          "LeftStickRight"
        ],
        "selectorDown": [
          "DpadDown"
        ],
        "selectorLeft": [
          "DpadLeft"
        ],
        "selectorRight": [
          "DpadRight"
        ],
        "selectorUp": [
          "DpadUp"
        ],
        "shieldPotion": [],
        "strafe": [
          "RightTrigger"
        ],
        "strongAttack": [
          "Y"
        ],
        "throwPotion": [],
        "turbo": [
          "B"
        ],
        "up": [
          "LeftStickUp"
        ],
        "usePotion": [
          "X"
        ]
      },
      "padMagicGestures": true,
      "stickDeadZone": 0.25
    },
    "players": [
      {
        "device": "",
        "name": "",
        "occurrence": 0,
        "rumble": true,
        "scheme": "tank"
      },
      {
        "device": "",
        "name": "",
        "occurrence": 0,
        "rumble": true,
        "scheme": "tank"
      },
      {
        "device": "",
        "name": "",
        "occurrence": 0,
        "rumble": true,
        "scheme": "tank"
      },
      {
        "device": "",
        "name": "",
        "occurrence": 0,
        "rumble": true,
        "scheme": "tank"
      }
    ]
  },
  "display": {
    "ambientOcclusion": false,
    "authoredSilhouettes": true,
    "bloom": false,
    "depthOfField": false,
    "frameHeight": 448,
    "frameWidth": 640,
    "maxFrameRate": 30,
    "monitor": -1,
    "renderer": "vulkan",
    "sampleCount": 1,
    "smoothSprites": false,
    "textureFiltering": 8,
    "virtualHeight": 384,
    "virtualWidth": 512,
    "vsync": true,
    "windowHeight": 896,
    "windowMode": "windowed",
    "windowWidth": 1280
  },
  "game": {
    "autoSave": false,
    "combat": {
      "autoActivateItems": true,
      "autoAim": true,
      "autoMelee": false,
      "enemyHealthBars": true
    },
    "difficulty": "normal",
    "expandedShop": false,
    "mechanics": {},
    "multiplayer": "normal",
    "soloCamera": false
  },
  "save": {
    "directory": "",
    "slots": 8
  },
  "text": {
    "language": "en"
  },
  "timing": {
    "gameplayFrameRate": 30,
    "tickRate": 60
  }
}
)json";

/** The settings file `awayFromDefaults` writes. */
constexpr std::string_view kChangedSettingsFile = R"json({
  "audio": {
    "effectsVolume": 0.0,
    "masterVolume": 0.501960813999176,
    "movieVolume": 0.25,
    "musicVolume": 0.10000000149011612,
    "stereo": false
  },
  "camera": {
    "compass": true,
    "horizontalFovDegrees": 75.5
  },
  "controls": {
    "explicitMovement": true,
    "keyboard": {
      "back": [],
      "console": [
        "F12"
      ],
      "down": [],
      "escape": [
        "Escape"
      ],
      "left": [
        "J",
        "Left"
      ],
      "right": [
        "L"
      ],
      "select": [
        "F",
        "Enter"
      ],
      "shopExit": [
        "L",
        "Escape"
      ],
      "shopSell": [
        "K"
      ],
      "start": [
        "F2"
      ],
      "up": [
        "I"
      ]
    },
    "pad": {
      "back": [
        "A"
      ],
      "down": [
        "DpadDown"
      ],
      "left": [
        "LeftBumper"
      ],
      "right": [
        "RightBumper",
        "LeftStickRight"
      ],
      "select": [
        "B"
      ],
      "shopExit": [
        "Start"
      ],
      "shopSell": [],
      "start": [
        "Back"
      ],
      "up": []
    },
    "play": {
      "actionChords": false,
      "keyboard": {
        "attack": [
          "Space"
        ],
        "charge": [],
        "combo": [
          "G"
        ],
        "down": [],
        "left": [
          "4"
        ],
        "right": [
          "6",
          "Right"
        ],
        "selectorDown": [
          "PageDown"
        ],
        "selectorLeft": [
          "ScrollUp"
        ],
        "selectorRight": [
          "ScrollDown"
        ],
        "selectorUp": [
          "PageUp"
        ],
        "shieldPotion": [],
        "strafe": [
          "LeftAlt"
        ],
        "strongAttack": [
          "MouseMiddle"
        ],
        "throwPotion": [
          "T"
        ],
        "turbo": [
          "Tab"
        ],
        "up": [
          "8"
        ],
        "usePotion": []
      },
      "keyboardMagicGestures": true,
      "magicDoubleTapSeconds": 0.5,
      "magicHoldSeconds": 2.0,
      "pad": {
        "attack": [
          "Button12"
        ],
        "charge": [
          "RightTrigger"
        ],
        "combo": [],
        "down": [
          "DpadDown"
        ],
        "left": [],
        "right": [
          "DpadRight",
          "LeftStickRight"
        ],
        "selectorDown": [
          "Guide"
        ],
        "selectorLeft": [
          "Button1"
        ],
        "selectorRight": [
          "Button32"
        ],
        "selectorUp": [],
        "shieldPotion": [
          "LeftThumb"
        ],
        "strafe": [],
        "strongAttack": [
          "RightThumb"
        ],
        "throwPotion": [
          "X"
        ],
        "turbo": [],
        "up": [],
        "usePotion": [
          "Y"
        ]
      },
      "padMagicGestures": false,
      "stickDeadZone": 0.10000000149011612
    },
    "players": [
      {
        "bindings": {
          "attack": {
            "buttons": [],
            "keys": [
              "MouseRight"
            ]
          },
          "charge": {
            "buttons": [
              "LeftTrigger"
            ],
            "keys": [
              "F"
            ]
          },
          "combo": {
            "buttons": [
              "RightBumper"
            ],
            "keys": [
              "V"
            ]
          },
          "down": {
            "buttons": [
              "LeftStickDown"
            ],
            "keys": [
              "Down",
              "S"
            ]
          },
          "left": {
            "buttons": [
              "LeftStickLeft"
            ],
            "keys": [
              "Left",
              "A"
            ]
          },
          "menuBack": {
            "buttons": [],
            "keys": [
              "Backspace"
            ]
          },
          "menuDown": {
            "buttons": [
              "DpadDown",
              "LeftStickDown"
            ],
            "keys": [
              "Down",
              "S"
            ]
          },
          "menuLeft": {
            "buttons": [
              "DpadLeft",
              "LeftStickLeft"
            ],
            "keys": [
              "Left",
              "A"
            ]
          },
          "menuRight": {
            "buttons": [
              "DpadRight",
              "LeftStickRight"
            ],
            "keys": [
              "Right",
              "D"
            ]
          },
          "menuSelect": {
            "buttons": [
              "A"
            ],
            "keys": [
              "Q"
            ]
          },
          "menuStart": {
            "buttons": [
              "Start"
            ],
            "keys": [
              "Enter"
            ]
          },
          "menuUp": {
            "buttons": [
              "DpadUp",
              "LeftStickUp"
            ],
            "keys": [
              "Up",
              "W"
            ]
          },
          "right": {
            "buttons": [
              "LeftStickRight"
            ],
            "keys": [
              "Right",
              "D"
            ]
          },
          "selectorDown": {
            "buttons": [
              "DpadDown"
            ],
            "keys": [
              "K"
            ]
          },
          "selectorLeft": {
            "buttons": [
              "DpadLeft"
            ],
            "keys": [
              "J"
            ]
          },
          "selectorRight": {
            "buttons": [
              "DpadRight"
            ],
            "keys": [
              "L"
            ]
          },
          "selectorUp": {
            "buttons": [
              "DpadUp"
            ],
            "keys": [
              "I"
            ]
          },
          "shieldPotion": {
            "buttons": [],
            "keys": [
              "C",
              "X"
            ]
          },
          "shopExit": {
            "buttons": [
              "B"
            ],
            "keys": [
              "Escape"
            ]
          },
          "shopSell": {
            "buttons": [
              "X"
            ],
            "keys": [
              "Backspace"
            ]
          },
          "strafe": {
            "buttons": [
              "RightTrigger"
            ],
            "keys": [
              "LeftControl"
            ]
          },
          "strongAttack": {
            "buttons": [
              "Y"
            ],
            "keys": [
              "R",
              "MouseRight"
            ]
          },
          "throwPotion": {
            "buttons": [],
            "keys": [
              "Q"
            ]
          },
          "turbo": {
            "buttons": [
              "B"
            ],
            "keys": [
              "LeftShift"
            ]
          },
          "up": {
            "buttons": [
              "LeftStickUp"
            ],
            "keys": [
              "Up",
              "W"
            ]
          },
          "usePotion": {
            "buttons": [
              "X"
            ],
            "keys": [
              "E"
            ]
          }
        },
        "device": "keyboard",
        "gestures": {
          "keyboard": false,
          "pad": true
        },
        "name": "Desk \"one\"",
        "occurrence": 1,
        "rumble": false,
        "scheme": "robotron"
      },
      {
        "device": "030000005e0400008e02000000007200",
        "name": "Pad two",
        "occurrence": 2,
        "rumble": false,
        "scheme": "robotron"
      },
      {
        "device": "none",
        "name": "",
        "occurrence": 15,
        "rumble": false,
        "scheme": "tank"
      },
      {
        "device": "",
        "name": "Spare",
        "occurrence": 3,
        "rumble": true,
        "scheme": "robotron"
      }
    ]
  },
  "display": {
    "ambientOcclusion": true,
    "authoredSilhouettes": false,
    "bloom": true,
    "depthOfField": true,
    "frameHeight": 600,
    "frameWidth": 800,
    "maxFrameRate": 0,
    "monitor": 2,
    "renderer": "opengl",
    "sampleCount": 4,
    "smoothSprites": true,
    "textureFiltering": 16,
    "virtualHeight": 480,
    "virtualWidth": 640,
    "vsync": false,
    "windowHeight": 1080,
    "windowMode": "borderless",
    "windowWidth": 1920
  },
  "game": {
    "autoSave": true,
    "combat": {
      "autoActivateItems": false,
      "autoAim": false,
      "autoMelee": true,
      "enemyHealthBars": false
    },
    "difficulty": "bogus",
    "expandedShop": true,
    "mechanics": {
      "calm": false,
      "rage": true
    },
    "multiplayer": "stun",
    "soloCamera": true
  },
  "save": {
    "directory": "D:\\Saves\\sävé-日本",
    "slots": 3
  },
  "text": {
    "language": "fr"
  },
  "timing": {
    "gameplayFrameRate": 60,
    "tickRate": 120
  }
}
)json";

/** Every setting away from its default: bindings changed, emptied and reordered, a customized
 * profile beside three that are not, two mechanics and a difficulty no table names. Escape
 * keeps its fixed binding, which reading the file's menu keys restores. */
GameConfig awayFromDefaults() {
    GameConfig config;
    auto& display = config.display;
    display.renderer = GraphicsApi::OpenGl;
    display.virtualWidth = 640;
    display.virtualHeight = 480;
    display.frameWidth = 800;
    display.frameHeight = 600;
    display.windowWidth = 1920;
    display.windowHeight = 1080;
    display.windowMode = WindowMode::BorderlessFullscreen;
    display.monitor = 2;
    display.vsync = false;
    display.depthOfField = true;
    display.bloom = true;
    display.ambientOcclusion = true;
    display.smoothSprites = true;
    display.authoredSilhouettes = false;
    display.sampleCount = 4;
    display.textureFiltering = 16;
    display.maxFrameRate = 0;
    config.timing.tickRate = 120;
    config.timing.gameplayFrameRate = 60;
    config.camera.horizontalFovDegrees = 75.5f;
    config.camera.compass = true;
    config.audio.masterVolume = 128.0f / 255.0f;
    config.audio.musicVolume = 0.1f;
    config.audio.effectsVolume = 0.0f;
    config.audio.movieVolume = 0.25f;
    config.audio.stereo = false;
    config.text.language = "fr";
    config.save.directory = R"(D:\Saves\sävé-日本)";
    config.save.slots = 3;
    config.difficulty.level = "bogus";
    config.multiplayer.mode = MultiplayerMode::Stun;
    config.multiplayer.soloCamera = true;
    config.combat.autoMelee = true;
    config.combat.enemyHealthBars = false;
    config.combat.autoActivateItems = false;
    config.combat.autoAim = false;
    config.game.expandedShop = true;
    config.game.autoSave = true;
    config.game.mechanics = {{"calm", false}, {"rage", true}};

    auto& menu = config.menu;
    menu.up = {Key::I};
    menu.down.clear();
    menu.left = {Key::J, Key::Left};
    menu.right = {Key::L};
    menu.select = {Key::F, Key::Enter};
    menu.back.clear();
    menu.start = {Key::F2};
    menu.shopSell = {Key::K};
    menu.shopExit = {Key::L, Key::Escape};
    menu.console = {Key::F12};
    menu.padUp.clear();
    menu.padDown = {PadButton::DpadDown};
    menu.padLeft = {PadButton::LeftBumper};
    menu.padRight = {PadButton::RightBumper, PadButton::LeftStickRight};
    menu.padSelect = {PadButton::B};
    menu.padBack = {PadButton::A};
    menu.padStart = {PadButton::Back};
    menu.padShopSell.clear();
    menu.padShopExit = {PadButton::Start};

    auto& play = config.play;
    play.up = {Key::Num8};
    play.down.clear();
    play.left = {Key::Num4};
    play.right = {Key::Num6, Key::Right};
    play.padUp.clear();
    play.padDown = {PadButton::DpadDown};
    play.padLeft.clear();
    play.padRight = {PadButton::DpadRight, PadButton::LeftStickRight};
    play.attack = {Key::Space};
    play.padAttack = {PadButton::Button12};
    play.usePotion.clear();
    play.padUsePotion = {PadButton::Y};
    play.throwPotion = {Key::T};
    play.padThrowPotion = {PadButton::X};
    play.shieldPotion.clear();
    play.padShieldPotion = {PadButton::LeftThumb};
    play.strafe = {Key::LeftAlt};
    play.padStrafe.clear();
    play.strongAttack = {Key::MouseMiddle};
    play.padStrongAttack = {PadButton::RightThumb};
    play.turbo = {Key::Tab};
    play.padTurbo.clear();
    play.charge.clear();
    play.padCharge = {PadButton::RightTrigger};
    play.combo = {Key::G};
    play.padCombo.clear();
    play.selectorUp = {Key::PageUp};
    play.selectorDown = {Key::PageDown};
    play.selectorLeft = {Key::ScrollUp};
    play.selectorRight = {Key::ScrollDown};
    play.padSelectorUp.clear();
    play.padSelectorDown = {PadButton::Guide};
    play.padSelectorLeft = {PadButton::Button1};
    play.padSelectorRight = {PadButton::Button32};
    play.stickDeadZone = 0.1f;
    play.magicHoldSeconds = 2.0f;
    play.magicDoubleTapSeconds = 0.5f;
    play.padMagicGestures = false;
    play.keyboardMagicGestures = true;
    play.actionChords = false;

    auto& desk = config.controls[0];
    desk.device = "keyboard";
    desk.name = R"(Desk "one")";
    desk.occurrence = 1;
    desk.customized = true;
    desk.rumble = false;
    desk.scheme = KeyboardScheme::Robotron;
    desk.menu.select = {Key::Q};
    desk.menu.padBack.clear();
    desk.play.attack = {Key::MouseRight};
    desk.play.padAttack.clear();
    desk.play.shieldPotion = {Key::C, Key::X};
    auto& pad = config.controls[1];
    pad.device = "030000005e0400008e02000000007200";
    pad.name = "Pad two";
    pad.occurrence = 2;
    pad.rumble = false;
    pad.scheme = KeyboardScheme::Robotron;
    auto& off = config.controls[2];
    off.device = "none";
    off.occurrence = Input::kMaxPads - 1;
    off.rumble = false;
    auto& spare = config.controls[3];
    spare.name = "Spare";
    spare.occurrence = 3;
    spare.scheme = KeyboardScheme::Robotron;
    return config;
}

TEST_CASE("the defaults are written to the settings file byte for byte",
          "[game][config][settings-file]") {
    CHECK(GameConfig{}.toJson() == kDefaultSettingsFile);
    GameConfig restored;
    const auto merged = mergeLogged(restored, kDefaultSettingsFile);
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(restored.toJson() == kDefaultSettingsFile);
}

TEST_CASE("every setting away from its default is written byte for byte and read back whole",
          "[game][config][settings-file]") {
    const GameConfig config = awayFromDefaults();
    CHECK(config.toJson() == kChangedSettingsFile);
    GameConfig restored;
    const auto merged = mergeLogged(restored, kChangedSettingsFile);
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(restored.toJson() == kChangedSettingsFile);
    CHECK(restored.controls[0].customized);
    CHECK(restored.controls[0].play.shieldPotion == std::vector{Key::C, Key::X});
    CHECK_FALSE(restored.controls[1].customized);
}

TEST_CASE("the console key is read under its former name, then under its own",
          "[game][config][settings-file]") {
    GameConfig former;
    auto merged = mergeLogged(former, R"({"controls":{"keyboard":{"debug":["F1"]}}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(former.menu.console == std::vector{Key::F1});
    GameConfig both;
    merged = mergeLogged(both, R"({"controls":{"keyboard":{"debug":["F1"],"console":["F2"]}}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(both.menu.console == std::vector{Key::F2});
    const auto saved = nlohmann::json::parse(both.toJson());
    CHECK(saved.at("controls").at("keyboard").at("console") == nlohmann::json::array({"F2"}));
    CHECK_FALSE(saved.at("controls").at("keyboard").contains("debug"));
    GameConfig unknown;
    merged = mergeLogged(unknown, R"({"controls":{"keyboard":{"debug":["Nope"]}}})");
    CHECK(merged.warnings == std::vector<std::string>{"Config: unknown key 'Nope' for 'debug'"});
    CHECK(merged.failure == nullptr);
    CHECK(unknown.menu.console.empty());
}

TEST_CASE("a file that does not say movement is explicit adds the stick to walking on a pad",
          "[game][config][settings-file]") {
    GameConfig older;
    older.play.padUp.clear();
    older.play.padDown = {PadButton::A};
    auto merged = mergeLogged(older, R"({"controls":{}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(older.play.padUp == std::vector{PadButton::LeftStickUp});
    CHECK(older.play.padDown == std::vector{PadButton::A, PadButton::LeftStickDown});
    CHECK(older.play.padLeft == std::vector{PadButton::LeftStickLeft});
    CHECK(older.play.padRight == std::vector{PadButton::LeftStickRight});
    CHECK(older.menu.padUp == MenuBindings{}.padUp);
    GameConfig unsaid;
    unsaid.play.padUp.clear();
    merged = mergeLogged(unsaid, R"({"controls":{"explicitMovement":false}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(unsaid.play.padUp == std::vector{PadButton::LeftStickUp});
    for (const auto* file : {R"({"controls":{"explicitMovement":true}})", R"({"display":{}})"}) {
        CAPTURE(file);
        GameConfig explicitly;
        explicitly.play.padUp.clear();
        merged = mergeLogged(explicitly, file);
        CHECK(merged.warnings.empty());
        CHECK(merged.failure == nullptr);
        CHECK(explicitly.play.padUp.empty());
    }
    // A customized profile gains the stick too. One that is not keeps the copy of the shared
    // bindings it took before the stick was added; nothing reads it while it is not customized.
    GameConfig profiles;
    merged = mergeLogged(profiles, R"({"controls":{"play":{"pad":{"up":[]}},
        "players":[{}, {"bindings":{"down":{"buttons":[]}}}, {}, {}]}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(profiles.play.padUp == std::vector{PadButton::LeftStickUp});
    CHECK(profiles.controls[1].play.padUp == std::vector{PadButton::LeftStickUp});
    CHECK(profiles.controls[1].play.padDown == std::vector{PadButton::LeftStickDown});
    CHECK(profiles.controls[0].play.padUp.empty());
}

TEST_CASE("Escape is kept out of every key binding but the shop's exit",
          "[game][config][settings-file]") {
    GameConfig shape;
    shape.controls[1].customized = true;
    auto file = nlohmann::json::parse(shape.toJson());
    const auto escapeFirst = nlohmann::json::array({"Escape", "F5"});
    for (auto* lists : {&file["controls"]["keyboard"], &file["controls"]["play"]["keyboard"]}) {
        for (const auto& [name, keys] : lists->items()) {
            keys = escapeFirst;
        }
    }
    for (const auto& [id, binding] : file["controls"]["players"][1]["bindings"].items()) {
        binding["keys"] = escapeFirst;
    }
    GameConfig config;
    config.menu.escape = {Key::F1};
    const auto merged = mergeLogged(config, file.dump());
    CHECK(merged.warnings.empty());
    REQUIRE(merged.failure == nullptr);
    CHECK(config.menu.escape == std::vector{Key::Escape});
    const auto expected = [](std::string_view name) {
        if (name == "shopExit") {
            return nlohmann::json::array({"Escape", "F5"});
        }
        return nlohmann::json::array({name == "escape" ? "Escape" : "F5"});
    };
    const auto saved = nlohmann::json::parse(config.toJson());
    const auto& controls = saved.at("controls");
    for (const auto* lists : {&controls.at("keyboard"), &controls.at("play").at("keyboard")}) {
        for (const auto& [name, keys] : lists->items()) {
            CAPTURE(name);
            CHECK(keys == expected(name));
        }
    }
    for (const auto& [id, binding] : controls.at("players").at(1).at("bindings").items()) {
        CAPTURE(id);
        CHECK(binding.at("keys") == expected(id));
    }
    // Escape is restored only by a file that says how the menus' keys are bound.
    GameConfig padOnly;
    padOnly.menu.escape = {Key::F1};
    CHECK(mergeLogged(padOnly, R"({"controls":{"pad":{}}})").failure == nullptr);
    CHECK(padOnly.menu.escape == std::vector{Key::F1});
}

TEST_CASE("a merge passes over what it does not know, and the save leaves it out",
          "[game][config][settings-file]") {
    GameConfig config;
    const auto merged = mergeLogged(config, R"({"bogus":{"a":1},
        "display":{"bogus":true,"vsync":false},
        "controls":{"keyboard":{"jump":["J"]},"pad":{"jump":["A"]},
            "play":{"keyboard":{"jump":["J"]},"pad":{"jump":["A"]},"bogus":2},
            "players":[{"colour":"red"},{"bindings":{"jump":{"keys":["J"]}}},{},{}]}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    // A profile with bindings is customized even when it names no binding there is.
    GameConfig expected;
    expected.display.vsync = false;
    expected.controls[1].customized = true;
    const auto saved = config.toJson();
    CHECK(saved == expected.toJson());
    for (const auto* unknown : {"bogus", "jump", "colour"}) {
        CAPTURE(unknown);
        CHECK(saved.find(unknown) == std::string::npos);
    }
    // Neither a file that is not an object nor sections that are not objects say anything.
    for (const auto* other : {"[]", "5", R"("settings")", "null", R"({"display":5,"audio":[]})"}) {
        CAPTURE(other);
        GameConfig left;
        const auto ignored = mergeLogged(left, other);
        CHECK(ignored.warnings.empty());
        CHECK(ignored.failure == nullptr);
        CHECK(left.toJson() == kDefaultSettingsFile);
    }
}

TEST_CASE("texture filtering and sample counts not offered fall back to their plainest",
          "[game][config][settings-file]") {
    const auto filteringAfter = [](std::string_view value) {
        GameConfig config;
        config.display.textureFiltering = 16;
        const auto merged =
            mergeLogged(config, R"({"display":{"textureFiltering":)" + std::string(value) + "}}");
        CHECK(merged.warnings.empty());
        CHECK(merged.failure == nullptr);
        return config.display.textureFiltering;
    };
    CHECK(filteringAfter("3") == 1);
    CHECK(filteringAfter("4.0") == 4);
    CHECK(filteringAfter(R"("4")") == 1);
    const auto samplesAfter = [](std::string_view value) {
        GameConfig config;
        config.display.sampleCount = 4;
        const auto merged =
            mergeLogged(config, R"({"display":{"sampleCount":)" + std::string(value) + "}}");
        CHECK(merged.warnings.empty());
        CHECK(merged.failure == nullptr);
        return config.display.sampleCount;
    };
    CHECK(samplesAfter("3") == 1);
    CHECK(samplesAfter("2.0") == 2);
    CHECK(samplesAfter(R"("2")") == 1);
}

TEST_CASE("a frame rate that is not a whole count is warned of and left as it was",
          "[game][config][settings-file]") {
    for (const auto* rate : {"-1", "60.0"}) {
        CAPTURE(rate);
        GameConfig config;
        config.display.maxFrameRate = 45;
        config.timing.gameplayFrameRate = 50;
        const auto merged = mergeLogged(
            config, R"({"display":{"maxFrameRate":)" + std::string(rate) +
                        R"(},"timing":{"gameplayFrameRate":)" + std::string(rate) + "}}");
        CHECK(merged.warnings ==
              std::vector<std::string>{
                  "Config: invalid frame rate 'maxFrameRate' left unchanged",
                  "Config: invalid frame rate 'gameplayFrameRate' left unchanged"});
        CHECK(merged.failure == nullptr);
        CHECK(config.display.maxFrameRate == 45);
        CHECK(config.timing.gameplayFrameRate == 50);
    }
    GameConfig monitorPaced;
    const auto merged = mergeLogged(monitorPaced, R"({"display":{"maxFrameRate":0}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(monitorPaced.display.maxFrameRate == 0);
}

TEST_CASE("a binding of one name reads as a list of it, and a null one as an empty list",
          "[game][config][settings-file]") {
    GameConfig config;
    auto merged = mergeLogged(config, R"({"controls":{
        "keyboard":{"back":"Q","start":null,"select":{"first":"F"}},
        "pad":{"back":"X","start":null},
        "play":{"keyboard":{"attack":"T"},"pad":{"attack":null}}}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(config.menu.back == std::vector{Key::Q});
    CHECK(config.menu.start.empty());
    CHECK(config.menu.select == std::vector{Key::F}); // an object's values, as a list's
    CHECK(config.menu.padBack == std::vector{PadButton::X});
    CHECK(config.menu.padStart.empty());
    CHECK(config.play.attack == std::vector{Key::T});
    CHECK(config.play.padAttack.empty());
    GameConfig unknown;
    merged = mergeLogged(unknown, R"({"controls":{"keyboard":{"back":"Nope"}}})");
    CHECK(merged.warnings == std::vector<std::string>{"Config: unknown key 'Nope' for 'back'"});
    CHECK(merged.failure == nullptr);
    CHECK(unknown.menu.back.empty());
    GameConfig number;
    merged = mergeLogged(number, R"({"controls":{"keyboard":{"back":5}}})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "302"));
    CHECK(number.menu.back.empty());
}

TEST_CASE("the slot count is read unsigned: minus one wraps round and nought is refused",
          "[game][config][settings-file]") {
    GameConfig config;
    auto merged = mergeLogged(config, R"({"save":{"slots":-1}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(config.save.slots == std::numeric_limits<u32>::max());
    merged = mergeLogged(config, R"({"save":{"slots":2.9}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(config.save.slots == 2);
    merged = mergeLogged(config, R"({"save":{"slots":0}})");
    CHECK(merged.warnings.empty());
    CHECK(failureOf<FormatError>(merged) == "config sizes and the tick rate must be positive");
    CHECK(config.save.slots == 0);
}

TEST_CASE("a merge that fails part way keeps everything it read before the failure",
          "[game][config][settings-file]") {
    // A name that is not a string in the play keys: the menu keys and pad buttons before them
    // are read, the strafe list holds what came before the number, and nothing after it is.
    GameConfig config;
    config.play.padUp.clear();
    auto merged = mergeLogged(config, R"({"display":{"windowWidth":800},
        "game":{"difficulty":"hard"},
        "controls":{"keyboard":{"up":["I"]},"pad":{"up":["Y"]},
            "play":{"keyboard":{"attack":["Q"],"strafe":["Z",5],"strongAttack":["T"]},
                "pad":{"attack":["B"]},"stickDeadZone":0.5},
            "players":[{}, {"bindings":{"attack":{"keys":["P"]}}}, {}, {}]}})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "302"));
    GameConfig expected;
    expected.display.windowWidth = 800;
    expected.difficulty.level = "hard";
    expected.menu.up = {Key::I};
    expected.menu.padUp = {PadButton::Y};
    expected.play.attack = {Key::Q};
    expected.play.strafe = {Key::Z};
    expected.play.padUp.clear();
    CHECK(config.toJson() == expected.toJson());

    // A window mode that is not a string, between the sizes and the monitor.
    GameConfig mode;
    merged = mergeLogged(
        mode,
        R"({"display":{"virtualWidth":640,"windowMode":1,"monitor":2},"timing":{"tickRate":30}})");
    CHECK(merged.warnings.empty());
    CHECK(failureOf<FormatError>(merged) == "unknown window mode");
    expected = GameConfig{};
    expected.display.virtualWidth = 640;
    CHECK(mode.toJson() == expected.toJson());

    // A switch that is not a boolean, between the monitor and the effects.
    GameConfig vsync;
    merged = mergeLogged(
        vsync, R"({"display":{"monitor":2,"vsync":"yes","bloom":true},"timing":{"tickRate":30}})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "302"));
    expected = GameConfig{};
    expected.display.monitor = 2;
    CHECK(vsync.toJson() == expected.toJson());

    // An explicit-movement word that is not a boolean, read after the bindings and profiles:
    // the stick is not added and the windows are not checked, so a refused value is kept.
    GameConfig movement;
    merged = mergeLogged(movement, R"({"controls":{"explicitMovement":"yes",
        "keyboard":{"up":["I"]},"play":{"pad":{"up":[]},"magicHoldSeconds":0},
        "players":[{"device":"keyboard"}, {}, {}, {}]}})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "302"));
    expected = GameConfig{};
    expected.menu.up = {Key::I};
    expected.play.padUp.clear();
    expected.play.magicHoldSeconds = 0.0f;
    expected.controls[0].device = "keyboard";
    CHECK(movement.toJson() == expected.toJson());

    // Controls that are not an object: asked whether movement is explicit, they throw.
    GameConfig listed;
    merged = mergeLogged(listed, R"({"display":{"windowWidth":800},"controls":[]})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "306"));
    expected = GameConfig{};
    expected.display.windowWidth = 800;
    CHECK(listed.toJson() == expected.toJson());

    // The checks come last and keep what they refuse.
    GameConfig refused;
    merged = mergeLogged(
        refused, R"({"display":{"virtualWidth":0},"controls":{"play":{"magicHoldSeconds":0}}})");
    CHECK(merged.warnings.empty());
    CHECK(failureOf<FormatError>(merged) == "magic gesture windows must be in (0, 2] seconds");
    CHECK(refused.display.virtualWidth == 0);
    CHECK(refused.play.magicHoldSeconds == 0.0f);
    GameConfig wide;
    merged = mergeLogged(wide, R"({"display":{"windowWidth":40000}})");
    CHECK(merged.warnings.empty());
    CHECK(failureOf<FormatError>(merged) == "window dimensions exceed platform limits");
    CHECK(wide.display.windowWidth == 40000);
}

TEST_CASE("a profile refuses a binding name it does not know, where the shared bindings skip it",
          "[game][config][settings-file]") {
    GameConfig shared;
    auto merged = mergeLogged(shared, R"({"controls":{
        "keyboard":{"up":["Nope","I"]},"pad":{"up":["Nope","Y"]},
        "play":{"keyboard":{"attack":["Nope","Q"]},"pad":{"attack":["Nope","B"]}}}})");
    const std::vector<std::string> skipped{"Config: unknown key 'Nope' for 'up'",
                                           "Config: unknown pad button 'Nope' for 'up'",
                                           "Config: unknown key 'Nope' for 'attack'",
                                           "Config: unknown pad button 'Nope' for 'attack'"};
    CHECK(merged.failure == nullptr);
    CHECK(merged.warnings == skipped);
    CHECK(shared.menu.up == std::vector{Key::I});
    CHECK(shared.menu.padUp == std::vector{PadButton::Y});
    CHECK(shared.play.attack == std::vector{Key::Q});
    CHECK(shared.play.padAttack == std::vector{PadButton::B});
    GameConfig restored;
    merged = mergeLogged(restored, shared.toJson());
    CHECK(merged.warnings.empty());
    CHECK(restored.toJson() == shared.toJson());

    // The profiles are taken whole or not at all, after the shared bindings.
    for (const auto* binding : {R"("keys":["Nope"])", R"("buttons":["Nope"])"}) {
        CAPTURE(binding);
        GameConfig profiled;
        merged = mergeLogged(profiled, R"({"controls":{"keyboard":{"up":["I"]},
            "players":[{"device":"keyboard"}, {"bindings":{"attack":{)" +
                                           std::string(binding) + "}}}, {}, {}]}}");
        CHECK(merged.warnings.empty());
        CHECK(failureOf<FormatError>(merged) == (std::string_view{binding}.starts_with(R"("keys")")
                                                     ? "unknown control key"
                                                     : "unknown control button"));
        CHECK(profiled.menu.up == std::vector{Key::I});
        CHECK(profiled.controls[0].device.empty());
        CHECK_FALSE(profiled.controls[1].customized);
    }
    GameConfig number;
    merged = mergeLogged(
        number, R"({"controls":{"players":[{}, {"bindings":{"attack":{"keys":[5]}}}, {}, {}]}})");
    CHECK(merged.warnings.empty());
    CHECK(threwTypeError(merged, "302"));
    CHECK_FALSE(number.controls[1].customized);
    GameConfig few;
    merged = mergeLogged(few, R"({"controls":{"players":[{}]}})");
    CHECK(merged.warnings.empty());
    CHECK(failureOf<FormatError>(merged) == "controls.players must contain four profiles");

    // One name stands for a list of it there too, and null for none; both are kept.
    GameConfig lone;
    merged = mergeLogged(
        lone,
        R"({"controls":{"players":[{}, {"bindings":{"attack":{"keys":"P","buttons":null}}}, {}, {}]}})");
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(lone.controls[1].play.attack == std::vector{Key::P});
    CHECK(lone.controls[1].play.padAttack.empty());
    GameConfig again;
    merged = mergeLogged(again, lone.toJson());
    CHECK(merged.warnings.empty());
    CHECK(merged.failure == nullptr);
    CHECK(again.toJson() == lone.toJson());
}

} // namespace
