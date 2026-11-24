#include "game/party/PartyCollision.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

#include "game/party/BodyCollision.h"
#include "game/players/ExitWait.h"

namespace gdl::game {

namespace {

/** Which of the members a step runs into decides: the one nearest its end, or the first in
 * seat order (PlayerCollidePlayers' stopFirst). */
enum class Decider : u8 { Nearest, First };

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

/** PlayerCollidePlayers (0x80087830): the member `mover`'s step from `from` to `to` runs into
 * within `share` of its width and the whole of theirs, `to` put out to that from them. */
std::optional<usize> meet(std::span<const PlayerRuntime> players, usize mover, const Vec3& from,
                          Vec3& to, f32 share, Decider decider) {
    if (mover >= players.size()) {
        return std::nullopt;
    }
    const PlayerActor& self = players[mover].actor;
    const Vec2 start{from.x, from.z};
    const Vec2 end{to.x, to.z};
    // LineCylinderCollide's band about the other's collision centre is the mover's col_height,
    // half its height.
    const f32 middle = to.y + BodyCollision::contactHeight(self);
    const f32 band = 0.5f * self.height();
    std::optional<usize> met;
    f32 best = 0.0f;
    for (usize i = 0; i < players.size(); ++i) {
        const PlayerRuntime& other = players[i];
        // Only the standing count, and not one carried off or hung on a partner (hud_flags
        // 0x20).
        if (i == mover || other.life != PlayerLife::Standing || other.departed ||
            other.capture.held() || other.combo.riding) {
            continue;
        }
        const Vec3& centre = other.actor.position();
        const Vec2 at{centre.x, centre.z};
        // Only what lies ahead of the step.
        if (glm::dot(at - start, end - start) < 0.0f) {
            continue;
        }
        const f32 reach = share * self.reach() + other.actor.reach();
        const Vec2 contact = closestOnStep(at, start, end);
        const f32 rise = centre.y + BodyCollision::contactHeight(other.actor) - middle;
        if (std::abs(rise) > band || glm::distance(contact, at) > reach) {
            continue;
        }
        const f32 distance = glm::distance(contact, end);
        if (!met.has_value() || distance < best) {
            met = i;
            best = distance;
        }
        if (decider == Decider::First) {
            break;
        }
    }
    if (!met.has_value()) {
        return std::nullopt;
    }
    const PlayerActor& other = players[*met].actor;
    const Vec2 away = end - Vec2{other.position().x, other.position().z};
    const f32 distance = glm::length(away);
    if (distance > PartyCollision::kCoincident) {
        const f32 scale = (share * self.reach() + other.reach() - distance) / distance;
        to.x += away.x * scale;
        to.z += away.y * scale;
    } else {
        to = from;
    }
    return met;
}

} // namespace

std::optional<usize> PartyCollision::resolve(std::span<const PlayerRuntime> players, usize mover,
                                             const Vec3& from, Vec3& to, f32 share) {
    return meet(players, mover, from, to, share, Decider::Nearest);
}

Vec3 PartyCollision::block(std::span<const PlayerRuntime> players, usize mover, const Vec3& from,
                           Vec3& to, f32 seconds) {
    Vec3 out = to;
    if (!meet(players, mover, from, out, kBlockShare, Decider::First).has_value()) {
        return Vec3{0.0f};
    }
    const BodyCollision::Trim trim = BodyCollision::trimToward(from, to, out, seconds);
    to = trim.to;
    return trim.push;
}

std::optional<usize> PartyCollision::step(std::span<PlayerRuntime> players, usize mover,
                                          const Vec3& from, const Vec3& meant, f32 seconds,
                                          const WorldCollision* collision) {
    if (mover >= players.size()) {
        return std::nullopt;
    }
    PlayerActor& actor = players[mover].actor;
    Vec3 to = meant;
    const auto other = resolve(players, mover, from, to);
    if (!other.has_value()) {
        return std::nullopt;
    }
    // One standing on an exit for the rest of the party holds its ground (idle_timer, +0x950).
    if (players[*other].exitWait.ticks() == 0) {
        players[*other].knockback.shove(meant - from, seconds);
    }
    const Vec3 out = to - meant;
    if (collision != nullptr) {
        actor.slide(out, collision);
    } else {
        actor.place(actor.position() + out);
    }
    return other;
}

} // namespace gdl::game
