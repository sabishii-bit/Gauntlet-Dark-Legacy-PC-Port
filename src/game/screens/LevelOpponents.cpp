#include "game/screens/LevelOpponents.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/Progression.h"
#include "game/screens/LevelFixtures.h"
namespace gdl::game {
void LevelOpponents::close() {
    if (m_resources.has_value()) {
        m_combatantProjectiles.clear(m_resources->effects);
        for (const CritterEffect& cue : m_critterEffects) {
            m_resources->effects.stop(cue.effect);
        }
        for (const u32 effect : m_cueEffects) {
            m_resources->effects.stop(effect);
        }
    }
    m_critterEffects.clear();
    m_moveEffects.clear();
    m_cueEffects.clear();
    m_generators.clear();
    m_enemyMissiles.clear();
    m_critters.close();
    m_bossMeter.clear();
    m_bosses.close();
    m_enemies.close();
    m_resources.reset();
}

/** The level's swarm and the generators that breed it, at the level's scales: what they can
 * take and deal is the level's own, how fast they go and see and how a generator breeds grow
 * with the difficulty setting. Placements of ordinary strength stand where the level puts
 * them, asleep when of no strength; the archer, bomber and suicide variants wait for their
 * missiles. */
void LevelOpponents::open(const Resources& resources, std::span<const PlayerRuntime> players) {
    close();
    m_resources.emplace(resources);
    RenderDevice& device = resources.device;
    const LevelWorld& world = resources.world;
    const LevelInfo* level = world.level();
    const f32 gain = resources.difficultyGain;
    EnemyScales scales;
    GeneratorScales breeding;
    s32 most = Enemies::kMost;
    if (level != nullptr) {
        scales.health = level->tuning.enemyHealth;
        scales.speed = level->tuning.enemySpeedScale(gain);
        scales.sight = level->tuning.enemySightScale(gain);
        scales.damage = level->tuning.enemyDamage;
        scales.playerLevel = level->tuning.playerLevel;
        breeding.health = level->tuning.generatorHealth;
        breeding.rate = level->tuning.generatorRateScale(gain);
        breeding.most = level->tuning.generatorMostScale(gain);
        most = level->maxEnemies;
    }
    const auto seed = static_cast<u32>(std::hash<std::string>{}(world.ref().name));
    m_enemies.open(device, resources.root, &world.collision(), most, scales, seed);
    const std::string& levelName = world.ref().name;
    m_critters.open(device, resources.root, &world.collision(), scales,
                    levelName.empty() ? 'G' : levelName.front());
    m_bosses.open(device, resources.root, &world.collision(), scales,
                  levelName.empty() ? 'G' : levelName.front());
    m_critterExperienceOwed.fill(0.0f);
    const auto playerCount = static_cast<s32>(players.size());
    const std::span<const LevelEnemy> roster = level != nullptr
                                                   ? std::span<const LevelEnemy>(level->enemies)
                                                   : std::span<const LevelEnemy>{};
    m_generators.bind(device, world.layout(), m_enemies, &world.collision(), breeding, playerCount,
                      roster);
    // The level's boss, at its boss mark.
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (const WorldLocator* mark = world.layout().findLocator(LocatorKind::Boss);
            mark != nullptr) {
            m_bosses.spawn(level->bossType, mark->position, mark->rotation.y);
            if (const HealthMeterDefinition* meter = m_bosses.meter(); meter != nullptr) {
                ItemArchive* archive = m_bosses.archive();
                m_bossMeter.bind(*meter, archive != nullptr ? &archive->textures : nullptr);
                const BossView boss = m_bosses.view();
                m_bossMeter.update(0, boss.health, boss.maxHealth, boss.alive, false);
            }
            // The first of the party carrying its legend item brings it to the fight.
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.save().progress().relics.hasLegend(m_bosses.legendRealm()) &&
                    m_bosses.bringLegend(actor.player())) {
                    log::info("Level {}: player {} brings the {} its legend item", levelName,
                              actor.player() + 1, bossNameOf(level->bossType));
                    break;
                }
            }
        }
    }
    const std::vector<ItemInfo>& infos = world.layout().itemInfos();
    for (const ItemInstance& instance : world.layout().itemInstances()) {
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kPlacedEnemy ||
            !shownToParty(instance.minPlayers, playerCount)) {
            continue;
        }
        const auto named = enemyKindOf(info.name);
        const s32 strength = Generators::paramOf(instance, 0);
        if (!named.has_value()) {
            continue;
        }
        const std::optional<s32> kind = levelKindOf(roster, *named, strength);
        // The great ones stand where they are put, facing as placed.
        const Mat4 stood = itemPlacement(instance.position, instance.rotation);
        const f32 facing = std::atan2(stood[2][0], stood[2][2]);
        if (*kind == kGolemEnemyKind) {
            m_critters.spawnGolem(instance.position, facing);
            continue;
        }
        if (*kind == kGeneralEnemyKind) {
            m_critters.spawnGeneral(instance.position, facing);
            continue;
        }
        if (*kind == kGargoyleEnemyKind) {
            m_critters.spawnGargoyle(instance.position, facing);
            continue;
        }
        if (*kind >= kSwarmKindCount || !m_enemies.loadKind(*kind)) {
            continue;
        }
        EnemySpawn spawn;
        spawn.kind = *kind;
        spawn.tier = std::max(strength, 1);
        spawn.algorithm = Generators::paramOf(instance, 1);
        if (const s32 interval = Generators::paramOf(instance, 3); interval > 0) {
            spawn.idleTicks = interval;
        }
        spawn.position = instance.position;
        const Mat4 placement = itemPlacement(instance.position, instance.rotation);
        spawn.direction = Vec3{placement[2][0], 0.0f, placement[2][2]};
        spawn.placed = true;
        spawn.asleep = strength == 0;
        m_enemies.spawn(spawn, {}, m_generators.obstacles());
    }
}

