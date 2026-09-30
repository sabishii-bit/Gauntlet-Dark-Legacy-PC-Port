#include "game/enemies/CombatantGaze.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
/** Yaw about the upright and pitch of a direction (GetYawPitch). */
Vec2 bearingOf(const Vec3& direction) {
    return Vec2{std::atan2(direction.x, direction.z),
                std::atan2(direction.y, std::hypot(direction.x, direction.z))};
}
/** `current` moved toward `wanted` by the shortest way, no further than `most`. */
f32 approach(f32 current, f32 wanted, f32 most) {
    return current + std::clamp(TreePose::wrapAngle(wanted - current), -most, most);
}
} // namespace

Vec2 CombatantGaze::tracked(usize index) const {
    const Bearing& bearing = m_tracked[std::min(index, m_tracked.size() - 1)];
    return Vec2{bearing.yaw, bearing.pitch};
}

void CombatantGaze::aim(TreePose& pose, const TreeInfo& tree, const Mat4& model,
                        std::span<const LookDefinition> looks, const std::optional<Vec3>& target,
                        f32 seconds) {
    const f32 chase = kChaseRate * seconds;
    for (usize i = 0; i < std::min(looks.size(), m_tracked.size()); ++i) {
        const LookDefinition& look = looks[i];
        if (!look.turns() || !pose.posed()) {
            continue;
        }
        std::optional<u32> node = tree.findNode(look.node);
        if (node.has_value() && look.parent && tree.nodes[*node].parent >= 0) {
            node = static_cast<u32>(tree.nodes[*node].parent);
        }
        if (!node.has_value() || *node >= pose.size()) {
            continue;
        }
        // Where the target lies from the way the node faces now, pitched by the bias; nowhere
        // without one.
        Vec2 wanted{0.0f, 0.0f};
        if (target.has_value()) {
            const Mat4 world = model * pose.matrices()[*node];
            const Vec2 facing = bearingOf(Vec3{world[2]});
            wanted = bearingOf(*target - Vec3{world[3]}) - facing + Vec2{0.0f, look.pitchBias};
        }
        Bearing& bearing = m_tracked[i];
        bearing.yaw = approach(bearing.yaw, wanted.x, chase);
        bearing.pitch = approach(bearing.pitch, wanted.y, chase);
        // The node turns from its animated pose toward the tracked bearing, no further than
        // its limits.
        Vec3 angles = pose.readAngles(*node);
        angles.y = approach(angles.y, bearing.yaw, look.yawRate);
        angles.x = approach(angles.x, bearing.pitch, look.pitchRate);
        pose.setPitchYawRoll(*node, angles);
    }
}
} // namespace gdl::game
