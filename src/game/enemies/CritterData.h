#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"

namespace gdl::game {

/** When a move may be chosen against its target. */
struct CritterTarget {
    float minDistance = 0.0f;
    float maxDistance = 0.0f; ///< none when nought or under
    float yaw = 0.0f;         ///< where the cone points, from ahead: behind for a turn about
    float minDot = -1.0f;     ///< how squarely the cone must hold the target
    float maxVertical = 0.0f; ///< none when nought

    /** Whether a target `distance` away, `bearing` round from ahead and `vertical` above or
     * below, passes. */
    bool allows(float distance, float bearing, float vertical) const;
};

/** How a move harms: a blow about a part of the body, a breath, or a ring about the feet. */
struct CritterDamage {
    static constexpr std::int16_t kBlow = 0;
    static constexpr std::int16_t kProjectile = 1;
    static constexpr std::int16_t kRing = 3; ///< a stomp's, over its reach
    static constexpr std::int16_t kBreath = 4;
    static constexpr std::int16_t kGrab = 7;
    static constexpr std::int16_t kSpew = 9; ///< a boss's death throwing its coins out
    static constexpr std::uint32_t kCurbed =
        0x4000; ///< a legend item's weakness takes this from it

    std::int16_t type = 0;
    std::uint16_t behaviorFlags = 0; ///< launch/attachment policy, distinct from player harm flags
    std::uint32_t flags = 0;
    float radius = 0.0f;
    float maxDistance = 0.0f;
    float minDistance = 0.0f; ///< inner horizontal limit of a damaging breath
    float yaw = 0.0f;         ///< a spew's way, turned from the body's facing
    float minDot = 0.0f;      ///< also the cosine of half a spew's arc
    float pitch = 0.0f;       ///< a spew's way, tipped (under nought: up)
    Vec3 offset{0.0f, 0.0f, 0.0f};
    float damage = 0.0f;
    float speed = 0.0f; ///< a spew's, in units a second
    float maxSpeed = 0.0f;
    float gravity = 0.0f;
    float morphLife = 0.0f;
    float yawSpread = 0.0f;
    std::int32_t sound = -1; ///< the sound record started where it strikes, or -1
    std::int32_t hitSound = -1;
    std::int32_t morph = -1;
    std::int32_t morphEnd = -1;

    /** The way a spew goes from a body facing `yaw` (radians about the upright), and how
     * fast; half its arc each side of that is `acos(minDot)`. */
    Vec3 spewVelocity(float bodyYaw) const;
    float spewHalfAngle() const;
};

/** An effect and a sound a critter's move, strike or hurt starts: the tree of its own
 * archive, the sound named with the level's letter, and how it is placed. */
struct CritterSound {
    static constexpr std::uint32_t kFollows = 0x801; ///< rides on the body rather than staying put
    static constexpr std::uint32_t kShakes = 0x2;    ///< shakes the camera
    static constexpr std::uint32_t kDeathMark = 0x40000; ///< the one that marks the death

    std::string tree;        ///< "ATK01FX"; "NULLFX" or empty shows nothing
    std::string soundFormat; ///< "S_GOL%cSWING"
    std::uint32_t flags = 0;
    std::int32_t link = -1; ///< another started with it
    Vec3 offset{0.0f, 0.0f, 0.0f};
    float life = 0.0f;
    float scale = 1.0f;

    bool shows() const { return !tree.empty() && tree != "NULLFX"; }
    bool follows() const { return (flags & kFollows) != 0; }
    /** The sound's name for a level whose name starts with `letter`. */
    std::string soundFor(char letter) const;
};

/** One thing a critter does, and when it does it. */
struct CritterMove {
    static constexpr std::int32_t kInit = 0;
    static constexpr std::int32_t kStart = 16;
    static constexpr std::int32_t kDeath = 17;
    static constexpr std::int32_t kReady = 32;
    static constexpr std::int32_t kTaunt = 33;
    static constexpr std::int32_t kRoar = 34;
    static constexpr std::int32_t kBlock = 35;
    static constexpr std::int32_t kStepFrom = 48; ///< the steps: turns, walks, back-steps
    static constexpr std::int32_t kWalk = 52;
    static constexpr std::int32_t kStepTo = 64;
    static constexpr std::int32_t kKnockBack = 65;
    static constexpr std::int32_t kKnockDown = 66;
    static constexpr std::int32_t kAttackFrom = 128; ///< attacks are this and over

