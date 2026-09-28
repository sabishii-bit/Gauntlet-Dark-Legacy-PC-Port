#include "game/screens/LevelOpponents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/PlayerPowerups.h"
#include "game/world/BodyCollision.h"
#include "game/world/DynamicLights.h"
namespace gdl::game {
namespace {
// The swarm's own sounds (sounds_evt.c): a throw going off, a suicide's cry and bang.
constexpr std::string_view kArrowSound = "S_ENEMYARROW";
constexpr std::string_view kBoltSound = "S_ENEMYFIREBALL";
constexpr std::string_view kDemonBoltSound = "S_FIREHOLE"; ///< the realm's own in C, D and F
constexpr std::string_view kSuicideYellSound = "S_SUICIDE_YELL";
constexpr std::string_view kSuicideBombSound = "S_SUICIDE_BOMB";
constexpr f32 kQuietSound = 127.0f; ///< the levels of 255 they are played at
constexpr f32 kLoudSound = 224.0f;
constexpr f32 kFullLevel = 255.0f;
constexpr s32 kDemonKind = 2;
constexpr f32 kHeardWhole = 1.4f;  ///< how loud a sound is at nobody's distance, before the cap
constexpr f32 kHeardFade = 50.0f;  ///< and the distance over which it loses all of that
constexpr f32 kUnseenBurst = 1.0f; ///< a burst's life when its effect cannot be shown
constexpr std::string_view kRicochetSound = "S_RICOCHET";
constexpr u32 kReflectingArmor = 0x01020000; ///< reflective armour and armour reflect
constexpr s32 kCritterIds = 3000;            ///< the swarm's ids past the ordinary enemies
constexpr s32 kBossIds = 4000;
// SuicideExplosion (sfx.c): a burning blast, or in the town and the sky a poison cloud, and
// either way the thrower kind's own smoke.
constexpr std::string_view kSuicideBlast = "EXPLOSION";
constexpr std::string_view kSuicideRing = "EXPRING";
constexpr std::string_view kSuicideSmoke = "SUICIDEEXP";
constexpr std::array<std::string_view, 3> kSuicideCloud{"POISONEXP1", "POISONEXP2", "POISONEXP3"};
constexpr f32 kCloudHold = 2.0f; ///< the cloud's second stage lasts this
constexpr f32 kBlastRadius = 6.0f;
constexpr f32 kCloudRadius = 7.5f;
constexpr u32 kBlastFlags = 0x421; ///< fire, knocked down, an explosion
constexpr f32 kBlastLight = 20.0f;
constexpr f32 kRingScale = 1.2f;
constexpr f32 kCloudDrop = 1.0f;
constexpr Vec3 kCloudStretch{2.5f, 1.0f, 2.5f};
constexpr f32 kSmokeFade = 0.5f;
constexpr f32 kBlastSeconds = 1.0f; ///< the blast's and the cloud's first stage without art
constexpr f32 kCloudSeconds = 2.0f / 3.0f;

bool cloudRealm(const std::string& level) {
    return !level.empty() && (level.front() == 'G' || level.front() == 'K');
}
} // namespace

f32 LevelOpponents::attenuation(const Vec3& at, std::span<const Vec3> hearers) {
    if (hearers.empty()) {
        return 1.0f;
    }
    f32 nearest = std::numeric_limits<f32>::max();
    for (const Vec3& hearer : hearers) {
        nearest = std::min(nearest, glm::distance(hearer, at));
    }
    return std::clamp(kHeardWhole - nearest / kHeardFade, 0.0f, 1.0f);
}

void LevelOpponents::hearFrom(std::span<const PlayerRuntime> players) {
    m_hearers.clear();
    for (const PlayerRuntime& runtime : players) {
        if (runtime.life == PlayerLife::Standing) {
            m_hearers.push_back(runtime.actor.followPoint());
        }
    }
}

SoundHandle LevelOpponents::playAt(std::string_view name, f32 level, const Vec3& at) {
    if (!m_resources.has_value() || name.empty()) {
        return kNoSound;
    }
    const f32 heard = attenuation(at, m_hearers);
    if (heard <= 0.0f) {
        return kNoSound;
    }
    return m_resources->audio.playNamed(name, level / kFullLevel * heard);
}

void LevelOpponents::lights(std::vector<PointLight>& out) const {
    m_enemyMissiles.lights(out);
}

/** A throw going off sounds as it leaves (fn_8004E448): the arrow's twang, a bolt's (a
 * demon's the realm's fire-hole roar); a suicide cries out as it starts its run. */
void LevelOpponents::playEnemyCues() {
    if (!m_resources.has_value()) {
        return;
    }
    for (const EnemyCue& cue : m_enemies.takeCues()) {
        switch (cue.kind) {
        case EnemyCue::Kind::Arrow: playAt(kArrowSound, kQuietSound, cue.position); break;
        case EnemyCue::Kind::Bolt:
            if (cue.enemyKind == kDemonKind) {
                const std::string& level = m_resources->world.ref().name;
                const char realm = level.empty() ? ' ' : level.front();
                playAt(realm == 'C' || realm == 'D' || realm == 'F'
                           ? std::format("{}{}", kDemonBoltSound, realm)
                           : std::string{kDemonBoltSound},
                       kQuietSound, cue.position);
            } else {
                playAt(kBoltSound, kQuietSound, cue.position);
            }
            break;
        case EnemyCue::Kind::Yell:
            if (const SoundHandle yell = playAt(kSuicideYellSound, kLoudSound, cue.position);
                yell != kNoSound) {
                m_yells.push_back(yell);
            }
            break;
        }
    }
}

/** The swarm and the great ones as a missile sent back or a blast reaches them, by id: the
 * ordinary enemies by their slots, the great ones and the boss past those. */
std::vector<MissileTarget> LevelOpponents::swarmTargets() const {
    std::vector<MissileTarget> swarm = m_enemies.targets();
    for (MissileTarget target : m_critters.targets(true)) {
        target.id += kCritterIds;
        swarm.push_back(target);
    }
    for (MissileTarget target : m_bosses.targets()) {
        target.id += kBossIds;
        swarm.push_back(target);
    }
    return swarm;
}

/** A hit on one of the swarm by its id in `swarmTargets`, credited to nobody. */
void LevelOpponents::strikeSwarm(s32 id, f32 damage, u32 flags, const Vec3& direction,
                                 std::span<const PlayerRuntime> players) {
    if (id >= kBossIds) {
        EnemyHit hit;
        hit.damage = damage;
        hit.flags = flags;
        hit.direction = direction;
        m_bosses.hurt(hit, id - kBossIds);
    } else if (id >= kCritterIds) {
        strikeCritter(id - kCritterIds, damage, flags, direction, -1, std::nullopt, false, players);
    } else {
        strikeEnemy(id, damage, flags, direction, -1, players);
    }
}

/** What the throwers let fly lands (ProcessEffects): on a player it hurts as a blow, heard
 * as an arrow or a bolt; armour that reflects rings as it turns one back; on the world or an
 * item a shot leaves its element's spark; sent back, it hurts the swarm; a lob bursts where
 * it ends, and the burst grows over its effect's life. A blast's reach hurts whoever it
 * finds, gas as gas. */
void LevelOpponents::landEnemyMissiles(std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const EnemyMissileHit& hit : m_enemyMissiles.takeHits()) {
        if (hit.ricochet) {
            playAt(kRicochetSound, kQuietSound, hit.position);
            continue;
        }
        if (hit.target >= 0) {
            strikeSwarm(hit.target, hit.damage, hit.flags, hit.direction, players);
        }
        u32 effect = 0;
        if (const std::string_view tree = hit.effect(); !tree.empty()) {
            EffectTrees::Setting setting;
            setting.unlit = true;
            setting.depthWrite = false;
            if (hit.burstRadius <= 0) {
                setting.tint.a = 96;
            }
            if (m_resources->weapons.loaded()) {
                effect = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                       tree, hit.position, setting);
                if (effect != 0) {
                    m_cueEffects.push_back(effect);
                }
            }
        }
        playAt(hit.sound(), kLoudSound, hit.position);
        const HurtKind kind = (hit.flags & EnemyBlast::kGas) != 0 ? HurtKind::Gas : HurtKind::Blow;
        for (usize i = 0; i < players.size(); ++i) {
            if (hit.player >= 0 && players[i].actor.player() == hit.player &&
                players[i].life == PlayerLife::Standing) {
                events.hurt(i, hit.damage, kind, true, {hit.flags, hit.direction});
            }
        }
        if (hit.burstRadius > 0.0f) {
            const f32 life = m_resources->effects.remaining(effect).value_or(kUnseenBurst);
            m_enemyMissiles.burst(hit.position, hit.burstRadius, hit.damage, hit.flags,
                                  life > 0.0f ? life : kUnseenBurst, hit.player);
        }
    }
}

