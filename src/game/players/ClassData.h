#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::game {

/** The playable classes: eight to start with, eight unlockable, and the hidden Sumner. */
inline constexpr std::int32_t kClassCount = 17;
inline constexpr std::int32_t kStartingClassCount = 8;
inline constexpr std::int32_t kSumnerClass = 16;
inline constexpr std::int32_t kColorCount = 4;

/** The asset code of a class ("WAR"), as the texture and data names use it. */
std::string_view classCode(std::int32_t classIndex);

/** The asset suffix of a costume colour ("RED"). */
std::string_view colorCode(std::int32_t color);

/** The index of a class or colour code, in any case; nullopt for an unknown one. */
std::optional<std::int32_t> classIndexOf(std::string_view code);
std::optional<std::int32_t> colorIndexOf(std::string_view code);

/** The tint a player's text and marks take from their costume colour. */
Color playerColor(std::int32_t color);

/** The tint of a player's status box: bright for a joined player, dim for an empty lane. */
Color boxTint(std::int32_t color, bool active);

/** Whether a class is available to a save: starting classes always, others once unlocked. */
bool classUnlocked(std::int32_t classIndex, std::uint16_t unlockMask);

/** An effect one of a class's moves shows. */
struct MoveEffect {
    std::int32_t next = -1; ///< another started with it
    std::string tree;       ///< of the costume colour's effects; none when empty or `NULLFX`
    std::string sound;
    Vec3 offset{0.0f, 0.0f, 0.0f};
    float scale = 1.0f;
};

/** One thing a move does at one of its frames. */
struct MoveStrike {
    static constexpr std::int32_t kWindow =
        0; ///< harms nothing: it only lasts, as to hide the weapon
    static constexpr std::int32_t kFlies = 2;
    static constexpr std::int32_t kSpreads = 3; ///< a burst of another kind
    static constexpr std::int32_t kBursts = 4;
    static constexpr std::int32_t kVolley = 10; ///< the class's own missiles, let fly as it lasts
    static constexpr std::int32_t kHidesWeapon = 0x400; ///< flags: the hand is empty while it lasts
    static constexpr std::int32_t kSweepsIn = 0x200;  ///< a volley's angle closes from full to none
    static constexpr std::int32_t kSweepsOut = 0x100; ///< or opens from none to full

    std::int32_t type = kBursts;
    float hitRadius = 0.0f;
    float radius = 0.0f;
    float delay = 0.0f;   ///< seconds from its start to its harm
    float maxTime = 0.0f; ///< how long what flies lasts
    float arc = -1.0f;    ///< the least cosine from the facing that is hit; -1 is all round
    Vec3 offset{0.0f, 0.0f, 0.0f};
    float amount = 0.0f; ///< harm; negative, that many times the character's own
    float speed = 0.0f;
    float angle = 0.0f;           ///< radians off the facing
    std::uint32_t damageType = 0; ///< the element and what it does to who it hits; kept for enemies
    std::int32_t effect = -1;
    std::int32_t hitEffect = -1; ///< shown where it harms something
    std::int32_t loopEffect =
        -1; ///< what its effect gives way to, repeating, for as long as it flies
    std::int32_t next = -1;
    std::int32_t startFrame = 0;
    std::int32_t endFrame = -1; ///< none: it lasts to the move's end
    std::int32_t flags = 0;
    std::int32_t help = -1; ///< the help message that names the move

    /** What a strike takes off the level's ambient light while it lasts: the greater the
     * move, the deeper the dark. */
    float dimming() const;
    bool harms() const { return type == kFlies || type == kSpreads || type == kBursts; }
    bool lasting(float frame) const {
        return frame >= static_cast<float>(startFrame) &&
               (endFrame < 0 || frame < static_cast<float>(endFrame));
    }
};

/** The moves a class's data names, each by its first strike (-1 when the class lacks it). */
struct ClassMoves {
    std::int32_t turboAThrow = -1; ///< the strong attack with nothing in reach
    std::int32_t turboB = -1;
    std::int32_t turboC1 = -1;
    std::int32_t turboC2 = -1;
    std::int32_t combo1 = -1;
    std::int32_t comboHit = -1;
};

/** A class's stat ranges and body size, from its unpacked data file. */
struct ClassStats {
    float fightMin = 0.0f;
    float fightMax = 0.0f;
    float speedMin = 0.0f;
    float speedMax = 0.0f;
    float armorMin = 0.0f;
    float armorMax = 0.0f;
    float magicMin = 0.0f;
    float magicMax = 0.0f;
    float height = 0.0f;
    float width = 0.0f;
    float collisionY = 0.0f; ///< the body's centre above the feet, which the camera follows
    Vec3 weaponOffset{0.0f, 0.0f, 0.0f}; ///< where a thrown weapon leaves, from the centre
    float powerupTime = 1.0f;            ///< how much longer (or shorter) powerups last this class
    ClassMoves moves;
    std::vector<MoveEffect> moveEffects;
    std::vector<MoveStrike> moveStrikes;

    /** The strikes a move runs: its first and every one chained to it. */
    std::vector<std::int32_t> strikesOf(std::int32_t first) const;
};

/** Every class's stats, read from `<directory>/<CODE>.json`. */
class ClassDataSet {
public:
    /** Loads what is there; false (with a warning) when no class file could be read. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return m_loadedCount > 0; }
    std::size_t loadedCount() const { return m_loadedCount; }

    /** The stats of a class, or nullptr when its file was missing. */
    const ClassStats* stats(std::int32_t classIndex) const;

private:
    std::array<std::optional<ClassStats>, kClassCount> m_classes{};
    std::size_t m_loadedCount = 0;
};

} // namespace gdl::game
