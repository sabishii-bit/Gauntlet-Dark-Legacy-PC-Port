#include "game/config/GameConfig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/platform/DisplayMode.h"
#include "engine/platform/Input.h"
#include "engine/platform/Paths.h"
#include "engine/render/RenderTypes.h"

#include "game/config/BindingNames.h"
#include "game/config/ControlProfiles.h"

namespace gdl::game {

bool DisplayConfig::matchesWindow(WindowMode mode, Extent2D size) const {
    return windowMode == mode && (mode == WindowMode::BorderlessFullscreen ||
                                  size == Extent2D{windowWidth, windowHeight});
}

namespace {

constexpr std::string_view kSettingsFolder = "GauntletDarkLegacy";
constexpr std::string_view kSettingsFile = "settings.json";
constexpr std::string_view kSavesFolder = "saves";

using Json = nlohmann::json;

/** The mechanics a file switches on or off, by id. */
using Switches = std::map<std::string, bool, std::less<>>;

/** The window modes' names in the settings, in enum order. */
constexpr std::array<std::string_view, 3> kWindowModeNames{"windowed", "fullscreen", "borderless"};
static_assert(kWindowModeNames.size() == static_cast<usize>(WindowMode::BorderlessFullscreen) + 1);

/** The texture filtering a file may choose: off, trilinear, or 2x to 16x anisotropic. */
constexpr std::array<u32, 6> kTextureFilterings{0, 1, 2, 4, 8, 16};
/** The sample counts a file may choose besides one. */
constexpr std::array<u32, 2> kSampleCounts{2, 4};
/** What a filtering or sample count not offered becomes: trilinear, or one sample. */
constexpr u32 kPlainestChoice = 1;

/** A binding as the settings file lists it: the name it goes by among the keys and among the
 * pad buttons, and where each list is kept. */
template <class Bindings> struct BindingRow {
    const char* key = "";
    std::vector<Key> Bindings::*keys = nullptr;
    std::vector<PadButton> Bindings::*buttons = nullptr;
    bool keepsEscape = false; ///< Escape may be among its keys
};
using MenuRow = BindingRow<MenuBindings>;
using PlayRow = BindingRow<PlayBindings>;

/** The menus' bindings, in the order a merge reads each list. */
constexpr std::array kMenuRows{
    MenuRow{"up", &MenuBindings::up, &MenuBindings::padUp},
    MenuRow{"down", &MenuBindings::down, &MenuBindings::padDown},
    MenuRow{"left", &MenuBindings::left, &MenuBindings::padLeft},
    MenuRow{"right", &MenuBindings::right, &MenuBindings::padRight},
    MenuRow{"select", &MenuBindings::select, &MenuBindings::padSelect},
    MenuRow{"back", &MenuBindings::back, &MenuBindings::padBack},
    MenuRow{"start", &MenuBindings::start, &MenuBindings::padStart},
};

/** The shop's own bindings, read after the menus' and, among the keys, the fixed Escape. */
constexpr std::array kShopRows{
    MenuRow{"shopSell", &MenuBindings::shopSell, &MenuBindings::padShopSell},
    MenuRow{.key = "shopExit",
            .keys = &MenuBindings::shopExit,
            .buttons = &MenuBindings::padShopExit,
            .keepsEscape = true},
};

/** The play bindings, in the order a merge reads each list. */
constexpr std::array kPlayRows{
    PlayRow{"up", &PlayBindings::up, &PlayBindings::padUp},
    PlayRow{"down", &PlayBindings::down, &PlayBindings::padDown},
    PlayRow{"left", &PlayBindings::left, &PlayBindings::padLeft},
    PlayRow{"right", &PlayBindings::right, &PlayBindings::padRight},
    PlayRow{"attack", &PlayBindings::attack, &PlayBindings::padAttack},
    PlayRow{"usePotion", &PlayBindings::usePotion, &PlayBindings::padUsePotion},
    PlayRow{"throwPotion", &PlayBindings::throwPotion, &PlayBindings::padThrowPotion},
    PlayRow{"shieldPotion", &PlayBindings::shieldPotion, &PlayBindings::padShieldPotion},
    PlayRow{"strafe", &PlayBindings::strafe, &PlayBindings::padStrafe},
    PlayRow{"strongAttack", &PlayBindings::strongAttack, &PlayBindings::padStrongAttack},
    PlayRow{"turbo", &PlayBindings::turbo, &PlayBindings::padTurbo},
    PlayRow{"charge", &PlayBindings::charge, &PlayBindings::padCharge},
    PlayRow{"combo", &PlayBindings::combo, &PlayBindings::padCombo},
    PlayRow{"selectorUp", &PlayBindings::selectorUp, &PlayBindings::padSelectorUp},
    PlayRow{"selectorDown", &PlayBindings::selectorDown, &PlayBindings::padSelectorDown},
    PlayRow{"selectorLeft", &PlayBindings::selectorLeft, &PlayBindings::padSelectorLeft},
    PlayRow{"selectorRight", &PlayBindings::selectorRight, &PlayBindings::padSelectorRight},
};

Json keyNames(const std::vector<Key>& keys) {
    Json names = Json::array();
    for (const Key key : keys) {
        names.push_back(std::string(keyName(key)));
    }
    return names;
}

Json buttonNames(const std::vector<PadButton>& buttons) {
    Json names = Json::array();
    for (const PadButton button : buttons) {
        names.push_back(std::string(padButtonName(button)));
    }
    return names;
}

/** Reads a merge's settings out of one object of the file, each only when the file has it. A
 * value of the wrong type throws nlohmann's type_error, keeping everything read before it. */
class SettingsReader {
public:
    static constexpr bool kReads = true;

