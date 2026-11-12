#include "game/party/FloorRiding.h"

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

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
    actor.carry(Vec3{moved * Vec4{actor.position(), 1.0f}});
    const f32 yaw = actor.yaw();
    const Vec3 facing = Mat3{moved} * Vec3{portable::sin(yaw), 0.0f, portable::cos(yaw)};
    if (facing.x != 0.0f || facing.z != 0.0f) {
        actor.turnTo(portable::atan2(facing.x, facing.z));
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

std::optional<FloorHit> FloorRiding::support(const PlayerActor& actor,
                                             const WorldCollision& collision) {
    // FloorCollide's sphere from the collision centre: the face under the point, else the
    // nearest edge within the body's width, as a player's probe meets them.
    return collision.floorUnder(actor.followPoint(),
                                PlayerActor::kFloorProbe + actor.height() * 0.5f, actor.reach(),
                                WorldCollision::Prober::Player);
}

void FloorRiding::land(std::span<PlayerRuntime> players, usize index, const Vec3& from,
                       const WorldCollision& collision) {
    if (index >= players.size()) {
        return;
    }
    PlayerRuntime& runtime = players[index];
    PlayerActor& actor = runtime.actor;
    std::optional<FloorHit> hit = support(actor, collision);
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
        hit = support(actor, collision);
    }
    PlayerRuntime::Floor& floor = runtime.floor;
    floor.object = hit.has_value() ? hit->object : -1;
    floor.flags = hit.has_value() ? hit->objectFlags : 0;
    floor.placement =
        (floor.flags & kMoving) != 0 ? collision.objectTransform(floor.object) : std::nullopt;
}

} // namespace gdl::game