    std::int32_t type = 0;
    std::uint32_t flags = 0;
    std::int32_t priority = 0;
    std::string name;
    std::string anim;
    std::string colnode;
    std::int32_t frameStart = -1;
    std::int32_t frameEnd = -1;
    std::int32_t frameStart2 = -1;
    std::int32_t frameEnd2 = -1;
    float framePeriod = 0.0f; ///< repeat spacing for move type 133
    std::int32_t damage0 = -1;
    std::int32_t damage1 = -1;
    std::int32_t link = -1;
    std::int32_t interrupt = 0;
    std::int32_t sound = -1; ///< the sound record started as the move passes `soundFrame`
    std::int32_t soundFrame = 0;
    std::int32_t sound2 = -1; ///< and a second, at `sound2Frame`
    std::int32_t sound2Frame = 0;
    CritterTarget target;
    float cooldown = 0.0f;
    float speed = 0.0f;    ///< units a second while it plays
    float turnRate = 0.0f; ///< radians a second
    float hold = 0.0f;

    bool attack() const { return type >= kAttackFrom; }
    bool harms() const { return damage0 >= 0 || damage1 >= 0; }
    /** Projectile triggers crossed between integer animation frames; -1 precedes frame zero. */
    std::int32_t projectileTriggers(std::int32_t previous, std::int32_t current,
                                    bool second = false) const;
};

/** A part of the body that can be struck. */
struct CritterPart {
    std::string node;
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float damageScale = 1.0f;
};

/** How a boss's health meter is laid out across the top of the screen: strips of 256, the
 * first's cap and the last's taken off the fill. */
struct CritterMeter {
    static constexpr std::uint32_t kShown = 4;       ///< the type flag for a HUD meter
    static constexpr std::uint32_t kBacked = 8;      ///< and for its backgrounds
    static constexpr std::uint32_t kInWorld = 0x800; ///< the bar that hangs off the body

    std::int32_t pieces = 0;
    std::int32_t advance = 0;
    std::int32_t leftInset = 0;
    std::int32_t rightInset = 0;
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
    std::int32_t kind() const { return m_kind; } ///< 3 a golem, 7 a gargoyle, 8 a general
    float radius() const { return m_radius; }
    float wallRadius() const { return m_wallRadius; }
    float armor() const { return m_armor; }
    float maxHealth() const { return m_maxHealth; }
    float experience() const { return m_experience; }
    float wakeThreshold() const { return m_wake; } ///< how near the party comes before a boss stirs
    float vertDrift() const { return m_vertDrift; }
    /** Height of the model root above the floor anchor; may be negative. */
    float floorOffset() const { return m_floorOffset; }
    const Vec3& originOffset() const { return m_originOffset; }
    const CritterTarget& sight() const { return m_sight; }
    const CritterMeter& meter() const { return m_meter; }
    std::span<const CritterMove> moves() const { return m_moves; }
    std::span<const CritterDamage> damages() const { return m_damages; }
    std::span<const CritterPart> parts() const { return m_parts; }
    std::span<const CritterSound> sounds() const { return m_sounds; }
    const CritterDamage* damage(std::int32_t index) const;
    const CritterSound* sound(std::int32_t index) const;
    /** The sound records started where it is struck: by a missile, by a blow. */
    std::int32_t hitSoundFar() const { return m_hitSoundFar; }
    std::int32_t hitSoundClose() const { return m_hitSoundClose; }
    /** The first move of a type, if any. */
    std::optional<std::size_t> moveOfType(std::int32_t type) const;
    std::optional<std::size_t> moveNamed(std::string_view name) const;

private:
    std::string m_name;
    std::string m_folder;
    std::string m_prefix;
    std::string m_suffix;
    std::int32_t m_kind = 0;
    float m_radius = 1.0f;
    float m_wallRadius = 1.0f;
    float m_armor = 0.0f;
    float m_maxHealth = 1.0f;
    float m_experience = 0.0f;
    float m_wake = 0.0f;
    float m_vertDrift = 0.0f;
    float m_floorOffset = 0.0f;
    Vec3 m_originOffset{0.0f, 0.0f, 0.0f};
    CritterTarget m_sight;
    CritterMeter m_meter;
    std::vector<CritterMove> m_moves;
    std::vector<CritterDamage> m_damages;
    std::vector<CritterPart> m_parts;
    std::vector<CritterSound> m_sounds;
    std::int32_t m_hitSoundFar = -1;
    std::int32_t m_hitSoundClose = -1;
};

} // namespace gdl::game
