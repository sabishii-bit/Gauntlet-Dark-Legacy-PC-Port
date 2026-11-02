#include "game/enemies/LevelOpponents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>
#include <limits>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/characters/Progression.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/CritterCueEmission.h"
#include "game/effects/DynamicLights.h"
#include "game/enemies/EnemyFeedbackEmission.h"
#include "game/enemies/EnemyMissileEmission.h"
#include "game/enemies/OpponentPlacements.h"
#include "game/enemies/OpponentSimulation.h"
#include "game/hud/HelpMessages.h"
#include "game/level/SimulationSeed.h"
#include "game/party/BodyCollision.h"
#include "game/party/PartyDamage.h"
#include "game/party/PlayerPowerups.h"
#include "game/players/EnemyShrink.h"
#include "game/players/ItemPickup.h"
#include "game/players/PlayerAnimator.h"
#include "game/players/PowerupEffects.h"
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
// either way the thrower kind's own fragments.
constexpr std::string_view kSuicideBlast = "EXPLOSION";
constexpr std::string_view kSuicideRing = "EXPRING";
constexpr std::string_view kSuicideFragments = "SUICIDEEXP";
constexpr std::string_view kDeathBurst = "DEATHFX1"; ///< the brood's burst, held where it lay
constexpr std::string_view kDeathShot = "DEATHFX2";  ///< and the shot that flies on from it
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
constexpr f32 kPlacementWakeReach = 10.0f; ///< a trigger wakes the placed enemy nearest within

bool cloudRealm(const std::string& level) {
    return !level.empty() && (level.front() == 'G' || level.front() == 'K');
}

} // namespace

void LevelOpponents::appendFollowupValues(NativeStateDigest& digest, const Simulation& values) {
    const auto& [clouds, deathShots, carried, bags, bagSeed, barrelRams, rockHits, hitStreak,
                 lessons, critterEffects, moveEffects, cueEffects, deathEffects, deathContact,
                 deathSound, yells, yellClocks, hearers, shrink, critterExperienceOwed,
                 generatorRewards, unseen, borrowedRoster] = values;
    const auto rows = [&digest]<usize N>(const auto& list) {
        digest.wide(list.size());
        for (const auto& row : list) {
            digest.fields<N>(row);
        }
    };
    const auto cue = [&digest](const SoundCueToken& token) {
        const auto& [event] = token;
        const auto& [epoch, tick, emitter, kind, occurrence] = event;
        digest.values(epoch, tick);
        digest.fields<4>(emitter);
        digest.values(kind, occurrence);
    };
    rows.template operator()<3>(clouds);
    rows.template operator()<5>(deathShots);
    digest.value(carried);
    rows.template operator()<9>(bags);
    digest.value(bagSeed);
    rows.template operator()<2>(barrelRams);
    digest.values(rockHits, hitStreak, lessons);
    rows.template operator()<9>(critterEffects);
    rows.template operator()<3>(moveEffects);
    digest.values(cueEffects, deathEffects, deathContact);
    cue(deathSound);
    digest.wide(yells.size());
    for (const auto& yell : yells) {
        const auto& [emitter, instance, start, timing, volume, position] = yell;
        digest.fields<4>(emitter);
        digest.value(instance);
        cue(start);
        digest.fields<2>(timing);
        digest.values(volume, position);
    }
    rows.template operator()<2>(yellClocks);
    digest.values(hearers, shrink, critterExperienceOwed);
    rows.template operator()<3>(generatorRewards);
    digest.value(unseen);
    rows.template operator()<4>(borrowedRoster);
}

bool LevelOpponents::appendNativeFollowupDigest(NativeStateDigest& digest) const {
    appendFollowupValues(digest, m_sim);
    return digest.good();
}

bool LevelOpponents::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("LevelOpponents.native.3");
    return m_generators.appendNativeDigest(digest) && m_enemyMissiles.appendNativeDigest(digest) &&
           m_combatantProjectiles.appendNativeDigest(digest) &&
           m_enemies.appendNativeDigest(digest) && m_placements.appendNativeDigest(digest) &&
           m_statues.appendNativeDigest(digest) && m_critters.appendNativeDigest(digest) &&
           m_bosses.appendNativeDigest(digest) && m_bossMeter.appendNativeDigest(digest) &&
           appendNativeFollowupDigest(digest);
}

bool LevelOpponents::appendLocalDigest(NativeStateDigest& digest,
                                       const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("LevelOpponents.native.3");
    return m_generators.appendLocalDigest(digest, saved.m_generators) &&
           m_enemyMissiles.appendLocalDigest(digest, saved.m_missiles) &&
           m_combatantProjectiles.appendLocalDigest(digest, saved.m_projectiles) &&
           m_enemies.appendLocalDigest(digest, saved.m_enemies) &&
           m_placements.appendLocalDigest(digest, saved.m_placements) &&
           m_statues.appendLocalDigest(digest, saved.m_statues) &&
           m_critters.appendLocalDigest(digest, saved.m_critters) &&
           m_bosses.appendLocalDigest(digest, saved.m_bosses) &&
           m_bossMeter.appendLocalDigest(digest, saved.m_meter) &&
           appendLocalFollowupDigest(digest, saved);
}

bool LevelOpponents::appendLocalFollowupDigest(NativeStateDigest& digest,
                                               const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    appendFollowupValues(digest, saved.m_values);
    return digest.good();
}

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
    m_sim.hearers.clear();
    for (const PlayerRuntime& runtime : players) {
        if (runtime.life == PlayerLife::Standing) {
            m_sim.hearers.push_back(runtime.actor.position());
        }
    }
}

SoundHandle LevelOpponents::playAt(std::string_view name, f32 level, const Vec3& at) {
    if (!m_resources.has_value() || name.empty()) {
        return kNoSound;
    }
    const f32 heard = attenuation(at, m_sim.hearers);
    if (heard <= 0.0f) {
        return kNoSound;
    }
    // Distance was resolved against the nearest standing player above; still pan from
    // the camera, as sndFxPlay3DAtten does. Zero avoids attenuating a second time.
    return m_resources->audio.playAt(name, at, 0.0f, level / kFullLevel * heard);
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
        if (auto* imported = m_resources->world.imported();
            imported && imported->playEnemyCue(cue)) {
            continue;
        }
        auto& audio = m_resources->audio;
        if (audio.recordingSounds() &&
            (cue.enemy < 0 || cue.enemy >= Enemies::kMost || cue.instance == 0)) {
            audio.recorder()->unsupported("enemy cue without its source lifetime");
        }
        const FrameEventEmitter emitter{20, static_cast<u64>(std::max(cue.enemy, 0)), cue.instance};
        auto scope = audio.soundScope(emitter, static_cast<u32>(cue.kind) + 1);
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
            if (audio.recordingSounds()) {
                const f32 volume =
                    kLoudSound / kFullLevel * attenuation(cue.position, m_sim.hearers);
                if (volume > 0) {
                    const auto start = audio.beginNamedCue(kSuicideYellSound);
                    if (start) {
                        auto& clock = m_sim.yellClocks[static_cast<usize>(cue.enemy)];
                        if (clock.source != cue.instance) {
                            clock = {cue.instance, 1};
                        }
                        if (clock.next == 0) {
                            audio.recorder()->unsupported("exhausted bomber cry lifetimes");
                        }
                        m_sim.yells.push_back({emitter, clock.next++, start,
                                               audio.namedTiming(kSuicideYellSound), volume,
                                               cue.position});
                    }
                }
            } else {
                if (const SoundHandle yell = playAt(kSuicideYellSound, kLoudSound, cue.position);
                    yell != kNoSound) {
                    m_yells.push_back(yell);
                }
            }
            break;
        }
    }
}

/** The swarm and the great ones as a missile sent back or a blast reaches them, by id: the
 * ordinary enemies by their slots, the great ones and the boss past those. */
std::vector<MissileTarget> LevelOpponents::swarmTargets() const {
    return OpponentSimulation::targets(m_enemies, m_critters.targets(true), m_bosses.targets());
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
        (void)dispatchEnemyMissileHit(hit, players, events);
    }
}

bool LevelOpponents::dispatchEnemyMissileHit(const EnemyMissileHit& hit,
                                             std::span<PlayerRuntime> players,
                                             const Events& events) {
    if (!m_resources) {
        return false;
    }
    if (m_resources->audio.recordingSounds() && hit.instance == 0) {
        m_resources->audio.recorder()->unsupported("enemy missile hit without its source lifetime");
    }
    // The shooter or flight vector may already have been recycled. A piercing
    // shot's distinct victims also keep separate identities if contacts reorder.
    const u64 target = hit.target >= 0 ? static_cast<u64>(hit.target) + 1 : 0;
    const u8 seat = hit.player >= 0 && hit.player < 4 ? static_cast<u8>(hit.player)
                                                      : FrameEventEmitter::kNoSeat;
    const u32 cue = hit.ricochet ? 1 : hit.fromBurst ? 2 : hit.liquid ? 3 : 4;
    auto soundScope = m_resources->audio.soundScope({22, target, hit.instance, seat}, cue);
    if (hit.worldObject >= 0) {
        m_resources->world.explodeObject(hit.worldObject, hit.position);
    }
    if (!hit.ricochet && hit.target >= 0) {
        strikeSwarm(hit.target, hit.damage, hit.flags, hit.direction, players);
    }
    const auto emission = EnemyMissileEmission::resolve(hit, m_resources->weapons);
    u32 effect = 0;
    if (!hit.ricochet && !hit.effect().empty() && m_resources->weapons.loaded()) {
        effect = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                               hit.effect(), hit.position, emission.setting);
    }
    if (effect != 0) {
        m_sim.cueEffects.push_back(effect);
    }
    auto* imported = m_resources->world.imported();
    const bool nativeBomb = emission.sound == "S_LOBBER_BOMB" && imported != nullptr &&
                            imported->playOpponentSound(emission.sound, hit.position);
    if (!nativeBomb) {
        playAt(emission.sound, emission.level, hit.position);
    }
    if (hit.ricochet) {
        return true;
    }
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
    return true;
}