    explicit SettingsReader(const Json& object) : m_object(object) {}

    /** Reads the section `name` through `visit` when the file has it, even when it is not an
     * object: then no value is found in it. */
    template <class Visit> void section(const char* name, const Visit& visit) const {
        if (m_object.contains(name)) {
            SettingsReader inner{m_object.at(name)};
            visit(inner);
        }
    }

    /** Reads `field` as get converts it: a negative count wraps and a fraction truncates. */
    template <class T> void value(const char* name, T& field) const {
        if (m_object.contains(name)) {
            field = m_object.at(name).get<T>();
        }
    }

    /** Reads a whole number of frames a second, warning of anything else and keeping `rate`. */
    void frameRate(const char* name, u32& rate) const {
        if (!m_object.contains(name)) {
            return;
        }
        const auto& given = m_object.at(name);
        if (given.is_number_integer() && given >= 0 && given <= std::numeric_limits<u32>::max()) {
            rate = given.get<u32>();
        } else {
            log::warn("Config: invalid frame rate '{}' left unchanged", name);
        }
    }

    /** Reads one of `choices`, or kPlainestChoice for anything else. */
    void offered(const char* name, u32& choice, std::span<const u32> choices) const {
        if (!m_object.contains(name)) {
            return;
        }
        choice = kPlainestChoice;
        for (const u32 offer : choices) {
            if (m_object.at(name) == offer) {
                choice = offer;
            }
        }
    }

    /** Reads a mode by its name among `names`, in enum order, refusing anything else. */
    template <class Mode>
    void named(const char* name, Mode& mode, std::span<const std::string_view> names,
               const char* refusal) const {
        if (!m_object.contains(name)) {
            return;
        }
        const auto& given = m_object.at(name);
        if (!given.is_string()) {
            throw FormatError(refusal);
        }
        const auto chosen = std::ranges::find(names, given.get<std::string>());
        if (chosen == names.end()) {
            throw FormatError(refusal);
        }
        mode = static_cast<Mode>(std::distance(names.begin(), chosen));
    }

    /** Reads switches named true or false in place of all those `held`, refusing a section that
     * is not an object or a switch that is not a boolean, with those before it kept. */
    void switches(const char* name, Switches& held, const char* refusal) const {
        if (!m_object.contains(name)) {
            return;
        }
        const auto& given = m_object.at(name);
        if (!given.is_object()) {
            throw FormatError(refusal);
        }
        held.clear();
        for (const auto& [id, on] : given.items()) {
            if (!on.is_boolean()) {
                throw FormatError(refusal);
            }
            held[id] = on.get<bool>();
        }
    }

    /** Reads a key list, warning of a name it does not know and reading on. */
    void keys(const char* name, std::vector<Key>& list, bool keepsEscape = false) const {
        if (m_object.contains(name)) {
            readKeyNames(m_object.at(name), name, UnknownBinding::WarnAndSkip, keepsEscape, list);
        }
    }

    /** Reads a pad button list, warning of a name it does not know and reading on. */
    void buttons(const char* name, std::vector<PadButton>& list) const {
        if (m_object.contains(name)) {
            readButtonNames(m_object.at(name), name, UnknownBinding::WarnAndSkip, list);
        }
    }