/** A suicide going up (SuicideExplosion): a burning blast of six with its ring and red light,
 * or in the town and the sky a poison cloud of seven and a half that hangs on, and the
 * kind's smoke either way, fading out. The blast's harm grows out from it as it fades. */
void LevelOpponents::explodeSuicide(const EnemyBurst& burst) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyBlast blast;
    blast.position = burst.position;
    blast.damage = burst.damage;
    const bool cloud = cloudRealm(m_resources->world.ref().name);
    ItemArchive& weapons = m_resources->weapons;
    const auto start = [&](std::string_view tree, const Vec3& at,
                           const EffectTrees::Setting& setting) -> u32 {
        if (!weapons.loaded() || !weapons.trees.find(tree).has_value()) {
            return 0;
        }
        const u32 id =
            m_resources->effects.startSet(m_resources->device, weapons, tree, at, setting);
        if (id != 0) {
            m_cueEffects.push_back(id);
        }
        return id;
    };
    if (cloud) {
        EffectTrees::Setting setting;
        setting.stretch = kCloudStretch;
        const Vec3 at = burst.position - Vec3{0.0f, kCloudDrop, 0.0f};
        const u32 first = start(kSuicideCloud[0], at, setting);
        m_clouds.push_back(Cloud{first, 1, at});
        blast.radius = kCloudRadius;
        blast.flags = EnemyBlast::kGas;
        blast.stages = {m_resources->effects.remaining(first).value_or(kCloudSeconds), kCloudHold};
    } else {
        EffectTrees::Setting setting;
        setting.light = EffectTrees::Light{DynamicLights::blast(), kBlastLight, true};
        const u32 fire = start(kSuicideBlast, burst.position, setting);
        EffectTrees::Setting ring;
        ring.scale = kRingScale;
        start(kSuicideRing, burst.position, ring);
        blast.radius = kBlastRadius;
        blast.flags = kBlastFlags;
        blast.stages = {m_resources->effects.remaining(fire).value_or(kBlastSeconds)};
    }
    // The smoke is the thrower kind's own, in the first archive that has it.
    ItemArchive* smoke = m_enemies.archive(burst.kind);
    if (smoke == nullptr || !smoke->trees.find(kSuicideSmoke).has_value()) {
        smoke = nullptr;
        for (s32 kind = 0; kind < kSwarmKindCount && smoke == nullptr; ++kind) {
            ItemArchive* archive = m_enemies.kindLoaded(kind) ? m_enemies.archive(kind) : nullptr;
            if (archive != nullptr && archive->trees.find(kSuicideSmoke).has_value()) {
                smoke = archive;
            }
        }
    }
    if (smoke != nullptr) {
        EffectTrees::Setting setting;
        setting.fadeSeconds = kSmokeFade;
        const u32 id = m_resources->effects.startSet(m_resources->device, *smoke, kSuicideSmoke,
                                                     burst.position, setting);
        if (id != 0) {
            m_cueEffects.push_back(id);
        }
    }
    m_enemyMissiles.blast(std::move(blast));
}