/** A suicide going up (SuicideExplosion): a burning blast of six with its ring and red light,
 * or in the town and the sky a poison cloud of seven and a half that hangs on, and the
 * kind's fragments either way, fading out. The blast's harm grows out from it as it fades. */
void LevelOpponents::explodeSuicide(const EnemyBurst& burst) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyBlast blast;
    blast.position = burst.position;
    blast.damage = burst.damage;
    const auto* imported = m_resources->world.imported();
    const bool cloud =
        imported ? imported->suicideUsesGas().value_or(cloudRealm(m_resources->world.ref().name))
                 : cloudRealm(m_resources->world.ref().name);
    ItemArchive& weapons = m_resources->weapons;
    const auto start = [&](std::string_view tree, const Vec3& at,
                           const EffectTrees::Setting& setting) -> u32 {
        if (!weapons.loaded() || !weapons.trees.find(tree).has_value()) {
            return 0;
        }
        const u32 id =
            m_resources->effects.startSet(m_resources->device, weapons, tree, at, setting);
        if (id != 0) {
            m_sim.cueEffects.push_back(id);
        }
        return id;
    };
    if (cloud) {
        EffectTrees::Setting setting;
        setting.stretch = kCloudStretch;
        const Vec3 at = burst.position - Vec3{0.0f, kCloudDrop, 0.0f};
        const u32 first = start(kSuicideCloud[0], at, setting);
        m_sim.clouds.push_back(Cloud{first, 1, at});
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
        // Intentional port change: unlike retail's 0x29 collision mask, fiery suicide
        // blasts also reach destructible scenery and loose items.
        blast.stages = {m_resources->effects.remaining(fire).value_or(kBlastSeconds)};
    }
    // The fragments are the thrower kind's own, in the first archive that has them.
    ItemArchive* fragments = m_enemies.archive(burst.kind);
    if (fragments == nullptr || !fragments->trees.find(kSuicideFragments).has_value()) {
        fragments = nullptr;
        for (s32 kind = 0; kind < kSwarmKindCount && fragments == nullptr; ++kind) {
            ItemArchive* archive = m_enemies.kindLoaded(kind) ? m_enemies.archive(kind) : nullptr;
            if (archive != nullptr && archive->trees.find(kSuicideFragments).has_value()) {
                fragments = archive;
            }
        }
    }
    if (fragments != nullptr) {
        EffectTrees::Setting setting;
        setting.fadeSeconds = kSmokeFade;
        const u32 id = m_resources->effects.startSet(m_resources->device, *fragments,
                                                     kSuicideFragments, burst.position, setting);
        if (id != 0) {
            m_sim.cueEffects.push_back(id);
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
    for (Cloud& cloud : m_sim.clouds) {
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
            m_sim.cueEffects.push_back(cloud.effect);
        }
        ++cloud.stage;
    }
    std::erase_if(m_sim.clouds, [](const Cloud& cloud) {
        return cloud.effect == 0 && cloud.stage >= kSuicideCloud.size();
    });
}

/** The garm brood's death shot (StartEnemyDeathFX): its archive's DEATHFX1 bursts where the
 * corpse lay and is held there for its sequence, striking what stands in it, then DEATHFX2
 * flies on from it at the player for three seconds; the harm rides an unseen missile, held as
 * long as the burst and then flying with the shot, through players, walls and items alike. */
void LevelOpponents::fireDeathShot(const EnemyDeathShot& shot) {
    if (!m_resources.has_value()) {
        return;
    }
    EnemyMissileKind kind = EnemyMissileKind::deathShot();
    DeathShot pending;
    pending.kind = shot.kind;
    pending.position = shot.position;
    pending.velocity = shot.direction * kind.speed;
    ItemArchive* archive = m_enemies.archive(shot.kind);
    if (archive != nullptr && archive->trees.find(kDeathBurst).has_value()) {
        EffectTrees::Setting setting;
        setting.yaw = portable::atan2(shot.direction.x, shot.direction.z);
        pending.burst = m_resources->effects.startSet(m_resources->device, *archive, kDeathBurst,
                                                      shot.position, setting);
        if (pending.burst != 0) {
            m_sim.cueEffects.push_back(pending.burst);
            kind.held = m_resources->effects.remaining(pending.burst).value_or(0.0f);
        }
    }
    m_enemyMissiles.launch(kind, shot.position, shot.position + shot.direction, 1.0f, nullptr, -1);
    m_sim.deathShots.push_back(pending);
}

/** Once a death shot's burst has played out, its shot flies on from where it was, holding its
 * last pose for its three seconds (SfxSetMorph, ChangeEffect: oneshot). */
void LevelOpponents::advanceDeathShots() {
    if (!m_resources.has_value()) {
        return;
    }
    for (DeathShot& shot : m_sim.deathShots) {
        if (shot.burst != 0 && m_resources->effects.playing(shot.burst)) {
            continue;
        }
        shot.flying = true;
        ItemArchive* archive = m_enemies.archive(shot.kind);
        if (archive == nullptr || !archive->trees.find(kDeathShot).has_value()) {
            continue;
        }
        EffectTrees::Setting setting;
        setting.velocity = shot.velocity;
        // StartEnemyDeathFX turns the burst toward its launch velocity; the
        // morph retains that orientation instead of returning to world +z.
        setting.yaw = portable::atan2(shot.velocity.x, shot.velocity.z);
        setting.seconds = EnemyMissiles::kLife;
        setting.loop = false;
        const u32 id = m_resources->effects.startSet(m_resources->device, *archive, kDeathShot,
                                                     shot.position, setting);
        if (id != 0) {
            m_sim.cueEffects.push_back(id);
        }
    }
    std::erase_if(m_sim.deathShots, [](const DeathShot& shot) { return shot.flying; });
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
    const auto generators = m_generators.obstacles();
    // A slide along a creature must still respect the level walls. If wall resolution
    // would move back inside a creature, retain the already-safe pre-step position.
    return BodyCollision::resolveMovement(player, from, to, bodies, generators,
                                          m_resources ? &m_resources->world.collision() : nullptr);
}

bool LevelOpponents::saveLocal(LocalSnapshot& saved) const {
    saved.m_owner.reset();
    saved.m_epoch = 0;
    if (!m_resources || !m_enemies.saveLocal(saved.m_enemies) ||
        !m_critters.saveLocal(saved.m_critters) || !m_bosses.saveLocal(saved.m_bosses)) {
        return false;
    }
    saved.m_values = m_sim;
    m_placements.saveLocal(saved.m_placements);
    m_statues.saveLocal(saved.m_statues);
    m_generators.saveLocal(saved.m_generators);
    m_bossMeter.saveLocal(saved.m_meter);
    m_enemyMissiles.saveLocal(saved.m_missiles);
    m_combatantProjectiles.saveLocal(saved.m_projectiles);
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
    return true;
}
bool LevelOpponents::canRestoreLocal(const LocalSnapshot& saved) const {
    return m_resources && saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch &&
           m_enemies.canRestoreLocal(saved.m_enemies) &&
           m_critters.canRestoreLocal(saved.m_critters) &&
           m_bosses.canRestoreLocal(saved.m_bosses) &&
           m_placements.canRestoreLocal(saved.m_placements) &&
           m_statues.canRestoreLocal(saved.m_statues) &&
           m_generators.canRestoreLocal(saved.m_generators) &&
           m_bossMeter.canRestoreLocal(saved.m_meter) &&
           m_enemyMissiles.canRestoreLocal(saved.m_missiles) &&
           m_combatantProjectiles.canRestoreLocal(saved.m_projectiles);
}
bool LevelOpponents::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    // All owners have been checked before mutation. Native copies never dispatch
    // events or stop/restart audio already submitted to the presentation mixer.
    m_enemies.restoreLocal(saved.m_enemies);
    m_critters.restoreLocal(saved.m_critters);
    m_bosses.restoreLocal(saved.m_bosses);
    m_placements.restoreLocal(saved.m_placements);
    m_statues.restoreLocal(saved.m_statues);
    m_generators.restoreLocal(saved.m_generators);
    m_bossMeter.restoreLocal(saved.m_meter);
    m_enemyMissiles.restoreLocal(saved.m_missiles);
    m_combatantProjectiles.restoreLocal(saved.m_projectiles);
    m_sim = saved.m_values;
    refreshBorrowedRoster();
    ++m_bindingGeneration; // any prepared diagnostic transaction is now stale
    return true;
}

bool LevelOpponents::equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const {
    return canRestoreLocal(a) && canRestoreLocal(b) && a.m_values == b.m_values &&
           m_placements.equalLocal(a.m_placements, b.m_placements) &&
           m_statues.equalLocal(a.m_statues, b.m_statues) &&
           m_enemies.equalLocal(a.m_enemies, b.m_enemies) &&
           m_generators.equalLocal(a.m_generators, b.m_generators) &&
           m_critters.equalLocal(a.m_critters, b.m_critters) &&
           m_bosses.equalLocal(a.m_bosses, b.m_bosses) &&
           m_bossMeter.equalLocal(a.m_meter, b.m_meter) &&
           m_enemyMissiles.equalLocal(a.m_missiles, b.m_missiles) &&
           m_combatantProjectiles.equalLocal(a.m_projectiles, b.m_projectiles);
}

