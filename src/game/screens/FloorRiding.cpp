#include "game/screens/FloorRiding.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {

void FloorRiding::carry(PlayerRuntime& runtime, const WorldCollision& collision) {
    PlayerRuntime::Floor& floor = runtime.floor;
    if (!floor.placement.has_value() || (floor.flags & kMoving) == 0) {
        return;
    }
    const std::optional<Mat4> now = collision.objectTransform(floor.object);
    if (!now.has_value()) {
        return;
    }
    // The body keeps its place and facing in the floor's own space.
    const Mat4 moved = *now * glm::inverse(*floor.placement);
    PlayerActor& actor = runtime.actor;
    actor.place(Vec3{moved * Vec4{actor.position(), 1.0f}});
    const f32 yaw = actor.yaw();
    const Vec3 facing = Mat3{moved} * Vec3{std::sin(yaw), 0.0f, std::cos(yaw)};
    if (facing.x != 0.0f || facing.z != 0.0f) {
        actor.turnTo(std::atan2(facing.x, facing.z));
    }
    floor.placement = now;
}

bool FloorRiding::keptApart(std::span<const PlayerRuntime> players, usize index, s32 object) {
    for (usize i = 0; i < players.size(); ++i) {
        const PlayerRuntime& other = players[i];
        if (i != index && other.life == PlayerLife::Standing && other.floor.object >= 0 &&
            other.floor.object != object && (other.floor.flags & kKeptApart) != 0) {
            return true;
        }
    }
    return false;
}

void FloorRiding::land(std::span<PlayerRuntime> players, usize index, const Vec3& from,
                       const WorldCollision& collision) {
    if (index >= players.size()) {
        return;
    }
    PlayerRuntime& runtime = players[index];
    PlayerActor& actor = runtime.actor;
    const auto support = [&] {
        auto hit = collision.floorAt(actor.position(), kProbeAbove, kProbeBelow,
                                     PlayerActor::kFloorEdgeReach);
        // Keep ownership consistent with the footprint supporting the feet,
        // even when lower ground lies beneath the body's centre. A centre
        // contact already at foot height wins over an adjacent near-level face.
        const auto footprint = collision.floorAt(actor.position(), PlayerActor::kFloorEdgeReach,
                                                 kProbeBelow, actor.radius());
        if (footprint && (!hit || (hit->y < actor.position().y - PlayerActor::kFloorEdgeReach &&
                                   footprint->y > hit->y))) {
            hit = footprint;
        }
        return hit ? hit
                   : collision.floorAt(actor.position(), kProbeAbove, kProbeBelow, actor.radius());
    };
    std::optional<FloorHit> hit = support();
    // PlayerNewFloor: an active lift keeps its riders on that same floor.
    // `from` already includes carry(), so refusing a step does not undo lift travel.
    if (collision.floorExitBlocked(runtime.floor.object) &&
        (!hit.has_value() || hit->object != runtime.floor.object)) {
        actor.place(from);
        return;
    }
    if (hit.has_value() && (hit->objectFlags & kMoving) != 0 &&
        keptApart(players, index, hit->object)) {
        actor.place(Vec3{from.x, actor.position().y, from.z});
        hit = support();
    }
    PlayerRuntime::Floor& floor = runtime.floor;
    floor.object = hit.has_value() ? hit->object : -1;
    floor.flags = hit.has_value() ? hit->objectFlags : 0;
    floor.placement =
        (floor.flags & kMoving) != 0 ? collision.objectTransform(floor.object) : std::nullopt;
}

} // namespace gdl::game