/** A poison cloud turns from its first tree to the one that hangs for two seconds, then to
 * the one it clears in (SfxSetMorph). */
void LevelOpponents::advanceClouds() {
    if (!m_resources.has_value()) {
        return;
    }
    ItemArchive& weapons = m_resources->weapons;
    for (Cloud& cloud : m_clouds) {
        if (cloud.effect != 0 && m_resources->effects.playing(cloud.effect)) {
            continue;
        }
        cloud.effect = 0;
        if (cloud.stage >= kSuicideCloud.size() || !weapons.loaded() ||
            !weapons.trees.find(kSuicideCloud[cloud.stage]).has_value()) {
            cloud.stage = kSuicideCloud.size();
            continue;
        }
        EffectTrees::Setting setting;
        setting.stretch = kCloudStretch;
        if (cloud.stage == 1) {
            setting.seconds = kCloudHold;
        }
        cloud.effect = m_resources->effects.startSet(
            m_resources->device, weapons, kSuicideCloud[cloud.stage], cloud.position, setting);
        if (cloud.effect != 0) {
            m_cueEffects.push_back(cloud.effect);
        }
        ++cloud.stage;
    }
    std::erase_if(m_clouds, [](const Cloud& cloud) {
        return cloud.effect == 0 && cloud.stage >= kSuicideCloud.size();
    });
}