    /** Reads the players' profiles over the shared bindings read before them. */
    void profiles(const char* name, GameConfig& config) const {
        if (m_object.contains(name)) {
            readControlProfiles(m_object.at(name), config);
        }
    }

    /** Whether the section says `name` is true. Throws when the section is not an object or the
     * value is not a boolean. */
    bool says(const char* name) const { return m_object.value(name, false); }

private:
    const Json& m_object;
};

/** Writes every setting into one object of the file, in the type the reader takes it in. */
class SettingsWriter {
public:
    static constexpr bool kReads = false;

    explicit SettingsWriter(Json& object) : m_object(object) {}

    /** Writes the section `name` through `visit`. */
    template <class Visit> void section(const char* name, const Visit& visit) {
        SettingsWriter inner{m_object[name]};
        visit(inner);
    }

    template <class T> void value(const char* name, const T& field) { m_object[name] = field; }

    void frameRate(const char* name, u32 rate) { m_object[name] = rate; }

    void offered(const char* name, u32 choice, std::span<const u32> /*choices*/) {
        m_object[name] = choice;
    }

    /** Writes a mode's name; a mode past the names, which no reading gives, as the first. */
    template <class Mode>
    void named(const char* name, Mode mode, std::span<const std::string_view> names,
               const char* /*refusal*/) {
        const auto index = static_cast<usize>(mode);
        m_object[name] = index < names.size() ? names[index] : names.front();
    }

    void switches(const char* name, const Switches& held, const char* /*refusal*/) {
        Json written = Json::object();
        for (const auto& [id, on] : held) {
            written[id] = on;
        }
        m_object[name] = std::move(written);
    }

    void keys(const char* name, const std::vector<Key>& list, bool /*keepsEscape*/ = false) {
        m_object[name] = keyNames(list);
    }

    void buttons(const char* name, const std::vector<PadButton>& list) {
        m_object[name] = buttonNames(list);
    }