void LevelOpponents::close() {
    m_importedActors.clear();
    m_importedGenerators.clear();
    m_importedGeneratorSounds.clear();
    ++m_localEpoch;
    if (m_bindingGeneration != 0) {
        ++m_bindingGeneration; // zero permanently refuses preparations after exhaustion
    }
    m_placements.clear();
    m_statues.clear();
    m_sim.borrowedRoster.clear();
    m_sim.unseen = {};
    m_levelNow.reset();
    clearDeaths();
    if (m_resources.has_value()) {
        m_combatantProjectiles.clear(m_resources->effects);
        for (const Bag& bag : m_sim.bags) {
            m_resources->effects.stop(bag.effect);
        }
        for (const CritterEffect& cue : m_sim.critterEffects) {
            m_resources->effects.stop(cue.effect);
        }
        for (const u32 effect : m_sim.cueEffects) {
            m_resources->effects.stop(effect);
        }
    }
    m_sim.critterEffects.clear();
    m_sim.bags.clear();
    m_sim.moveEffects.clear();
    m_sim.cueEffects.clear();
    m_generators.clear();
    m_sim.generatorRewards.clear();
    m_enemyMissiles.clear();
    m_sim.rockHits.clear();
    m_sim.clouds.clear();
    m_sim.deathShots.clear();
    m_yells.clear();
    m_sim.yells.clear();
    m_sim.yellClocks.fill({});
    m_sim.hearers.clear();
    m_sim.lessons.clear();
    m_sim.hitStreak.fill(0); // load_player resets the new party's hit_streak
    m_critters.close();
    m_bossMeter.clear();
    m_bosses.close();
    m_importedBossTextures.reset();
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
    m_sim.shrink = EnemyShrink::kWhole;
    RenderDevice& device = resources.device;
    LevelWorld& world = resources.world;
    const LevelInfo* level = world.level();
    const auto* importedBoss = world.imported() ? world.imported()->importedBoss() : nullptr;
    const f32 gain = resources.difficultyGain;
    EnemyScales scales;
    scales.players = static_cast<s32>(std::max<usize>(players.size(), 1));
    scales.bossEncounter = importedBoss != nullptr;
    GeneratorScales breeding;
    s32 most = Enemies::kMost;
    if (level != nullptr) {
        scales.health = level->tuning.enemyHealth;
        scales.speed = level->tuning.enemySpeedScale(gain);
        scales.sight = level->tuning.enemySightScale(gain);
        scales.damage = level->tuning.enemyDamage;
        scales.missileRate = level->tuning.enemyMissileWaitScale(gain);
        scales.missileAim = level->tuning.enemyMissileAim;
        scales.playerLevel = level->tuning.playerLevel;
        scales.bossEncounter = level->bossType >= 0;
        breeding.health = level->tuning.generatorHealth;
        breeding.rate = level->tuning.generatorRateScale(gain);
        breeding.most = level->tuning.generatorMostScale(gain);
        most = level->maxEnemies;
    }
    const u32 seed = simulationSeed(world.ref().name);
    m_enemies.open(device, resources.root, &world.collision(), most, scales, seed);
    if (const auto* imported = world.imported(); imported && imported->hasEnemyAssets()) {
        m_enemies.setArchiveLoader(
            [imported](s32 kind) { return imported->loadEnemyAssets(kind); });
    }
    m_enemies.setContactGridBounds(world.layout().minBounds(), world.layout().maxBounds());
    m_statues.setContactGridBounds(world.layout().minBounds(), world.layout().maxBounds());
    m_enemies.setHazards(&world.hazards());
    m_enemies.setLookouts(LookoutRoute::of(world.layout().locators()));
    const std::string& levelName = world.ref().name;
    const std::span<const LevelEnemy> roster = level != nullptr
                                                   ? std::span<const LevelEnemy>(level->enemies)
                                                   : std::span<const LevelEnemy>{};
    // init_next_level's GAR_%s.WAD comes from ENMY +0x10, not its audio name.
    const auto gargoyle = std::ranges::find(roster, kGargoyleEnemyKind, &LevelEnemy::kind);
    const std::string_view gargoyleForm =
        gargoyle != roster.end() ? std::string_view(gargoyle->form) : std::string_view{};
    const std::array creatureTextures{&world.textures()};
    char creatureRealm = levelName.empty() ? 'G' : levelName.front();
    if (const auto* imported = world.imported();
        imported != nullptr && !imported->importedActors().empty()) {
        creatureRealm = imported->importedActors().front().costumeRealm;
    }
    m_critters.open(device, resources.root, &world.collision(), scales, creatureRealm,
                    creatureTextures, gargoyleForm);
    m_critters.setHazards(&world.hazards());
    m_critters.setLookouts(LookoutRoute::of(world.layout().locators()));
    m_bosses.open(device, resources.root, &world.collision(), scales,
                  levelName.empty() ? 'G' : levelName.front(), creatureTextures);
    // The white skin a hard hit flashes over a great one, the swarm's and the party's own.
    if (const auto white = world.powerups().textures.find(kHitFlashSkin); white.has_value()) {
        try {
            const Texture* flash = &world.powerups().textures.texture(device, *white);
            m_critters.setHitFlash(flash);
            m_bosses.setHitFlash(flash);
        } catch (const std::exception& e) {
            log::warn("Critter hit skin {}: {}", kHitFlashSkin, e.what());
        }
    }
    m_sim.critterExperienceOwed.fill(0.0f);
    const auto playerCount = static_cast<s32>(std::ranges::count_if(
        players, [](const PlayerRuntime& player) { return !player.departed; }));
    const std::array generatorTextures{&world.textures(), &world.items().textures,
                                       &world.realmItems().textures};
    m_generators.bind(device, world.layout(), m_enemies, &world.collision(), breeding, playerCount,
                      roster, static_cast<s32>(world.ref().realmId), &world.items(),
                      generatorTextures);
    if (const auto* imported = world.imported()) {
        for (const auto& generator : imported->importedGenerators()) {
            const auto id = m_generators.spawnImported(device, m_enemies, generator);
            m_importedGenerators.emplace_back(generator.nativeIndex, id.value_or(-1));
            if (!id) {
                log::warn("Imported generator {}: failed to stand kind {}", generator.nativeIndex,
                          generator.kind);
            }
        }
        openImportedActors();
        bindImportedSounds();
    }
    if (importedBoss) {
        openImportedBoss(scales);
        const PlayerRuntime* bearer = nullptr;
        for (const auto& runtime : players) {
            if (!runtime.departed && runtime.life == PlayerLife::Standing &&
                world.imported()->hasBossLegend(runtime.actor.save()) &&
                (!bearer || runtime.actor.player() < bearer->actor.player())) {
                bearer = &runtime;
            }
        }
        if (bearer) {
            m_bosses.bringLegend(bearer->actor.player());
        }
    }
    // The level's boss, at its boss mark.
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (const WorldLocator* mark = world.layout().findLocator(LocatorKind::Boss);
            mark != nullptr) {
            m_bosses.spawn(level->bossType, mark->position, mark->rotation.y);
            ItemArchive* archive = m_bosses.archive();
            m_bossMeter.bind(m_bosses.healthMeters(),
                             archive != nullptr ? &archive->textures : nullptr);
            // The first eligible controller carries the item, independently of party
            // storage order. Fallen players waiting in the tower cannot perform the rite.
            const PlayerRuntime* bearer = nullptr;
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (!runtime.departed && runtime.life == PlayerLife::Standing &&
                    actor.save().progress().relics.hasLegend(m_bosses.legendRealm()) &&
                    (bearer == nullptr || actor.player() < bearer->actor.player())) {
                    bearer = &runtime;
                }
            }
            if (bearer != nullptr && m_bosses.bringLegend(bearer->actor.player())) {
                log::info("Level {}: player {} brings the {} its legend item", levelName,
                          bearer->actor.player() + 1, bossNameOf(level->bossType));
            }
        }
    }
    m_placements.bind(OpponentPlacements::load(device, world.layout(), roster, playerCount,
                                               m_enemies, m_critters, m_statues, &world.collision(),
                                               [&](const Vec3& position, f32 reach, f32 rise) {
                                                   return world.claimItem(position, reach, rise);
                                               }),
                      &world.collision());
    if (!resources.standOnSight) {
        standPlacements(std::nullopt);
    }
}

std::vector<ImportedGeneratorState> LevelOpponents::importedGeneratorStates() const {
    std::vector<ImportedGeneratorState> result;
    result.reserve(m_importedGenerators.size());
    for (const auto& [nativeIndex, id] : m_importedGenerators) {
        ImportedGeneratorState state;
        state.nativeIndex = nativeIndex;
        state.pool = id;
        if (id >= 0 && static_cast<usize>(id) < m_generators.count()) {
            state.state = m_generators.stateOf(id);
            state.position = m_generators.positionOf(id);
            state.standing = m_generators.standing(id);
        }
        result.push_back(state);
    }
    return result;
}

void LevelOpponents::watch(const ViewVolume& view, const Vec3& attention, bool standPlaced) {
    watch(ViewSet::of(view, attention), standPlaced);
}

void LevelOpponents::watch(const ViewSet& views, bool standPlaced) {
    m_enemies.setViews(views);
    // main.c selects the distance-limited normal camera unless a boss is present.
    const LevelInfo* level = m_resources ? m_resources->world.level() : nullptr;
    m_generators.setViews(views, level == nullptr || level->bossType < 0);
    if (standPlaced) {
        standPlacements(views);
    } else {
        m_placements.observeSet(views);
    }
}

bool LevelOpponents::hasUncapturedState() const {
    return m_importedActors.count() != 0 || m_critters.count() != 0 || m_bosses.present() ||
           m_statues.count() != 0 || m_combatantProjectiles.count() != 0 || !m_sim.bags.empty() ||
           !m_sim.clouds.empty() || !m_sim.deathShots.empty() ||
           m_sim.shrink != EnemyShrink::kWhole;
}

