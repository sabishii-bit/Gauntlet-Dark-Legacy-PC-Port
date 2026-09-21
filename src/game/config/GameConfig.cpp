#include "game/config/GameConfig.h"

#include <cstdlib>
#include <exception>
#include <numbers>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl::game {

namespace {

constexpr std::string_view kSettingsFolder = "GauntletDarkLegacy";
constexpr std::string_view kSettingsFile = "settings.json";

using Json = nlohmann::json;

template <typename T> void read(const Json& object, const char* key, T& value) {
    if (object.contains(key)) {
        value = object.at(key).get<T>();
    }
}

void readKeys(const Json& object, const char* key, std::vector<Key>& keys) {
    if (!object.contains(key)) {
        return;
    }
    keys.clear();
    for (const Json& name : object.at(key)) {
        const auto parsed = keyFromName(name.get<std::string>());
        if (parsed.has_value()) {
            keys.push_back(*parsed);
        } else {
            log::warn("Config: unknown key '{}' for '{}'", name.get<std::string>(), key);
        }
    }
}

void readButtons(const Json& object, const char* key, std::vector<PadButton>& buttons) {
    if (!object.contains(key)) {
        return;
    }
    buttons.clear();
    for (const Json& name : object.at(key)) {
        const auto parsed = padButtonFromName(name.get<std::string>());
        if (parsed.has_value()) {
            buttons.push_back(*parsed);
        } else {
            log::warn("Config: unknown pad button '{}' for '{}'", name.get<std::string>(), key);
        }
    }
}

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

std::filesystem::path environmentPath(const char* variable) {
    const char* value = std::getenv(variable); // NOLINT(concurrency-mt-unsafe): read at startup
    return value == nullptr ? std::filesystem::path{} : std::filesystem::path(value);
}

} // namespace

