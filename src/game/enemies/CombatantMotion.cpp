#include <cmath>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
#include "game/enemies/EnemyMind.h"
namespace gdl::game {
namespace {
f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
f32 yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}
} // namespace

namespace {
constexpr f32 kStepUp = 2.0f;
constexpr f32 kDrop = 6.0f;
constexpr f32 kFootClearance = 0.1f;
constexpr f32 kBlindTurnShare = 0.1f;
} // namespace
void Combatant::carry(Actor& critter, f32 seconds, const MoveDefinition* move,
                      std::span<const EnemyView> players, std::span<const Combatant> peers) {
    const CritterMovement& movement = critter.definition->movement();
    const bool bounded = critter.stock->definition.boundsToHome;
    // CritterBossAI (GC 0x80039ad8) probes the supporting floor every frame,
    // independently of locomotion. In A5 this is the descending elevator.
    // position is floor-space here; rootTransform adds the TYPE floorOffset.
    if (bounded && m_collision != nullptr) {
        if (const auto floor = m_collision->floorAt(critter.position, 4.0f, 1000.0f)) {
            critter.position.y = floor->y;
        }
    }
    const EnemyView* view = viewOf(players, critter.target);
    if (move != nullptr && move->turnRate > 0.0f && view != nullptr && critter.grabbed < 0) {
        const f32 wanted =
            movement.facing(yawBetween(critter.position, view->position), critter.initialYaw);
        const f32 d = wrapAngle(wanted - critter.yaw);
        // Blinded, it turns at a tenth of its rate.
        const f32 step =
            move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw =
            wrapAngle(std::abs(d) <= step ? wanted : critter.yaw + (d > 0.0f ? step : -step));
    }
    if (bounded && movement.roamRadius <= 0.0f) {
        return; // Zero-radius bosses may turn, but neither locomotion nor knockback moves them.
    }
    Vec3 translation = critter.push * seconds;
    if (move != nullptr && move->type == MoveDefinition::kStepToPoint && view != nullptr) {
        // The ready-move search refreshes targetPos during local locomotion.
        // CritterInitHeader enables that search from the MOVE roster, even when
        // the raw TYPE does not yet have its derived 0x10000 flag.
        critter.stepTarget = view->position;
    }
    if (move != nullptr && move->speed != 0.0f && critter.state == State::Active) {
        const f32 pace = move->speed * m_scales.speed * seconds;
        const f32 basis = movement.initialStepBasis ? critter.initialYaw : critter.yaw;
        if (move->type == MoveDefinition::kStepToPoint && critter.stepTarget.has_value()) {
            const Vec3 delta = *critter.stepTarget - critter.position;
            const f32 distance = glm::length(Vec2{delta.x, delta.z});
            if (distance > 0.0001f) {
                translation += Vec3{delta.x, 0, delta.z} * (pace / distance);
            }
        } else {
            translation += CritterMovement::direction(move->type, basis) * pace;
        }
    }
    if (glm::length(translation) <= 0.0f) {
        return;
    }
    Vec3 to = critter.position + translation;
    if (bounded && critter.state != State::Dying) {
        to = movement.constrain(to, critter.homePosition);
    }
    // Retail bosses clamp to their home region directly. Their authored stage/root
    // heights are not ordinary walking-floor probes (several stand above the arena).
    if (bounded) {
        critter.position = to;
        return;
    }
    // Never onto a player: it stops against them.
    for (const EnemyView& other : players) {
        if (!other.hidden &&
            flatDistance(other.position, to) < other.radius + critter.definition->radius()) {
            return;
        }
    }
    if (m_collision != nullptr) {
        const f32 wallRadius = critter.definition->wallRadius();
        to = m_collision->resolveWalls(to, wallRadius, to.y + kFootClearance, to.y + 8.0f);
        const auto floor = m_collision->floorAt(to, kStepUp, kDrop);
        if (!floor.has_value()) {
            return;
        }
        to.y = floor->y;
    }
    for (const Combatant& peer : peers) {
        const Actor& other = peer.m_actor;
        if (&other == &critter || other.state == State::Inactive) {
            continue;
        }
        if (flatDistance(other.position, to) <
            other.stock->data.radius() + critter.definition->radius()) {
            return;
        }
    }
    critter.position = to;
}

} // namespace gdl::game