/** The party as the swarm sees it: where each stands, how big, how seasoned; the fallen and
 * those in the tower are not seen. */
std::vector<EnemyView> LevelOpponents::enemyViews(std::span<const PlayerRuntime> players) {
    std::vector<EnemyView> views;
    views.reserve(players.size());
    for (const PlayerRuntime& player : players) {
        const PlayerActor& actor = player.actor;
        EnemyView view;
        view.player = actor.player();
        view.position = actor.position();
        view.radius = actor.radius();
        view.height = actor.height();
        view.level = experienceLevel(actor.save().experience());
        view.hidden = player.life != PlayerLife::Standing;
        view.captured = player.capture.held();
        views.push_back(view);
    }
    return views;
}

void LevelOpponents::applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                      const Events& events) {
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        if (player.actor.player() != blow.player || player.life != PlayerLife::Standing ||
            (blow.breath && player.breathGap > 0.0f) || (blow.area && player.effectGap > 0.0f)) {
            continue;
        }
        if (blow.breath) {
            const Vec3 centre = player.actor.position() + Vec3{0, player.actor.height() * 0.5f, 0};
            if (events.blocksBreath && events.blocksBreath(blow.origin, centre)) {
                continue;
            }
            player.breathGap = 0.25f;
        }
        if (blow.area) {
            const Vec3 centre = player.actor.position() + Vec3{0, player.actor.height() * 0.5f, 0};
            const Vec3 delta = centre - blow.origin;
            if (glm::length(Vec2{delta.x, delta.z}) > 10.0f && events.blocksArea &&
                events.blocksArea(blow.origin, centre)) {
                continue;
            }
            player.effectGap = blow.repeatGap;
        }
        events.hurt(i, blow.damage, blow.breath ? HurtKind::Burn : HurtKind::Blow, true,
                    {blow.flags, blow.direction});
    }
}

void LevelOpponents::applyGrab(const CombatGrab& grab, bool boss,
                               std::span<PlayerRuntime> players) {
    for (PlayerRuntime& player : players) {
        if (player.actor.player() != grab.player || player.life != PlayerLife::Standing) {
            continue;
        }
        PlayerCapture& capture = player.capture;
        if (capture.held() && (capture.owner() != grab.critter || capture.boss() != boss)) {
            continue;
        }
        if (grab.attachment.has_value()) {
            capture.attach(grab.critter, boss, *grab.attachment, player.actor);
            player.reaction = PlayerDeed::None;
            player.rammed.clear();
        } else {
            capture.release(grab.velocity, grab.damage);
        }
    }
}

/** The generators breed, the swarm goes about its business, and what it lands on the party
 * is taken: a power blow from a tall one is a knock that makes its victim flinch. What the
 * party has done to it is paid in experience. */
