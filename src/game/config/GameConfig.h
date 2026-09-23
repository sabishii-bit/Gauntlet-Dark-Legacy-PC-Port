#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/Input.h"

namespace gdl::game {

struct DisplayConfig {
    std::uint32_t virtualWidth = 512; ///< the 2D layer's coordinate space
    std::uint32_t virtualHeight = 384;
    std::uint32_t frameWidth = 640; ///< the frame the game composes, letterboxed onto the window
    std::uint32_t frameHeight = 448;
    std::uint32_t windowWidth = 1280;
    std::uint32_t windowHeight = 896;
    bool vsync = true;
    std::uint32_t maxFrameRate =
        60; ///< frames per second outside play (the menus); 0 leaves it to vsync
};

struct TimingConfig {
    std::uint32_t tickRate = 60; ///< logic ticks per second; the original counts 60 Hz retraces
    std::uint32_t gameplayFrameRate = 30; ///< the rate gameplay was tuned for: two ticks per frame
};

struct CameraConfig {
    float horizontalFovDegrees = 60.0f;
};

struct AudioConfig {
    float masterVolume = 1.0f;
    float musicVolume = 0.7f;
    float effectsVolume = 1.0f;
};

struct TextConfig {
    std::string language = "en";
};

/** How hard the game is: easy, normal or hard, which scales the levels' own tuning. */
struct DifficultyConfig {
    static constexpr std::array<std::string_view, 3> kNames{"easy", "normal", "hard"};
    static constexpr std::array<float, 3> kGains{0.667f, 1.0f, 1.5f};

    std::string level = "normal";

    /** What the level's scales are multiplied by; normal's for a name it does not know. */
    float gain() const;
};

/** Where characters are saved: a `saves` folder beside the game when the directory is
 * empty, the directory itself when it is absolute, else that directory beside the game. */
struct SaveConfig {
    std::string directory;
    std::uint32_t slots = 8;
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
    /** On a pad the stick walks; the directional buttons work the powerup selector, as the
     * original's do, and walk only when bound to. */
    std::vector<PadButton> padUp;
    std::vector<PadButton> padDown;
    std::vector<PadButton> padLeft;
    std::vector<PadButton> padRight;
    std::vector<Key> attack{Key::Space};
    std::vector<PadButton> padAttack{PadButton::A};
    std::vector<Key> usePotion{Key::E};
    std::vector<PadButton> padUsePotion{PadButton::B};
    std::vector<Key> throwPotion{Key::Q};
    std::vector<PadButton> padThrowPotion{PadButton::X};
    std::vector<Key> shieldPotion{Key::C}; ///< a potion spent on a ring of its magic
    std::vector<PadButton> padShieldPotion{PadButton::RightThumb};
    std::vector<Key> strafe{Key::LeftControl}; ///< held: steps keep the facing
    std::vector<PadButton> padStrafe{PadButton::LeftThumb};
    std::vector<Key> strongAttack{
        Key::R}; ///< the slow attack: with nothing in reach, a strong throw
    std::vector<PadButton> padStrongAttack{PadButton::LeftBumper};
    std::vector<Key> turbo{Key::LeftShift}; ///< held with the attack for a turbo attack
    std::vector<PadButton> padTurbo{PadButton::RightBumper};
    std::vector<Key> charge{Key::F}; ///< the shove that runs the turbo meter down
    std::vector<PadButton> padCharge{PadButton::Y};
    std::vector<Key> selectorUp{Key::I};
    std::vector<Key> selectorDown{Key::K};
    std::vector<Key> selectorLeft{Key::J};
    std::vector<Key> selectorRight{Key::L};
    std::vector<PadButton> padSelectorUp{PadButton::DpadUp};
    std::vector<PadButton> padSelectorDown{PadButton::DpadDown};
    std::vector<PadButton> padSelectorLeft{PadButton::DpadLeft};
    std::vector<PadButton> padSelectorRight{PadButton::DpadRight};
    float stickDeadZone = 0.25f; ///< stick deflection ignored as rest
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
    DifficultyConfig difficulty;
    MenuBindings menu;
    PlayBindings play;

    /** Merges every value the file provides; false (with a warning) when it cannot be read. */
    bool loadFile(const std::filesystem::path& file);

    /** Merges values from JSON text; throws FormatError when it is not valid JSON. */
    void mergeJson(std::string_view json);

    /** Writes the whole configuration as JSON; throws FileError. */
    void saveFile(const std::filesystem::path& file) const;
    std::string toJson() const;

    float horizontalFovRadians() const;

    /** The save directory, resolved from the settings or the user's configuration folder. */
    std::filesystem::path saveDirectory() const;
    /** The same, for a game standing in `gameDirectory`. */
    std::filesystem::path saveDirectory(const std::filesystem::path& gameDirectory) const;

    /** Where this user's settings live: under the platform's per-user configuration directory. */
    static std::filesystem::path userSettingsPath();
};

} // namespace gdl::game
