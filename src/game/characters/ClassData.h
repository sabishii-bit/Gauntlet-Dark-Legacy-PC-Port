#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/EffectFlags.h"

namespace gdl::game {

/** The playable classes: eight to start with, eight unlockable, and the hidden Sumner. */
inline constexpr s32 kClassCount = 17;
inline constexpr s32 kStartingClassCount = 8;
/** Each class's index, in the order of its asset code (classCode) and its name in the text
 * tables: the eight starting classes, the eight unlockables, each shadowing the starting class
 * eight before it (baseClassOf), then Sumner. */
inline constexpr s32 kWarriorClass = 0;
inline constexpr s32 kValkyrieClass = 1;
inline constexpr s32 kWizardClass = 2;
inline constexpr s32 kArcherClass = 3;
inline constexpr s32 kDwarfClass = 4;
inline constexpr s32 kKnightClass = 5;
inline constexpr s32 kSorceressClass = 6;
inline constexpr s32 kJesterClass = 7;
inline constexpr s32 kMinotaurClass = 8;
inline constexpr s32 kFalconessClass = 9;
inline constexpr s32 kJackalClass = 10;
inline constexpr s32 kTigressClass = 11;
inline constexpr s32 kOgreClass = 12;
inline constexpr s32 kUnicornClass = 13;
inline constexpr s32 kMedusaClass = 14;
inline constexpr s32 kHyenaClass = 15;
inline constexpr s32 kSumnerClass = 16;
inline constexpr s32 kColorCount = 4;
/** The costume Sumner wears, under the wizard's archive. */
inline constexpr std::string_view kSumnerCostume = "SUM";

/** The starting class a character plays as: a starting class itself, an unlockable the one it
 * shadows, and Sumner the wizard (change_player's character 2 behind the "sum" costume). An
 * index out of range is returned as it is. */
s32 baseClassOf(s32 classIndex);

/** The class whose archives a character wears: its own, except Sumner, who wears the
 * wizard's (load_player_model's players/wiz/sum). */
s32 archiveClassOf(s32 classIndex);

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
    s32 next = -1;    ///< another started with it
    std::string tree; ///< of the costume colour's effects; none when empty or `NULLFX`
    std::string sound;
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;
    u32 flags = 0; ///< effect_flags::
    f32 lifetime = 0.0f;
    f32 radius = 0.0f;      ///< a particle record's emission rate per game frame
    s16 alphaMod = 0;       ///< a particle record's speed in hundredths of a unit/second
    u32 color = 0xFFFFFFFF; ///< SFXX tree RGB; the high byte is not tree opacity
    /** DoPlyrSfx's MBTreeSetColor -> setPrimColor uses only RGB. FFFFFFFF leaves the
     * tree unchanged, represented by the renderer's identity (white) multiplier. */
    Color tint() const;
    /** A particle emitter rather than a tree and a sound: `tree` names its texture and
     * `sound` the node it is hung from (PsfxDoParticle), such as the magic users' hand glow. */
    bool particle() const { return (flags & effect_flags::kParticleMask) != 0; }
};

/** One thing a move does at one of its frames. */
struct MoveStrike {
    static constexpr s32 kWindow = 0; ///< harms nothing: it only lasts, as to hide the weapon
    static constexpr s32 kFlies = 2;
    static constexpr s32 kSpreads = 3; ///< a burst of another kind
    static constexpr s32 kBursts = 4;
    static constexpr s32 kVolley = 10;         ///< the class's own missiles, let fly as it lasts
    static constexpr s32 kHidesWeapon = 0x400; ///< flags: the hand is empty while it lasts
    static constexpr s32 kSweepsIn = 0x200;    ///< a volley's angle closes from full to none
    static constexpr s32 kSweepsOut = 0x100;   ///< or opens from none to full
    static constexpr s32 kWaitsForMorph =
        0x800; ///< what flies launches once its effect has morphed