void LevelOpponents::update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                            std::span<const Obstacle> fixtures, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (PlayerRuntime& player : players) {
        player.breathGap = std::max(0.0f, player.breathGap - std::max(seconds, 0.0f));
        player.effectGap = std::max(0.0f, player.effectGap - std::max(seconds, 0.0f));
    }
    const std::vector<EnemyView> views = enemyViews(players);
    std::vector<Obstacle> boxes = m_generators.obstacles();
    boxes.insert(boxes.end(), fixtures.begin(), fixtures.end());
    const LevelInfo* level = m_resources->world.level();
    const f32 missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1.0f;
    m_generators.update(ticks, m_enemies, views, boxes);
    m_enemies.update(ticks, seconds, views, boxes, &m_enemyMissiles, missileSpeed);
    m_enemyMissiles.update(seconds, &m_resources->world.collision(), views);
    // What the throwers let fly lands on the party, or bursts where it fell; what blows
    // itself up blasts everything about it.
    for (const EnemyMissileHit& hit : m_enemyMissiles.takeHits()) {
        for (usize i = 0; i < players.size(); ++i) {
            if (hit.player >= 0 && players[i].actor.player() == hit.player &&
                players[i].life == PlayerLife::Standing) {
                events.hurt(i, hit.damage, HurtKind::Pierce, true, {hit.flags, hit.direction});
            }
        }
        if (hit.burstRadius > 0.0f) {
            events.blast(hit.position, hit.burstRadius, hit.damage);
        }
    }
    for (const EnemyBurst& burst : m_enemies.takeBursts()) {
        events.blast(burst.position, LevelFixtures::kBlastRadius, burst.damage);
    }
    events.settleBlasts();
    m_critters.update(ticks, seconds, views);
    m_bosses.setArenaAnchors(events.arenaAnchors ? events.arenaAnchors() : std::vector<Mat4>{});
    m_bosses.setArenaTargets(events.arenaTargets ? events.arenaTargets()
                                                 : std::vector<CombatArenaTarget>{});
    m_bosses.update(ticks, seconds, views);
    for (const auto& grab : m_critters.takeGrabs()) {
        applyGrab(grab, false, players);
    }
    for (const auto& grab : m_bosses.takeGrabs()) {
        applyGrab(grab, true, players);
    }
    for (PlayerRuntime& player : players) {
        PlayerCapture& capture = player.capture;
        if (player.life != PlayerLife::Standing) {
            capture.clear();
        } else if (capture.held() && !(capture.boss() ? m_bosses.present() && m_bosses.view().alive
                                                      : m_critters.alive(capture.owner()))) {
            capture.release(Vec3{0}, 0);
        }
    }
    for (const auto& activation : m_bosses.takeArenaActivations()) {
        if (events.activateArena) {
            events.activateArena(activation);
        }
    }
    const auto shotSound = [&](std::string_view name) { m_resources->audio.playNamed(name); };
    for (const CombatShot& shot : m_bosses.takeShots()) {
        if (ItemArchive* archive = m_bosses.archive(); archive != nullptr) {
            m_combatantProjectiles.launch(shot, *archive, m_resources->device, m_resources->effects,
                                          shotSound);
        }
    }
    for (const CombatShot& shot : m_critters.takeShots()) {
        if (ItemArchive* archive = m_critters.archiveOf(shot.critter); archive != nullptr) {
            m_combatantProjectiles.launch(shot, *archive, m_resources->device, m_resources->effects,
                                          shotSound);
        }
    }
    m_combatantProjectiles.update(seconds, &m_resources->world.collision(), views,
                                  m_resources->device, m_resources->effects, shotSound);
    for (const CombatantProjectileHit& hit : m_combatantProjectiles.takeHits()) {
        for (usize player = 0; player < players.size(); ++player) {
            if (players[player].actor.player() == hit.player &&
                players[player].life == PlayerLife::Standing && players[player].effectGap <= 0) {
                events.hurt(player, hit.damage, HurtKind::Pierce, true, {hit.flags, hit.direction});
                players[player].effectGap = hit.repeatGap;
            }
        }
    }
    if (m_bossMeter.bound()) {
        const BossView boss = m_bosses.view();
        m_bossMeter.update(ticks, boss.health, boss.maxHealth, m_bosses.present() && boss.alive,
                           m_bosses.frozen());
    }
    // The legend item held up is the bearer's no more; the rite is shown as it goes.
    for (const LegendEvent& event : m_bosses.takeLegendEvents()) {
        if (event.cue == LegendCue::Brandished) {
            for (PlayerRuntime& runtime : players) {
                PlayerActor& actor = runtime.actor;
                if (actor.player() == event.player) {
                    actor.save().progress().relics.spendLegend(event.realm);
                }
            }
        }
        events.legend(event);
    }
    events.advanceLegend(seconds);
    for (const CombatBlow& blow : m_bosses.takeBlows()) {
        applyCritterBlow(blow, players, events);
    }
    awardBossLosses(players, events);
    events.advanceVictory(ticks, seconds);
    // The great ones' effects and sounds: a move's, a strike's, a hit's.
    for (const CombatCue& cue : m_bosses.takeCues()) {
        if (cue.shakes && events.shake) {
            events.shake();
        }
        showCritterCue(cue, m_bosses.archive(), true);
    }
    for (const CombatCue& cue : m_critters.takeCues()) {
        if (cue.shakes && events.shake) {
            events.shake();
        }
        showCritterCue(cue, m_critters.archiveOf(cue.critter), false);
    }
    followCritterEffects();
    for (const CombatBlow& blow : m_critters.takeBlows()) {
        applyCritterBlow(blow, players, events);
    }
    awardCritterLosses(players, events);
    events.levels();
    for (const EnemyBlow& blow : m_enemies.takeBlows()) {
        for (usize i = 0; i < players.size(); ++i) {
            if (players[i].actor.player() != blow.player ||
                players[i].life != PlayerLife::Standing) {
                continue;
            }
            events.hurt(i, blow.damage, HurtKind::Blow, true,
                        {blow.knocksBack ? PlayerImpact::kKnockBack : 0, blow.direction});
        }
    }
    for (const EnemyLoss& loss : m_enemies.takeLosses()) {
        events.award(loss.player, loss.experience, loss.killed);
    }
}