bool LevelOpponents::inView(const Vec3& at, f32 radius) const {
    return m_placements.inView(at, radius);
}

void LevelOpponents::wakeStatueNear(const Vec3& spot) {
    // fn_80062FF0 finds the nearest placed enemy of any kind within ten (the statues, and
    // the swarm's placements yet to stand); only a statue is changed by the waking.
    const auto statue = m_statues.nearestAsleep(spot);
    if (!statue.has_value() || statue->second >= kPlacementWakeReach) {
        return;
    }
    for (const usize index : m_placements.pending()) {
        const auto& pending = m_placements.roster()[index];
        if (glm::distance(pending.spawn.position, spot) < statue->second) {
            return;
        }
    }
    m_statues.wake(statue->first);
}

/** Each birth of a body whose reach is past two (the medium and large kinds) is marked by
 * the realm's burst for its tier, GENFX1 to GENFX3 (generate_enemy). The level finds them
 * once, in the first loaded kind's archive holding them, kind by kind in order (InitEffects),
 * a tier without its own sharing the first's, and only the kinds up to that one are marked. */
bool LevelOpponents::playBirths(const BirthPublisher& publish) {
    const auto births = m_enemies.takeBirths();
    if (births.empty()) {
        return true;
    }
    std::array<ItemArchive*, kSwarmKindCount> archives{};
    for (s32 kind = 0; kind < kSwarmKindCount; ++kind) {
        archives[static_cast<usize>(kind)] = m_enemies.archive(kind);
    }
    for (const EnemyBirth& birth : births) {
        const auto emission = EnemyBirthEmission::resolve(birth, archives);
        if (emission.archive == nullptr) {
            continue; // The native provider/size rule requests no artwork.
        }
        const auto effect = publish ? publish(emission)
                                    : std::optional{m_resources->effects.startSet(
                                          m_resources->device, *emission.archive, emission.tree,
                                          emission.position, {})};
        if (!effect) {
            return false; // The complete combat caller owns the queue/effect undo.
        }
        if (*effect != 0) {
            m_sim.cueEffects.push_back(*effect);
        }
    }
    return true;
}

void LevelOpponents::updateStatues(s32 ticks, f32 seconds, std::span<PlayerRuntime> players) {
    if (m_statues.count() == 0) {
        return;
    }
    m_statues.syncFloors();
    // The placement's sight wakes it on approach; only its solid body stops a player.
    for (PlayerRuntime& player : players) {
        if (player.life == PlayerLife::Standing) {
            PlayerActor& actor = player.actor;
            const Vec3 pushed = m_statues.touch(actor.position(), actor.radius(), actor.height());
            actor.slide(pushed - actor.position(), &m_resources->world.collision());
        }
    }
    for (const Vec3& spot : m_resources->world.takeTriggerWakes()) {
        wakeStatueNear(spot);
    }
    m_statues.update(ticks, seconds,
                     [this](const Vec3& at, f32 radius) { return inView(at, radius); });
    for (const CritterStatues::Placement& risen : m_statues.takeRisen()) {
        if (risen.enemy) {
            m_enemies.spawn(*risen.enemy, {}, m_generators.obstacles());
            continue;
        }
        const Mat4 stood = itemPlacement(risen.instance.position, risen.instance.rotation);
        Placement placement;
        placement.kind = risen.kind == CombatantKind::Golem ? kGolemEnemyKind : kGargoyleEnemyKind;
        placement.facing = portable::atan2(stood[2][0], stood[2][2]);
        placement.viewRadius = risen.viewRadius;
        placement.sight = risen.sight;
        placement.carried = risen.carried;
        placement.spawn.position = risen.instance.position;
        stand(placement);
    }
}

/** Each placed enemy stands once the camera sees its spot, by its size, looking from no more
 * than fifty away (fn_80060114); seen, it is gone from the waiting list whether or not the
 * swarm had room for it. With no view, all stand. */
void LevelOpponents::standPlacements(const std::optional<ViewSet>& views) {
    m_placements.observeSet(views);
    m_placements.activate([this](const Placement& placement) { stand(placement); });
}

void LevelOpponents::stand(const Placement& placement) {
    const Vec3& at = placement.spawn.position;
    const auto carry = [&](std::optional<s32> id) {
        if (id.has_value() && *id >= 0 && static_cast<usize>(*id) < m_sim.carried.size()) {
            m_sim.carried[static_cast<usize>(*id)] = placement.carried;
        }
    };
    switch (placement.kind) {
    case kGolemEnemyKind: carry(m_critters.spawnGolem(at, placement.facing)); return;
    case kGeneralEnemyKind:
        carry(m_critters.spawnGeneral(at, placement.facing, placement.sight));
        return;
    case kGargoyleEnemyKind: carry(m_critters.spawnGargoyle(at, placement.facing)); return;
    default: m_enemies.spawn(placement.spawn, {}, m_generators.obstacles()); return;
    }
}

/** Every standing shrinker worn holds the swarm and the great ones at two thirds again, where
 * no boss is fought; the scale rising back is heard (SetPlayerVars, gamemain.c 1269). */
void LevelOpponents::shrinkOpponents(std::span<const PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    const LevelInfo* level = m_resources->world.level();
    const f32 scale = PlayerPowerups::enemyShrink(
        players, (level != nullptr && level->bossType >= 0) ||
                     (m_resources->world.imported() != nullptr &&
                      m_resources->world.imported()->importedBoss() != nullptr));
    if (EnemyShrink::rises(m_sim.shrink, scale)) {
        m_resources->audio.playNamed(EnemyShrink::kUnshrinkSound);
    }
    m_sim.shrink = scale;
    m_enemies.setShrink(scale);
    m_critters.setShrink(scale);
    m_enemyMissiles.setShrink(scale);
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
        view.decoy = player.mikey.target();
        view.radius = actor.radius();
        view.collisionRadius = actor.reach();
        view.height = actor.height();
        view.collisionHeight = actor.followPoint().y - actor.position().y;
        view.level = experienceLevel(actor.save().experience());
        view.hidden = player.life != PlayerLife::Standing;
        view.damageable = PlayerHealth::canBeDamaged(player);
        view.recentlyHit = player.effectGap > 0 || player.breathGap > 0;
        view.blockableAttack = player.figure != nullptr && PlayerAnimator::isBlockableAttack(
                                                               player.figure->animator().action());
        const auto powerups = PowerupEffects::of(actor.save().progress().inventory);
        view.invisible = powerups.invisible();
        view.antiDeath = (powerups.armor & DeathRules::kProtection) != 0;
        view.heldDeath = player.deathHeld;
        view.reflects = (powerups.armor & kReflectingArmor) != 0;
        if ((powerups.special & powerup::kHealthVamp) != 0) {
            view.meleeWard = EnemyMeleeWard::HealthVamp;
        } else if ((powerups.special & powerup::kHandOfDeath) != 0) {
            view.meleeWard = EnemyMeleeWard::HandOfDeath;
        }
        view.captured = player.capture.held();
        view.it = player.itTicks > 0;
        views.push_back(view);
    }
    return views;
}

std::vector<EnemyView> LevelOpponents::views(std::span<const PlayerRuntime> players) const {
    std::vector<EnemyView> views = enemyViews(players);
    for (EnemyView& view : views) {
        view.invisible = view.invisible || unseen(view.player);
    }
    return views;
}

void LevelOpponents::setUnseen(s32 player, bool unseen) {
    if (player >= 0 && static_cast<usize>(player) < m_sim.unseen.size()) {
        m_sim.unseen[static_cast<usize>(player)] = unseen;
    }
}

void LevelOpponents::borrowRoster(std::span<const LevelEnemy> entries) {
    if (!m_resources.has_value()) {
        return;
    }
    const LevelInfo* level = m_resources->world.level();
    const auto known = [](std::span<const LevelEnemy> roster, s32 kind) {
        return std::ranges::find(roster, kind, &LevelEnemy::kind) != roster.end();
    };
    for (const LevelEnemy& entry : entries) {
        if (entry.kind < 0 || (level != nullptr && known(level->enemies, entry.kind)) ||
            known(m_sim.borrowedRoster, entry.kind)) {
            continue;
        }
        m_sim.borrowedRoster.push_back(entry);
    }
    refreshBorrowedRoster();
}

void LevelOpponents::refreshBorrowedRoster() {
    const LevelInfo* level = m_resources ? m_resources->world.level() : nullptr;
    const bool imported = m_resources && m_resources->world.imported() != nullptr;
    if ((!level && !imported) || m_sim.borrowedRoster.empty()) {
        m_levelNow.reset();
        return;
    }
    // This view is consumed only by attack/impact sound resolution. Do not
    // install a borrowed stage's boss, tuning, relics or campaign identity.
    m_levelNow = level ? *level : LevelInfo{};
    m_levelNow->enemies.insert(m_levelNow->enemies.end(), m_sim.borrowedRoster.begin(),
                               m_sim.borrowedRoster.end());
}

