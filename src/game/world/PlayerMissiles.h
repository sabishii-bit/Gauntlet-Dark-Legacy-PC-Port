#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/** How a class's thrown weapon flies, from the original's tables. */
struct MissileSpec {
    std::string_view model; ///< the throw trees are named `<model>_THROW<tier>`
    std::string_view tiers; ///< the tier for each ten levels; '0' is the costume's own
    f32 radius = 1.0f;      ///< what walls stop
    f32 spin = 0.0f;        ///< radians a second it tumbles forwards
    f32 weight = 8.0f;      ///< how hard it falls, units a second squared
    bool staysInHand = false; ///< a staff or a bow is not what flies

    /** The spec of a class (the unlockable classes fly like the class they shadow). */
    static const MissileSpec& of(s32 classIndex);
    /** The name of the class's throw tree at `level`, and whether the costume's own archive
     * holds it (else the costume colour's effects archive does). */
    static std::string treeName(s32 classIndex, s32 level, bool* inCostume = nullptr);
    /** Whether a class throws by its magic rather than its strength. */
    static bool byMagic(s32 classIndex);
};

/** What sets a missile off. */
struct MissileLaunch {
    s32 owner = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< along the ground, unit length
    f32 speed = 20.0f;
    f32 reach = 15.0f; ///< how far off it comes back down to just under where it left
    const MissileSpec* spec = nullptr;
    const TreeModel* model = nullptr; ///< must outlive the missile
};

/**
 * The weapons the party has thrown, flying the way the original's do: along the thrower's
 * facing at the pace their strength (or magic) gives, lobbed so that gravity brings them
 * back to half a unit under their start at their reach, tumbling if their kind does, and
 * gone the moment a wall or floor stops them or their time runs out.
 */
class PlayerMissiles {
public:
    static constexpr f32 kSlowest = 20.0f; ///< units a second with no strength at all
    static constexpr f32 kFastest = 60.0f; ///< and at a stat of 1000
    static constexpr f32 kStatScale = 0.001f;
    static constexpr f32 kReach = 15.0f;        ///< of a tapped throw
    static constexpr f32 kReachPerSecond = 200.0f; ///< more for each second the attack is held
    static constexpr f32 kHoldDelay = 0.27f;    ///< before holding counts
    static constexpr f32 kHoldMost = 0.1f;      ///< and the most of it that does
    static constexpr f32 kDrop = 0.5f;          ///< under its start, where its reach lands it
    static constexpr f32 kMuzzle = 2.0f;        ///< ahead of the hand, where it appears
    static constexpr f32 kLifeSeconds = 3.0f;

    /** One weapon in flight. */
    struct Missile {
        s32 owner = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        f32 tumble = 0.0f; ///< how far it has turned over
        f32 age = 0.0f;
        const MissileSpec* spec = nullptr;
        const TreeModel* model = nullptr;
    };

    /** A missile's pace from the stat that throws it. */
    static f32 speedFor(s32 stat);
    /** How far a throw reaches when the attack had been going `attackSeconds`. */
    static f32 reachFor(f32 attackSeconds);
    /** The velocity that sets a missile off along `direction` to come down at its reach. */
    static Vec3 launchVelocity(const Vec3& direction, f32 speed, f32 reach, f32 weight);

    /** Sets a missile flying; false when the launch names no spec. */
    bool launch(const MissileLaunch& launch);
    /** Flies every missile on by `seconds`; those a wall or floor stops are taken away. */
    void update(f32 seconds, const WorldCollision* collision);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();

    usize count() const { return m_missiles.size(); }
    const Missile& missile(usize index) const { return m_missiles[index]; }
    /** Where missiles were stopped since the last call; each is handed out once. */
    std::vector<Vec3> takeImpacts();
    /** Model space (flying along +z) to the world, for a missile. */
    static Mat4 transformOf(const Missile& missile);

private:
    std::vector<Missile> m_missiles;
    std::vector<Vec3> m_impacts;
};

} // namespace gdl::game
