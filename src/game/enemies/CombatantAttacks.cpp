#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
#include "game/enemies/CritterBreath.h"
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
    const AttackDefinition* damage = critter.stock->data.damage(damageIndex);
    if (damage == nullptr || damage->damage <= 0.0f) {
        return;
    }
    Vec3 centre;
    f32 reach = 0.0f;
    std::optional<CritterBreath> breath;
    switch (damage->type) {
    case AttackDefinition::kBlow:
        centre = partPosition(critter, move.colnode) + damage->offset;
        reach = damage->radius + damage->maxDistance;
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
        breath = CritterBreath::fromNode(partTransform(critter, move.colnode), *damage);
        centre = breath->origin;
        break;
    default: return;
    }
    for (const EnemyView& view : players) {
        if (view.hidden ||
            (!breath.has_value() && std::ranges::find(critter.struckThisMove, view.player) !=
                                        critter.struckThisMove.end())) {
            continue;
        }
        const Vec3 feet = view.position;
        const Vec3 body = feet + Vec3{0.0f, 0.5f * view.height, 0.0f};
        const bool within =
            breath.has_value()
                ? breath->touches(*damage, body, view.radius, 0.5f * view.height)
                : flatDistance(centre, feet) <= reach + view.radius &&
                      std::abs(centre.y - body.y) <= 0.5f * view.height + damage->radius;
        if (!within) {
            continue;
        }
        CombatBlow blow;
        blow.player = view.player;
        blow.critter = id;
        blow.damage = damage->damage * m_scales.damage;
        blow.breath = breath.has_value();
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
        } else {
            critter.struckThisMove.push_back(view.player);
        }
        m_blows.push_back(blow);
    }
}

void Combatant::shoot(const Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
                      std::span<const EnemyView> players) {
    const AttackDefinition* damage = critter.stock->data.damage(damageIndex);
    if (damage == nullptr) {
        return;
    }
    CombatShot shot;
    shot.data = &critter.stock->data;
    shot.critter = id;
    shot.damageIndex = damageIndex;
    // The launch point follows the active node, but the offset and facing use the body.
    const Mat4 body = modelTransform(critter);
    shot.origin = partPosition(critter, move.colnode) + Vec3{body * Vec4{damage->offset, 0.0f}};
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

void Combatant::cue(const Actor& critter, s32 id, s32 index, const Vec3& position,
                    std::optional<std::string_view> node) {
    const CritterData& data = critter.stock->data;
    for (s32 at = index, guard = 0; at >= 0 && guard < 8; ++guard) {
        const CombatEffectDefinition* record = data.sound(at);
        if (record == nullptr) {
            break;
        }
        CombatCue out;
        out.critter = id;
        out.tree = record->shows() ? record->tree : std::string{};
        out.sound = record->soundFor(m_realm);
        // Explicit positions are already world-space. Only a parent transform
        // rotates an offset; impact marks must not inherit the attacker's yaw.
        out.position = position + record->offset * critter.scale;
        out.yaw = record->follows() ? critter.yaw : 0.0f;
        out.scale = record->scale * critter.scale;
        out.life = record->life;
        out.follows = record->follows();
        out.shakes = (record->flags & CombatEffectDefinition::kShakes) != 0;
        // Without a root/entity/global parenting override, a move effect uses its
        // active animated node. Hit marks have no requested attachment.
        constexpr u32 kAlternateParent = 0x2000U | 0x800U | 0x80U | 0x40U | 1U;
        const bool root =
            (record->flags & 1U) != 0 && (record->flags & (0x2000U | 0x800U | 0x40U)) == 0;
        if (root && !out.tree.empty()) {
            out.rootAttachment = true;
            out.nodeOffset = record->offset;
            out.position = Vec3{modelTransform(critter) * Vec4{record->offset, 1.0f}};
            out.scale = record->scale;
            out.follows = true;
        } else if ((record->flags & 0x80U) != 0 && (record->flags & 0x801U) == 0) {
            out.position = critter.initialRoot + record->offset * critter.scale;
            out.follows = false;
            out.yaw = 0;
        } else if (node.has_value() && !out.tree.empty() &&
                   (record->flags & kAlternateParent) == 0) {
            out.node = *node;
            out.nodeOffset = record->offset;
            out.position = Vec3{attachmentTransform(critter, *node) * Vec4{record->offset, 1.0f}};
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
            out.yaw = 0;
        }
        if (!out.tree.empty() || !out.sound.empty()) {
            m_cues.push_back(std::move(out));
        }
        at = record->link;
    }
}

} // namespace gdl::game