void LevelOpponents::bindImportedSounds() {
    if (!m_resources || !m_resources->world.imported()) {
        return;
    }
    if (m_resources->world.imported()->hasEnemyAssets()) {
        return;
    }
    std::array<bool, kSwarmKindCount> needed{};
    for (const auto& actor : m_resources->world.imported()->importedActors()) {
        if (actor.spawn.kind >= 0 && actor.spawn.kind < kSwarmKindCount) {
            needed[static_cast<usize>(actor.spawn.kind)] = true;
        }
    }
    for (const auto& [nativeIndex, pool] : m_importedGenerators) {
        if (pool >= 0) {
            needed[static_cast<usize>(m_generators.kindOf(pool))] = true;
            const auto definitions = m_resources->world.imported()->importedGenerators();
            const auto definition =
                std::ranges::find(definitions, nativeIndex, &ImportedGenerator::nativeIndex);
            if (definition != definitions.end() && definition->broodTier != 0) {
                for (const auto kind : definition->broodKinds) {
                    if (kind >= 0 && kind < kSwarmKindCount) {
                        needed[static_cast<usize>(kind)] = true;
                    }
                }
            }
        }
    }
    if (std::ranges::none_of(needed, [](bool value) { return value; })) {
        return;
    }
    LevelCatalog catalog;
    if (!catalog.load(m_resources->root)) {
        log::warn("Imported opponents: no GDL sound roster is available");
        return;
    }
    for (const auto& realm : catalog.realms()) {
        // WDATA/TEST is realm zero. Its T1/T2 placeholder rosters contain
        // scorpions, trolls and skeletons, but no S_GENDAMT/S_GENKILLT cues.
        // Borrow the counterpart's playable campaign realm instead.
        if (realm.id == 0) {
            continue;
        }
        WorldData data;
        if (!data.load(m_resources->root / "wdata" / (realm.file + ".json"))) {
            continue;
        }
        for (const auto& source : data.levels()) {
            const auto* audio = data.audio(source.audioIndex);
            if (source.bossType >= 0 || source.name.empty() || !audio || audio->bank.empty()) {
                continue;
            }
            for (const auto& enemy : source.enemies) {
                if (enemy.kind < 0 || enemy.kind >= kSwarmKindCount || enemy.stream.empty() ||
                    !needed[static_cast<usize>(enemy.kind)]) {
                    continue;
                }
                // These are the real GDL voices of the admitted counterpart,
                // as for console-spawned visitors; native stage music stays put.
                if (!m_resources->audio.borrowBank(m_resources->root / "audio" / audio->bank)) {
                    continue;
                }
                GeneratorEvent probe;
                probe.kind = enemy.kind;
                const auto hit = GeneratorFeedbackEmission::resolve(probe, nullptr, &source,
                                                                    realm.id, source.name);
                probe.destroyed = true;
                const auto death = GeneratorFeedbackEmission::resolve(probe, nullptr, &source,
                                                                      realm.id, source.name);
                if (!m_resources->audio.hasNamed(hit.sound) ||
                    !m_resources->audio.hasNamed(death.sound)) {
                    continue;
                }
                borrowRoster(std::span{&enemy, usize{1}});
                m_importedGeneratorSounds.push_back({enemy.kind, realm.id, source.name});
                needed[static_cast<usize>(enemy.kind)] = false;
            }
        }
        if (std::ranges::none_of(needed, [](bool value) { return value; })) {
            break;
        }
    }
    for (usize kind = 0; kind < needed.size(); ++kind) {
        if (needed[kind]) {
            log::warn("Imported opponent {}: no GDL voice bank is available", kind);
        }
    }
}

const LevelInfo* LevelOpponents::levelNow() const {
    if (m_levelNow.has_value()) {
        return &*m_levelNow;
    }
    return m_resources.has_value() ? m_resources->world.level() : nullptr;
}

bool LevelOpponents::unseen(s32 player) const {
    return player >= 0 && static_cast<usize>(player) < m_sim.unseen.size() &&
           m_sim.unseen[static_cast<usize>(player)];
}

void LevelOpponents::applyEnemyBlow(const EnemyBlow& blow, std::span<PlayerRuntime> players,
                                    const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        const LevelInfo* level = levelNow();
        const auto impact =
            PartyDamage::enemy(blow, player,
                               level != nullptr ? std::span<const LevelEnemy>(level->enemies)
                                                : std::span<const LevelEnemy>{},
                               level != nullptr ? level->bossType : -1);
        if (!impact) {
            continue;
        }
        auto* imported = m_resources->world.imported();
        const bool nativeSound =
            imported != nullptr && imported->playEnemyMelee(blow.kind, blow.tier);
        if (impact->sound && !nativeSound) {
            m_resources->audio.playNamed(*impact->sound);
        }
        if (blow.ward == EnemyMeleeWard::None) {
            events.hurt(i, blow.damage, nativeSound ? HurtKind::Blow : impact->kind, true,
                        impact->impact);
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
                m_sim.cueEffects.push_back(effect);
            }
        }
    }
}