Vec3 LevelOpponents::resolveMovement(const PlayerActor& player, const Vec3& from,
                                     const Vec3& to) const {
    auto bodies = m_enemies.targets();
    if ((PowerupEffects::of(player.save().progress().inventory).armor & DeathRules::kProtection) ==
        0) {
        std::erase_if(bodies, [this](const MissileTarget& body) {
            return m_enemies.kindOf(body.id) == kDeathKind;
        });
    }
    const auto critters = m_critters.targets(true);
    const auto bosses = m_bosses.targets();
    bodies.insert(bodies.end(), critters.begin(), critters.end());
    bodies.insert(bodies.end(), bosses.begin(), bosses.end());
    Vec3 resolved = BodyCollision::resolve(from, to, player.radius(), player.height(), bodies);
    // A slide along a creature must still respect the level walls. If wall resolution
    // would move back inside a creature, retain the already-safe pre-step position.
    if (m_resources.has_value()) {
        resolved = m_resources->world.collision().resolveWalls(
            resolved, player.radius(), resolved.y, resolved.y + player.height());
        const Vec3 checked =
            BodyCollision::resolve(from, resolved, player.radius(), player.height(), bodies);
        if (glm::distance(checked, resolved) > 1e-4f) {
            return {from.x, to.y, from.z};
        }
    }
    return resolved;
}

