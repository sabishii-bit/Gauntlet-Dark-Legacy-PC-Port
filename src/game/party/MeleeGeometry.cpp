#include "game/party/MeleeGeometry.h"

#include <cmath>
#include <vector>

#include "engine/math/PortableMath.h"

#include "game/combat/BodyContact.h"
#include "game/party/TargetAssist.h"

namespace gdl::game {
MissileTarget MeleeGeometry::body(const Target& target) {
    MissileTarget result{target.id, target.base, target.radius, target.height};
    result.acquisition = target.acquisition;
    return result;
}

MeleeSelection::Target MeleeGeometry::candidate(const Vec3& feet, f32 height,
                                                const MissileTarget& target, bool low) {
    const Vec3 origin = feet + Vec3{0, height * 0.5f, 0};
    const Vec3 point = target.acquisition ? target.acquisition->point : target.pointNear(origin);
    const Vec3 offset = point - origin;
    const f32 distance = TargetAssist::distanceTo(feet, height, target);
    const f32 score = target.acquisition ? glm::length(offset) * target.acquisition->distanceScale -
                                               target.acquisition->radius
                                         : distance;
    return {target.base, offset, distance, score, low};
}

std::optional<MeleeGeometry::Contact> MeleeGeometry::contact(std::span<const Target> targets,
                                                             const Vec3& from, const Vec3& to,
                                                             f32 width, f32 halfHeight,
                                                             const WorldCollision* collision) {
    std::optional<Contact> best;
    s32 bestCell = 0;
    for (usize remaining = targets.size(); remaining > 0; --remaining) {
        const usize index = remaining - 1;
        const auto& target = targets[index];
        if (!target.contact) {
            continue;
        }
        const Vec3 centre = target.acquisition.point;
        const f32 radius = width + target.radius;
        const f32 enemyHalfHeight = target.height * 0.5f;
        const Vec3 away = centre - to;
        Vec3 touch;
        if (away.x * away.x + away.z * away.z >= radius * radius ||
            std::abs(away.y) >= enemyHalfHeight ||
            !movementTouchesBody(from, to, centre, radius, halfHeight + enemyHalfHeight, &touch)) {
            continue;
        }
        const f32 distance = portable::hypot(touch.x - to.x, touch.z - to.z);
        if (!best || distance < best->endpointDistance ||
            (distance == best->endpointDistance && target.contactCell < bestCell)) {
            best = Contact{index, distance};
            bestCell = target.contactCell;
        }
    }
    if (best && collision != nullptr &&
        collision->wallBetween(from, targets[best->index].acquisition.point)) {
        return std::nullopt;
    }
    return best;
}

MeleeSelection::Snapshot MeleeGeometry::snapshot(std::span<const Target> targets, const Vec3& feet,
                                                 f32 height, f32 reach, f32 range, f32 facingDot,
                                                 const Vec3& followPoint, const Vec3& from,
                                                 const Vec3& to, const WorldCollision* collision) {
    MeleeSelection::Snapshot result;
    result.feet = feet;
    result.reach = reach;
    result.range = range;
    result.facingDot = facingDot;
    const f32 queryReach = reach + MeleeSelection::kStepReach + MeleeSelection::kHeldReach;
    for (const auto& target : targets) {
        const auto physical = body(target);
        const auto value = candidate(feet, height, physical, target.low);
        if (!target.acquisition.enabled ||
            std::abs(value.offset.y) > target.acquisition.maxHeight ||
            value.distance >= queryReach || value.score >= range ||
            !TargetAssist::around(feet, height, std::span{&physical, 1}, queryReach, collision)) {
            continue;
        }
        result.targets.push_back(value);
    }
    if (const auto selected = contact(targets, from, to, reach, height * 0.5f, collision)) {
        const auto& target = targets[selected->index];
        auto value = candidate(feet, height, body(target), target.low);
        value.offset = target.acquisition.point - followPoint;
        value.distance = glm::length(value.offset) - target.radius;
        result.contact = MeleeSelection::Contact{value, target.requireFacing};
    }
    return result;
}

std::optional<Vec3> MeleeGeometry::aim(std::span<const Target> targets, const Vec3& origin,
                                       const Vec3& facing, f32 range,
                                       const WorldCollision* collision, f32 facingDot,
                                       const MeleeSelection::Snapshot& acquisition) {
    if (acquisition.contact && MeleeSelection::accepts(*acquisition.contact, facing, range)) {
        return origin + acquisition.contact->target.offset;
    }
    std::vector<MissileTarget> bodies;
    bodies.reserve(targets.size());
    for (const auto& target : targets) {
        bodies.push_back(body(target));
    }
    return TargetAssist::select(origin, facing, bodies, range, collision, facingDot);
}
} // namespace gdl::game
