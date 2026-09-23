#include <cmath>
#include <limits>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
void Combatant::grab(Actor& actor, const MoveDefinition& move, const AttackDefinition& damage,
                     bool release, std::span<const EnemyView> players) {
    if (release) {
        if (actor.grabbed >= 0) {
            carryGrab(actor, players);
            if (actor.grabbed < 0) {
                return;
            }
            // CritterReleasePlayer uses the body's forward, with Y replaced by -0.1.
            const Vec3 direction{std::sin(actor.yaw), -0.1f, std::cos(actor.yaw)};
            m_grabs.push_back({actor.grabbed, m_id, std::nullopt,
                               glm::normalize(direction) * damage.speed,
                               damage.damage * m_scales.damage});
            actor.grabbed = -1;
        }
        return;
    }
    if (actor.grabbed >= 0) {
        return;
    }
    // Collision uses the active node's world origin plus a BODY-rotated offset.
    // Once attached, the offset is local to the animated node instead.
    const Vec3 centre =
        partPosition(actor, move.colnode) + Vec3{modelTransform(actor) * Vec4{damage.offset, 0}};
    f32 nearest = std::numeric_limits<f32>::max();
    for (const EnemyView& player : players) {
        const Vec3 delta = player.position + Vec3{0, player.height * 0.5f, 0} - centre;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        const Vec3 toBody = player.position - actor.position;
        const f32 bodyDistance = glm::length(Vec2{toBody.x, toBody.z});
        if (!player.hidden && !player.captured && bodyDistance < nearest &&
            distance <= player.radius + damage.maxDistance &&
            std::abs(delta.y) <= player.height * 0.5f + damage.maxDistance) {
            nearest = bodyDistance;
            actor.grabbed = player.player;
        }
    }
    if (actor.grabbed >= 0) {
        actor.grabMove = actor.move;
        actor.grabNode = move.colnode;
        actor.grabOffset = damage.offset;
    }
}
void Combatant::carryGrab(Actor& actor, std::span<const EnemyView> players) {
    if (actor.grabbed < 0) {
        return;
    }
    const EnemyView* player = viewOf(players, actor.grabbed);
    if (player == nullptr || player->hidden || actor.state != State::Active) {
        m_grabs.push_back({actor.grabbed, m_id, std::nullopt, Vec3{0}, 0});
        actor.grabbed = -1;
        return;
    }
    m_grabs.push_back({actor.grabbed, m_id,
                       glm::translate(attachmentTransform(actor, actor.grabNode), actor.grabOffset),
                       Vec3{0}, 0});
}
} // namespace gdl::game