void LevelOpponents::close() {
    m_pending.clear();
    clearDeaths();
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
    m_destroyedGenerators.clear();
    m_enemyMissiles.clear();
    m_clouds.clear();
    m_yells.clear();
    m_hearers.clear();
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
    LevelWorld& world = resources.world;
    const LevelInfo* level = world.level();
    const f32 gain = resources.difficultyGain;
    EnemyScales scales;
    scales.players = static_cast<s32>(std::max<usize>(players.size(), 1));
    GeneratorScales breeding;
    s32 most = Enemies::kMost;
    if (level != nullptr) {
        scales.health = level->tuning.enemyHealth;
        scales.speed = level->tuning.enemySpeedScale(gain);
        scales.sight = level->tuning.enemySightScale(gain);
        scales.damage = level->tuning.enemyDamage;
        scales.missileRate = level->tuning.enemyMissileRate;
        scales.missileAim = level->tuning.enemyMissileAim;
        scales.playerLevel = level->tuning.playerLevel;
        scales.bossEncounter = level->bossType >= 0;
        breeding.health = level->tuning.generatorHealth;
        breeding.rate = level->tuning.generatorRateScale(gain);
        breeding.most = level->tuning.generatorMostScale(gain);
        most = level->maxEnemies;
    }
    const auto seed = static_cast<u32>(std::hash<std::string>{}(world.ref().name));
    m_enemies.open(device, resources.root, &world.collision(), most, scales, seed);
    m_enemies.setHazards(&world.hazards());
    m_enemies.setLookouts(LookoutRoute::of(world.layout().locators()));
    const std::string& levelName = world.ref().name;
    m_critters.open(device, resources.root, &world.collision(), scales,
                    levelName.empty() ? 'G' : levelName.front());
    m_critters.setHazards(&world.hazards());
    m_bosses.open(device, resources.root, &world.collision(), scales,
                  levelName.empty() ? 'G' : levelName.front());
    m_critterExperienceOwed.fill(0.0f);
    const auto playerCount = static_cast<s32>(players.size());
    const std::span<const LevelEnemy> roster = level != nullptr
                                                   ? std::span<const LevelEnemy>(level->enemies)
                                                   : std::span<const LevelEnemy>{};
    m_generators.bind(device, world.layout(), m_enemies, &world.collision(), breeding, playerCount,
                      roster, static_cast<s32>(world.ref().realmId), &world.items());
    // The level's boss, at its boss mark.
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (const WorldLocator* mark = world.layout().findLocator(LocatorKind::Boss);
            mark != nullptr) {
            m_bosses.spawn(level->bossType, mark->position, mark->rotation.y);
            ItemArchive* archive = m_bosses.archive();
            m_bossMeter.bind(m_bosses.healthMeters(),
                             archive != nullptr ? &archive->textures : nullptr);
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
        Placement placement;
        placement.kind = *kind;
        placement.facing = std::atan2(stood[2][0], stood[2][2]);
        placement.viewRadius = 2.0f * std::max(info.radius, info.height);
        placement.spawn.position = instance.position;
        const bool great =
            *kind == kGolemEnemyKind || *kind == kGeneralEnemyKind || *kind == kGargoyleEnemyKind;
        if (great) {
            m_pending.push_back(placement);
            continue;
        }
        // IT waits for company (fn_80060114: never with one player).
        if (*kind == kItKind && playerCount <= 1) {
            continue;
        }
        if ((*kind >= kSwarmKindCount && *kind != kDeathKind) || !m_enemies.loadKind(*kind)) {
            continue;
        }
        EnemySpawn& spawn = placement.spawn;
        spawn.kind = *kind;
        spawn.tier = std::max(strength, 1);
        spawn.algorithm = Generators::paramOf(instance, 1);
        if (const s32 interval = Generators::paramOf(instance, 3); interval > 0) {
            spawn.idleTicks = interval;
        }
        spawn.position = instance.position;
        spawn.direction = Vec3{stood[2][0], 0.0f, stood[2][2]};
        spawn.placed = true;
        spawn.priority = EnemySpawn::Priority::Visible;
        spawn.asleep = strength == 0 && *kind != kDeathKind;
        m_pending.push_back(placement);
    }
    if (!resources.standOnSight) {
        standPlacements(std::nullopt, Vec3{0.0f});
    }
}

void LevelOpponents::watch(const ViewVolume& view, const Vec3& attention) {
    m_enemies.setView(view);
    m_generators.setView(view);
    standPlacements(view, attention);
}

/** Each placed enemy stands once the camera sees its spot, by its size, looking from no more
 * than fifty away (fn_80060114); seen, it is gone from the waiting list whether or not the
 * swarm had room for it. With no view, all stand. */
void LevelOpponents::standPlacements(std::optional<ViewVolume> view, const Vec3& attention) {
    constexpr f32 kPlacementReach = 50.0f;
    std::erase_if(m_pending, [&](const Placement& placement) {
        const Vec3& at = placement.spawn.position;
        if (view.has_value() && (!view->sees(at, placement.viewRadius) ||
                                 glm::distance(attention, at) > kPlacementReach)) {
            return false;
        }
        // Seen, a placement may take the place of what is on screen (generate_enemy's
        // importance 1); all standing at once, they only fill what is free.
        Placement standing = placement;
        if (!view.has_value()) {
            standing.spawn.priority = EnemySpawn::Priority::FreeSlotOnly;
        }
        stand(standing);
        return true;
    });
}

