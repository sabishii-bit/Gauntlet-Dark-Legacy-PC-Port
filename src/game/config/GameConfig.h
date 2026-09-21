#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/core/Types.h"
#include "engine/platform/Input.h"

namespace gdl::game {

struct DisplayConfig {
    u32 virtualWidth = 512; ///< the 2D layer's coordinate space
    u32 virtualHeight = 384;
    u32 frameWidth = 640; ///< the frame the game composes, letterboxed onto the window
    u32 frameHeight = 448;
    u32 windowWidth = 1280;
    u32 windowHeight = 896;
    bool vsync = true;
    u32 maxFrameRate = 60; ///< frames per second outside play (the menus); 0 leaves it to vsync
};

struct TimingConfig {
    u32 tickRate = 60;          ///< logic ticks per second; the original counts 60 Hz retraces
    u32 gameplayFrameRate = 30; ///< the rate gameplay was tuned for: two ticks per frame
};

struct CameraConfig {
    f32 horizontalFovDegrees = 60.0f;
};

struct AudioConfig {
    f32 masterVolume = 1.0f;
    f32 musicVolume = 0.7f;
    f32 effectsVolume = 1.0f;
};

struct TextConfig {
    std::string language = "en";
};

/** Where characters are saved; an empty directory means beside the user's settings. */
struct SaveConfig {
    std::string directory;
    u32 slots = 8;
};

/** Keys and pad buttons that drive the menus. */
struct MenuBindings {
    std::vector<Key> up{Key::Up, Key::W};
    std::vector<Key> down{Key::Down, Key::S};
    std::vector<Key> left{Key::Left, Key::A};
    std::vector<Key> right{Key::Right, Key::D};
    std::vector<Key> select{Key::Enter, Key::Space};
    std::vector<Key> back{Key::Backspace};
    std::vector<Key> start{Key::Enter};
    std::vector<Key> escape{Key::Escape}; ///< leaves a name being typed; quits elsewhere
    std::vector<PadButton> padUp{PadButton::DpadUp};
    std::vector<PadButton> padDown{PadButton::DpadDown};
    std::vector<PadButton> padLeft{PadButton::DpadLeft};
    std::vector<PadButton> padRight{PadButton::DpadRight};
    std::vector<PadButton> padSelect{PadButton::A};
    std::vector<PadButton> padBack{PadButton::B};
    std::vector<PadButton> padStart{PadButton::Start};
};

/** Keys, pad buttons and the stick that walk a character. */
struct PlayBindings {
    std::vector<Key> up{Key::Up, Key::W};
    std::vector<Key> down{Key::Down, Key::S};
    std::vector<Key> left{Key::Left, Key::A};
    std::vector<Key> right{Key::Right, Key::D};
    std::vector<PadButton> padUp{PadButton::DpadUp};
    std::vector<PadButton> padDown{PadButton::DpadDown};
    std::vector<PadButton> padLeft{PadButton::DpadLeft};
    std::vector<PadButton> padRight{PadButton::DpadRight};
    std::vector<Key> attack{Key::Space};
    std::vector<PadButton> padAttack{PadButton::A};
    f32 stickDeadZone = 0.25f; ///< stick deflection ignored as rest
};

/**
 * Everything the game reads instead of hard-coding: the shipped defaults in `data/config.json`
 * with the player's settings merged over them.
 */
struct GameConfig {
    DisplayConfig display;
    TimingConfig timing;
    CameraConfig camera;
    AudioConfig audio;
    TextConfig text;
    SaveConfig save;
    MenuBindings menu;
    PlayBindings play;

    /** Merges every value the file provides; false (with a warning) when it cannot be read. */
    bool loadFile(const std::filesystem::path& file);

    /** Merges values from JSON text; throws FormatError when it is not valid JSON. */
    void mergeJson(std::string_view json);

    /** Writes the whole configuration as JSON; throws FileError. */
    void saveFile(const std::filesystem::path& file) const;
    std::string toJson() const;

    f32 horizontalFovRadians() const;

    /** The save directory, resolved from the settings or the user's configuration folder. */
    std::filesystem::path saveDirectory() const;

    /** Where this user's settings live: under the platform's per-user configuration directory. */
    static std::filesystem::path userSettingsPath();
};

} // namespace gdl::game
