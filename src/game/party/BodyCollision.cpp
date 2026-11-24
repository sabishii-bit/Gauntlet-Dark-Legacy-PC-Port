#include "game/party/BodyCollision.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/combat/BodyContact.h"
#include "game/players/PlayerActor.h"

namespace gdl::game {
namespace {
/** What a step may not cross: a centre along the ground and how near it lets the step come. */
struct Circle {
    Vec2 centre{0};
    f32 reach = 0;
};

/** How far a step may head in toward a body's middle and still run along it, as a share of its
 * length times its distance from the middle: what rounding leaves of a slide along the body. */
constexpr f32 kAlongSlack = 1e-4f;
} // namespace

ContactCylinder BodyCollision::critterContact(const MissileTarget& volume, f32 width) {
    const f32 halfHeight = 0.5f * volume.height;
    return {volume.base + Vec3{0, halfHeight, 0}, width + volume.radius, width + halfHeight};
}

void BodyCollision::addCritterContacts(std::vector<ContactCylinder>& cylinders,
                                       const PlayerActor& player,
                                       std::span<const MissileTarget> volumes) {
    // PlayerCollideItems meets the great ones by col_radius, the class's whole width
    // (CritterMoveNodeCol, 0x800374FC).
    for (const MissileTarget& volume : volumes) {
        cylinders.push_back(critterContact(volume, player.reach()));
    }
}

f32 BodyCollision::contactHeight(const PlayerActor& player) {
    return player.followPoint().y - player.position().y;
}

bool BodyCollision::touches(const Vec3& from, const Vec3& to,
                            std::span<const ContactCylinder> cylinders) {
    return std::ranges::any_of(cylinders, [&](const ContactCylinder& cylinder) {
        return movementTouchesBody(from, to, cylinder.centre, cylinder.radius, cylinder.halfHeight);
    });
}

BodyCollision::Trim BodyCollision::trimToward(const Vec3& from, const Vec3& to, const Vec3& hit) {
    const auto axis = [](f32 step, f32 out, f32& away) {
        if (out > 0.0f && step < 0.0f) {
            return std::min(step + out, 0.0f);
        }
        if (out < 0.0f && step > 0.0f) {
            return std::max(step + out, 0.0f);
        }
        away += out;
        return 0.0f;
    };
    Trim trim;
    const f32 x = axis(to.x - from.x, hit.x - to.x, trim.away.x);
    const f32 z = axis(to.z - from.z, hit.z - to.z, trim.away.z);
    trim.to = Vec3{from.x + x, to.y, from.z + z};
    return trim;
}

Vec3 BodyCollision::holdOff(const PlayerActor& player, const Vec3& from, const Vec3& to,
                            std::span<const Obstacle> items) {
    return resolveItems(from, to, player.reach(), player.height(), items);
}

Vec3 BodyCollision::resolveItems(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                                 std::span<const Obstacle> items) {
    if (items.empty() || radius <= 0 || height <= 0) {
        return to;
    }
    const f32 distance = glm::length(Vec2{to.x - from.x, to.z - from.z});
    const auto steps = std::max(1, static_cast<s32>(std::ceil(distance / (radius * 0.5f))));
    const Vec3 step = (to - from) / static_cast<f32>(steps);
    Vec3 position = from;
    for (s32 i = 0; i < steps; ++i) {
        position += step;
        for (const Obstacle& item : items) {
            if (!item.solid || position.y >= item.centre.y + item.height ||
                position.y + height <= item.centre.y) {
                continue;
            }
            // Horizontal push-out uses the body's actual vertical overlap, not the item's
            // own height as a substitute for the player's height.
            const Vec3 probe{position.x, item.centre.y, position.z};
            const Vec3 pushed = item.pushOut(probe, radius);
            position.x = pushed.x;
            position.z = pushed.z;
        }
    }
    position.y = to.y;
    return position;
}

Vec3 BodyCollision::resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                            std::span<const MissileTarget> bodies) {
    return resolve(from, to, radius, height, bodies, 0, {});
}