    s32 type = kBursts;
    f32 hitRadius = 0.0f;
    f32 radius = 0.0f;
    f32 delay = 0.0f;   ///< seconds from its start to its harm
    f32 maxTime = 0.0f; ///< how long what flies lasts
    f32 arc = -1.0f;    ///< the least cosine from the facing that is hit; -1 is all round
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 amount = 0.0f; ///< harm; negative, that many times the character's own
    f32 speed = 0.0f;
    f32 angle = 0.0f;   ///< radians off the facing
    u32 damageType = 0; ///< the element and what it does to who it hits; kept for enemies
    s32 effect = -1;
    s32 hitEffect = -1;  ///< shown where it harms something
    s32 loopEffect = -1; ///< what its effect gives way to, repeating, for as long as it flies
    s32 next = -1;
    s32 startFrame = 0;
    s32 endFrame = -1; ///< none: it lasts to the move's end
    s32 flags = 0;
    s32 help = -1; ///< the help message that names the move

    /** What a strike takes off the level's ambient light while it lasts: the greater the
     * move, the deeper the dark. */
    f32 dimming() const;
    bool harms() const { return type == kFlies || type == kSpreads || type == kBursts; }
    bool lasting(f32 frame) const {
        return frame >= static_cast<f32>(startFrame) &&
               (endFrame < 0 || frame < static_cast<f32>(endFrame));
    }
};

/** The moves a class's data names, each by its first strike (-1 when the class lacks it). */
struct ClassMoves {
    s32 turboAClose = -1; ///< the first power swing of a chain
    s32 turboALow = -1;   ///< the first at something short
    s32 turboAStep = -1;  ///< the second
    s32 turboA360 = -1;   ///< the spin, the third
    s32 turboAThrow = -1; ///< the strong attack with nothing in reach
    s32 turboB = -1;
    s32 turboC1 = -1;
    s32 turboC2 = -1;
    s32 combo1 = -1;
    s32 comboHit = -1;
};

/** A class's stat ranges and body size, from its player data file. */
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
    f32 attentionY = 0.0f; ///< name and attention anchor above the feet (PDAT attny)
    f32 collisionY = 0.0f; ///< the body's centre above the feet, which the camera follows
    Vec3 weaponOffset{0.0f, 0.0f, 0.0f}; ///< where a thrown weapon leaves, from the centre
    Vec3 familiarOffset{0.0f};     ///< permanent familiar attachment in the player's local space
    Vec3 familiarShotOffset{0.0f}; ///< separate projectile origin from PDAT
    /** Where an elemental weapon's glow sits in the hand and how large, a tier of ten levels
     * each (`weaponGlowOffsets`, `weaponGlowScales`; nought for no scale). */
    static constexpr usize kGlowTiers = 10;
    std::array<Vec3, kGlowTiers> weaponGlowOffsets{};
    std::array<Vec3, kGlowTiers> weaponGlowScales{};
    f32 powerupTime = 1.0f;   ///< how much longer (or shorter) powerups last this class
    f32 streakForward = 0.0f; ///< missile streak head lead from PDAT, in thirtieths of a second
    ClassMoves moves;
    std::vector<MoveEffect> moveEffects;
    std::vector<MoveStrike> moveStrikes;

    /** The strikes a move runs: its first and every one chained to it. */
    std::vector<s32> strikesOf(s32 first) const;
};

/** Every class's stats, preferring `<directory>/<CODE>.WAD` to legacy JSON exports. */
class ClassDataSet {
public:
    /** Loads what is there; malformed native files never fall back to exports.
     * False (with a warning) when no class file could be read. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return m_loadedCount > 0; }
    usize loadedCount() const { return m_loadedCount; }

    /** The stats of a class, or nullptr when its file was missing. */
    /** The class's record; a hidden costume's (Sumner's) is its base class's. */
    const ClassStats* stats(s32 classIndex) const;

private:
    std::array<std::optional<ClassStats>, kClassCount> m_classes{};
    usize m_loadedCount = 0;
};

} // namespace gdl::game
