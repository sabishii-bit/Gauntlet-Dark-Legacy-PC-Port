#include "game/screens/PartyCollision.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

/** The nearest point to `point` of the step from `from` to `to`, along the ground. */
Vec2 closestOnStep(const Vec2& point, const Vec2& from, const Vec2& to) {
    const Vec2 step = to - from;
    const f32 length = glm::dot(step, step);
    if (length <= 0.0f) {
        return from;
    }
    const f32 along = std::clamp(glm::dot(point - from, step) / length, 0.0f, 1.0f);
    return from + step * along;
}

} // namespace

std::optional<usize> PartyCollision::resolve(std::span<const PlayerRuntime> players, usize mover,
                                             const Vec3& from, Vec3& to) {
    if (mover >= players.size()) {
        return std::nullopt;
    }
    const PlayerActor& self = players[mover].actor;
    const Vec2 start{from.x, from.z};
    const Vec2 end{to.x, to.z};
    std::optional<usize> nearest;
    f32 best = 0.0f;
    for (usize i = 0; i < players.size(); ++i) {
        const PlayerRuntime& other = players[i];
        // Only the standing count, and not one carried off or hung on a partner (hud_flags
        // 0x20).
        if (i == mover || other.life != PlayerLife::Standing || other.capture.held() ||
            other.combo.riding) {
            continue;
        }
        const Vec3& centre = other.actor.position();
        const Vec2 at{centre.x, centre.z};
        // Only what lies ahead of the step.
        if (glm::dot(at - start, end - start) < 0.0f) {
            continue;
        }
        const f32 reach = self.radius() + other.actor.radius();
        const Vec2 contact = closestOnStep(at, start, end);
        const bool overlaps =
            to.y < centre.y + other.actor.height() && to.y + self.height() > centre.y;
        if (!overlaps || glm::distance(contact, at) > reach) {
            continue;
        }
        const f32 distance = glm::distance(contact, end);
        if (!nearest.has_value() || distance < best) {
            nearest = i;
            best = distance;
        }
    }
    if (!nearest.has_value()) {
        return std::nullopt;
    }
    const PlayerActor& other = players[*nearest].actor;
    const Vec2 away = end - Vec2{other.position().x, other.position().z};
    const f32 distance = glm::length(away);
    if (distance > kCoincident) {
        const f32 scale = (self.radius() + other.radius() - distance) / distance;
        to.x += away.x * scale;
        to.z += away.y * scale;
    } else {
        to = from;
    }
    return nearest;
}

void PartyCollision::step(std::span<PlayerRuntime> players, usize mover, const Vec3& from,
                          f32 seconds, const WorldCollision* collision) {
    if (mover >= players.size()) {
        return;
    }
    PlayerActor& actor = players[mover].actor;
    const Vec3 meant = actor.position();
    Vec3 to = meant;
    if (const auto other = resolve(players, mover, from, to)) {
        players[*other].knockback.shove(meant - from, seconds);
        if (collision != nullptr) {
            actor.slide(to - actor.position(), collision);
        } else {
            actor.place(to);
        }
    }
}

} // namespace gdl::game