/** A hit on one of the swarm, from a player or the world. */
void LevelOpponents::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                                 std::span<const PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_enemies.hurt(id, hit);
}

/** A hit on one of the great ones. */
void LevelOpponents::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction,
                                   s32 byPlayer, std::optional<Vec3> where, bool close,
                                   std::span<const PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    hit.where = where;
    hit.close = close;
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_critters.hurt(id, hit);
}

/** A hit on a generator: as it crumbles a state its kind's hit or death effect plays over
 * it to the realm's own sound (`S_GENDAMG`, `S_GENKILLG`), and, gone, its brood is freed of
 * it. */
void LevelOpponents::strikeGenerator(s32 id, f32 power, s32 byPlayer) {
    if (!m_resources.has_value()) {
        return;
    }
    const auto event = m_generators.strike(id, power, byPlayer);
    if (!event.has_value()) {
        return;
    }
    if (ItemArchive* archive = m_enemies.archive(event->kind); archive != nullptr) {
        const std::string_view tree = event->destroyed ? "GENDIE" : "GENHIT";
        if (archive->trees.find(tree).has_value()) {
            m_resources->effects.start(m_resources->device, *archive, tree, event->position);
        }
    }
    const std::string& levelName = m_resources->world.ref().name;
    m_resources->audio.playNamed(std::format("{}{}", event->destroyed ? "S_GENKILL" : "S_GENDAM",
                                             levelName.empty() ? 'G' : levelName.front()));
    if (event->destroyed) {
        m_enemies.generatorGone(id);
    }
}

/** Plays what one of the great ones (the boss with `ofBoss`) has set off: its tree from its
 * own archive (or the weapons', which holds the common marks of a hit) where it happened,
 * riding along with it when it follows, and its sound. */
void LevelOpponents::showCritterCue(const CombatCue& cue, ItemArchive* archive, bool ofBoss) {
    if (!m_resources.has_value()) {
        return;
    }
    if (cue.stopMoveEffect) {
        std::erase_if(m_moveEffects, [&](const MoveEffect& previous) {
            if (previous.critter != cue.critter || previous.ofBoss != ofBoss) {
                return false;
            }
            m_resources->effects.stop(previous.effect);
            return true;
        });
        return;
    }
    if (cue.arena && m_bosses.present()) {
        const std::string_view object = bossArenaObject(m_bosses.view().kind);
        if (!object.empty()) {
            m_resources->world.setObjectVisible(object, false);
        }
    }
    if (archive == nullptr || !archive->trees.find(cue.tree).has_value()) {
        archive =
            m_resources->weapons.loaded() && m_resources->weapons.trees.find(cue.tree).has_value()
                ? &m_resources->weapons
                : nullptr;
    }
    if (!cue.tree.empty() && archive != nullptr) {
        EffectTrees::Setting setting;
        setting.scale = cue.scale;
        setting.yaw = cue.yaw;
        setting.seconds = cue.life;
        setting.loop = cue.loop;
        const u32 effect = m_resources->effects.startSet(m_resources->device, *archive, cue.tree,
                                                         cue.position, setting);
        if (effect != 0) {
            m_cueEffects.push_back(effect);
            if (cue.untilNextMove) {
                m_moveEffects.push_back({effect, cue.critter, ofBoss});
            }
        }
        if (effect != 0 && cue.placement.has_value()) {
            m_resources->effects.placeAt(effect, *cue.placement);
        }
        if (effect != 0 && cue.follows) {
            const Vec3* at = ofBoss ? m_bosses.position() : &m_critters.positionOf(cue.critter);
            m_critterEffects.push_back(
                CritterEffect{effect, cue.critter, ofBoss,
                              at != nullptr ? cue.position - *at : Vec3{0.0f, 0.0f, 0.0f}, cue.node,
                              cue.nodeOffset, cue.rootAttachment, cue.pitchYaw});
        }
    }
    if (!cue.sound.empty()) {
        m_resources->audio.playNamed(cue.sound);
    }
}

