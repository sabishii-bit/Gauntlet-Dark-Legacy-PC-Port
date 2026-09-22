#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

/** When a move may be chosen against its target. */
struct CritterTarget {
    f32 minDistance = 0.0f;
    f32 maxDistance = 0.0f; ///< none when nought or under
    f32 yaw = 0.0f;         ///< where the cone points, from ahead: behind for a turn about
    f32 minDot = -1.0f;     ///< how squarely the cone must hold the target
    f32 maxVertical = 0.0f; ///< none when nought

    /** Whether a target `distance` away, `bearing` round from ahead and `vertical` above or
     * below, passes. */
    bool allows(f32 distance, f32 bearing, f32 vertical) const;
};

/** How a move harms: a blow about a part of the body, a breath, or a ring about the feet. */
struct CritterDamage {
    static constexpr s16 kBlow = 0;
    static constexpr s16 kBurst = 1;   ///< the death's
    static constexpr s16 kRing = 3;    ///< a stomp's, over its reach
    static constexpr s16 kBreath = 4;
    static constexpr s16 kGrab = 7;
    static constexpr s16 kSpew = 9;    ///< a boss's death throwing its coins out
    static constexpr u32 kCurbed = 0x4000; ///< a legend item's weakness takes this from it

    s16 type = 0;
    u32 flags = 0;
    f32 radius = 0.0f;
    f32 maxDistance = 0.0f;
    f32 yaw = 0.0f;   ///< a spew's way, turned from the body's facing
    f32 minDot = 0.0f; ///< also the cosine of half a spew's arc
    f32 pitch = 0.0f;  ///< a spew's way, tipped (under nought: up)
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 damage = 0.0f;
    f32 speed = 0.0f;  ///< a spew's, in units a second
    s32 sound = -1; ///< the sound record started where it strikes, or -1

    /** The way a spew goes from a body facing `yaw` (radians about the upright), and how
     * fast; half its arc each side of that is `acos(minDot)`. */
    Vec3 spewVelocity(f32 bodyYaw) const;
    f32 spewHalfAngle() const;
};

/** An effect and a sound a critter's move, strike or hurt starts: the tree of its own
 * archive, the sound named with the level's letter, and how it is placed. */
struct CritterSound {
    static constexpr u32 kFollows = 0x801;    ///< rides on the body rather than staying put
    static constexpr u32 kShakes = 0x2;       ///< shakes the camera
    static constexpr u32 kDeathMark = 0x40000; ///< the one that marks the death

    std::string tree;         ///< "ATK01FX"; "NULLFX" or empty shows nothing
    std::string soundFormat;  ///< "S_GOL%cSWING"
    u32 flags = 0;
    s32 link = -1;            ///< another started with it
    Vec3 offset{0.0f, 0.0f, 0.0f};
    f32 life = 0.0f;
    f32 scale = 1.0f;

    bool shows() const { return !tree.empty() && tree != "NULLFX"; }
    bool follows() const { return (flags & kFollows) != 0; }
    /** The sound's name for a level whose name starts with `letter`. */
    std::string soundFor(char letter) const;
};

/** One thing a critter does, and when it does it. */
struct CritterMove {
    static constexpr s32 kInit = 0;
    static constexpr s32 kStart = 16;
    static constexpr s32 kDeath = 17;
    static constexpr s32 kReady = 32;
    static constexpr s32 kTaunt = 33;
    static constexpr s32 kRoar = 34;
    static constexpr s32 kBlock = 35;
    static constexpr s32 kStepFrom = 48; ///< the steps: turns, walks, back-steps
    static constexpr s32 kWalk = 52;
    static constexpr s32 kStepTo = 64;
    static constexpr s32 kKnockBack = 65;
    static constexpr s32 kKnockDown = 66;
    static constexpr s32 kAttackFrom = 128; ///< attacks are this and over

    s32 type = 0;
    u32 flags = 0;
    s32 priority = 0;
    std::string name;
    std::string anim;
    std::string colnode;
    s32 frameStart = -1;
    s32 frameEnd = -1;
    s32 frameStart2 = -1;
    s32 frameEnd2 = -1;
    s32 damage0 = -1;
    s32 damage1 = -1;
    s32 link = -1;
    s32 interrupt = 0;
    s32 sound = -1;       ///< the sound record started as the move passes `soundFrame`
    s32 soundFrame = 0;
    s32 sound2 = -1;      ///< and a second, at `sound2Frame`
    s32 sound2Frame = 0;
    CritterTarget target;
    f32 cooldown = 0.0f;
    f32 speed = 0.0f;    ///< units a second while it plays
    f32 turnRate = 0.0f; ///< radians a second
    f32 hold = 0.0f;

