#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

#include "game/combat/BodyContact.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/EnemyMind.h"
namespace gdl::game {
namespace {
f32 yawBetween(const Vec3& from, const Vec3& to) {
    return std::atan2(to.x - from.x, to.z - from.z);
}
} // namespace

namespace {
constexpr f32 kStepUp = 2.0f;
constexpr f32 kDrop = 6.0f;
constexpr f32 kFootClearance = 0.1f;
constexpr f32 kWallProbeHeight = 8.0f; ///< how high over its feet walls stop it
constexpr f32 kBlindTurnShare = 0.1f;
constexpr f32 kSupportReach = 0.1f;
} // namespace
void Combatant::rememberFloor() {
    Actor& actor = m_actor;
    actor.floor.reset();
    if (m_collision == nullptr || !present() || actor.parent != nullptr ||
        actor.stock->definition.boundsToHome) {
        return;
    }
    const auto floor = m_collision->floorAt(actor.position, kSupportReach, kSupportReach);
    if (floor) {
        if (const auto placement = m_collision->objectTransform(floor->object)) {
            actor.floor = Actor::Floor{floor->object,
                                       Vec3{glm::inverse(*placement) * Vec4{actor.position, 1}}};
        }
    }
}

void Combatant::syncFloor() {
    Actor& actor = m_actor;
    if (!present() || m_collision == nullptr || !actor.floor) {
        return;
    }
    const auto placement = m_collision->objectTransform(actor.floor->object);
    if (!placement || !m_collision->solid(actor.floor->object)) {
        actor.floor.reset();
        return;
    }
    const Vec3 carried = Vec3{*placement * Vec4{actor.floor->local, 1}};
    actor.presentation.position += carried - actor.position;
    actor.position = carried;
}

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
    const EnemyView* view = viewOf(players, critter.moveTarget);
    if (move != nullptr && move->turnRate > 0.0f && view != nullptr && critter.grabbed < 0) {
        const f32 wanted =
            movement.facing(yawBetween(critter.position, view->position), critter.initialYaw);
        const f32 d = wrapAngle(wanted - critter.yaw);
        // Blinded, it turns at a tenth of its rate.
        const f32 step =
            move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw =
            wrapAngle(std::abs(d) <= step ? wanted : critter.yaw + (d > 0.0f ? step : -step));
    } else if (move != nullptr && move->turnRate > 0.0f && view == nullptr &&
               critter.patrolAim.has_value() && critter.grabbed < 0) {
        // With no player, it turns to the lookout it makes for, unbounded by the facing
        // limit (CritterRotate's waypoint branch).
        const f32 d = wrapAngle(yawBetween(critter.position, *critter.patrolAim) - critter.yaw);
        const f32 step =
            move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw = wrapAngle(critter.yaw + std::clamp(d, -step, step));
    }
    if (bounded && movement.roamRadius <= 0.0f) {
        return; // Zero-radius bosses may turn, but neither locomotion nor knockback moves them.
    }
    Vec3 translation = critter.push * seconds;
    const EnemyView* destination = viewOf(players, critter.target);
    if (move != nullptr && move->type == MoveDefinition::kStepToPoint && destination != nullptr) {
        // The ready-move search refreshes targetPos during local locomotion.
        // CritterInitHeader enables that search from the MOVE roster, even when
        // the raw TYPE does not yet have its derived 0x10000 flag.
        critter.stepTarget = destination->position;
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
    // CritterCollidePlayers sweeps collision centres, not floor positions. An
    // overlapping body may move outward; players on another floor do not block it.
    const Vec3 offset = to - critter.position;
    const auto playerSeparation = [&](const EnemyView& player) -> std::optional<Vec3> {
        const Vec3 centre =
            player.position + Vec3{0, player.collisionHeight.value_or(0.5f * player.height), 0};
        constexpr u32 kNodeMovement = 0x100;
        if ((critter.definition->typeFlags() & kNodeMovement) != 0) {
            std::optional<Vec3> separation;
            f32 nearest = 0;
            const auto parts = critter.definition->parts();
            for (usize i = 0; i < parts.size(); ++i) {
                const auto& part = parts[i];
                if ((part.flags & CritterPart::kSolid) == 0 || critter.hitNodes[i].health <= 0 ||
                    !nodeAvailable(critter, part.node)) {
                    continue;
                }
                const Vec3 from{attachmentTransform(critter, part.node) * Vec4{part.position, 1}};
                if (movementTouchesBody(from, from + offset, centre, player.radius + part.radius,
                                        0.5f * player.height + part.radius)) {
                    const f32 distance = glm::length(
                        Vec2{centre.x - from.x - offset.x, centre.z - from.z - offset.z});
                    if (!separation || distance < nearest) {
                        nearest = distance;
                        separation = centre - from;
                    }
                }
            }
            return separation;
        }
        const Vec3 from = partPosition(critter, {});
        if (movementTouchesBody(from, from + offset, centre,
                                player.radius + critter.definition->wallRadius(),
                                0.5f * player.height + critter.definition->radius())) {
            return centre - (from + offset);
        }
        return std::nullopt;
    };
    bool playerContact = false;
    for (const EnemyView& other : players) {
        if (other.hidden) {
            continue;
        }
        if (const auto separation = playerSeparation(other)) {
            playerContact = true;
            const f32 length = glm::length(*separation);
            if (length > 0) {
                const f32 depth = std::clamp(
                    critter.definition->wallRadius() + other.radius - length, 1.0f, 3.0f);
                m_pushes.push_back({other.player, *separation * (2.0f * depth / length)});
            }
        }
    }
    if (playerContact) {
        return;
    }
    if (m_collision != nullptr) {
        const f32 wallRadius = critter.definition->wallRadius();
        to = m_collision->resolveWalls(to, wallRadius, to.y + kFootClearance,
                                       to.y + kWallProbeHeight);
        const auto floor = m_collision->floorAt(to, kStepUp, kDrop);
        if (!floor.has_value()) {
            return;
        }
        to.y = floor->y;
    }
    if (blockedByItems(critter, to)) {
        return;
    }
    if (blockedBySwarm(critter, to)) {
        return;
    }
    for (const Combatant& peer : peers) {
        const Actor& other = peer.m_actor;
        if (&other == &critter || other.state == State::Inactive) {
            continue;
        }
        const Vec3 from = partPosition(critter, {});
        const Vec3 peerDestination = from + to - critter.position;
        const f32 radius = critter.definition->wallRadius();
        // CritterMoveNodeCol mode 1 checks active nodes, then the body's fallback.
        // Dying critters keep that body until their instance is removed.
        if ((other.definition->typeFlags() & 2U) != 0) {
            const auto parts = other.definition->parts();
            for (usize i = 0; i < parts.size(); ++i) {
                const auto& part = parts[i];
                if (other.hitNodes[i].health <= 0 || !nodeAvailable(other, part.node)) {
                    continue;
                }
                const Vec3 centre{attachmentTransform(other, part.node) * Vec4{part.position, 1}};
                if (movementTouchesBody(from, peerDestination, centre, radius + part.radius,
                                        radius + part.radius)) {
                    return;
                }
            }
        }
        if (movementTouchesBody(from, peerDestination, partPosition(other, {}),
                                radius + other.definition->wallRadius(),
                                radius + other.definition->radius())) {
            return;
        }
    }
    critter.position = to;
    // A wall it walks against or a floor it walks onto may hurt it: the swarm's harms, burns
    // and knocks at five and the felling kinds at fifteen (CritterWorldDamage). The port
    // deliberately limits burning-floor hits to once per quarter second.
    if (m_hazards != nullptr && m_collision != nullptr && critter.state != State::Dying) {
        const auto touch = m_hazards->touching(*m_collision, to, critter.definition->wallRadius(),
                                               kWallProbeHeight);
        if (const auto harm = touch.has_value()
                                  ? HazardSurfaces::enemyHarmOf(m_hazards->flagsOf(touch->object))
                                  : std::nullopt) {
            EnemyHit hit;
            if (HazardSurfaces::burning(m_hazards->flagsOf(touch->object))) {
                if (critter.burnGap > 0) {
                    return;
                }
                critter.burnGap = HazardSurfaces::kEnemyBurnGap;
            }
            hit.damage = harm->damage;
            hit.flags = harm->impact;
            hit.direction = touch->away;
            hit.where = to;
            hurt(hit);
        }
    }
}

bool Combatant::blockedBySwarm(Actor& critter, const Vec3& to) {
    // CritterCollideEnemies chooses one nearest contact. Only a golem may
    // trample enemies whose half-height is at most two; other contacts stop it.
    const Vec3 from = partPosition(critter, {});
    const Vec3 delta = to - critter.position;
    const EnemyBody* nearest = nullptr;
    Vec3 contact{0};
    f32 nearestDistance = 0;
    for (const EnemyBody& enemy : m_swarm) {
        const f32 radius = critter.definition->wallRadius() + enemy.radius;
        const f32 height = critter.definition->radius() + enemy.halfHeight;
        std::optional<Vec3> hit;
        const auto touch = [&](const Vec3& start, f32 r, f32 h) -> std::optional<Vec3> {
            if (!movementTouchesBody(start, start + delta, enemy.centre, r, h)) {
                return std::nullopt;
            }
            const f32 length = glm::dot(delta, delta);
            const f32 t =
                length > 0 ? std::clamp(glm::dot(enemy.centre - start, delta) / length, 0.0f, 1.0f)
                           : 0;
            return start + t * delta;
        };
        if ((critter.definition->typeFlags() & 0x100U) != 0) {
            const auto parts = critter.definition->parts();
            for (usize i = 0; i < parts.size(); ++i) {
                const auto& part = parts[i];
                if ((part.flags & CritterPart::kSolid) == 0 || critter.hitNodes[i].health <= 0 ||
                    !nodeAvailable(critter, part.node)) {
                    continue;
                }
                hit = touch(Vec3{attachmentTransform(critter, part.node) * Vec4{part.position, 1}},
                            radius + part.radius, height + part.radius);
                if (hit) {
                    break;
                }
            }
        } else {
            hit = touch(from, radius, height);
        }
        if (hit) {
            const f32 distance =
                glm::length(Vec2{hit->x - from.x - delta.x, hit->z - from.z - delta.z});
            if (nearest == nullptr || distance < nearestDistance) {
                nearest = &enemy;
                nearestDistance = distance;
                contact = *hit;
            }
        }
    }
    if (nearest == nullptr) {
        return false;
    }
    if (critter.stock->definition.kind == CombatantKind::Golem && nearest->halfHeight <= 2) {
        m_tramples.push_back(
            {nearest->id, critter.definition->itemDamage() * m_scales.damage, contact});
        return false;
    }
    return true;
}

/** Whether the level's items keep the great one from `to` (CritterCollideItems): a golem
 * or gargoyle walks through a chest and strikes what is breakable, the level's enemy damage
 * times its type's, every step it is against it, stopped only while that stands or as it
 * blows up; everything else solid stops it. */
bool Combatant::blockedByItems(Actor& critter, const Vec3& to) {
    const f32 reach = critter.definition->wallRadius();
    const bool breaks = critter.stock->definition.breaksItems;
    const f32 blow = critter.definition->itemDamage() * m_scales.damage;
    return std::ranges::any_of(m_obstacles, [&](const CombatantObstacle& item) {
        if (!item.box.solid || !item.box.touchedBy(to, reach, 0.0f)) {
            return false;
        }
        if (!breaks || item.kind == CombatantObstacle::Kind::Blocks) {
            return true;
        }
        if (item.kind == CombatantObstacle::Kind::Chest) {
            return false;
        }
        m_rams.push_back(CombatantRam{item.id, blow});
        const f32 felt = std::max(1.0f, std::round(blow - static_cast<f32>(item.armor)));
        return item.explodes || static_cast<f32>(item.health) > felt;
    });
}

} // namespace gdl::game
