#include <algorithm>
#include <cmath>
#include <optional>

#include "engine/core/Types.h"
#include "engine/math/ApproximateLength.h"
#include "engine/math/PortableMath.h"

#include "game/combat/BodyContact.h"
#include "game/critters/Combatant.h"
#include "game/enemies/EnemyMind.h"
#include "game/level/CombatantObstacle.h"
namespace gdl::game {
namespace {
f32 yawBetween(const Vec3& from, const Vec3& to) {
    return portable::atan2(to.x - from.x, to.z - from.z);
}
} // namespace

namespace {
constexpr f32 kStepUp = 2.0f;
constexpr f32 kDrop = 6.0f;
constexpr f32 kFootClearance = 0.1f;
constexpr f32 kWallProbeHeight = 8.0f; ///< how high over its feet walls stop it
constexpr f32 kBlindTurnShare = 0.1f;
constexpr f32 kSupportReach = 0.1f;
constexpr u32 kReturnFacing = 0x20;
constexpr u32 kNodeMovement = 0x100;
constexpr f32 kTrampleHeight = 2.0f; ///< the tallest half-height of a swarm body trampled
/** How far a boss's floor probe reaches up and down (CritterBossAI's FloorCollide). */
constexpr f32 kBossFloorAbove = 4.0f;
constexpr f32 kBossFloorBelow = 1000.0f;
constexpr f32 kBossFloorEdgeReach = 0.0f; ///< only the faces under it, no edge beside it

Vec3 travelWorld(const WorldCollision& collision, const Vec3& from, const Vec3& wanted,
                 f32 radius) {
    const Vec3 offset{wanted.x - from.x, 0, wanted.z - from.z};
    const f32 stepLength = std::max(kFootClearance, 0.5f * radius);
    const auto steps = std::max(1, static_cast<s32>(std::ceil(glm::length(offset) / stepLength)));
    const Vec3 stride = offset / static_cast<f32>(steps);
    Vec3 position = from;
    for (s32 i = 0; i < steps; ++i) {
        const Vec3 next =
            collision.sweepWalls(position, position + stride, radius, position.y + kFootClearance,
                                 position.y + kWallProbeHeight);
        const Vec3 delta = next - position;
        const f32 distance = glm::length(Vec2{delta.x, delta.z});
        if (distance <= 0) {
            break;
        }
        // CritterCollideWorld (0x80035408) checks support a wall radius beyond
        // the destination before committing movement. A floor ray at the centre
        // alone lets a charge step onto a ship's sloping hull and off the deck.
        const f32 reach = radius + distance;
        const Vec3 ahead = position + delta * (reach / distance);
        const auto lead = collision.floorAt(ahead, kStepUp, kDrop);
        if (!lead || std::abs(lead->y - position.y) > 2.0f * reach) {
            break;
        }
        const auto floor = collision.floorAt(next, kStepUp, kDrop);
        if (!floor) {
            break;
        }
        position = {next.x, floor->y, next.z};
    }
    return position;
}
} // namespace
void Combatant::rememberFloor() {
    Actor& actor = m_sim.actor;
    actor.floor.reset();
    if (m_sim.collision == nullptr || !present() || actor.parent != nullptr ||
        actor.stock->definition.boundsToHome) {
        return;
    }
    const auto floor = m_sim.collision->floorAt(actor.position, kSupportReach, kSupportReach);
    if (floor) {
        if (const auto placement = m_sim.collision->objectTransform(floor->object)) {
            actor.floor = Actor::Floor{floor->object,
                                       Vec3{glm::inverse(*placement) * Vec4{actor.position, 1}}};
        }
    }
}

void Combatant::syncFloor() {
    Actor& actor = m_sim.actor;
    // Nothing carries a boss between its updates: it settles in its own (settleOnFloor).
    if (!present() || m_sim.collision == nullptr || actor.stock->definition.boundsToHome ||
        !actor.floor) {
        return;
    }
    const auto placement = m_sim.collision->objectTransform(actor.floor->object);
    if (!placement || !m_sim.collision->solid(actor.floor->object)) {
        actor.floor.reset();
        return;
    }
    const Vec3 carried = Vec3{*placement * Vec4{actor.floor->local, 1}};
    actor.presentation.position += carried - actor.position;
    actor.position = carried;
}

std::optional<FloorHit> Combatant::settleOnFloor() {
    Actor& actor = m_sim.actor;
    if (!present() || m_sim.collision == nullptr || actor.parent != nullptr ||
        !actor.stock->definition.boundsToHome) {
        return std::nullopt;
    }
    // In every state, asleep and through its INIT too (DC ProcessCritter 0x8c08b742 as well):
    // in A5 the Chimera rides its elevator down before it wakes. position is floor-space; the
    // model root adds the TYPE floorOffset.
    const auto floor = bossFloorUnder(*m_sim.collision, actor.position);
    if (floor) {
        actor.position.y = floor->y;
        for (auto& part : children()) {
            part->m_sim.actor.position.y = floor->y;
        }
    }
    return floor;
}

std::optional<FloorHit> Combatant::bossFloorUnder(const WorldCollision& collision,
                                                  const Vec3& from) {
    return collision.floorAt(from, kBossFloorAbove, kBossFloorBelow, kBossFloorEdgeReach,
                             WorldCollision::kBossSide);
}

void Combatant::holdTerritory() {
    Actor& actor = m_sim.actor;
    if (actor.state != State::Active || actor.parent != nullptr ||
        !actor.stock->definition.boundsToHome || actor.definition->movement().roamRadius <= 0.0f) {
        return;
    }
    keepToTerritory(actor);
    for (auto& part : children()) {
        part->m_sim.actor.position.x = actor.position.x;
        part->m_sim.actor.position.z = actor.position.z;
    }
}

void Combatant::keepToTerritory(Actor& critter) {
    // CritterTranslate keeps the fast length of the offset its step proposed, before the clamp
    // (+0x4ac, 0x8003ad54; DC +0x238), for CritterGetTargetSub's home distance.
    critter.homeDistance = approximateFlatLength(critter.position - critter.homePosition);
    critter.position =
        critter.definition->movement().constrain(critter.position, critter.homePosition);
}

void Combatant::carry(Actor& critter, f32 seconds, const MoveDefinition* move,
                      std::span<const EnemyView> players, std::span<const Combatant> peers) {
    // Both native AIs translate using the previous facing, then turn. Only
    // CritterGolemAI (GC 0x800396a4) skips the turn on player contact;
    // CritterBossAI (0x80039ad8) always turns after its home-clamped step,
    // which never reports a contact.
    const bool playerContact = translate(critter, seconds, move, players, peers);
    if (!playerContact) {
        rotate(critter, seconds, move, players);
    }
}

void Combatant::rotate(Actor& critter, f32 seconds, const MoveDefinition* move,
                       std::span<const EnemyView> players) {
    const CritterMovement& movement = critter.definition->movement();
    const EnemyView* view = viewOf(players, critter.moveTarget);
    if (move != nullptr && move->turnRate > 0.0f) {
        f32 wanted = critter.yaw;
        // CritterRotate (GC 0x8003af4c) checks this before the grabbed-player
        // branch: Skorne's GRAB and Wraith's DEATH return to their authored facing.
        if ((move->flags & kReturnFacing) != 0) {
            wanted = critter.initialYaw;
        } else if (critter.grabbed < 0 && view != nullptr) {
            wanted =
                movement.facing(yawBetween(critter.position, view->position), critter.initialYaw);
        } else if (critter.grabbed < 0 && critter.patrolAim.has_value()) {
            // With no player, the waypoint branch is not facing-limit clamped.
            wanted = yawBetween(critter.position, *critter.patrolAim);
        }
        const f32 delta = wrapAngle(wanted - critter.yaw);
        // Blinded, it turns at a tenth of its rate.
        const f32 step =
            move->turnRate * seconds * (critter.blindTicks > 0 ? kBlindTurnShare : 1.0f);
        critter.yaw = wrapAngle(critter.yaw + std::clamp(delta, -step, step));
    }
}

bool Combatant::translate(Actor& critter, f32 seconds, const MoveDefinition* move,
                          std::span<const EnemyView> players, std::span<const Combatant> peers) {
    const CritterMovement& movement = critter.definition->movement();
    const bool bounded = critter.stock->definition.boundsToHome;
    if (bounded && movement.roamRadius <= 0.0f) {
        return false; // Zero-radius bosses may still turn, but cannot change position.
    }
    Vec3 translation = critter.push * seconds;
    const EnemyView* destination = viewOf(players, critter.target);
    if (move != nullptr && move->type == MoveDefinition::kStepToPoint && destination != nullptr) {
        // The ready-move search refreshes targetPos during local locomotion.
        // CritterInitHeader enables that search from the MOVE roster, even when
        // the raw TYPE does not yet have its derived 0x10000 flag.
        critter.stepTarget = destination->position;
    }
    // The move's pace carries it in every state, so a dying Chimera backs away on its DEATH's
    // (CritterTranslate 0x8003a9ec-0x8003aa0c, DC 0x8c08a5da-0x8c08a5e4, N64 Boss.bin
    // 0xe0883d38-0xe0883d5c); dying only frees it from its territory, below.
    if (move != nullptr && move->speed != 0.0f) {
        const f32 pace = move->speed * m_sim.scales.speed * seconds;
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
    // Retail bosses clamp to their home region directly, on a frame they stand still too
    // (CritterTranslate has no return for a zero step, 0x8003ad04-0x8003ae00; DC 0x8c08a5c0,
    // N64 Boss.bin 0xe0883d14), but not while dying (0x8003ad3c-0x8003ad44; DC
    // 0x8c08a864-0x8c08a86a; N64 0xe0883ee4-0xe0883eec). Their authored stage/root heights are
    // not ordinary walking-floor probes (several stand above the arena).
    if (bounded) {
        critter.position += translation;
        if (critter.state != State::Dying) {
            keepToTerritory(critter);
        }
        return false;
    }
    if (glm::length(translation) <= 0.0f) {
        return false;
    }
    Vec3 to = critter.position + translation;
    // CritterCollidePlayers sweeps collision centres, not floor positions. An
    // overlapping body may move outward; players on another floor do not block it.
    const Vec3 offset = to - critter.position;
    const auto playerSeparation = [&](const EnemyView& player) -> std::optional<Vec3> {
        const Vec3 centre =
            player.position + Vec3{0, player.collisionHeight.value_or(0.5f * player.height), 0};
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
                m_sim.pushes.push_back({other.player, *separation * (2.0f * depth / length)});
            }
        }
    }
    if (playerContact) {
        return true;
    }
    if (m_sim.collision != nullptr) {
        to = travelWorld(*m_sim.collision, critter.position, to, critter.definition->wallRadius());
    }
    if (blockedByItems(critter, to)) {
        return false;
    }
    if (blockedBySwarm(critter, to)) {
        return false;
    }
    for (const Combatant& peer : peers) {
        const Actor& other = peer.m_sim.actor;
        if (&other == &critter || other.state == State::Inactive) {
            continue;
        }
        const Vec3 from = partPosition(critter, {});
        const Vec3 peerDestination = from + to - critter.position;
        const f32 radius = critter.definition->wallRadius();
        // CritterMoveNodeCol mode 1 checks active nodes, then the body's fallback.
        // Dying critters keep that body until their instance is removed.
        if ((other.definition->typeFlags() & CritterData::kByParts) != 0) {
            const auto parts = other.definition->parts();
            for (usize i = 0; i < parts.size(); ++i) {
                const auto& part = parts[i];
                if (other.hitNodes[i].health <= 0 || !nodeAvailable(other, part.node)) {
                    continue;
                }
                const Vec3 centre{attachmentTransform(other, part.node) * Vec4{part.position, 1}};
                if (movementTouchesBody(from, peerDestination, centre, radius + part.radius,
                                        radius + part.radius)) {
                    return false;
                }
            }
        }
        if (movementTouchesBody(from, peerDestination, partPosition(other, {}),
                                radius + other.definition->wallRadius(),
                                radius + other.definition->radius())) {
            return false;
        }
    }
    critter.position = to;
    // A wall it walks against or a floor it walks onto may hurt it: the swarm's harms, burns
    // and knocks at five and the felling kinds at fifteen (CritterWorldDamage). The port
    // deliberately limits burning-floor hits to once per quarter second.
    if (m_sim.hazards != nullptr && m_sim.collision != nullptr && critter.state != State::Dying) {
        const auto touch = m_sim.hazards->touching(
            *m_sim.collision, to, critter.definition->wallRadius(), kWallProbeHeight);
        if (const auto harm = touch.has_value() ? HazardSurfaces::enemyHarmOf(
                                                      m_sim.hazards->flagsOf(touch->object))
                                                : std::nullopt) {
            EnemyHit hit;
            if (HazardSurfaces::burning(m_sim.hazards->flagsOf(touch->object))) {
                if (critter.burnGap > 0) {
                    return false;
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
    return false;
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
        if ((critter.definition->typeFlags() & CritterData::kSolidParts) != 0) {
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
    if (critter.stock->definition.tramplesSwarm && nearest->halfHeight <= kTrampleHeight) {
        m_sim.tramples.push_back(
            {nearest->id, critter.definition->itemDamage() * m_sim.scales.damage, contact});
        return false;
    }
    return true;
}

/** Whether the level's items keep the great one from `to` (CritterCollideItems): a golem
 * or gargoyle walks through a chest and strikes what is breakable, the level's enemy damage
 * times its type's, every step it is against it, stopped only while that stands or as it
 * blows up; everything else solid stops it. */
bool Combatant::blockedByItems(Actor& critter, const Vec3& to) {
    const bool breaks = critter.stock->definition.breaksItems;
    const f32 blow = critter.definition->itemDamage() * m_sim.scales.damage;
    const Vec3 delta = to - critter.position;
    return std::ranges::any_of(m_obstacles, [&](const CombatantObstacle& item) {
        // fn_8005D5C8: low generators may be crushed by golems/gargoyles,
        // but a general simply steps over them rather than ramming or stopping.
        if (!item.box.solid || (!breaks && item.kind == CombatantObstacle::Kind::LowGenerator)) {
            return false;
        }
        const auto touches = [&](const Vec3& from, f32 radius, f32 halfHeight) {
            const Vec3 destination = from + delta;
            if (destination.y + halfHeight < item.box.centre.y ||
                destination.y - halfHeight > item.box.centre.y + item.box.height) {
                return false;
            }
            // Obstacle's ordinary touchedBy takes floor-space positions. Here the
            // native item test receives a collision centre and separate vertical
            // radius, so test the footprint independently of that floor convention.
            const Vec3 footprint{destination.x, item.box.centre.y, destination.z};
            if (!item.box.touchedBy(footprint, radius, 0)) {
                return false;
            }
            // fn_8005F0F4 permits movement outward from an existing overlap.
            const Vec3 startFootprint{from.x, item.box.centre.y, from.z};
            const Vec2 toward{item.box.centre.x - from.x, item.box.centre.z - from.z};
            return !item.box.touchedBy(startFootprint, radius, 0) ||
                   glm::dot(Vec2{delta.x, delta.z}, toward) >= 0;
        };
        bool contact = false;
        // CritterCollideItems (GC 0x80034f60): TYPE 0x100 uses live solid
        // NODE spheres, not a second cylinder invented around the actor's feet.
        if ((critter.definition->typeFlags() & kNodeMovement) != 0) {
            const auto parts = critter.definition->parts();
            for (usize i = 0; i < parts.size(); ++i) {
                const auto& part = parts[i];
                if ((part.flags & CritterPart::kSolid) == 0 || critter.hitNodes[i].health <= 0 ||
                    !nodeAvailable(critter, part.node)) {
                    continue;
                }
                const Vec3 centre{attachmentTransform(critter, part.node) * Vec4{part.position, 1}};
                if (touches(centre, part.radius, part.radius)) {
                    contact = true;
                    break;
                }
            }
        } else {
            contact = touches(partPosition(critter, {}), critter.definition->wallRadius(),
                              critter.definition->radius());
        }
        if (!contact) {
            return false;
        }
        if (!breaks || item.kind == CombatantObstacle::Kind::Blocks) {
            return true;
        }
        if (item.kind == CombatantObstacle::Kind::Chest) {
            return false;
        }
        m_sim.rams.push_back(CombatantRam{item.id, blow});
        const f32 felt = std::max(1.0f, std::round(blow - static_cast<f32>(item.armor)));
        return item.explodes || static_cast<f32>(item.health) > felt;
    });
}

} // namespace gdl::game
