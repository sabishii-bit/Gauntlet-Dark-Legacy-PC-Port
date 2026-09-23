#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/** How a class's thrown weapon flies, from the original's tables. */
struct MissileSpec {
    std::string_view model;   ///< the throw trees are named `<model>_THROW<tier>`
    std::string_view tiers;   ///< the tier for each ten levels; '0' is the costume's own
    float radius = 1.0f;      ///< what walls stop
    float spin = 0.0f;        ///< radians a second it tumbles forwards
    float weight = 8.0f;      ///< how hard it falls, units a second squared
    bool staysInHand = false; ///< a staff or a bow is not what flies

    /** The spec of a class (the unlockable classes fly like the class they shadow). */
    static const MissileSpec& of(std::int32_t classIndex);
    /** The name of the class's throw tree at `level`, and whether the costume's own archive
     * holds it (else the costume colour's effects archive does). */
    static std::string treeName(std::int32_t classIndex, std::int32_t level,
                                bool* inCostume = nullptr);
    /** Whether a class throws by its magic rather than its strength. */
    static bool byMagic(std::int32_t classIndex);
    /** How a thrown potion flies. */
    static const MissileSpec& potion();
};

/** What sets a missile off. */
struct MissileLaunch {
    std::int32_t owner = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< along the ground, unit length
    float speed = 20.0f;
    float reach = 15.0f; ///< how far off it comes back down to just under where it left
    const MissileSpec* spec = nullptr;
    const TreeModel* model = nullptr; ///< must outlive the missile
    std::optional<Vec3> velocity;     ///< set, it flies off at this instead of being lobbed
    std::int32_t potion = 0;          ///< the kind of potion it is, which bursts where it lands
    float potency = 0.0f;             ///< the magic power its burst goes off with
    float damage = 0.0f;              ///< what it does to what it hits
    float scale = 1.0f;               ///< how large it is drawn: a strong throw's is doubled
};

/** Something standing that a missile stops against: an upright cylinder from its base. */
struct MissileTarget {
    std::int32_t id = -1;
    Vec3 base{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
    float height = 1.0f;
};

/** Where a missile was stopped. */
struct MissileImpact {
    Vec3 position{0.0f, 0.0f, 0.0f};
    std::int32_t owner = 0;
    std::int32_t potion = 0;
    float potency = 0.0f;
    float damage = 0.0f;
    std::int32_t target =
        -1; ///< the id of the target it stopped against; none for a wall or the floor
};

/**
 * The weapons the party has thrown, flying the way the original's do: along the thrower's
 * facing at the pace their strength (or magic) gives, lobbed so that gravity brings them
 * back to half a unit under their start at their reach, tumbling if their kind does, and
 * gone the moment a wall or floor stops them or their time runs out.
 */
class PlayerMissiles {
public:
    static constexpr float kSlowest = 20.0f; ///< units a second with no strength at all
    static constexpr float kFastest = 60.0f; ///< and at a stat of 1000
    static constexpr float kStatScale = 0.001f;
    static constexpr float kReach = 15.0f;           ///< of a tapped throw
    static constexpr float kReachPerSecond = 200.0f; ///< more for each second the attack is held
    static constexpr float kHoldDelay = 0.27f;       ///< before holding counts
    static constexpr float kHoldMost = 0.1f;         ///< and the most of it that does
    static constexpr float kDrop = 0.5f;             ///< under its start, where its reach lands it
    static constexpr float kMuzzle = 2.0f;           ///< ahead of the hand, where it appears
    static constexpr float kLifeSeconds = 3.0f;
    static constexpr float kLeastDamage = 5.0f; ///< of a missile, with no strength at all
    static constexpr float kMostDamage = 20.0f; ///< and at a stat of 1000

    /** One weapon in flight. */
    struct Missile {
        std::int32_t owner = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        float tumble = 0.0f; ///< how far it has turned over
        float age = 0.0f;
        std::int32_t potion = 0;
        float potency = 0.0f;
        float damage = 0.0f;
        float scale = 1.0f;
        const MissileSpec* spec = nullptr;
        const TreeModel* model = nullptr;
    };

    /** A missile's pace from the stat that throws it. */
    static float speedFor(std::int32_t stat);
    /** What a missile does to what it hits, by the thrower's strength (or magic). */
    static float damageFor(std::int32_t stat);
    /** How far a throw reaches when the attack had been going `attackSeconds`. */
    static float reachFor(float attackSeconds);
    /** The velocity that sets a missile off along `direction` to come down at its reach. */
    static Vec3 launchVelocity(const Vec3& direction, float speed, float reach, float weight);

    /** Sets a missile flying; false when the launch names no spec. */
    bool launch(const MissileLaunch& launch);
    /** Flies every missile on by `seconds`; those a wall or floor stops are taken away. */
    void update(float seconds, const WorldCollision* collision,
                std::span<const MissileTarget> targets = {});
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();

    std::size_t count() const { return m_missiles.size(); }
    const Missile& missile(std::size_t index) const { return m_missiles[index]; }
    /** Where missiles were stopped since the last call; each is handed out once. */
    std::vector<MissileImpact> takeImpacts();
    /** The directions of `shots` missiles about `direction`: fifteen degrees apart. */
    static std::vector<Vec3> spread(const Vec3& direction, std::int32_t shots);
    static constexpr float kSpreadStep = 0.2617994f; ///< fifteen degrees
    /** Model space (flying along +z) to the world, for a missile. */
    static Mat4 transformOf(const Missile& missile);

private:
    std::vector<Missile> m_missiles;
    std::vector<MissileImpact> m_impacts;
};

} // namespace gdl::game
