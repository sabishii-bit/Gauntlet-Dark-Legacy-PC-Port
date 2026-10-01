#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
#include "game/enemies/CombatantBreath.h"
namespace gdl::game {
namespace {
f32 flatDistance(const Vec3& a, const Vec3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}
} // namespace

void Combatant::strikeWith(Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
                           std::span<const EnemyView> players) {
    const AttackDefinition* damage = critter.definition->damage(damageIndex);
    if (damage == nullptr || damage->damage <= 0.0f) {
        return;
    }
    Vec3 centre;
    f32 reach = 0.0f;
    std::optional<CombatantBreath> breath;
    switch (damage->type) {
    case AttackDefinition::kBlow:
        // The part's offset turns with the part; the player's cylinder grows by the reach
        // alone (CritterNodePlayerCollide).
        centre = Vec3{partTransform(critter, move.colnode) * Vec4{damage->offset, 1.0f}};
        reach = damage->maxDistance;
        break;
    case AttackDefinition::kRing:
        centre = critter.position;
        reach = damage->maxDistance;
        break;
    case AttackDefinition::kTargetArea:
        if (!critter.attackTarget.has_value()) {
            return;
        }
        centre = *critter.attackTarget + Vec3{modelTransform(critter) * Vec4{damage->offset, 0.0f}};
        reach = damage->maxDistance;
        break;
    case AttackDefinition::kBreath:
        breath = CombatantBreath::fromNode(partTransform(critter, move.colnode), *damage);
        centre = breath->origin;
        break;
    default: return;
    }
    // A blow lands every update of its window, held off by the player's hit gap; a ring or
    // a targeted area once a move.
    const bool gated = breath.has_value() || damage->type == AttackDefinition::kBlow;
    for (const EnemyView& view : players) {
        if (view.hidden || (!gated && std::ranges::find(critter.struckThisMove, view.player) !=
                                          critter.struckThisMove.end())) {
            continue;
        }
        const Vec3 feet = view.position;
        const Vec3 body = feet + Vec3{0.0f, 0.5f * view.height, 0.0f};
        bool within = false;
        if (breath.has_value()) {
            within = breath->touches(*damage, body, view.radius, 0.5f * view.height);
        } else if (damage->type == AttackDefinition::kBlow) {
            within = flatDistance(centre, feet) <= reach + view.radius &&
                     centre.y >= feet.y - reach && centre.y <= feet.y + view.height + reach;
        } else {
            within = flatDistance(centre, feet) <= reach + view.radius &&
                     std::abs(centre.y - body.y) <= 0.5f * view.height + damage->radius;
        }
        if (!within) {
            continue;
        }
        CombatBlow blow;
        blow.player = view.player;
        blow.critter = id;
        blow.damage = dealt(damage->damage * m_scales.damage);
        blow.breath = breath.has_value();
        blow.gated = gated;
        blow.flags = damage->flags;
        blow.origin = centre;
        const Vec3 away = feet - critter.position;
        const f32 length = flatDistance(feet, critter.position);
        blow.direction = length > 0.001f ? Vec3{away.x / length, 0.0f, away.z / length}
                                         : Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)};
        if (breath.has_value()) {
            const Vec3 direction = breath->end - breath->origin;
            const f32 size = glm::length(direction);
            blow.direction = size > 0.0f ? direction / size : Vec3{0.0f};
        } else if (!gated) {
            critter.struckThisMove.push_back(view.player);
        }
        m_blows.push_back(blow);
    }
}

void Combatant::shoot(const Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
                      std::span<const EnemyView> players) {
    const AttackDefinition* damage = critter.definition->damage(damageIndex);
    if (damage == nullptr) {
        return;
    }
    CombatShot shot;
    shot.data = critter.definition;
    shot.critter = id;
    shot.damageIndex = damageIndex;
    // The launch point follows the active node, but the offset and facing use the body.
    const Mat4 body = modelTransform(critter);
    shot.origin = Vec3{attachmentTransform(critter, move.colnode)[3]} +
                  Vec3{body * Vec4{damage->offset, 0.0f}};
    shot.forward = Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)};
    if (const EnemyView* target = viewOf(players, critter.target); target != nullptr) {
        shot.target = target->position + Vec3{0.0f, 0.5f * target->height, 0.0f};
    }
    shot.rate = attackRate(critter);
    shot.scale = critter.scale;
    shot.damageScale = m_scales.damage;
    if ((damage->behaviorFlags & AttackDefinition::kCurbed) != 0 && critter.curbSeconds > 0.0f) {
        shot.birthLife = critter.curbSeconds;
    }
    shot.realm = m_realm;
    m_shots.push_back(shot);
}

/** Where the target stood when the move looked, the record's offset turned with the body
 * (CritterDoTexmodNode with c->targetPos); the level's projectiles keep it as a web. */
void Combatant::plant(const Actor& critter, s32 id, s32 damageIndex) {
    if (!critter.attackTarget.has_value()) {
        return;
    }
    const AttackDefinition* damage = critter.definition->damage(damageIndex);
    if (damage == nullptr) {
        return;
    }
    CombatShot shot;
    shot.data = critter.definition;
    shot.critter = id;
    shot.damageIndex = damageIndex;
    shot.origin =
        *critter.attackTarget + Vec3{modelTransform(critter) * Vec4{damage->offset, 0.0f}};
    shot.forward = Vec3{std::sin(critter.yaw), 0.0f, std::cos(critter.yaw)};
    shot.rate = attackRate(critter);
    shot.scale = critter.scale;
    shot.damageScale = m_scales.damage;
    shot.realm = m_realm;
    m_shots.push_back(shot);
}