void LevelOpponents::stand(const Placement& placement) {
    const Vec3& at = placement.spawn.position;
    switch (placement.kind) {
    case kGolemEnemyKind: m_critters.spawnGolem(at, placement.facing); return;
    case kGeneralEnemyKind: m_critters.spawnGeneral(at, placement.facing); return;
    case kGargoyleEnemyKind: m_critters.spawnGargoyle(at, placement.facing); return;
    default: m_enemies.spawn(placement.spawn, {}, m_generators.obstacles()); return;
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
        const auto powerups = PowerupEffects::of(actor.save().progress().inventory);
        view.invisible = powerups.invisible();
        view.antiDeath = (powerups.armor & DeathRules::kProtection) != 0;
        view.reflects = (powerups.armor & kReflectingArmor) != 0;
        if ((powerups.special & powerup::kHealthVamp) != 0) {
            view.meleeWard = EnemyMeleeWard::HealthVamp;
        } else if ((powerups.special & powerup::kHandOfDeath) != 0) {
            view.meleeWard = EnemyMeleeWard::HandOfDeath;
        }
        view.captured = player.capture.held();
        views.push_back(view);
    }
    return views;
}

void LevelOpponents::applyEnemyBlow(const EnemyBlow& blow, std::span<PlayerRuntime> players,
                                    const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        if (player.actor.player() != blow.player || player.life != PlayerLife::Standing) {
            continue;
        }
        const LevelInfo* level = m_resources->world.level();
        const auto sound =
            EnemyFeedback::meleeSound(blow.kind, blow.tier,
                                      level != nullptr ? std::span<const LevelEnemy>(level->enemies)
                                                       : std::span<const LevelEnemy>{},
                                      level != nullptr ? level->bossType : -1);
        if (sound.has_value()) {
            m_resources->audio.playNamed(*sound);
        }
        if (blow.ward == EnemyMeleeWard::None) {
            events.hurt(i, blow.damage, sound.has_value() ? HurtKind::QuietBlow : HurtKind::Blow,
                        true, {blow.flags, blow.direction});
            continue;
        }
        if (blow.ward == EnemyMeleeWard::HealthVamp && blow.damage > 0) {
            CharacterSave& save = player.actor.save();
            const s32 cap = mostHealth(experienceLevel(save.experience()));
            if (save.health() < cap) {
                save.progress().health =
                    std::min(cap, save.health() + static_cast<s32>(std::lround(blow.damage)));
            }
        }
        ItemArchive& archive = m_resources->world.powerups();
        if (archive.trees.find("GETGEMORANGE").has_value()) {
            const u32 effect = m_resources->effects.startSet(
                m_resources->device, archive, "GETGEMORANGE", player.actor.followPoint(), {});
            if (effect != 0) {
                m_cueEffects.push_back(effect);
            }
        }
    }
}

