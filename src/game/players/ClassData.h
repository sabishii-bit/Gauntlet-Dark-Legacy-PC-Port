#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** The playable classes: eight to start with, eight unlockable, and the hidden Sumner. */
inline constexpr s32 kClassCount = 17;
inline constexpr s32 kStartingClassCount = 8;
inline constexpr s32 kSumnerClass = 16;
inline constexpr s32 kColorCount = 4;

/** The asset code of a class ("WAR"), as the texture and data names use it. */
std::string_view classCode(s32 classIndex);

/** The asset suffix of a costume colour ("RED"). */
std::string_view colorCode(s32 color);

/** The index of a class or colour code, in any case; nullopt for an unknown one. */
std::optional<s32> classIndexOf(std::string_view code);
std::optional<s32> colorIndexOf(std::string_view code);

/** The tint a player's text and marks take from their costume colour. */
Color playerColor(s32 color);

/** The tint of a player's status box: bright for a joined player, dim for an empty lane. */
Color boxTint(s32 color, bool active);

/** Whether a class is available to a save: starting classes always, others once unlocked. */
bool classUnlocked(s32 classIndex, u16 unlockMask);

/** A class's stat ranges and body size, from its unpacked data file. */
struct ClassStats {
    f32 fightMin = 0.0f;
    f32 fightMax = 0.0f;
    f32 speedMin = 0.0f;
    f32 speedMax = 0.0f;
    f32 armorMin = 0.0f;
    f32 armorMax = 0.0f;
    f32 magicMin = 0.0f;
    f32 magicMax = 0.0f;
    f32 height = 0.0f;
    f32 width = 0.0f;
    f32 collisionY = 0.0f; ///< the body's centre above the feet, which the camera follows
    Vec3 weaponOffset{0.0f, 0.0f, 0.0f}; ///< where a thrown weapon leaves, from the centre
    f32 powerupTime = 1.0f; ///< how much longer (or shorter) powerups last this class
};

/** Every class's stats, read from `<directory>/<CODE>.json`. */
class ClassDataSet {
public:
    /** Loads what is there; false (with a warning) when no class file could be read. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return m_loadedCount > 0; }
    usize loadedCount() const { return m_loadedCount; }

    /** The stats of a class, or nullptr when its file was missing. */
    const ClassStats* stats(s32 classIndex) const;

private:
    std::array<std::optional<ClassStats>, kClassCount> m_classes{};
    usize m_loadedCount = 0;
};

} // namespace gdl::game
