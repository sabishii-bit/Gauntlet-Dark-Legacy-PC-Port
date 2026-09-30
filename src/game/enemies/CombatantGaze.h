#pragma once
#include <array>
#include <optional>
#include <span>

#include "engine/assets/AnimationSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/TreePose.h"

#include "game/enemies/MoveDefinition.h"

namespace gdl::game {
/**
 * The turn of a fighter's head and eyes toward what it fights (CritterLookAtPlayer,
 * NodeLookAtPos): a bearing tracked for each look node chases the target's, a quarter turn a
 * second, and the node is turned toward it from its animated pose, afresh every frame, by no
 * more than its limit. Without a target the bearing goes back to nought and the head with it.
 */
class CombatantGaze {
public:
    static constexpr f32 kChaseRate = kHalfPi; ///< radians a second the tracked bearing moves

    /** Turns `pose`'s look nodes (of `tree`, placed in the world by `model`) toward `target`,
     * or back to the animation without one, `seconds` on from the last call. */
    void aim(TreePose& pose, const TreeInfo& tree, const Mat4& model,
             std::span<const LookDefinition> looks, const std::optional<Vec3>& target, f32 seconds);
    void reset() { m_tracked = {}; }
    /** The bearing tracked for look node `index`: yaw about the upright, then pitch. */
    Vec2 tracked(usize index) const;

private:
    struct Bearing {
        f32 yaw = 0.0f;
        f32 pitch = 0.0f;
    };
    std::array<Bearing, 2> m_tracked{};
};
} // namespace gdl::game