    bool attack() const { return type >= kAttackFrom; }
    bool harms() const { return damage0 >= 0 || damage1 >= 0; }
};

/** A part of the body that can be struck. */
struct CritterPart {
    std::string node;
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
    f32 damageScale = 1.0f;
};

/** How a boss's health meter is laid out across the top of the screen: strips of 256, the
 * first's cap and the last's taken off the fill. */
struct CritterMeter {
    static constexpr u32 kShown = 4;      ///< the type flag for a HUD meter
    static constexpr u32 kBacked = 8;     ///< and for its backgrounds
    static constexpr u32 kInWorld = 0x800; ///< the bar that hangs off the body

    s32 pieces = 0;
    s32 advance = 0;
    s32 leftInset = 0;
    s32 rightInset = 0;
    bool shown = false;
    bool backed = false;
    Vec3 barOffset{0.0f, 0.0f, 0.0f}; ///< where the in-world bar hangs
};

/** What the original keeps of a great creature in its `CRITTER/<NAME>.WAD`. */
class CritterData {
public:
    /** Loads `critter/<NAME>.json`; false when it is not there or holds no type. */
    bool load(const std::filesystem::path& file);
    bool loaded() const { return !m_moves.empty(); }

    std::string_view name() const { return m_name; }         ///< "GOLEM"
    std::string_view folder() const { return m_folder; }     ///< "golem": the archive's
    std::string_view prefix() const { return m_prefix; }     ///< "GOLEM"
    std::string tree() const { return m_prefix + m_suffix; } ///< "GOLEM1"
    s32 kind() const { return m_kind; }                      ///< 3 a golem, 7 a gargoyle, 8 a general
    f32 radius() const { return m_radius; }
    f32 wallRadius() const { return m_wallRadius; }
    f32 armor() const { return m_armor; }
    f32 maxHealth() const { return m_maxHealth; }
    f32 experience() const { return m_experience; }
    f32 wakeThreshold() const { return m_wake; } ///< how near the party comes before a boss stirs
    f32 vertDrift() const { return m_vertDrift; }
    const Vec3& originOffset() const { return m_originOffset; }
    const CritterTarget& sight() const { return m_sight; }
    const CritterMeter& meter() const { return m_meter; }
    std::span<const CritterMove> moves() const { return m_moves; }
    std::span<const CritterDamage> damages() const { return m_damages; }
    std::span<const CritterPart> parts() const { return m_parts; }
    std::span<const CritterSound> sounds() const { return m_sounds; }
    const CritterDamage* damage(s32 index) const;
    const CritterSound* sound(s32 index) const;
    /** The sound records started where it is struck: by a missile, by a blow. */
    s32 hitSoundFar() const { return m_hitSoundFar; }
    s32 hitSoundClose() const { return m_hitSoundClose; }
    /** The first move of a type, if any. */
    std::optional<usize> moveOfType(s32 type) const;
    std::optional<usize> moveNamed(std::string_view name) const;

private:
    std::string m_name;
    std::string m_folder;
    std::string m_prefix;
    std::string m_suffix;
    s32 m_kind = 0;
    f32 m_radius = 1.0f;
    f32 m_wallRadius = 1.0f;
    f32 m_armor = 0.0f;
    f32 m_maxHealth = 1.0f;
    f32 m_experience = 0.0f;
    f32 m_wake = 0.0f;
    f32 m_vertDrift = 0.0f;
    Vec3 m_originOffset{0.0f, 0.0f, 0.0f};
    CritterTarget m_sight;
    CritterMeter m_meter;
    std::vector<CritterMove> m_moves;
    std::vector<CritterDamage> m_damages;
    std::vector<CritterPart> m_parts;
    std::vector<CritterSound> m_sounds;
    s32 m_hitSoundFar = -1;
    s32 m_hitSoundClose = -1;
};

} // namespace gdl::game