/** Effects riding on the great ones go where they go, and are let go of when they end. */
void LevelOpponents::followCritterEffects() {
    if (!m_resources.has_value()) {
        return;
    }
    std::erase_if(m_cueEffects,
                  [this](u32 effect) { return !m_resources->effects.playing(effect); });
    std::erase_if(m_moveEffects, [this](const MoveEffect& effect) {
        return !m_resources->effects.playing(effect.effect);
    });
    for (usize i = 0; i < m_critterEffects.size();) {
        const CritterEffect& riding = m_critterEffects[i];
        const bool alive =
            riding.ofBoss ? m_bosses.present()
                          : m_critters.alive(riding.critter) || m_critters.dying(riding.critter);
        if (!m_resources->effects.playing(riding.effect) || !alive) {
            if (!alive && (riding.node.has_value() || riding.rootAttachment)) {
                m_resources->effects.stop(riding.effect);
            }
            m_critterEffects.erase(m_critterEffects.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const Vec3* at =
            riding.ofBoss ? m_bosses.position() : &m_critters.positionOf(riding.critter);
        if (riding.rootAttachment) {
            const auto parent = riding.ofBoss ? m_bosses.rootTransform()
                                              : m_critters.rootTransformOf(riding.critter);
            if (parent.has_value()) {
                m_resources->effects.placeAt(
                    riding.effect,
                    CritterArea::placement(*parent, riding.nodeOffset, riding.pitchYaw));
            }
        } else if (riding.node.has_value()) {
            const auto parent = riding.ofBoss
                                    ? m_bosses.nodeTransform(*riding.node)
                                    : m_critters.nodeTransformOf(riding.critter, *riding.node);
            if (parent.has_value()) {
                m_resources->effects.placeAt(
                    riding.effect,
                    CritterArea::placement(*parent, riding.nodeOffset, riding.pitchYaw));
            }
        } else if (at != nullptr) {
            m_resources->effects.moveTo(riding.effect, *at + riding.offset);
        }
        ++i;
    }
}

/** The boss's worth goes the great ones' way: shares to the hitter, a kill's to everyone. */
void LevelOpponents::awardBossLosses(std::span<const PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const CombatSpew& spew : m_bosses.takeSpews()) {
        events.spew(spew);
    }
    for (const CombatLoss& loss : m_bosses.takeLosses()) {
        if (loss.killed) {
            events.fallen(loss.position);
        }
        for (const PlayerRuntime& runtime : players) {
            const PlayerActor& actor = runtime.actor;
            const s32 player = actor.player();
            if ((loss.player >= 0 && loss.player != player) || player < 0 ||
                static_cast<usize>(player) >= m_critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_critterExperienceOwed[static_cast<usize>(player)];
            owed += loss.experience;
            const auto whole = static_cast<s32>(std::floor(owed));
            if (whole > 0) {
                owed -= static_cast<f32>(whole);
                events.award(player, whole, loss.killed);
            }
        }
    }
}

/** What the great ones are worth: a share to whoever hurt one, whole points as they add
 * up, and a kill's share to everyone. */
void LevelOpponents::awardCritterLosses(std::span<const PlayerRuntime> players,
                                        const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const CombatLoss& loss : m_critters.takeLosses()) {
        // A gargoyle slain leaves the key its form is named by where it fell.
        if (loss.killed && loss.kind == CombatantKind::Gargoyle && !loss.form.empty()) {
            m_resources->world.placeItem(m_resources->device, "GARG" + loss.form, loss.position);
        }
        for (const PlayerRuntime& runtime : players) {
            const PlayerActor& actor = runtime.actor;
            const s32 player = actor.player();
            if (loss.player >= 0 && loss.player != player) {
                continue;
            }
            if (player < 0 || static_cast<usize>(player) >= m_critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_critterExperienceOwed[static_cast<usize>(player)];
            owed += loss.experience;
            const auto whole = static_cast<s32>(std::floor(owed));
            if (whole > 0) {
                owed -= static_cast<f32>(whole);
                events.award(player, whole, loss.killed);
            }
        }
    }
}

} // namespace gdl::game