void LevelOpponents::applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                      const Events& events) {
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        if (player.actor.player() != blow.player || player.life != PlayerLife::Standing ||
            ((blow.breath || blow.gated) && player.breathGap > 0.0f) ||
            (blow.area && player.effectGap > 0.0f)) {
            continue;
        }
        if (blow.breath) {
            const Vec3 centre = player.actor.position() + Vec3{0, player.actor.height() * 0.5f, 0};
            if (events.blocksBreath && events.blocksBreath(blow.origin, centre)) {
                continue;
            }
        }
        if (blow.breath || blow.gated) {
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
    hearFrom(players);
    const std::vector<EnemyView> views = enemyViews(players);
    std::vector<Obstacle> boxes = m_generators.obstacles();
    boxes.insert(boxes.end(), fixtures.begin(), fixtures.end());
    const LevelInfo* level = m_resources->world.level();
    const f32 missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1.0f;
    const bool timeStopped = PlayerPowerups::timeStopped(players);
    m_generators.update(ticks, m_enemies, views, boxes, timeStopped);
    m_enemies.update(ticks, seconds, views, boxes, &m_enemyMissiles, missileSpeed, timeStopped);
    m_enemyMissiles.update(seconds, &m_resources->world.collision(), views, swarmTargets(), boxes);
    landEnemyMissiles(players, events);
    playEnemyCues();
    // What blows itself up goes up with the bomb's bang; one struck down stops crying out
    // (enemy_dies, AudioKillBySound).
    for (const EnemyBurst& burst : m_enemies.takeBursts()) {
        if (burst.silencesYell) {
            for (const SoundHandle yell : m_yells) {
                m_resources->audio.stop(yell);
            }
            m_yells.clear();
        }
        playAt(kSuicideBombSound, kLoudSound, burst.position);
        explodeSuicide(burst);
    }
    advanceClouds();
    events.settleBlasts();
    m_critters.update(ticks, seconds, views, timeStopped);
    // Boss AI is independent of Stop Time; fired missiles likewise keep moving.
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
    for (const Mat4& placement : m_combatantProjectiles.takeGenerators()) {
        if (level == nullptr || level->enemies.empty() || !m_bosses.view().alive) {
            continue;
        }
        const s32 kind = level->enemies.front().kind;
        if (kind == level->bossType) {
            continue;
        }
        for (const ItemInfo& info : m_resources->world.layout().itemInfos()) {
            if (info.type == ItemInfo::kGenerator && info.name == "BOSSGEN") {
                m_generators.placeBoss(m_resources->device, info, m_resources->world.items(),
                                       m_enemies, kind, placement, &m_resources->world.collision());
                break;
            }
        }
    }
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
        m_bossMeter.update(ticks, m_bosses.healthMeters(), m_bosses.present() && boss.alive,
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
    finishSummons(views);
    for (const CombatBlow& blow : m_critters.takeBlows()) {
        applyCritterBlow(blow, players, events);
    }
    awardCritterLosses(players, events);
    events.levels();
    for (const EnemyBlow& blow : m_enemies.takeBlows()) {
        applyEnemyBlow(blow, players, events);
    }
    updateDeaths(players, events);
    awardEnemyLosses(events);
}

void LevelOpponents::settleRewards(std::span<const PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    hearFrom(players);
    awardBossLosses(players, events);
    awardCritterLosses(players, events);
    awardEnemyLosses(events);
    events.levels();
}

void LevelOpponents::awardEnemyLosses(const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const EnemyFeedback& feedback : m_enemies.takeFeedback()) {
        if (const auto* level = m_resources->world.level()) {
            playAt(feedback.sound(level->enemies, level->bossType), kLoudSound, feedback.position);
        }
        const std::string_view tree = feedback.effect();
        ItemArchive* archive = m_enemies.archive(feedback.kind);
        if (archive == nullptr || !archive->trees.find(tree).has_value()) {
            archive = &m_resources->weapons;
        }
        if (!tree.empty() && archive->trees.find(tree).has_value()) {
            EffectTrees::Setting setting;
            setting.scale = feedback.effectScale();
            setting.yaw = feedback.yaw;
            // The common impact rows set node alpha to 96; TREEHIT/TREEDIE
            // leave the archive's own opacity unchanged.
            if (tree != "TREEHIT" && tree != "TREEDIE") {
                setting.tint.a = 96;
            }
            const u32 effect = m_resources->effects.startSet(m_resources->device, *archive, tree,
                                                             feedback.position, setting);
            if (effect != 0) {
                m_cueEffects.push_back(effect);
            }
        }
    }
    for (const EnemyLoss& loss : m_enemies.takeLosses()) {
        events.award(loss.player, loss.experience, loss.killed);
    }
    for (const s32 player : m_destroyedGenerators) {
        events.award(player, 0, true);
    }
    m_destroyedGenerators.clear();
}

/** A hit on one of the swarm, from a player or the world. */
void LevelOpponents::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                                 std::span<const PlayerRuntime> players, bool close,
                                 std::optional<Vec3> where) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    hit.close = close;
    hit.where = where;
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
            hit.antiDeath = (PowerupEffects::of(actor.save().progress().inventory).armor &
                             DeathRules::kProtection) != 0;
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

f32 LevelOpponents::generatorPowerScale(s32 level, f32 placeLevel) {
    constexpr f32 kUnder = 0.01f; ///< softer a level under the place's
    constexpr f32 kOver = 0.1f;   ///< and harder a level over it
    if (placeLevel <= 0.0f) {
        return 1.0f;
    }
    const f32 gap = static_cast<f32>(level) - placeLevel;
    return gap < 0.0f ? 1.0f + kUnder * gap : 1.0f + kOver * gap;
}