bool LevelOpponents::applyCritterBlow(const CombatBlow& blow, std::span<PlayerRuntime> players,
                                      const Events& events) {
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& player = players[i];
        if (player.actor.player() != blow.player || !PlayerHealth::canBeDamaged(player) ||
            ((blow.breath || blow.gated) && player.breathGap > 0.0f) ||
            (blow.area && player.effectGap > 0.0f)) {
            continue;
        }
        if (blow.breath) {
            const Vec3 centre = player.actor.followPoint();
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
        return true;
    }
    return false;
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
            if (!PlayerHealth::canBeDamaged(player)) {
                continue;
            }
            capture.attach(grab.critter, boss, *grab.attachment, player.actor, grab.restoreSupport);
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
/** What stands where the great ones walk: the fixtures' and the generators, a short one
 * (three high at most) breakable (fn_8005D5C8). */
std::vector<CombatantObstacle>
LevelOpponents::critterObstacles(std::span<const CombatantObstacle> fixtures) const {
    std::vector<CombatantObstacle> items(fixtures.begin(), fixtures.end());
    for (s32 g = 0; g < static_cast<s32>(m_generators.count()); ++g) {
        if (!m_generators.standing(g)) {
            continue;
        }
        CombatantObstacle item;
        item.box = m_generators.boxOf(g);
        if (item.box.height <= kShortGenerator) {
            item.kind = CombatantObstacle::Kind::LowGenerator;
            item.id = kGeneratorRamBase + g;
            item.health = std::numeric_limits<s32>::max(); // it stops them while it stands
        }
        items.push_back(item);
    }
    return items;
}

void LevelOpponents::syncFloors() {
    m_importedActors.syncFloors();
    m_statues.syncFloors();
    m_enemies.syncFloors();
    m_importedActors.syncLive(m_enemies);
    m_critters.syncFloors();
    m_generators.syncFloors();
}

void LevelOpponents::beginOrdinaryFrame(f32 seconds, std::span<PlayerRuntime> players) {
    for (auto& yell : m_sim.yells) {
        yell.timing.advance(seconds);
    }
    std::erase_if(m_sim.yells, [](const Yell& yell) { return !yell.timing.playing(); });
    for (PlayerRuntime& player : players) {
        player.breathGap = std::max(0.0f, player.breathGap - std::max(seconds, 0.0f));
        player.effectGap = std::max(0.0f, player.effectGap - std::max(seconds, 0.0f));
    }
    hearFrom(players);
}

void LevelOpponents::countOrdinaryPlayers(std::span<const PlayerRuntime> players) {
    const auto present = static_cast<s32>(std::ranges::count_if(
        players, [](const PlayerRuntime& player) { return !player.departed; }));
    m_generators.setPlayerCount(m_partyCountOverride.value_or(present));
}

void LevelOpponents::advanceCritters(s32 ticks, f32 seconds, std::span<const EnemyView> views,
                                     bool timeStopped,
                                     std::span<const CombatantObstacle> walkedInto,
                                     std::span<PlayerRuntime> players) {
    m_critters.update(ticks, seconds, views, timeStopped, critterObstacles(walkedInto), &m_enemies);
    for (const CombatPush& push : m_critters.takePushes()) {
        for (PlayerRuntime& player : players) {
            if (player.actor.player() == push.player && player.life == PlayerLife::Standing &&
                !player.departed && !player.capture.active() && !player.combo.riding) {
                player.knockback.addVelocity(push.velocity);
            }
        }
    }
}

void LevelOpponents::setMechanics(MechanicsFacade* mechanics) {
    m_mechanics = mechanics;
    m_enemies.setRules({.swarm = mechanics, .births = mechanics, .appearance = mechanics});
    m_generators.setRules(mechanics);
}

void LevelOpponents::update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                            std::span<const Obstacle> fixtures, const Events& events,
                            std::span<const MissileStop> stops,
                            std::span<const CombatantObstacle> walkedInto) {
    if (!m_resources.has_value()) {
        return;
    }
    beginOrdinaryFrame(seconds, players);
    updateBags(seconds);
    shrinkOpponents(players);
    const std::vector<EnemyView> views = this->views(players);
    countOrdinaryPlayers(players);
    const LevelInfo* level = m_resources->world.level();
    const f32 missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1.0f;
    const bool timeStopped = PlayerPowerups::timeStopped(players);
    if (m_importedActors.count() != 0 || m_resources->world.imported()) {
        std::vector<EnemyView> living;
        for (const auto& view : views) {
            const auto player = std::ranges::find_if(players, [&](const PlayerRuntime& runtime) {
                return runtime.actor.player() == view.player && !runtime.departed &&
                       runtime.life == PlayerLife::Standing;
            });
            if (player != players.end()) {
                living.push_back(view);
            }
        }
        updateImportedActors(seconds, living, timeStopped);
    }
    // The mechanics may answer the party's runs of close blows, held still with time.
    if (!timeStopped && m_mechanics != nullptr && !m_mechanics->empty()) {
        std::vector<SeatRun> runs;
        runs.reserve(players.size());
        for (const PlayerRuntime& runtime : players) {
            runs.push_back({.count = runtime.streak.count(),
                            .running = runtime.streak.running(),
                            .departed = runtime.departed});
        }
        m_mechanics->step({.seconds = seconds, .runs = runs});
    }
    const SwarmTuning tuning = m_mechanics != nullptr ? m_mechanics->tuning() : SwarmTuning{};
    // fn_8004646C checks the live critter pool before ordinary enemy bodies,
    // both for route probes and check_enemy_pos's generator-birth clearance.
    auto combatants = m_critters.targets(true);
    const auto bosses = m_bosses.targets(true);
    combatants.insert(combatants.end(), bosses.begin(), bosses.end());
    m_enemies.setCombatantBodies(combatants);
    auto inTheWay = OpponentSimulation{m_generators, m_enemies, m_enemyMissiles}.advance(
        {ticks, seconds, views, fixtures, stops, m_critters.targets(true), m_bosses.targets(),
         &m_resources->world.collision(), missileSpeed, timeStopped, tuning},
        events.blastScenery, [this] { playBirths(); });
    m_importedActors.syncLive(m_enemies);
    landEnemyMissiles(players, events);
    playEnemyCues();
    // What blows itself up goes up with the bomb's bang; one struck down stops crying out
    // (enemy_dies, AudioKillBySound).
    for (const EnemyBurst& burst : m_enemies.takeBursts()) {
        auto& audio = m_resources->audio;
        if (audio.recordingSounds() &&
            (burst.enemy < 0 || burst.enemy >= Enemies::kMost || burst.instance == 0)) {
            audio.recorder()->unsupported("bomber burst without its source lifetime");
        }
        auto scope =
            audio.soundScope({20, static_cast<u64>(std::max(burst.enemy, 0)), burst.instance}, 48);
        if (burst.silencesYell) {
            if (!m_resources->audio.recordingSounds()) {
                for (const SoundHandle yell : m_yells) {
                    m_resources->audio.stop(yell);
                }
                m_yells.clear();
            }
            m_sim.yells.clear();
        }
        auto* const imported = m_resources->world.imported();
        if (!imported || !imported->playOpponentSound(kSuicideBombSound, burst.position)) {
            playAt(kSuicideBombSound, kLoudSound, burst.position);
        }
        explodeSuicide(burst);
    }
    advanceClouds();
    for (const EnemyDeathShot& shot : m_enemies.takeDeathShots()) {
        fireDeathShot(shot);
    }
    advanceDeathShots();
    events.settleBlasts();
    updateStatues(ticks, seconds, players);
    advanceCritters(ticks, seconds, views, timeStopped, walkedInto, players);
    for (const CombatantRam& ram : m_critters.takeRams()) {
        if (ram.id >= kGeneratorRamBase) {
            strikeGenerator(ram.id - kGeneratorRamBase, ram.damage, -1, players);
        } else {
            m_sim.barrelRams.push_back(ram);
        }
    }
    // Boss AI is independent of Stop Time; fired missiles likewise keep moving.
    m_bosses.setArenaAnchors(events.arenaAnchors ? events.arenaAnchors() : std::vector<Mat4>{});
    m_bosses.setArenaTargets(events.arenaTargets ? events.arenaTargets()
                                                 : std::vector<CombatArenaTarget>{});
    auto* importedArena = m_resources->world.imported();
    const bool bossWasAwake = importedArena != nullptr && m_bosses.view().awake;
    m_bosses.update(ticks, seconds, views);
    if (importedArena != nullptr && !bossWasAwake && m_bosses.view().awake &&
        m_bosses.position() != nullptr) {
        importedArena->bossIntro(*m_bosses.position());
    }
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
    const auto shotSound = [&](const CombatantProjectiles::SoundCue& cue) {
        if (cue.ownerKind == CombatantKind::Boss) {
            if (auto* imported = m_resources->world.imported();
                imported && imported->playOpponentSound(cue.name, cue.position)) {
                return;
            }
        }
        auto& audio = m_resources->audio;
        if (audio.recordingSounds() && (cue.instance == 0 || cue.effect < 0)) {
            audio.recorder()->unsupported("combatant projectile cue without its source lifetime");
        }
        auto scope = audio.soundScope({23, static_cast<u64>(cue.effect) + 1, cue.instance},
                                      static_cast<u32>(cue.phase));
        audio.playNamed(cue.name);
    };
    for (const CombatShot& shot : m_bosses.takeShots()) {
        if (ItemArchive* archive = m_bosses.archive(); archive != nullptr) {
            m_combatantProjectiles.launch(shot, *archive, m_resources->device, m_resources->effects,
                                          shotSound, &m_resources->world.collision());
        }
    }
    for (const CombatShot& shot : m_critters.takeShots()) {
        if (ItemArchive* archive = m_critters.archiveOf(shot.critter); archive != nullptr) {
            m_combatantProjectiles.launch(shot, *archive, m_resources->device, m_resources->effects,
                                          shotSound, &m_resources->world.collision());
        }
    }
    const auto swarmHits = m_enemyMissiles.takeRockHits();
    m_sim.rockHits.insert(m_sim.rockHits.end(), swarmHits.begin(), swarmHits.end());
    for (MissileStop& item : inTheWay) {
        for (const RockHit& hit : swarmHits) {
            if (item.rock >= 0 && static_cast<usize>(item.rock) == hit.rock) {
                item.rockHealth -= std::max(
                    1, static_cast<s32>(std::round(hit.damage - static_cast<f32>(item.rockArmor))));
            }
        }
    }
    m_combatantProjectiles.update(seconds, &m_resources->world.collision(), views,
                                  m_resources->device, m_resources->effects, shotSound, inTheWay,
                                  swarmTargets());
    const auto combatantHits = m_combatantProjectiles.takeRockHits();
    m_sim.rockHits.insert(m_sim.rockHits.end(), combatantHits.begin(), combatantHits.end());
    for (const CombatantWorldHit& hit : m_combatantProjectiles.takeWorldHits()) {
        if (m_resources->audio.recordingSounds() && hit.instance == 0) {
            m_resources->audio.recorder()->unsupported(
                "combatant world impact without its source lifetime");
        }
        if (hit.object >= 0) {
            m_resources->world.explodeObject(hit.object, hit.position);
        }
        if (hit.splash) {
            auto scope = m_resources->audio.soundScope(
                {23, 0, hit.instance}, static_cast<u32>(CombatantProjectiles::SoundPhase::Splash));
            playAt("S_SPLASH", LevelSoundscape::kSplashLevel, hit.position);
        }
    }
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
                ItemArchive* archive = &m_resources->world.items();
                std::string_view tree = "BOSSGEN";
                bool settled = false;
                // AtreeMatchAnyHeader searches loaded monster archives too.
                // A boss's generator body is not necessarily in ITEMS/LEVEL*.
                if (!archive->trees.find(tree)) {
                    if (ItemArchive* boss = m_bosses.archive(); boss != nullptr) {
                        const auto body = bossGeneratorVisual(level->bossType);
                        if (boss->trees.find(body.tree)) {
                            archive = boss;
                            tree = body.tree;
                            settled = body.settled;
                        }
                    }
                }
                m_generators.placeBoss(m_resources->device, info, *archive, m_enemies, kind,
                                       placement, &m_resources->world.collision(), tree, settled);
                break;
            }
        }
    }
    for (const CombatantProjectileHit& hit : m_combatantProjectiles.takeHits()) {
        if (m_resources->audio.recordingSounds() && hit.instance == 0) {
            m_resources->audio.recorder()->unsupported(
                "combatant projectile impact without its source lifetime");
        }
        auto scope = m_resources->audio.soundScope(
            {23, 0, hit.instance},
            static_cast<u32>(hit.ricochet ? CombatantProjectiles::SoundPhase::Ricochet
                                          : CombatantProjectiles::SoundPhase::Impact));
        if (hit.ricochet) {
            playAt(kRicochetSound, kQuietSound, hit.position);
            continue;
        }
        if (hit.target >= 0) {
            strikeSwarm(hit.target, hit.damage, hit.flags, hit.direction, players);
            continue;
        }
        for (usize player = 0; player < players.size(); ++player) {
            if (players[player].actor.player() == hit.player &&
                players[player].life == PlayerLife::Standing && players[player].effectGap <= 0) {
                // ProcessEffects (80094BE0) calls damage_player with mode 1
                // for both missiles and lingering contact effects. Mode 3's
                // piercing-trap groan does not belong to these attacks.
                events.hurt(player, hit.damage, HurtKind::Blow, true, {hit.flags, hit.direction});
                players[player].effectGap = hit.repeatGap;
                if (hit.ownerKind == CombatantKind::Boss) {
                    m_bosses.damagedPlayer(hit.player, hit.damage, hit.critter);
                } else {
                    m_critters.damagedPlayer(hit.critter, hit.player, hit.damage);
                }
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
                    if (auto* imported = m_resources->world.imported()) {
                        imported->consumeBossLegend(actor.save());
                    } else {
                        actor.save().progress().relics.spendLegend(event.realm);
                    }
                }
            }
        }
        events.legend(event);
    }
    events.advanceLegend(seconds);
    for (const CombatBlow& blow : m_bosses.takeBlows()) {
        if (applyCritterBlow(blow, players, events)) {
            m_bosses.damagedPlayer(blow.player, blow.damage, blow.critter);
        }
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
    followCritterEffects(players);
    finishSummons(views);
    for (const CombatBlow& blow : m_critters.takeBlows()) {
        if (applyCritterBlow(blow, players, events)) {
            m_critters.damagedPlayer(blow.critter, blow.player, blow.damage);
        }
    }
    awardCritterLosses(players, events);
    events.levels();
    for (const EnemyBlow& blow : m_enemies.takeBlows()) {
        applyEnemyBlow(blow, players, events);
    }
    updateDeaths(players, events);
    // IT's touch makes its player it, whoever was before (fn_80046140).
    for (const s32 tagged : m_enemies.takeTagged()) {
        for (usize i = 0; i < players.size(); ++i) {
            if (players[i].actor.player() != tagged) {
                continue;
            }
            for (PlayerRuntime& other : players) {
                other.itTicks = 0;
            }
            players[i].itTicks = 1;
            log::info("Player {} is now it", tagged + 1);
            if (events.help) {
                events.help(HelpMessages::kNowIt, i);
            }
        }
    }
    deliverLessons(players, events);
    awardEnemyLosses(events);
}

