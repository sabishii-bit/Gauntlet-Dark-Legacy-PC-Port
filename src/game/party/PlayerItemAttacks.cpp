#include <algorithm>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/level/ImportedLevel.h"
#include "game/party/PlayerAttacks.h"

namespace gdl::game {
namespace {
Mat4 attachmentOf(const PlayerRuntime& player, bool head) {
    const auto worn = PowerupEffects::of(player.actor.save().progress().inventory);
    const Mat4 body =
        PlayerFigure::bodyPlacement(player.actor.transform(), player.actor.save(), worn);
    if (head && player.figure != nullptr) {
        if (player.figure->pojoActive()) {
            // PlayerMotion's AtreeFindMbidxNode name at GUNE5D 80114274.
            return player.figure->companion().attachment(body, "POJOBODY1_HE#1").value_or(body);
        }
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
    const auto worn = PowerupEffects::of(inventory);
    auto selected = ItemAttack::select(worn);
    const bool pojoBreath = (worn.special & powerup::kPojo) != 0 &&
                            player.figure->animator().itemReleased() == PlayerDeed::Breathe;
    if (pojoBreath) {
        // PlayerMotion gives horns/mask priority, then Pojo/fire over acid and
        // lightning. Only the turbo-button path spends meter; a carried breath
        // item still pays its own charge when used through the ordinary attack.
        PowerupEffects fire;
        fire.special =
            powerup::kFireBreath | (worn.special & (powerup::kSkorneHorns | powerup::kSkorneMask));
        selected = ItemAttack::select(fire);
        if (!selected) {
            return;
        }
        if (player.pojoTurbo) {
            if (!player.turbo.spend(TurboMeter::kStrongCost)) {
                return;
            }
            selected->chargeKind = 0;
        }
    }
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
    if (selected->deed == PlayerDeed::Hammer && m_resources->shake != nullptr) {
        m_resources->shake->start(CameraShake::Target::Attention, 0, 30, 0.3f, 200);
    }
    if (selected->deed == PlayerDeed::Hammer && m_resources->hammerImpact) {
        m_resources->hammerImpact(player.actor.player());
    }
    area.attack = *selected;
    area.actor = index;
    area.pojoTurbo = pojoBreath && player.pojoTurbo;
    if (area.pojoTurbo) {
        // Give the meter-funded breath the same presentation as a strong turbo.
        // An ordinary breath pickup must not darken the world or glow the bearer.
        m_resources->dimmer.ask(-0.4f);
        player.glow.raise(BodyGlow::kStrike);
    }
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
    if (pojoBreath) {
        // PlayerMotion calls AudioPlayerTurbo for Pojo even when a carried breath
        // initiated it. AudioTurboDefense (or horns/mask) may also play its own cue.
        PowerupEffects breathSounds;
        breathSounds.special =
            worn.special & (powerup::kBreath | powerup::kSkorneHorns | powerup::kSkorneMask);
        const auto carried = ItemAttack::select(breathSounds);
        if (carried && carried->deed == PlayerDeed::Breathe) {
            m_resources->audio.playNamed(carried->sound, 224.0f / 255.0f);
        }
        m_resources->audio.playNamed("S_POJOTURBO", 224.0f / 255.0f);
    } else {
        m_resources->audio.playNamed(selected->sound);
    }
    m_sim.items.push_back(std::move(area));
}

void PlayerAttacks::updateItems(f32 seconds, std::span<PlayerRuntime> players,
                                const Targets& targets) {
    if (!m_resources) {
        return;
    }
    for (auto& area : m_sim.items) {
        if (area.actor >= players.size() || players[area.actor].life != PlayerLife::Standing) {
            area.elapsed = area.lifetime;
            continue;
        }
        area.elapsed += seconds;
        auto& player = players[area.actor];
        if (area.pojoTurbo && area.elapsed < area.lifetime) {
            m_resources->dimmer.ask(-0.4f);
            player.glow.raise(BodyGlow::kStrike);
        }
        const Mat4 parent = attachmentOf(player, area.attack.head);
        m_resources->effects.placeAt(area.effect, parent);
        for (const auto& target : strikeTargets(targets)) {
            const auto kind = target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase
                                  ? ItemAttack::TargetKind::Swarm
                                  : ItemAttack::TargetKind::Item;
            if (std::ranges::find(area.hit, target.id) == area.hit.end() &&
                area.attack.reaches(parent, target, area.elapsed, area.lifetime, kind)) {
                area.hit.push_back(target.id);
                strikeTarget(target, area.attack.damageAt(area.elapsed, area.lifetime),
                             area.attack.flags, player.actor, players, targets, BlowSource::Breath);
            }
        }
    }
    targets.fixtures.settleBlasts(players, targets.fixtureHost);
    std::erase_if(m_sim.items, [this](const ItemArea& area) {
        if (area.elapsed < area.lifetime) {
            return false;
        }
        m_resources->effects.stop(area.effect);
        return true;
    });
}

void PlayerAttacks::strikeTarget(const MissileTarget& target, f32 damage, u32 flags,
                                 const PlayerActor& owner, std::span<PlayerRuntime> players,
                                 const Targets& targets, BlowSource source) {
    if (target.id >= kImportedTargetBase) {
        if (m_resources && m_resources->world.imported()) {
            m_resources->world.imported()->strike(target.id - kImportedTargetBase, damage, flags,
                                                  owner.player());
        }
        return;
    }
    // ProcessEffects' area query (CritterExpCollide) returns a horizontal unit
    // vector, not distance from the owner; that input is accumulated verbatim.
    const Vec3 away = target.base - owner.position();
    const f32 across = portable::hypot(away.x, away.z);
    const Vec3 direction = across > 0 ? Vec3{away.x, 0.0f, away.z} / across : Vec3{0};
    if (target.id >= kStatueTargetBase) {
        targets.opponents.wakeStatue(static_cast<usize>(target.id - kStatueTargetBase));
        return;
    }
    if (strikeSwitch(target.id, flags)) {
        return;
    }
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
        const Bosses& bosses = targets.opponents.bosses();
        const BossView before = bosses.view();
        targets.opponents.bosses().hurt(hit, target.id - kBossTargetBase);
        countBossBlow(targets, players, owner.player(), source, before, bosses);
    } else if (target.id >= kCritterTargetBase) {
        const s32 critter = target.id - kCritterTargetBase;
        const Critters& critters = targets.opponents.critters();
        const bool stood = critters.alive(critter) && !critters.dying(critter);
        targets.opponents.strikeCritter(critter, damage, flags, direction, owner.player(),
                                        target.base, false, players);
        countCritterBlow(targets, players, owner.player(), source, critters, critter, stood);
    } else if (target.id >= kGeneratorTargetBase) {
        if (blowRulesOf(targets).generatorTakes(source)) {
            const s32 hut = target.id - kGeneratorTargetBase;
            const Generators& huts = targets.opponents.generators();
            const bool stood = huts.standing(hut);
            targets.opponents.strikeGenerator(hut, damage, owner.player(), players);
            countGeneratorBlow(targets, players, owner.player(), source, huts, hut, stood);
        }
    } else if (target.id >= kEnemyTargetBase) {
        const s32 enemy = target.id - kEnemyTargetBase;
        const Enemies& enemies = targets.opponents.enemies();
        const bool stood = enemies.alive(enemy);
        targets.opponents.strikeEnemy(enemy, damage, flags, direction, owner.player(), players);
        countEnemyBlow(targets, players, owner.player(), source, enemies, enemy, stood);
    } else {
        targets.fixtures.strikeBarrel(static_cast<usize>(target.id), damage, owner.player(),
                                      players, targets.fixtureHost);
    }
}
} // namespace gdl::game