bool GameConfig::loadFile(const std::filesystem::path& file) {
    try {
        const std::vector<u8> bytes = readFile(file);
        mergeJson(std::string(bytes.begin(), bytes.end()));
        return true;
    } catch (const std::exception& e) {
        log::warn("Config {}: {}", file.string(), e.what());
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
    if (root.contains("display")) {
        const Json& d = root.at("display");
        read(d, "virtualWidth", display.virtualWidth);
        read(d, "virtualHeight", display.virtualHeight);
        read(d, "frameWidth", display.frameWidth);
        read(d, "frameHeight", display.frameHeight);
        read(d, "windowWidth", display.windowWidth);
        read(d, "windowHeight", display.windowHeight);
        read(d, "vsync", display.vsync);
        read(d, "maxFrameRate", display.maxFrameRate);
    }
    if (root.contains("timing")) {
        const Json& t = root.at("timing");
        read(t, "tickRate", timing.tickRate);
        read(t, "gameplayFrameRate", timing.gameplayFrameRate);
    }
    if (root.contains("camera")) {
        read(root.at("camera"), "horizontalFovDegrees", camera.horizontalFovDegrees);
    }
    if (root.contains("audio")) {
        const Json& a = root.at("audio");
        read(a, "masterVolume", audio.masterVolume);
        read(a, "musicVolume", audio.musicVolume);
        read(a, "effectsVolume", audio.effectsVolume);
    }
    if (root.contains("text")) {
        read(root.at("text"), "language", text.language);
    }
    if (root.contains("save")) {
        const Json& s = root.at("save");
        read(s, "directory", save.directory);
        read(s, "slots", save.slots);
    }
    if (root.contains("controls")) {
        const Json& c = root.at("controls");
        if (c.contains("keyboard")) {
            const Json& k = c.at("keyboard");
            readKeys(k, "up", menu.up);
            readKeys(k, "down", menu.down);
            readKeys(k, "left", menu.left);
            readKeys(k, "right", menu.right);
            readKeys(k, "select", menu.select);
            readKeys(k, "back", menu.back);
            readKeys(k, "start", menu.start);
            readKeys(k, "escape", menu.escape);
        }
        if (c.contains("pad")) {
            const Json& p = c.at("pad");
            readButtons(p, "up", menu.padUp);
            readButtons(p, "down", menu.padDown);
            readButtons(p, "left", menu.padLeft);
            readButtons(p, "right", menu.padRight);
            readButtons(p, "select", menu.padSelect);
            readButtons(p, "back", menu.padBack);
            readButtons(p, "start", menu.padStart);
        }
        if (c.contains("play")) {
            const Json& moves = c.at("play");
            if (moves.contains("keyboard")) {
                const Json& k = moves.at("keyboard");
                readKeys(k, "up", play.up);
                readKeys(k, "down", play.down);
                readKeys(k, "left", play.left);
                readKeys(k, "right", play.right);
                readKeys(k, "attack", play.attack);
            }
            if (moves.contains("pad")) {
                const Json& p = moves.at("pad");
                readButtons(p, "up", play.padUp);
                readButtons(p, "down", play.padDown);
                readButtons(p, "left", play.padLeft);
                readButtons(p, "right", play.padRight);
                readButtons(p, "attack", play.padAttack);
            }
            read(moves, "stickDeadZone", play.stickDeadZone);
        }
    }
    if (save.slots == 0 || timing.tickRate == 0 || display.virtualWidth == 0 ||
        display.virtualHeight == 0 || display.frameWidth == 0 || display.frameHeight == 0 ||
        display.windowWidth == 0 || display.windowHeight == 0) {
        throw FormatError("config sizes and the tick rate must be positive");
    }
}

std::string GameConfig::toJson() const {
    Json root;
    root["display"] = {{"virtualWidth", display.virtualWidth},
                       {"virtualHeight", display.virtualHeight},
                       {"frameWidth", display.frameWidth},
                       {"frameHeight", display.frameHeight},
                       {"windowWidth", display.windowWidth},
                       {"windowHeight", display.windowHeight},
                       {"vsync", display.vsync},
                       {"maxFrameRate", display.maxFrameRate}};
    root["timing"] = {{"tickRate", timing.tickRate},
                      {"gameplayFrameRate", timing.gameplayFrameRate}};
    root["camera"] = {{"horizontalFovDegrees", camera.horizontalFovDegrees}};
    root["audio"] = {{"masterVolume", audio.masterVolume},
                     {"musicVolume", audio.musicVolume},
                     {"effectsVolume", audio.effectsVolume}};
    root["text"] = {{"language", text.language}};
    root["save"] = {{"directory", save.directory}, {"slots", save.slots}};
    root["controls"] = {{"keyboard",
                         {{"up", keyNames(menu.up)},
                          {"down", keyNames(menu.down)},
                          {"left", keyNames(menu.left)},
                          {"right", keyNames(menu.right)},
                          {"select", keyNames(menu.select)},
                          {"back", keyNames(menu.back)},
                          {"start", keyNames(menu.start)},
                          {"escape", keyNames(menu.escape)}}},
                        {"pad",
                         {{"up", buttonNames(menu.padUp)},
                          {"down", buttonNames(menu.padDown)},
                          {"left", buttonNames(menu.padLeft)},
                          {"right", buttonNames(menu.padRight)},
                          {"select", buttonNames(menu.padSelect)},
                          {"back", buttonNames(menu.padBack)},
                          {"start", buttonNames(menu.padStart)}}},
                        {"play",
                         {{"keyboard",
                           {{"up", keyNames(play.up)},
                            {"down", keyNames(play.down)},
                            {"left", keyNames(play.left)},
                            {"right", keyNames(play.right)},
                            {"attack", keyNames(play.attack)}}},
                          {"pad",
                           {{"up", buttonNames(play.padUp)},
                            {"down", buttonNames(play.padDown)},
                            {"left", buttonNames(play.padLeft)},
                            {"right", buttonNames(play.padRight)},
                            {"attack", buttonNames(play.padAttack)}}},
                          {"stickDeadZone", play.stickDeadZone}}}};
    return root.dump(2) + "\n";
}

void GameConfig::saveFile(const std::filesystem::path& file) const {
    std::filesystem::create_directories(file.parent_path());
    writeTextFile(file, toJson());
}

f32 GameConfig::horizontalFovRadians() const {
    return camera.horizontalFovDegrees * (std::numbers::pi_v<f32> / 180.0f);
}

std::filesystem::path GameConfig::saveDirectory() const {
    if (!save.directory.empty()) {
        return {save.directory};
    }
    return userSettingsPath().parent_path() / "saves";
}

std::filesystem::path GameConfig::userSettingsPath() {
#if defined(_WIN32)
    std::filesystem::path base = environmentPath("APPDATA");
#else
    std::filesystem::path base = environmentPath("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::filesystem::path home = environmentPath("HOME");
        if (!home.empty()) {
            base = home / ".config";
        }
    }
#endif
    if (base.empty()) {
        base = std::filesystem::current_path();
    }
    return base / kSettingsFolder / kSettingsFile;
}

} // namespace gdl::game