void LevelOpponents::deliverLessons(std::span<const PlayerRuntime> players, const Events& events) {
    // PlayerDamagedEnemy posts message22 during combat. A queued hit must reach
    // the next normal update, not wait for the portal's final reward settlement.
    for (const auto& [lesson, player] : std::exchange(m_sim.lessons, {})) {
        for (usize i = 0; i < players.size() && events.help; ++i) {
            if (players[i].actor.player() == player) {
                events.help(lesson, i);
            }
        }
    }
}

void LevelOpponents::settleRewards(std::span<const PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    deliverLessons(players, events);
    hearFrom(players);
    awardBossLosses(players, events);
    awardCritterLosses(players, events);
    awardEnemyLosses(events);
    events.levels();
}

bool LevelOpponents::awardEnemyLosses(const Events& events, const FeedbackPublisher& publish) {
    if (!m_resources.has_value()) {
        return true;
    }
    for (const EnemyFeedback& feedback : m_enemies.takeFeedback()) {
        auto& audio = m_resources->audio;
        if (audio.recordingSounds() &&
            (feedback.enemy < 0 || feedback.enemy >= Enemies::kMost || feedback.instance == 0)) {
            audio.recorder()->unsupported("enemy impact without its source lifetime");
        }
        // Hurt and death are different semantic cues, including when a corrected
        // hit becomes lethal. Other enemies never renumber this source's events.
        auto scope =
            audio.soundScope({20, static_cast<u64>(std::max(feedback.enemy, 0)), feedback.instance},
                             feedback.killed ? 17 : 16);
        const auto emission = EnemyFeedbackEmission::resolve(
            feedback, m_enemies.archive(feedback.kind), m_resources->weapons, levelNow());
        u32 effect = 0;
        if (publish) {
            const auto emitted = publish(emission);
            if (!emitted) {
                return false;
            }
            effect = *emitted;
        } else {
            auto* imported = m_resources->world.imported();
            if (!imported || !imported->playEnemyFeedback(feedback, feedback.remainingHealth)) {
                playAt(emission.sound, kLoudSound, emission.position);
            }
            if (emission.archive != nullptr) {
                effect = m_resources->effects.startSet(m_resources->device, *emission.archive,
                                                       emission.tree, emission.position,
                                                       emission.setting);
            }
        }
        if (effect != 0) {
            m_sim.cueEffects.push_back(effect);
        }
    }
    BagThrow drops{*this};
    for (const EnemyLoss& loss : m_enemies.takeLosses()) {
        events.award(loss.player, loss.experience, loss.killed);
        if (m_mechanics != nullptr) {
            m_mechanics->fell({.kind = loss.kind,
                               .affinity = loss.affinity,
                               .player = loss.player,
                               .position = loss.position,
                               .killed = loss.killed},
                              drops);
        }
    }
    for (const GeneratorReward& reward : m_sim.generatorRewards) {
        // combat.c awards generator experience with AddExp's kill flag clear.
        events.award(reward.player, reward.experience, false);
        if (reward.destroyed && events.destroyedGenerator) {
            events.destroyedGenerator(reward.player);
        }
    }
    m_sim.generatorRewards.clear();
    return true;
}

/** A hit on one of the swarm, from a player or the world. */
f32 LevelOpponents::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                                std::span<const PlayerRuntime> players, bool close,
                                std::optional<Vec3> where) {
    if (!m_resources.has_value()) {
        return 0;
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
    const f32 before = m_enemies.healthOf(id);
    const f32 credited = m_enemies.hurt(id, hit);
    // A player hitting what generators breed is taught, the tenth time, to go for them
    // instead (combat.c:307; not where a boss is fought).
    const LevelInfo* level = m_resources->world.level();
    if (byPlayer >= 0 && static_cast<usize>(byPlayer) < m_sim.hitStreak.size() &&
        m_enemies.bred(id) && before > 0.0f && m_enemies.healthOf(id) < before &&
        (level == nullptr || level->bossType < 0) &&
        ++m_sim.hitStreak[static_cast<usize>(byPlayer)] >= kStreakLesson) {
        m_sim.lessons.emplace_back(HelpMessages::kDestroyGenerators, byPlayer);
    }
    return credited;
}

/** A hit on one of the great ones. */
f32 LevelOpponents::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                                  std::optional<Vec3> where, bool close,
                                  std::span<const PlayerRuntime> players, s32 node) {
    if (!m_resources.has_value()) {
        return 0;
    }
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    hit.where = where;
    hit.close = close;
    hit.node = node;
    for (const PlayerRuntime& runtime : players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    return m_critters.hurt(id, hit);
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
/** A blast's ring reaching the swarm, the generators, the boss and the great ones, each once
 * a blast unless the blow is slight enough to come again; thrown down away from its centre. */
void LevelOpponents::blast(const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                           std::span<const PlayerRuntime> players, u32 flags) {
    constexpr f32 kHeldFrom = 2.0f;   ///< a blow over this is not dealt the same one again
    constexpr s32 kGenerators = 1000; ///< the blast's ids: enemies, then these
    constexpr s32 kCritters = 2000;
    constexpr s32 kBoss = 3000;
    const auto first = [&](s32 id) {
        if (std::ranges::find(reached, id) != reached.end()) {
            return false;
        }
        if (damage > kHeldFrom) {
            reached.push_back(id);
        }
        return true;
    };
    for (const s32 enemy : enemies().within(position, radius)) {
        if (!first(enemy)) {
            continue;
        }
        const Vec3 away = enemies().positionOf(enemy) - position;
        strikeEnemy(enemy, damage, flags, Vec3{away.x, 0.0f, away.z}, -1, players);
    }
    for (const s32 generator : generators().within(position, radius)) {
        if ((flags & 0x800) == 0 && first(kGenerators + generator)) {
            strikeGenerator(generator, damage, -1, players);
        }
    }
    if (bosses().within(position, radius) && first(kBoss)) {
        EnemyHit struck;
        struck.damage = damage;
        struck.flags = flags;
        if (const Vec3* at = bosses().position(); at != nullptr) {
            struck.direction = Vec3{at->x - position.x, 0.0f, at->z - position.z};
        }
        bosses().hurt(struck);
    }
    for (const s32 critter : critters().within(position, radius)) {
        if (!first(kCritters + critter)) {
            continue;
        }
        const Vec3 away = critters().positionOf(critter) - position;
        // CritterExpCollide supplies ProcessEffects a planar unit direction.
        // CritterDamage accumulates that input; distance is not blast force.
        const f32 across = portable::hypot(away.x, away.z);
        const Vec3 direction = across > 0 ? Vec3{away.x, 0.0f, away.z} / across : Vec3{0};
        strikeCritter(critter, damage, flags, direction, -1, std::nullopt, false, players);
    }
}

void LevelOpponents::strikeGenerator(s32 id, f32 power, s32 byPlayer,
                                     std::span<const PlayerRuntime> players) {
    (void)strikeGenerator(id, power, byPlayer, players, {});
}

bool LevelOpponents::strikeGenerator(s32 id, f32 power, s32 byPlayer,
                                     std::span<const PlayerRuntime> players,
                                     const GeneratorPublisher& publish) {
    if (!m_resources.has_value()) {
        return true;
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
        return true;
    }
    ItemArchive* archive =
        event->kind < 0 ? &m_resources->world.items() : m_enemies.archive(event->kind);
    const auto importedSound =
        std::ranges::find(m_importedGeneratorSounds, event->kind, &ImportedGeneratorSound::kind);
    const auto emission = GeneratorFeedbackEmission::resolve(
        *event, archive, level,
        importedSound == m_importedGeneratorSounds.end() ? m_resources->world.ref().realmId
                                                         : importedSound->realm,
        importedSound == m_importedGeneratorSounds.end() ? m_resources->world.ref().name
                                                         : importedSound->level);
    u32 effect = 0;
    if (publish) {
        const auto emitted = publish(emission);
        if (!emitted) {
            return false;
        }
        effect = *emitted;
    } else {
        if (emission.archive != nullptr) {
            effect = m_resources->effects.startSet(m_resources->device, *emission.archive,
                                                   emission.tree, emission.position, {});
            m_resources->effects.placeAt(effect, emission.placement);
        }
    }
    if (effect != 0) {
        m_sim.cueEffects.push_back(effect);
    }
    const bool nativeSound = !publish && m_resources->world.imported() != nullptr &&
                             m_resources->world.imported()->playGeneratorFeedback(*event);
    if (!publish && !nativeSound && !emission.sound.empty()) {
        // Slots are append-only for a loaded level; the native birth serial
        // distinguishes generated lifetimes and is restored with their state.
        const auto audioScope = m_resources->audio.soundScope(
            {46, static_cast<u64>(event->generator), event->instance}, event->destroyed      ? 3
                                                                       : event->stateChanged ? 2
                                                                                             : 1);
        m_resources->audio.playNamed(emission.sound);
    }
    if (byPlayer >= 0) {
        m_sim.generatorRewards.push_back(
            {byPlayer, generatorExperience(event->kind, event->destroyed), event->destroyed});
    }
    if (event->destroyed) {
        m_enemies.generatorGone(id);
        if (byPlayer >= 0 && static_cast<usize>(byPlayer) < m_sim.hitStreak.size()) {
            m_sim.hitStreak[static_cast<usize>(byPlayer)] = 0;
        }
    }
    return true;
}

/** Plays what one of the great ones (the boss with `ofBoss`) has set off: its tree from its
 * own archive (or the weapons', which holds the common marks of a hit) where it happened,
 * riding along with it when it follows, and its sound. */
bool LevelOpponents::showCritterCue(const CombatCue& cue, ItemArchive* archive, bool ofBoss,
                                    const CritterPublisher& publish) {
    if (!m_resources.has_value()) {
        return true;
    }
    if (cue.stopMoveEffect) {
        std::erase_if(m_sim.moveEffects, [&](const MoveEffect& previous) {
            if (previous.critter != cue.critter || previous.ofBoss != ofBoss) {
                return false;
            }
            m_resources->effects.stop(previous.effect);
            return true;
        });
        return true;
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
    // Imported native assets are installed without the donor texture context.
    // Their scoped effect banks are already composed into this archive: an
    // absent native effect must not silently become a similarly named GDL one.
    const auto* imported = m_resources->world.imported();
    const bool ownEffectsOnly = ofBoss && archive != nullptr && imported != nullptr &&
                                imported->importedBoss() != nullptr && !m_importedBossTextures;
    const auto emission =
        CritterCueEmission::resolve(cue, archive, ownEffectsOnly ? *archive : m_resources->weapons);
    if (emission.archive != nullptr) {
        const auto issued = publish ? publish(emission)
                                    : std::optional{m_resources->effects.startSet(
                                          m_resources->device, *emission.archive, emission.tree,
                                          emission.position, emission.setting)};
        if (!issued) {
            return false;
        }
        const u32 effect = *issued;
        if (effect != 0) {
            m_sim.cueEffects.push_back(effect);
            if (cue.untilNextMove) {
                m_sim.moveEffects.push_back({effect, cue.critter, ofBoss});
            }
        }
        if (effect != 0 && cue.placement.has_value()) {
            m_resources->effects.placeAt(effect, *cue.placement);
        }
        if (effect != 0 && cue.follows) {
            const Vec3* at = ofBoss ? m_bosses.position() : &m_critters.positionOf(cue.critter);
            m_sim.critterEffects.push_back(CritterEffect{
                effect, cue.critter, ofBoss,
                at != nullptr ? cue.position - *at : Vec3{0.0f, 0.0f, 0.0f}, cue.node,
                cue.nodeOffset, cue.rootAttachment, cue.pitchYaw, cue.playerAttachment});
        }
    }
    // The great ones' sounds play at 224 of 255 as heard from where the players stand
    // (CritterDoSfx: AudioPlay3DSel, sndFxPlay3DAtten); a death's at that level whole.
    const bool nativeSound =
        !publish && ofBoss && !cue.sound.empty() && m_resources->world.imported() != nullptr &&
        m_resources->world.imported()->playOpponentSound(cue.sound, cue.soundPosition);
    if (!publish && !nativeSound && !cue.sound.empty() && cue.attenuated) {
        playAt(cue.sound, kLoudSound, cue.soundPosition);
    } else if (!publish && !nativeSound && !cue.sound.empty()) {
        m_resources->audio.playAt(cue.sound, cue.soundPosition, 0.0f, kLoudSound / kFullLevel);
    }
    return true;
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
                spawn.direction =
                    Vec3{portable::rotate(Mat4{1}, yaw, Vec3{0, 1, 0}) * Vec4{forward, 0}};
                spawn.position = origin + kOffset * spawn.direction;
                spawn.clearance = 1;
                spawn.placed = true;
                m_enemies.spawn(spawn, players);
            }
        }
    }
}