/** A hit on a generator: as it crumbles a state its kind's hit or death effect plays over
 * it to the realm's own sound (`S_GENDAMG`, `S_GENKILLG`), and, gone, its brood is freed of
 * it. A player's hit is scaled by their level against the place's, never under one
 * (fn_8005C1DC's generator ramp). */
void LevelOpponents::strikeGenerator(s32 id, f32 power, s32 byPlayer,
                                     std::span<const PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    const LevelInfo* level = m_resources->world.level();
    if (byPlayer >= 0 && level != nullptr) {
        for (const PlayerRuntime& runtime : players) {
            if (runtime.actor.player() == byPlayer) {
                power *= generatorPowerScale(experienceLevel(runtime.actor.save().experience()),
                                             level->tuning.playerLevel);
                power = std::max(power, 1.0f);
            }
        }
    }
    const auto event = m_generators.strike(id, power, byPlayer);
    if (!event.has_value()) {
        return;
    }
    ItemArchive* archive =
        event->kind < 0 ? &m_resources->world.items() : m_enemies.archive(event->kind);
    if (event->stateChanged && archive != nullptr) {
        const std::string_view tree = event->destroyed ? "GENDIE" : "GENHIT";
        if (archive->trees.find(tree).has_value()) {
            const u32 effect = m_resources->effects.startSet(m_resources->device, *archive, tree,
                                                             event->position, {});
            m_resources->effects.placeAt(effect, event->placement);
            if (effect != 0) {
                m_cueEffects.push_back(effect);
            }
        }
    }
    const std::string& levelName = m_resources->world.ref().name;
    if (!event->destroyed || level == nullptr || level->bossType < 0) {
        const std::string suffix =
            m_resources->world.ref().realmId == 10 && event->kind == 24
                ? "WAR"
                : std::string(1, levelName.empty() ? 'G' : levelName.front());
        m_resources->audio.playNamed(
            std::format("{}{}", event->destroyed ? "S_GENKILL" : "S_GENDAM", suffix));
    }
    if (event->destroyed) {
        m_enemies.generatorGone(id);
        m_destroyedGenerators.push_back(byPlayer);
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
        if (ofBoss && m_bosses.view().kind == 42 && m_bosses.position() != nullptr) {
            m_resources->world.bossArenaCue(*m_bosses.position());
        }
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

void LevelOpponents::finishSummons(std::span<const EnemyView> players) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const Mat4& placement : m_combatantProjectiles.takeSummons()) {
        if (!m_bosses.view().alive || m_bosses.view().kind != 44) {
            continue;
        }
        // BossGenerateEnemy: two tier-three Garm minions, three units out,
        // yawed by +/- pi/4 from the summoning effect's forward direction.
        constexpr s32 kGarmMinion = 27;
        constexpr f32 kOffset = 3;
        constexpr f32 kYaw = 0.7853981633974483f;
        if (m_enemies.loadKind(kGarmMinion)) {
            const Vec3 origin{placement[3]};
            const Vec3 forward = glm::normalize(Vec3{placement[2]});
            for (const f32 yaw : {kYaw, -kYaw}) {
                EnemySpawn spawn;
                spawn.kind = kGarmMinion;
                spawn.tier = 3;
                spawn.direction = Vec3{glm::rotate(Mat4{1}, yaw, Vec3{0, 1, 0}) * Vec4{forward, 0}};
                spawn.position = origin + kOffset * spawn.direction;
                spawn.clearance = 1;
                spawn.placed = true;
                m_enemies.spawn(spawn, players);
            }
        }
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
    // CritterDelInst calls BossDeath after the animation/hold, independently
    // of the hit and kill experience awarded below.
    if (const auto defeated = m_bosses.takeDefeat(); defeated.has_value()) {
        events.fallen(*defeated);
    }
    for (const CombatLoss& loss : m_bosses.takeLosses()) {
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
            if (whole > 0 || loss.killed) {
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
            if (whole > 0 || loss.killed) {
                owed -= static_cast<f32>(whole);
                events.award(player, whole, loss.killed);
            }
        }
    }
}

} // namespace gdl::game
