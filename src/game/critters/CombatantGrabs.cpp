#include <cmath>
#include <limits>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/critters/Combatant.h"

namespace gdl::game {
void Combatant::grab(Actor& actor, const MoveDefinition& move, const Mat4& moveFrame,
                     const AttackDefinition& damage, bool release,
                     std::span<const EnemyView> players) {
    if (release) {
        if (actor.grabbed >= 0) {
            carryGrab(actor, players);
            if (actor.grabbed < 0) {
                return;
            }
            if (actor.definition->rules().pressureReferenceDamage > 0) {
                // Native boss release reaches the same raw contact tally as a
                // node strike, before the player applies immunity or armor.
                m_sim.pressure.note(actor.grabbed, damage.damage);
            }
            // CritterReleasePlayer uses the body's forward, with Y replaced by -0.1, and strikes
            // the player through CritterDamagePlayer, which sets its fxhittime as a blow's does.
            const Vec3 direction{portable::sin(actor.yaw), -0.1f, portable::cos(actor.yaw)};
            m_sim.grabs.push_back({.player = actor.grabbed,
                                   .critter = m_sim.id,
                                   .attachment = std::nullopt,
                                   .velocity = glm::normalize(direction) * damage.speed,
                                   .damage = damage.damage * m_sim.scales.damage,
                                   .breathGap = actor.definition->rules().playerBreathGap});
            actor.grabbed = -1;
        }
        return;
    }
    if (actor.grabbed >= 0) {
        return;
    }
    // Collision is a blow's: the offset turned with the move's node, whole however the body is
    // drawn (CritterGrabPlayer through CritterNodePlayerCollide's worldMoveMatrix). Once
    // attached, the player is held at the offset on that node (PlayerSetParent to obj_d0), as
    // the look leaves it.
    const Vec3 centre{moveFrame * Vec4{damage.offset, 1}};
    f32 nearest = std::numeric_limits<f32>::max();
    for (const EnemyView& player : players) {
        if (player.player < 0 || static_cast<usize>(player.player) >= kPlayerSlots) {
            continue;
        }
        const Vec3 delta = player.position + Vec3{0, player.height * 0.5f, 0} - centre;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        const Vec3 toBody = player.position - actor.position;
        const f32 bodyDistance = glm::length(Vec2{toBody.x, toBody.z});
        const bool closer =
            bodyDistance < nearest || (bodyDistance == nearest && player.player < actor.grabbed);
        // CritterNodePlayerCollide scans slots 0..3, regardless of the view's order, and widens
        // the reach by the player's whole width (col_radius).
        if (!player.hidden && player.damageable && !player.captured && closer &&
            distance <= player.collisionRadius.value_or(player.radius) + damage.maxDistance &&
            std::abs(delta.y) <= player.height * 0.5f + damage.maxDistance) {
            nearest = bodyDistance;
            actor.grabbed = player.player;
        }
    }
    if (actor.grabbed >= 0) {
        actor.grabMove = actor.move;
        actor.grabNode = move.colnode;
        actor.grabOffset = damage.offset;
        if (damage.hitSound >= 0) {
            cue(actor, m_sim.id, damage.hitSound, centre, std::nullopt, nullptr, actor.grabbed);
        }
    }
}
void Combatant::carryGrab(Actor& actor, std::span<const EnemyView> players) {
    if (actor.grabbed < 0) {
        return;
    }
    const EnemyView* player = viewOf(players, actor.grabbed);
    if (player == nullptr || player->hidden || actor.state != State::Active) {
        m_sim.grabs.push_back({actor.grabbed, m_sim.id, std::nullopt, Vec3{0}, 0});
        actor.grabbed = -1;
        return;
    }
    m_sim.grabs.push_back(
        {actor.grabbed, m_sim.id,
         glm::translate(moveNodeTransform(actor, actor.grabNode), actor.grabOffset), Vec3{0}, 0,
         actor.definition->rules().restoreCaptureSupport});
}
} // namespace gdl::game