std::vector<EffectTrees::DrawAttachment> LevelOpponents::presentedBossEffects(f32 alpha) const {
    std::vector<EffectTrees::DrawAttachment> result;
    if (!m_resources || !m_resources->world.imported() || alpha < 0 || !m_bosses.present()) {
        return result;
    }
    const auto& boss = m_bosses.fighter();
    for (const auto& riding : m_sim.critterEffects) {
        if (!riding.ofBoss || riding.playerAttachment || (!riding.rootAttachment && !riding.node)) {
            continue;
        }
        const std::string_view node =
            riding.rootAttachment ? boss.data()->rootNode() : std::string_view{*riding.node};
        if (const auto parent = boss.presentedNodeTransform(node, alpha)) {
            result.push_back({riding.effect,
                              CritterArea::placement(*parent, riding.nodeOffset, riding.pitchYaw)});
        }
    }
    return result;
}

/** Effects riding on the great ones go where they go, and are let go of when they end. */
void LevelOpponents::followCritterEffects(std::span<const PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    std::erase_if(m_sim.cueEffects,
                  [this](u32 effect) { return !m_resources->effects.playing(effect); });
    std::erase_if(m_sim.moveEffects, [this](const MoveEffect& effect) {
        return !m_resources->effects.playing(effect.effect);
    });
    for (usize i = 0; i < m_sim.critterEffects.size();) {
        const CritterEffect& riding = m_sim.critterEffects[i];
        if (riding.playerAttachment.has_value()) {
            const auto player = std::ranges::find_if(players, [&](const PlayerRuntime& runtime) {
                return runtime.actor.player() == *riding.playerAttachment &&
                       runtime.life == PlayerLife::Standing;
            });
            if (player == players.end() || !m_resources->effects.playing(riding.effect)) {
                m_resources->effects.stop(riding.effect);
                m_sim.critterEffects.erase(m_sim.critterEffects.begin() +
                                           static_cast<std::ptrdiff_t>(i));
                continue;
            }
            m_resources->effects.placeAt(
                riding.effect,
                CritterArea::placement(player->capture.body().value_or(player->actor.transform()),
                                       riding.nodeOffset, riding.pitchYaw));
            ++i;
            continue;
        }
        const bool alive =
            riding.ofBoss ? m_bosses.present()
                          : m_critters.alive(riding.critter) || m_critters.dying(riding.critter);
        if (!m_resources->effects.playing(riding.effect) || !alive) {
            if (!alive && (riding.node.has_value() || riding.rootAttachment)) {
                m_resources->effects.stop(riding.effect);
            }
            m_sim.critterEffects.erase(m_sim.critterEffects.begin() +
                                       static_cast<std::ptrdiff_t>(i));
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
                static_cast<usize>(player) >= m_sim.critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_sim.critterExperienceOwed[static_cast<usize>(player)];
            owed += loss.experience;
            const auto whole = static_cast<s32>(std::floor(owed));
            if (whole > 0 || loss.killed) {
                owed -= static_cast<f32>(whole);
                events.award(player, whole, loss.killed);
            }
        }
    }
}

void LevelOpponents::throwBag(Bag bag) {
    EffectTrees::Setting settings;
    settings.seconds = 3;
    bag.effect = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                               "BAG_THROW", bag.position, settings);
    m_sim.bagSeed = m_sim.bagSeed * 1664525U + 1013904223U;
    constexpr f32 kMinSpin = 0.261799388f;
    constexpr f32 kSpinRange = 1.04719758f;
    bag.spin = kMinSpin + kSpinRange * static_cast<f32>(m_sim.bagSeed >> 8) / 16777216.0f;
    if (bag.effect != 0) {
        m_sim.bags.push_back(std::move(bag));
    } else {
        releaseBag(bag); // Missing optional artwork must not lose the reward.
    }
}

void LevelOpponents::BagThrow::leave(std::string_view item, const Vec3& at) {
    if (!m_owner.m_resources.has_value()) {
        return;
    }
    Bag bag;
    bag.position = at;
    bag.name = std::string{item};
    m_owner.throwBag(std::move(bag));
}

std::vector<SwarmBodyView> LevelOpponents::swarmBodies() const {
    std::vector<SwarmBodyView> bodies;
    for (s32 id = 0; id < m_enemies.most(); ++id) {
        if (m_enemies.alive(id)) {
            bodies.push_back({.instance = m_enemies.instanceOf(id),
                              .affinity = m_enemies.affinityOf(id),
                              .feet = m_enemies.positionOf(id),
                              .radius = m_enemies.radiusOf(id),
                              .height = m_enemies.heightOf(id)});
        }
    }
    return bodies;
}

void LevelOpponents::dropCarried(const CombatLoss& loss, std::span<const PlayerRuntime> players,
                                 const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    Bag bag;
    bag.position = loss.position;
    if (loss.critter >= 0 && static_cast<usize>(loss.critter) < m_sim.carried.size()) {
        std::optional<usize>& carried = m_sim.carried[static_cast<usize>(loss.critter)];
        if (carried.has_value()) {
            bag.item = carried;
            carried.reset();
        }
    }
    if (!bag.item && loss.kind == CombatantKind::Gargoyle && !loss.form.empty()) {
        // A gargoyle carrying nothing leaves the piece its form is named by.
        bag.name = "GARG" + loss.form;
    }
    if (!bag.item && bag.name.empty()) {
        return;
    }
    throwBag(bag);
    if (!events.help) {
        return;
    }
    s32 lesson = 0;
    if (loss.kind == CombatantKind::General) {
        lesson = HelpMessages::kGeneralsCarry;
    } else if (loss.kind == CombatantKind::Gargoyle) {
        lesson = HelpMessages::kGargoylesRelease;
    } else {
        return;
    }
    for (usize i = 0; i < players.size(); ++i) {
        if (players[i].life == PlayerLife::Standing) {
            events.help(lesson, i);
            return;
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
        if (loss.drop) {
            dropCarried(loss, players, events);
            continue;
        }
        for (const PlayerRuntime& runtime : players) {
            const PlayerActor& actor = runtime.actor;
            const s32 player = actor.player();
            if (loss.player >= 0 && loss.player != player) {
                continue;
            }
            if (player < 0 || static_cast<usize>(player) >= m_sim.critterExperienceOwed.size()) {
                continue;
            }
            f32& owed = m_sim.critterExperienceOwed[static_cast<usize>(player)];
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