    void profiles(const char* name, const GameConfig& config) {
        m_object[name] = writeControlProfiles(config);
    }

private:
    Json& m_object;
};

/** Reads or writes each row's key list. */
template <class Io, class Rows, class Bindings>
void visitKeys(Io& io, const Rows& rows, Bindings& bindings) {
    for (const auto& row : rows) {
        io.keys(row.key, bindings.*row.keys, row.keepsEscape);
    }
}

/** Reads or writes each row's pad button list. */
template <class Io, class Rows, class Bindings>
void visitButtons(Io& io, const Rows& rows, Bindings& bindings) {
    for (const auto& row : rows) {
        io.buttons(row.key, bindings.*row.buttons);
    }
}

/** Adds the left stick to walking on a pad, in the shared bindings and every customized
 * profile, for a file that does not say its lists hold the stick themselves. */
void addStickToWalking(GameConfig& config) {
    // Older saves added the left stick outside the binding lists. Preserve that
    // input when importing them, while allowing new profiles to remap it completely.
    const auto migrate = [](PlayBindings& bindings) {
        const auto add = [](std::vector<PadButton>& buttons, PadButton direction) {
            if (std::ranges::find(buttons, direction) == buttons.end()) {
                buttons.push_back(direction);
            }
        };
        add(bindings.padUp, PadButton::LeftStickUp);
        add(bindings.padDown, PadButton::LeftStickDown);
        add(bindings.padLeft, PadButton::LeftStickLeft);
        add(bindings.padRight, PadButton::LeftStickRight);
    };
    migrate(config.play);
    for (auto& profile : config.controls) {
        if (profile.customized) {
            migrate(profile.play);
        }
    }
}

/** Reads (SettingsReader) or writes (SettingsWriter) every setting, section by section and
 * value by value in the order a merge reads them: what a merge that fails part way keeps
 * depends on it. */
template <class Io, class Config> void visitSettings(Io& io, Config& config) {
    io.section("display", [&](Io& display) {
        display.named("renderer", config.display.renderer, kGraphicsApiNames,
                      "renderer must be vulkan or opengl");
        display.value("virtualWidth", config.display.virtualWidth);
        display.value("virtualHeight", config.display.virtualHeight);
        display.value("frameWidth", config.display.frameWidth);
        display.value("frameHeight", config.display.frameHeight);
        display.value("windowWidth", config.display.windowWidth);
        display.value("windowHeight", config.display.windowHeight);
        display.named("windowMode", config.display.windowMode, kWindowModeNames,
                      "unknown window mode");
        display.value("monitor", config.display.monitor);
        display.value("vsync", config.display.vsync);
        display.value("depthOfField", config.display.depthOfField);
        display.value("bloom", config.display.bloom);
        display.value("ambientOcclusion", config.display.ambientOcclusion);
        display.value("smoothSprites", config.display.smoothSprites);
        display.value("authoredSilhouettes", config.display.authoredSilhouettes);
        display.offered("textureFiltering", config.display.textureFiltering, kTextureFilterings);
        display.offered("sampleCount", config.display.sampleCount, kSampleCounts);
        display.frameRate("maxFrameRate", config.display.maxFrameRate);
    });
    io.section("timing", [&](Io& timing) {
        timing.value("tickRate", config.timing.tickRate);
        timing.frameRate("gameplayFrameRate", config.timing.gameplayFrameRate);
    });
    io.section("camera", [&](Io& camera) {
        camera.value("horizontalFovDegrees", config.camera.horizontalFovDegrees);
        camera.value("compass", config.camera.compass);
    });
    io.section("audio", [&](Io& audio) {
        audio.value("masterVolume", config.audio.masterVolume);
        audio.value("musicVolume", config.audio.musicVolume);
        audio.value("effectsVolume", config.audio.effectsVolume);
        audio.value("movieVolume", config.audio.movieVolume);
        audio.value("stereo", config.audio.stereo);
    });
    io.section("text", [&](Io& text) { text.value("language", config.text.language); });
    io.section("save", [&](Io& save) {
        save.value("directory", config.save.directory);
        save.value("slots", config.save.slots);
    });
    io.section("game", [&](Io& game) {
        game.value("expandedShop", config.game.expandedShop);
        game.value("autoSave", config.game.autoSave);
        game.switches("mechanics", config.game.mechanics,
                      "game.mechanics must name each mechanic true or false");
        game.value("difficulty", config.difficulty.level);
        game.section("combat", [&](Io& combat) {
            combat.value("autoMelee", config.combat.autoMelee);
            combat.value("enemyHealthBars", config.combat.enemyHealthBars);
            combat.value("autoActivateItems", config.combat.autoActivateItems);
            combat.value("autoAim", config.combat.autoAim);
        });
        game.named("multiplayer", config.multiplayer.mode, MultiplayerConfig::kNames,
                   "multiplayer mode must be normal, stun or hurt");
        game.value("soloCamera", config.multiplayer.soloCamera);
    });
    io.section("controls", [&](Io& controls) {
        controls.section("keyboard", [&](Io& keyboard) {
            visitKeys(keyboard, kMenuRows, config.menu);
            // A fixed safety control, not a configurable action.
            if constexpr (Io::kReads) {
                config.menu.escape = {Key::Escape};
            } else {
                keyboard.keys("escape", config.menu.escape);
            }
            visitKeys(keyboard, kShopRows, config.menu);
            if constexpr (Io::kReads) {
                keyboard.keys("debug", config.menu.console); // the console key's former name
            }
            keyboard.keys("console", config.menu.console);
        });
        controls.section("pad", [&](Io& pad) {
            visitButtons(pad, kMenuRows, config.menu);
            visitButtons(pad, kShopRows, config.menu);
        });
        controls.section("play", [&](Io& play) {
            play.section("keyboard",
                         [&](Io& keyboard) { visitKeys(keyboard, kPlayRows, config.play); });
            play.section("pad", [&](Io& pad) { visitButtons(pad, kPlayRows, config.play); });
            play.value("stickDeadZone", config.play.stickDeadZone);
            play.value("magicHoldSeconds", config.play.magicHoldSeconds);
            play.value("magicDoubleTapSeconds", config.play.magicDoubleTapSeconds);
            play.value("padMagicGestures", config.play.padMagicGestures);
            play.value("keyboardMagicGestures", config.play.keyboardMagicGestures);
            play.value("actionChords", config.play.actionChords);
        });
        controls.profiles("players", config);
        // A file says when its lists hold the stick themselves, and every file written does.
        if constexpr (Io::kReads) {
            if (!controls.says("explicitMovement")) {
                addStickToWalking(config);
            }
        } else {
            controls.value("explicitMovement", true);
        }
    });
}

} // namespace

bool GameConfig::loadFile(const std::filesystem::path& file) {
    try {
        const std::vector<u8> bytes = readFile(file);
        mergeJson(std::string(bytes.begin(), bytes.end()));
        return true;
    } catch (const std::exception& e) {
        log::warn("Config {}: {}", paths::utf8(file), e.what());
        return false;
    }
}

void GameConfig::mergeJson(std::string_view json) {
    Json root;
    try {
        root = Json::parse(json);
    } catch (const std::exception& e) {
        throw FormatError(e.what());
    }
    SettingsReader reader{root};
    visitSettings(reader, *this);
    if (!std::isfinite(play.magicHoldSeconds) || play.magicHoldSeconds <= 0.0f ||
        play.magicHoldSeconds > 2.0f || !std::isfinite(play.magicDoubleTapSeconds) ||
        play.magicDoubleTapSeconds <= 0.0f || play.magicDoubleTapSeconds > 2.0f) {
        throw FormatError("magic gesture windows must be in (0, 2] seconds");
    }
    if (save.slots == 0 || timing.tickRate == 0 || display.virtualWidth == 0 ||
        display.virtualHeight == 0 || display.frameWidth == 0 || display.frameHeight == 0 ||
        display.windowWidth == 0 || display.windowHeight == 0) {
        throw FormatError("config sizes and the tick rate must be positive");
    }
    if (display.windowWidth > 32768 || display.windowHeight > 32768) {
        throw FormatError("window dimensions exceed platform limits");
    }
}

std::string GameConfig::toJson() const {
    Json root;
    SettingsWriter writer{root};
    visitSettings(writer, *this);
    return root.dump(2) + "\n";
}

void GameConfig::saveFile(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    replaceTextFile(file, toJson());
}

f32 DifficultyConfig::gain() const {
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto named = std::ranges::find(kNames, level);
    return named != kNames.end() ? kGains[static_cast<usize>(named - kNames.begin())] : 1.0f;
}

f32 GameConfig::horizontalFovRadians() const {
    return camera.horizontalFovDegrees * (std::numbers::pi_v<f32> / 180.0f);
}

std::filesystem::path GameConfig::saveDirectory() const {
    return saveDirectory(paths::executableDirectory());
}

std::filesystem::path GameConfig::saveDirectory(const std::filesystem::path& gameDirectory) const {
    if (save.directory.empty()) {
        return gameDirectory / kSavesFolder;
    }
    // The settings hold the name as UTF-8, whatever the system code page.
    const std::filesystem::path named = paths::fromUtf8(save.directory);
    return named.is_absolute() ? named : gameDirectory / named;
}

std::filesystem::path GameConfig::userSettingsPath() {
    return userSettingsPath(paths::executableDirectory());
}

std::filesystem::path GameConfig::userSettingsPath(const std::filesystem::path& gameDirectory) {
    return gameDirectory / "config" / kSettingsFile;
}

bool GameConfig::loadUserSettings() {
#if defined(_WIN32)
    const std::filesystem::path base = paths::environment("APPDATA");
#else
    std::filesystem::path base = paths::environment("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::filesystem::path home = paths::environment("HOME");
        if (!home.empty()) {
            base = home / ".config";
        }
    }
#endif
    if (base.empty()) {
        return loadUserSettings(paths::executableDirectory(), {});
    }
    return loadUserSettings(paths::executableDirectory(), base / kSettingsFolder / kSettingsFile);
}

bool GameConfig::loadUserSettings(const std::filesystem::path& gameDirectory,
                                  const std::filesystem::path& legacyFile) {
    const auto local = userSettingsPath(gameDirectory);
    try {
        // A damaged local file must not silently be replaced by stale settings.
        const bool installed = std::filesystem::exists(local);
        if (!installed && (legacyFile.empty() || !std::filesystem::is_regular_file(legacyFile))) {
            return false;
        }
        auto loaded = *this;
        if (!loaded.loadFile(installed ? local : legacyFile)) {
            return false;
        }
        if (!installed) {
            std::filesystem::create_directories(local.parent_path());
            // Another game instance may have created local settings since the
            // existence check. Never replace that instance's settings during migration.
            if (std::filesystem::copy_file(legacyFile, local,
                                           std::filesystem::copy_options::skip_existing)) {
                log::info("Imported existing settings into {}", paths::utf8(local));
            }
            loaded = *this;
            if (!loaded.loadFile(local)) {
                return false;
            }
        }
        *this = std::move(loaded);
        return true;
    } catch (const std::exception& error) {
        log::warn("Could not load local settings {}: {}", paths::utf8(local), error.what());
        return false;
    }
}

} // namespace gdl::game