void Combatant::cue(Actor& critter, s32 id, s32 index, const Vec3& position,
                    std::optional<std::string_view> node, const AttackDefinition* damage,
                    std::optional<s32> player) {
    const CritterData& data = *critter.definition;
    for (s32 at = index, guard = 0; at >= 0 && guard < 8; ++guard) {
        const CombatEffectDefinition* record = data.sound(at);
        if (record == nullptr) {
            break;
        }
        if ((record->flags & 0x400U) != 0) {
            critter.hidden = true;
        }
        CombatCue out;
        out.critter = id;
        out.tree = record->shows() ? record->tree : std::string{};
        out.sound = record->soundFor(m_realm);
        out.soundPosition = Vec3{modelTransform(critter)[3]};
        // A dying great one's sounds carry whole; the rest fade with distance.
        out.attenuated = critter.move < 0 || data.moves()[static_cast<usize>(critter.move)].type !=
                                                 MoveDefinition::kDeath;
        // Explicit positions are already world-space. Only a parent transform
        // rotates an offset; impact marks must not inherit the attacker's yaw.
        out.position = position + record->offset * critter.scale;
        out.yaw = record->follows() ? critter.yaw : 0.0f;
        out.scale = record->scale * critter.scale;
        out.life = record->life;
        const Vec3 offset = record->offset + (damage != nullptr ? damage->offset : Vec3{0});
        if (damage != nullptr) {
            out.pitchYaw = {damage->pitch, damage->yaw};
        }
        out.follows = record->follows();
        out.shakes = (record->flags & CombatEffectDefinition::kShakes) != 0;
        out.arena = (record->flags & CombatEffectDefinition::kArenaCue) != 0;
        out.untilNextMove = (record->flags & CombatEffectDefinition::kUntilNextMove) != 0;
        critter.moveEffect |= out.untilNextMove;
        // Without a root/entity/global parenting override, a move effect uses its
        // active animated node. Hit marks have no requested attachment.
        constexpr u32 kAlternateParent = 0x2000U | 0x800U | 0x80U | 0x40U | 1U;
        // SFXX 0x800 selects the animation root's parent; bit 1 selects the
        // animated root itself. Garm's hand-ball tracks rely on that root's lift.
        const bool root = (record->flags & 0x801U) != 0 && (record->flags & (0x2000U | 0x40U)) == 0;
        if (root && !out.tree.empty()) {
            const usize rootIndex = critter.branch.value_or(0);
            const auto& nodes = critter.stock->tree->nodes;
            const s32 attachment = (record->flags & 0x800U) != 0 ? nodes[rootIndex].parent
                                                                 : static_cast<s32>(rootIndex);
            out.rootAttachment = attachment < 0;
            Mat4 parent = modelTransform(critter);
            if (attachment >= 0) {
                const auto& attachmentName = nodes[static_cast<usize>(attachment)].name;
                out.node = attachmentName;
                parent = attachmentTransform(critter, attachmentName);
            }
            out.nodeOffset = offset;
            out.position = Vec3{parent * Vec4{offset, 1.0f}};
            out.scale = record->scale;
            out.follows = true;
        } else if ((record->flags & 0x80U) != 0 && (record->flags & 0x801U) == 0) {
            out.position = critter.initialRoot + record->offset * critter.scale;
            out.follows = false;
            out.yaw = 0;
        } else if (node.has_value() && !out.tree.empty() &&
                   (record->flags & kAlternateParent) == 0) {
            out.node = *node;
            out.nodeOffset = offset;
            out.position = Vec3{attachmentTransform(critter, *node) * Vec4{offset, 1.0f}};
            out.scale = record->scale; // creature scale is already in the parent matrix
            out.follows = true;
        } else if ((record->flags & 0x40U) != 0) {
            // A move's unattached effect is placed at the active node once. Damage
            // cues instead supply their already-resolved world point in position.
            out.position =
                node.has_value()
                    ? Vec3{attachmentTransform(critter, *node) * Vec4{record->offset, 1.0f}}
                    : position + record->offset * critter.scale;
            out.follows = false;
            // CritterDoSfxSub's detached branch calls SfxSetMat with the body's
            // orientation. In particular A13 must open ahead of the Lich, not
            // along the world's +Z axis after he has turned.
            out.yaw = critter.yaw;
        }
        if (out.node.has_value() || out.rootAttachment) {
            const Mat4 parent = out.rootAttachment ? modelTransform(critter)
                                                   : attachmentTransform(critter, *out.node);
            out.placement = CritterArea::placement(parent, out.nodeOffset, out.pitchYaw);
        }
        if (player.has_value()) {
            out.playerAttachment = player;
            out.node.reset();
            out.rootAttachment = false;
            out.placement.reset();
            out.nodeOffset = record->offset;
            out.scale = record->scale;
            out.follows = true;
        }
        if (!out.tree.empty() || !out.sound.empty() || out.shakes || out.arena) {
            m_cues.push_back(std::move(out));
        }
        at = record->link;
    }
}

} // namespace gdl::game
