#include <algorithm>

#include "engine/core/Types.h"

#include "game/players/Progression.h"
#include "game/screens/PlayerAttacks.h"

namespace gdl::game {
namespace {
Mat4 attachmentOf(const PlayerRuntime& player, bool head) {
    const auto worn = PowerupEffects::of(player.actor.save().progress().inventory);
    const Mat4 body = glm::scale(player.actor.transform(),
                                 Vec3{PlayerFigure::bodyScale(player.actor.save(), worn)});
    if (head && player.figure != nullptr) {
        if (const auto at = player.figure->attachment(body, "HEAD")) {
            return *at;
        }
    }
    return body;
}
} // namespace

void PlayerAttacks::useItemAttack(usize index, std::span<PlayerRuntime> players) {
    if (!m_resources || index >= players.size() || players[index].figure == nullptr) {
        return;
    }
    auto& player = players[index];
    auto& inventory = player.actor.save().progress().inventory;
    const auto selected = ItemAttack::select(PowerupEffects::of(inventory));
    if (!selected || selected->deed != player.figure->animator().itemReleased()) {
        return;
    }
    if (selected->deed == PlayerDeed::FireLeft || selected->deed == PlayerDeed::FireRight) {
        m_resources->arsenal.launchGauntlet(player.actor, player.figure.get(),
                                            selected->deed == PlayerDeed::FireLeft);
        return;
    }
    if (selected->chargeKind != 0 &&
        !inventory.spendPowerup(selected->chargeKind, selected->chargeMask,
                                m_resources->world.isTower())) {
        return;
    }
    ItemArea area;
    area.attack = *selected;
    area.actor = index;
    if (const auto tree = m_resources->weapons.trees.find(selected->tree)) {
        const auto& sequences = m_resources->weapons.trees.tree(*tree).sequences;
        if (!sequences.empty()) {
            AnimationPlayer animation;
            animation.start(sequences[0], 0);
            area.lifetime =
                std::max(1.0f / 30,
                         animation.secondsPerFrame() *
                             static_cast<f32>(sequences[0].frames > 0 ? sequences[0].frames : 30));
        }
        const EffectTrees::Setting setting;
        const Mat4 parent = attachmentOf(player, selected->head);
        area.effect = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                    selected->tree, Vec3{parent[3]}, setting);
        m_resources->effects.placeAt(area.effect, parent);
    }
    m_resources->audio.playNamed(selected->sound);
    m_items.push_back(std::move(area));
}

void PlayerAttacks::updateItems(f32 seconds, std::span<PlayerRuntime> players,
                                const Targets& targets) {
    if (!m_resources) {
        return;
    }
    for (auto& area : m_items) {
        if (area.actor >= players.size() || players[area.actor].life != PlayerLife::Standing) {
            area.elapsed = area.lifetime;
            continue;
        }
        area.elapsed += seconds;
        const auto& player = players[area.actor];
        const Mat4 parent = attachmentOf(player, area.attack.head);
        m_resources->effects.placeAt(area.effect, parent);
        for (const auto& target : projectileTargets(targets)) {
            if (std::ranges::find(area.hit, target.id) == area.hit.end() &&
                area.attack.reaches(parent, target, area.elapsed, area.lifetime)) {
                area.hit.push_back(target.id);
                strikeTarget(target, area.attack.damageAt(area.elapsed, area.lifetime),
                             area.attack.flags, player.actor, players, targets);
            }
        }
    }
    targets.fixtures.settleBlasts(players, targets.fixtureEvents);
    std::erase_if(m_items, [this](const ItemArea& area) {
        if (area.elapsed < area.lifetime) {
            return false;
        }
        m_resources->effects.stop(area.effect);
        return true;
    });
}

void PlayerAttacks::strikeTarget(const MissileTarget& target, f32 damage, u32 flags,
                                 const PlayerActor& owner, std::span<PlayerRuntime> players,
                                 const Targets& targets) {
    const Vec3 direction = target.base - owner.position();
    if (target.id >= kWallTargetBase) {
        targets.fixtures.strikeWall(static_cast<usize>(target.id - kWallTargetBase), damage, flags);
    } else if (target.id >= kSafeRockTargetBase) {
        targets.fixtures.strikeSafeRock(static_cast<usize>(target.id - kSafeRockTargetBase),
                                        damage);
    } else if (target.id >= kBossTargetBase) {
        EnemyHit hit;
        hit.damage = damage;
        hit.flags = flags;
        hit.player = owner.player();
        hit.level = experienceLevel(owner.save().experience());
        hit.direction = direction;
        hit.where = target.base;
        targets.opponents.bosses().hurt(hit);
    } else if (target.id >= kCritterTargetBase) {
        targets.opponents.strikeCritter(target.id - kCritterTargetBase, damage, flags, direction,
                                        owner.player(), target.base, false, players);
    } else if (target.id >= kGeneratorTargetBase) {
        targets.opponents.strikeGenerator(target.id - kGeneratorTargetBase, damage, owner.player());
    } else if (target.id >= kEnemyTargetBase) {
        targets.opponents.strikeEnemy(target.id - kEnemyTargetBase, damage, flags, direction,
                                      owner.player(), players);
    } else {
        targets.fixtures.strikeBarrel(static_cast<usize>(target.id), damage, owner.player(),
                                      players, targets.fixtureEvents);
    }
}
} // namespace gdl::game