Vec3 BodyCollision::resolve(const Vec3& from, const Vec3& to, f32 radius, f32 height,
                            std::span<const MissileTarget> bodies, f32 centreHeight,
                            std::span<const ContactCylinder> cylinders) {
    std::vector<Circle> circles;
    circles.reserve(bodies.size() + cylinders.size());
    for (const auto& body : bodies) {
        if (body.radius <= 0 || body.height <= 0 ||
            std::min(from.y, to.y) >= body.base.y + body.height ||
            std::max(from.y, to.y) + height <= body.base.y) {
            continue;
        }
        circles.push_back({{body.base.x, body.base.z}, radius + body.radius});
    }
    // LineCylinderCollide (0x8002FA70) lets the centre pass only beyond the half height, so a
    // band's edges still hold it.
    const f32 lowest = std::min(from.y, to.y) + centreHeight;
    const f32 highest = std::max(from.y, to.y) + centreHeight;
    for (const auto& cylinder : cylinders) {
        if (cylinder.radius <= 0 || cylinder.centre.y - cylinder.halfHeight > highest ||
            cylinder.centre.y + cylinder.halfHeight < lowest) {
            continue;
        }
        circles.push_back({{cylinder.centre.x, cylinder.centre.z}, cylinder.radius});
    }
    Vec2 position{from.x, from.z};
    Vec2 remaining{to.x - from.x, to.z - from.z};
    // Re-sweep the slide, so resolving one body cannot push through another.
    for (s32 iteration = 0; iteration < 4; ++iteration) {
        const f32 lengthSquared = glm::dot(remaining, remaining);
        if (lengthSquared < 1e-10f) {
            break;
        }
        f32 first = 1;
        Vec2 normal{0};
        bool hit = false;
        for (const Circle& circle : circles) {
            const Vec2 delta = position - circle.centre;
            const f32 separation = glm::dot(delta, delta) - circle.reach * circle.reach;
            const f32 toward = glm::dot(delta, remaining);
            // A spawn/animation may begin overlapping. Allow escape, not a deeper step. A slide
            // along a body it rests against is no deeper step either, though rounding leaves it
            // heading in by a hair, which would meet the body again where it stands.
            if (toward >= -kAlongSlack * std::sqrt(lengthSquared * glm::dot(delta, delta))) {
                continue;
            }
            const f32 discriminant = toward * toward - lengthSquared * separation;
            if (discriminant < 0) {
                continue;
            }
            const f32 time =
                separation <= 0 ? 0 : (-toward - std::sqrt(discriminant)) / lengthSquared;
            if (time < 0 || time > first) {
                continue;
            }
            const Vec2 contact = delta + remaining * time;
            const f32 distance = glm::length(contact);
            if (distance <= 1e-5f) {
                continue;
            }
            first = time;
            normal = contact / distance;
            hit = true;
        }
        position += remaining * first;
        if (!hit) {
            break;
        }
        remaining *= 1 - first;
        remaining -= normal * std::min(glm::dot(remaining, normal), 0.0f);
    }
    return {position.x, to.y, position.y};
}

Vec3 BodyCollision::resolveMovement(const PlayerActor& player, const Vec3& from, const Vec3& to,
                                    std::span<const MissileTarget> bodies,
                                    std::span<const Obstacle> generators,
                                    const WorldCollision* collision,
                                    std::span<const MissileTarget> critters) {
    // The great ones are met about the collision centre.
    std::vector<ContactCylinder> cylinders;
    cylinders.reserve(critters.size());
    addCritterContacts(cylinders, player, critters);
    const f32 centreHeight = contactHeight(player);
    Vec3 resolved =
        resolve(from, to, player.radius(), player.height(), bodies, centreHeight, cylinders);
    // The generators hold the body its whole width off, as the level's other items do.
    resolved = holdOff(player, from, resolved, generators);
    if (collision != nullptr) {
        // A body's push can leave the player in a wall: put PlayerWallCollide's sphere
        // back out of the still faces it is in front of. Animated contacts have already
        // been swept. A face the player stands behind is air to it (BTriLineCol), so it
        // is never put through a one-sided railing.
        const Vec3 lift{0.0f, centreHeight + PlayerActor::kWallSphereLift, 0.0f};
        resolved = collision->resolveSphere(resolved + lift, player.reach(), true) - lift;
        const Vec3 checked = resolve(from, resolved, player.radius(), player.height(), bodies,
                                     centreHeight, cylinders);
        if (glm::distance(checked, resolved) > 1e-4f) {
            return {from.x, to.y, from.z};
        }
        const Vec3 itemChecked = holdOff(player, resolved, resolved, generators);
        if (glm::distance(itemChecked, resolved) > 1e-4f) {
            return {from.x, to.y, from.z};
        }
    }
    return resolved;
}
} // namespace gdl::game
