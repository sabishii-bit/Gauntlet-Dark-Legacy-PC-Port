#pragma once

#include <vector>
#include <string>
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

/** An effect one of a class's moves shows. */
struct MoveEffect {
    s32 next = -1; ///< another started with it
    std::string tree; ///< of the costume colour's effects; none when empty or `NULLFX`
    std::string sound;
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;
};

/** One thing a move does at one of its frames. */
struct MoveStrike {
    static constexpr s32 kFlies = 2;
    static constexpr s32 kBursts = 4;

    s32 type = kBursts;
    f32 hitRadius = 0.0f;
    f32 radius = 0.0f;
    f32 delay = 0.0f;   ///< seconds from its start to its harm
    f32 maxTime = 0.0f; ///< how long what flies lasts
    f32 arc = -1.0f;    ///< the least cosine from the facing that is hit; -1 is all round
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 amount = 0.0f;  ///< harm; negative, that many times the character's own
    f32 speed = 0.0f;
    s32 effect = -1;
    s32 loopEffect = -1; ///< what its effect gives way to, repeating, for as long as it flies
    s32 next = -1;
    s32 startFrame = 0;
    s32 endFrame = -1;   ///< none: it lasts to the move's end
    s32 flags = 0;
    s32 help = -1;       ///< the help message that names the move

    /** What a strike takes off the level's ambient light while it lasts: the greater the
     * move, the deeper the dark. */
    f32 dimming() const;
    bool lasting(f32 frame) const {
        return frame >= static_cast<f32>(startFrame) &&
               (endFrame < 0 || frame < static_cast<f32>(endFrame));
    }
};

/** The moves a class's data names, each by its first strike (-1 when the class lacks it). */
struct ClassMoves {
    s32 turboB = -1;
    s32 turboC1 = -1;
    s32 turboC2 = -1;
    s32 combo1 = -1;
    s32 comboHit = -1;
};

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
    ClassMoves moves;
    std::vector<MoveEffect> moveEffects;
    std::vector<MoveStrike> moveStrikes;

    /** The strikes a move runs: its first and every one chained to it. */
    std::vector<s32> strikesOf(s32 first) const;
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
