#include "game/party/PlayerAttacks.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/PortableMath.h"
#include "engine/world/AnimationPlayer.h"

#include "game/bosses/LegendItems.h"
#include "game/characters/Progression.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/EffectFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/effects/DynamicLights.h"
#include "game/effects/WeaponGlow.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyKinds.h"
#include "game/hud/HelpMessages.h"
#include "game/level/BossKinds.h"
#include "game/level/Chests.h"
#include "game/level/ImportedLevel.h"
#include "game/party/PartyDamage.h"
#include "game/party/PlayerPowerups.h"
#include "game/party/PotionEmission.h"
#include "game/party/TargetAssist.h"
#include "game/players/ComboMove.h"
#include "game/players/ItemPickup.h"
#include "game/players/MagicPerks.h"
#include "game/players/PowerupEffects.h"
namespace gdl::game {
namespace {
constexpr std::string_view kNoEffectTree = "NULLFX"; ///< a move's effect row that shows nothing
constexpr std::array<std::string_view, 5> kShieldTrees{"MS_FIRE", "MS_FIRE", "MS_ELEC", "MS_LIGHT",
                                                       "MS_ACID"};
constexpr std::array<std::string_view, 5> kShieldSounds{"S_SHIELD2", "S_SHIELD2", "S_SHIELD1",
                                                        "S_SHIELD3", "S_SHIELD4"};
constexpr f32 kShieldSeconds = 3.0f; ///< how long a potion's ring lasts
constexpr f32 kShieldDamage = 25.0f;
constexpr f32 kShieldPotency = 0.25f; ///< of the character's magic power, its radius
constexpr f32 kGasEffectGap = 0.5f;   ///< a gas blow's wait before its next effect
constexpr std::string_view kBlockEffect = "BLOCKFX";
constexpr f32 kRamDamage = 3.0f;   ///< what a charge does to what it runs into
constexpr f32 kChargeBlow = 32.0f; ///< and to an enemy or a great one (PlayerMotion's anim 8)
constexpr u32 kFireElement = 1;    ///< the fire shield's harm
/** The lightning shield's blow: lightning, knocking down. */
constexpr u32 kShockFlags = damage::kElectric | damage::kKnockDown;
static_assert(kShockFlags == 0x22U);
constexpr f32 kFramesPerSecond = 30.0f;
constexpr std::string_view kShockEffect = "L_SHLD_ACTIVE"; ///< fx 55, from shield to struck
constexpr std::string_view kHaloSound = "S_HALO";          ///< once as Death is taken hold of
constexpr std::string_view kDeathCry = "S_DEATHDIE";       ///< while he is held
constexpr std::string_view kDeathSuck = "S_DEATHSUCK";     ///< about the one holding him
constexpr f32 kHaloVolume = 224.0f / 255.0f;
constexpr f32 kDeathVolume = 127.0f / 255.0f;
constexpr s32 kTicksPerFrame = 2; ///< of 60 Hz, in one 30 Hz frame

/** Long-range area hits look through the damageable item families, not solid-world walls. */
bool playerAreaBlocked(const Vec3& from, const Vec3& to, const LevelFixtures& fixtures,
                       const LevelWorld& world) {
    constexpr f32 kProbeRadius = 0.1f;
    for (const Obstacle& barrel : fixtures.barrels().obstacles()) {
        if (barrel.blocksSegment(from, to, kProbeRadius)) {
            return true;
        }
    }
    const auto& rocks = fixtures.safeRocks();
    for (usize i = 0; i < rocks.size(); ++i) {
        if (rocks.standing(i) && rocks.rock(i).armor > 0 &&
            rocks.rock(i).obstacle.blocksSegment(from, to, kProbeRadius)) {
            return true;
        }
    }
    const StrikeHit probe{
        .centre = to, .radius = kProbeRadius, .damage = 1, .from = from, .swept = true};
    const auto& walls = world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        if (walls.standing(i) && walls.target(i, 0).reachedBy(probe)) {
            return true;
        }
    }
    return false;
}

} // namespace
void PlayerAttacks::appendBindingDigest(NativeStateDigest& digest) const {
    digest.flag(m_resources.has_value());
    if (m_resources) {
        const auto& resources = *m_resources;
        digest.resource(&resources.world.layout());
        digest.flag(resources.weapons.loaded());
        if (resources.weapons.loaded()) {
            digest.resource(&resources.weapons);
        }
        digest.flag(resources.shake != nullptr);
        digest.flag(resources.multiplayer != nullptr);
        if (resources.multiplayer) {
            digest.value(*resources.multiplayer);
        }
        digest.value(resources.acquisitionCone);
    }
}

bool PlayerAttacks::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("PlayerAttacks.native.1");
    appendBindingDigest(digest);
    digest.value(m_sim);
    return m_strikes.appendNativeDigest(digest) && digest.good();
}

bool PlayerAttacks::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("PlayerAttacks.native.1");
    appendBindingDigest(digest);
    digest.value(saved.m_state);
    return m_strikes.appendLocalDigest(digest, saved.m_strikes) && digest.good();
}

void PlayerAttacks::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    m_strikes.saveLocal(saved.m_strikes);
    saved.m_owner = this;
    saved.m_lifetime = m_restoreOwner;
    saved.m_epoch = m_localEpoch;
}

bool PlayerAttacks::canRestoreLocal(const LocalSnapshot& saved) const {
    const auto lifetime = saved.m_lifetime.lock();
    return saved.m_owner == this && lifetime && lifetime == m_restoreOwner &&
           saved.m_epoch == m_localEpoch && m_strikes.canRestoreLocal(saved.m_strikes);
}

bool PlayerAttacks::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    m_strikes.restoreLocal(saved.m_strikes);
    ++m_restoreGeneration;
    return true;
}

void PlayerAttacks::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
}
void PlayerAttacks::clear() {
    ++m_restoreGeneration;
    ++m_localEpoch;
    if (m_resources.has_value()) {
        for (const auto& particle : m_sim.particleEffects) {
            m_resources->effects.stop(particle.effect);
        }
        for (const auto& item : m_sim.items) {
            m_resources->effects.stop(item.effect);
        }
        for (const StrikeEffect& effect : m_sim.strikeEffects) {
            m_resources->effects.stop(effect.effect);
        }
        for (const MoveAttachment& attachment : m_sim.moveAttachments) {
            m_resources->effects.stop(attachment.effect);
        }
        for (const PotionShield& shield : m_sim.shields) {
            m_resources->effects.stop(shield.effect);
        }
    }
    m_strikes.clear();
    m_sim.particleEffects.clear();
    m_sim.strikeEffects.clear();
    m_sim.moveAttachments.clear();
    m_sim.strikeSources.clear();
    m_sim.shields.clear();
    m_sim.potions.clear();
    m_sim.items.clear();
    m_sim.deathSounds.fill(std::nullopt);
    m_resources.reset();
}
namespace {

/** Does for `player`'s run what the rules say a blow of `source` with `outcome` on `target`
 * does; `telling` is the original's reading of it, for its own count. */
void applyRunEffect(const BlowRules& rules, std::span<PlayerRuntime> players, s32 player,
                    BlowSource source, BlowOutcome outcome, BlowTarget target, bool telling) {
    for (PlayerRuntime& runtime : players) {
        if (runtime.actor.player() != player) {
            continue;
        }
        const PowerupEffects worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        const BlowReport blow{.source = source,
                              .outcome = outcome,
                              .target = target,
                              .timeStopped = PlayerPowerups::timeStopped(players),
                              .invulnerable = worn.invulnerable(),
                              .invisible = worn.invisible()};
        switch (rules.runEffect(blow).value_or(originalRunEffect(blow))) {
        case RunEffect::Nothing: break;
        case RunEffect::Count: runtime.streak.record(false); break;
        case RunEffect::CountAsOriginal: runtime.streak.record(telling); break;
        }
        return;
    }
}

} // namespace

void PlayerAttacks::countEnemyBlow(const Targets& targets, std::span<PlayerRuntime> players,
                                   s32 player, BlowSource source, const Enemies& enemies, s32 id,
                                   bool stood) {
    // Only a blow on something taller than the short counts towards the run (hht > 2).
    if (enemies.heightOf(id) <= kLowEnemy) {
        return;
    }
    const bool alive = enemies.alive(id);
    applyRunEffect(blowRulesOf(targets), players, player, source,
                   stood && !alive ? BlowOutcome::Killed : BlowOutcome::Hurt, BlowTarget::Enemy,
                   !stood || !alive);
}

void PlayerAttacks::countCritterBlow(const Targets& targets, std::span<PlayerRuntime> players,
                                     s32 player, BlowSource source, const Critters& critters,
                                     s32 id, bool stood) {
    const bool down = !critters.alive(id) || critters.dying(id);
    applyRunEffect(blowRulesOf(targets), players, player, source,
                   stood && down ? BlowOutcome::Killed : BlowOutcome::Hurt, BlowTarget::Critter,
                   !stood || down);
}

void PlayerAttacks::countBossBlow(const Targets& targets, std::span<PlayerRuntime> players,
                                  s32 player, BlowSource source, const BossView& before,
                                  const Bosses& bosses) {
    const bool alive = bosses.view().alive;
    applyRunEffect(blowRulesOf(targets), players, player, source,
                   before.alive && !alive ? BlowOutcome::Killed : BlowOutcome::Hurt,
                   BlowTarget::Boss, !before.alive || !before.awake || !alive);
}

void PlayerAttacks::countGeneratorBlow(const Targets& targets, std::span<PlayerRuntime> players,
                                       s32 player, BlowSource source, const Generators& huts,
                                       s32 id, bool stood) {
    const bool destroyed = stood && !huts.standing(id);
    applyRunEffect(blowRulesOf(targets), players, player, source,
                   destroyed ? BlowOutcome::Destroyed : BlowOutcome::Hurt, BlowTarget::Generator,
                   destroyed);
}

/** What a charge runs into is struck, once each charge. */
void PlayerAttacks::ramBarrels(usize index, std::span<PlayerRuntime> players,
                               const Targets& targets) {
    if (!m_resources.has_value() || index >= players.size()) {
        return;
    }
    const PlayerActor& actor = players[index].actor;
    std::vector<usize>& rammed = players[index].rammed;
    const auto& walls = m_resources->world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        const usize key = static_cast<usize>(kWallTargetBase) + i;
        if (walls.standing(i) && std::ranges::find(rammed, key) == rammed.end() &&
            walls.target(i, 0).touches(actor.followPoint(), actor.radius() + kRamReach)) {
            rammed.push_back(key);
            targets.fixtures.strikeWall(i, kRamDamage);
        }
    }
    for (usize barrel = 0; barrel < targets.fixtures.barrels().size(); ++barrel) {
        if (!targets.fixtures.barrels().standing(barrel) ||
            std::ranges::find(rammed, barrel) != rammed.end() ||
            !targets.fixtures.barrels().barrel(barrel).box.touchedBy(actor.position(),
                                                                     actor.radius(), kRamReach)) {
            continue;
        }
        rammed.push_back(barrel);
        targets.fixtures.strikeBarrel(barrel, kRamDamage, actor.player(), players,
                                      targets.fixtureHost);
    }
    if (auto* imported = m_resources->world.imported()) {
        for (const auto& native : imported->targets()) {
            if (!native.valid() || !native.damageable) {
                continue;
            }
            const auto key =
                static_cast<usize>(kImportedTargetBase) + static_cast<usize>(native.id);
            const MissileTarget body{static_cast<s32>(key), native.base, native.radius,
                                     native.height, native.surface};
            if (std::ranges::find(rammed, key) == rammed.end() &&
                body.touches(actor.followPoint(), actor.radius() + kRamReach)) {
                rammed.push_back(key);
                imported->strike(native.id, kRamDamage, EnemyHit::kKnockDown, actor.player());
            }
        }
    }
    for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
        // Keep the shared per-charge hit ledger disjoint from barrel indices.
        const usize key = rock + static_cast<usize>(kSafeRockTargetBase);
        if (targets.fixtures.safeRocks().standing(rock) &&
            std::ranges::find(rammed, key) == rammed.end() &&
            targets.fixtures.safeRocks().rock(rock).obstacle.touchedBy(actor.position(),
                                                                       actor.radius(), kRamReach)) {
            rammed.push_back(key);
            targets.fixtures.strikeSafeRock(rock, kRamDamage);
        }
    }
    // A body run into is thrown down (PlayerMotion_DamageTarget, 0x20): swarm and great ones,
    // never a boss, each once a charge.
    const f32 blow = PowerupEffects::of(actor.save().progress().inventory).grown() ? 2 * kChargeBlow
                                                                                   : kChargeBlow;
    const Vec3 facing = actor.facing();
    const auto ram = [&](const MissileTarget& target, s32 base, const auto& strike) {
        const usize key = static_cast<usize>(base) + static_cast<usize>(target.id);
        if (std::ranges::find(rammed, key) != rammed.end() ||
            !target.touches(actor.followPoint(), actor.radius() + kRamReach)) {
            return;
        }
        rammed.push_back(key);
        strike(target);
    };
    for (const MissileTarget& target : targets.opponents.enemies().targets()) {
        ram(target, kEnemyTargetBase, [&](const MissileTarget& hit) {
            const Enemies& enemies = targets.opponents.enemies();
            const bool stood = enemies.alive(hit.id);
            targets.opponents.strikeEnemy(hit.id, blow, EnemyHit::kKnockDown, facing,
                                          actor.player(), players, true, hit.base);
            countEnemyBlow(targets, players, actor.player(), BlowSource::Charge, enemies, hit.id,
                           stood);
        });
    }
    for (const MissileTarget& target : targets.opponents.critters().targets()) {
        ram(target, kCritterTargetBase, [&](const MissileTarget& hit) {
            const Critters& critters = targets.opponents.critters();
            const bool stood = critters.alive(hit.id) && !critters.dying(hit.id);
            targets.opponents.strikeCritter(hit.id, blow, EnemyHit::kKnockDown, facing,
                                            actor.player(), hit.base, true, players);
            countCritterBlow(targets, players, actor.player(), BlowSource::Charge, critters, hit.id,
                             stood);
        });
    }
    targets.fixtures.settleBlasts(players, targets.fixtureHost);
}

void PlayerAttacks::comboStart(usize index, std::span<PlayerRuntime> players) {
    if (!m_resources.has_value() || index >= players.size() || !m_resources->weapons.loaded()) {
        return;
    }
    const PlayerRuntime& grabber = players[index];
    const s32 partner = grabber.combo.partner;
    const s32 partnerColor = partner >= 0 && static_cast<usize>(partner) < players.size()
                                 ? players[static_cast<usize>(partner)].actor.save().color
                                 : grabber.actor.save().color;
    const Vec3 at = grabber.actor.position();
    // The sphere takes the partner's colour and lights the ground in it; the burst is the
    // grabber's own (StartComboFX: the type by the grabber, the colour by the partner).
    EffectTrees::Setting sphere;
    sphere.unlit = true;
    sphere.depthWrite = false;
    sphere.tint = LegendShow::chargeTint(partnerColor);
    sphere.light =
        EffectTrees::Light{DynamicLights::ofCostume(partnerColor), DynamicLights::kChargeRadius};
    m_resources->effects.startSet(m_resources->device, m_resources->weapons, LegendShow::kAuraTree,
                                  at, sphere);
    EffectTrees::Setting burst;
    burst.unlit = true;
    burst.depthWrite = false;
    m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                  LegendShow::chargeTree(grabber.actor.save().color), at, burst);
}

bool PlayerAttacks::comboImpact(usize flier, usize thrower, f32 blow,
                                std::span<PlayerRuntime> players, const Targets& targets) {
    if (!m_resources.has_value() || flier >= players.size()) {
        return false;
    }
    const PlayerActor& actor = players[flier].actor;
    std::vector<usize>& struck = players[flier].rammed;
    bool hit = false;
    bool any = false;
    // Each thing once a flight; what stops the body (hitKind 1) and what it passes through.
    const auto once = [&](usize key, bool stops, const auto& strike) {
        if (std::ranges::find(struck, key) != struck.end()) {
            return;
        }
        struck.push_back(key);
        strike();
        any = true;
        hit = hit || stops;
    };
    const auto& walls = m_resources->world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        if (walls.standing(i) &&
            walls.target(i, 0).touches(actor.followPoint(), actor.radius() + kRamReach)) {
            once(static_cast<usize>(kWallTargetBase) + i, true,
                 [&] { targets.fixtures.strikeWall(i, blow, EnemyHit::kKnockDown); });
        }
    }
    for (usize barrel = 0; barrel < targets.fixtures.barrels().size(); ++barrel) {
        if (targets.fixtures.barrels().standing(barrel) &&
            targets.fixtures.barrels().barrel(barrel).box.touchedBy(actor.position(),
                                                                    actor.radius(), kRamReach)) {
            once(barrel, true, [&] {
                targets.fixtures.strikeBarrel(barrel, blow, actor.player(), players,
                                              targets.fixtureHost);
            });
        }
    }
    if (auto* imported = m_resources->world.imported()) {
        for (const auto& native : imported->targets()) {
            if (!native.valid() || !native.damageable) {
                continue;
            }
            const s32 id = kImportedTargetBase + native.id;
            const MissileTarget body{id, native.base, native.radius, native.height, native.surface};
            if (body.touches(actor.followPoint(), actor.radius() + kRamReach)) {
                once(static_cast<usize>(id), true, [&] {
                    imported->strike(native.id, blow, EnemyHit::kKnockDown, actor.player());
                });
            }
        }
    }
    for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
        if (targets.fixtures.safeRocks().standing(rock) &&
            targets.fixtures.safeRocks().rock(rock).obstacle.touchedBy(actor.position(),
                                                                       actor.radius(), kRamReach)) {
            once(rock + static_cast<usize>(kSafeRockTargetBase), true,
                 [&] { targets.fixtures.strikeSafeRock(rock, blow); });
        }
    }
    const Generators& generators = targets.opponents.generators();
    for (usize g = 0; g < generators.count(); ++g) {
        const auto id = static_cast<s32>(g);
        if (generators.standing(id) &&
            generators.boxOf(id).touchedBy(actor.position(), actor.radius(), kRamReach)) {
            once(static_cast<usize>(kGeneratorTargetBase) + g, true, [&] {
                if (blowRulesOf(targets).generatorTakes(BlowSource::Charge)) {
                    targets.opponents.strikeGenerator(id, blow, actor.player(), players);
                }
            });
        }
    }
    // A body run into is thrown down (PlayerMotion_DamageTarget, 0x20) within the sweep's
    // reach, four about a pinball and three about a charger (pmotion.c 1360), a pinball's
    // blow or a charger's twenty, twice for a grown body. The swarm is passed through; a
    // critter alone stops and turns the body (pmotion.c 1407).
    const bool pinball = ComboMove::flies(players[flier].combo);
    const f32 reach = pinball ? ComboMove::kPinballReach : ComboMove::kChargerReach;
    const f32 bodyBlow =
        (pinball ? blow : ComboMove::kChargerBodyBlow) *
        (PowerupEffects::of(actor.save().progress().inventory).grown() ? 2.0f : 1.0f);
    const Vec3 facing = actor.facing();
    for (const MissileTarget& target : targets.opponents.enemies().targets()) {
        if (target.touches(actor.followPoint(), reach)) {
            once(static_cast<usize>(kEnemyTargetBase) + static_cast<usize>(target.id), false, [&] {
                const Enemies& enemies = targets.opponents.enemies();
                const bool stood = enemies.alive(target.id);
                targets.opponents.strikeEnemy(target.id, bodyBlow, EnemyHit::kKnockDown, facing,
                                              actor.player(), players, true, target.base);
                countEnemyBlow(targets, players, actor.player(), BlowSource::Charge, enemies,
                               target.id, stood);
            });
        }
    }
    for (const MissileTarget& target : targets.opponents.critters().targets()) {
        if (target.touches(actor.followPoint(), reach)) {
            once(static_cast<usize>(kCritterTargetBase) + static_cast<usize>(target.id), true, [&] {
                const Critters& critters = targets.opponents.critters();
                const bool stood = critters.alive(target.id) && !critters.dying(target.id);
                targets.opponents.strikeCritter(target.id, bodyBlow, EnemyHit::kKnockDown, facing,
                                                actor.player(), target.base, true, players);
                countCritterBlow(targets, players, actor.player(), BlowSource::Charge, critters,
                                 target.id, stood);
            });
        }
    }
    if (any && thrower < players.size()) {
        // The thrower's combo-hit row bursts where its partner struck (pmotion.c 1306).
        if (const ClassStats* stats =
                m_resources->classes.stats(players[thrower].actor.save().character)) {
            fireStrike(thrower, stats->moves.comboHit, players, actor.position());
        }
    }
    return hit;
}

/** The costume colour's effects, which hold the trees a class's moves show; loaded when
 * first wanted. */
ItemArchive* PlayerAttacks::moveEffectsOf(usize index, std::span<PlayerRuntime> players) {
    PlayerFigure* figure = index < players.size() ? players[index].figure.get() : nullptr;
    return figure != nullptr ? figure->effects() : nullptr;
}

ItemArchive* PlayerAttacks::moveEffectArchive(usize index, std::span<PlayerRuntime> players,
                                              std::string_view tree) {
    // InitCustomEffectSub falls back from class trees to WEAPONS. NULLFX is
    // invisible, but its shared animation still supplies the damage lifetime.
    if (auto* archive = moveEffectsOf(index, players);
        archive != nullptr && archive->trees.find(tree)) {
        return archive;
    }
    return m_resources && m_resources->weapons.trees.find(tree) ? &m_resources->weapons : nullptr;
}

/** Base melee damage, also used by the authored effect-strike multipliers. */
f32 PlayerAttacks::meleeDamageOf(usize index, std::span<const PlayerRuntime> players) const {
    if (!m_resources.has_value() || index >= players.size()) {
        return PlayerMissiles::kLeastDamage;
    }
    const CharacterSave& save = players[index].actor.save();
    const ClassStats* stats = m_resources->classes.stats(save.character);
    if (stats == nullptr) {
        return PlayerMissiles::kLeastDamage;
    }
    const StatBlock block = characterStats(*stats, save);
    // PlyrSfxDoDamageSub (80089B30) scales negative DAMG amounts by Player+0x104
    // (stat_damage), not +0x114 (stat_missile_dmg). Even caster effects use Strength;
    // their ordinary projectiles use Magic separately in PlayerArsenal.
    return PlayerMissiles::damageFor(block.strength());
}

/** One strike of a move: its effects show and sound where the character stands, the meter
 * pays what the move still owes if the strike does harm, and the harm is set going. */
void PlayerAttacks::fireStrike(usize index, s32 strikeIndex, std::span<PlayerRuntime> players,
                               std::optional<Vec3> at, std::optional<usize> occurrence) {
    const auto prepared = prepareStrike(index, strikeIndex, players, at);
    if (!prepared) {
        return;
    }
    const StrikeEvents events{
        .tree =
            [this](ItemArchive& archive, std::string_view tree, const Vec3& position,
                   const EffectTrees::Setting& setting) {
                return std::optional{m_resources->effects.startSet(m_resources->device, archive,
                                                                   tree, position, setting)};
            },
        .particle =
            [this](ItemArchive& archive, const ParticleDescriptor& descriptor, u32 texture) {
                return std::optional{m_resources->effects.startParticles(
                    m_resources->device, archive, descriptor, texture, Mat4{1})};
            },
        .sound =
            [this, players](usize actor, usize, std::string_view sound) {
                if (players[actor].figure->voice().find(sound)) {
                    m_resources->audio.playFrom(players[actor].figure->voice(), sound);
                } else {
                    m_resources->audio.playNamed(sound);
                }
            },
        .shake =
            [this] {
                if (m_resources->shake != nullptr) {
                    m_resources->shake->start();
                }
            }};
    applyStrike(index, *prepared, players, occurrence, events, at);
}

std::optional<TurboEmission::Strike> PlayerAttacks::prepareStrike(usize index, s32 strikeIndex,
                                                                  std::span<PlayerRuntime> players,
                                                                  std::optional<Vec3> at) {
    if (!m_resources.has_value() || index >= players.size() || players[index].figure == nullptr) {
        return std::nullopt;
    }
    const ClassStats* stats = m_resources->classes.stats(players[index].actor.save().character);
    if (stats == nullptr || strikeIndex < 0 ||
        static_cast<usize>(strikeIndex) >= stats->moveStrikes.size()) {
        return std::nullopt;
    }
    const MoveStrike& strike = stats->moveStrikes[static_cast<usize>(strikeIndex)];
    const PlayerActor& actor = players[index].actor;
    const Vec3 facing = actor.facing();
    const Vec3 base = at.value_or(actor.position());
    f32 effectSeconds = 0;
    if (strike.effect >= 0 && static_cast<usize>(strike.effect) < stats->moveEffects.size()) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(strike.effect)];
        if (const auto* archive = moveEffectArchive(index, players, effect.tree)) {
            // DoPlyrSfx gives the primary tree's authored maxlen to the same effect
            // that carries damage. Animation length is only its untimed fallback.
            effectSeconds = TurboEmission::effectSeconds(
                effect, &archive->trees.tree(*archive->trees.find(effect.tree)));
        }
    }
    const s32 partner = players[index].combo.partner;
    const bool partnerAvailable = partner >= 0 && static_cast<usize>(partner) < players.size();
    const f32 meleeDamage = meleeDamageOf(index, players);
    return TurboEmission::prepare(
        *stats, strikeIndex,
        {actor.save().character, base, facing, at.has_value(), partnerAvailable}, meleeDamage,
        effectSeconds);
}

bool PlayerAttacks::applyStrike(usize index, const TurboEmission::Strike& prepared,
                                std::span<PlayerRuntime> players, std::optional<usize> occurrence,
                                const StrikeEvents& events, std::optional<Vec3> at) {
    const auto& actor = players[index].actor;
    const Vec3 base = at.value_or(actor.position());
    const Vec3 facing = actor.facing();
    const s32 partner = players[index].combo.partner;
    const bool partnerAvailable = partner >= 0 && static_cast<usize>(partner) < players.size();
    // A span that only lasts, or a volley, harms nothing of itself; the rest are set going.
    u32 id = 0;
    if (prepared.carrier) {
        id = m_strikes.start(prepared.volume, actor.player(), base, facing,
                             meleeDamageOf(index, players), prepared.effectSeconds);
        m_sim.strikeSources.push_back(StrikeSource{id,
                                                   index,
                                                   prepared.row,
                                                   {},
                                                   m_resources->multiplayer != nullptr
                                                       ? *m_resources->multiplayer
                                                       : MultiplayerMode::Normal,
                                                   prepared.volume.damageType});
    }
    // An effect may bring another with it.
    u32 previousEffect = 0;
    const auto& animator = players[index].figure->animator();
    const auto tag = [&](u32 effect, usize ordinal) {
        if (effect != 0 && occurrence && *occurrence < TurboMove::kMaxCheckpointRows &&
            ordinal <= std::numeric_limits<u16>::max() && !at && !partnerAvailable &&
            PlayerAnimator::rollbackTurboAction(animator.action())) {
            m_resources->effects.tag(effect,
                                     {actor.player(), animator.player().generation(),
                                      static_cast<u16>(*occurrence), static_cast<u16>(ordinal)});
        }
    };
    for (usize ordinal = 0; ordinal < prepared.effects.size(); ++ordinal) {
        const auto& emission = prepared.effects[ordinal];
        const MoveEffect& effect = emission.authored;
        if (emission.shake && events.shake) {
            events.shake();
        }
        if (emission.particle) {
            const u32 shown =
                startParticles(index, *emission.particle, previousEffect, players, events);
            tag(shown, ordinal);
            previousEffect = 0;
            continue;
        }
        previousEffect = 0;
        if (!effect.sound.empty() && events.sound) {
            events.sound(index, ordinal, effect.sound);
        }
        std::optional<MoveAttachment> attachment;
        if (emission.attachment) {
            const auto& authored = *emission.attachment;
            MoveAttachment parent;
            parent.actor = authored.actor == TurboEmission::Actor::Partner
                               ? static_cast<usize>(partner)
                               : index;
            parent.strike = authored.carrier ? id : 0;
            parent.bodyParent = authored.bodyParent;
            parent.local = authored.local;
            attachment = parent;
        }
        ItemArchive* archive = moveEffectArchive(index, players, effect.tree);
        if (effect.tree.empty() || effect.tree == kNoEffectTree || archive == nullptr) {
            if (attachment && attachment->strike != 0) {
                m_sim.moveAttachments.push_back(*attachment);
            }
            continue;
        }
        const auto started =
            events.tree(*archive, effect.tree, emission.position, emission.setting);
        if (!started) {
            return false;
        }
        const u32 shown = *started;
        tag(shown, ordinal);
        previousEffect = shown;
        if (attachment && (shown != 0 || attachment->strike != 0)) {
            attachment->effect = shown;
            m_sim.moveAttachments.push_back(*attachment);
        }
        if (shown != 0 && emission.carriesFlight) {
            m_sim.strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
    updateMoveAttachments(players);
    return true;
}

void PlayerAttacks::updateMoveAttachments(std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_sim.moveAttachments, [&](const MoveAttachment& attachment) {
        auto& effects = m_resources->effects;
        if (!effects.playing(attachment.effect) && m_strikes.find(attachment.strike) == nullptr) {
            return true;
        }
        if (attachment.actor >= players.size() || players[attachment.actor].figure == nullptr ||
            players[attachment.actor].life == PlayerLife::InTower ||
            players[attachment.actor].departed) {
            effects.stop(attachment.effect);
            m_strikes.stop(attachment.strike);
            return true;
        }
        const auto& player = players[attachment.actor];
        const auto& save = player.actor.save();
        const Mat4 body =
            PlayerFigure::bodyPlacement(player.capture.body().value_or(player.actor.transform()),
                                        save, PowerupEffects::of(save.progress().inventory));
        const Mat4 parent = attachment.bodyParent ? body : player.figure->rootAttachment(body);
        const Mat4 placement = parent * attachment.local;
        effects.placeAt(attachment.effect, placement);
        m_strikes.placeArea(attachment.strike, placement);
        return false;
    });
}

u32 PlayerAttacks::startParticles(usize index, const TurboEmission::Particle& emission, u32 parent,
                                  std::span<PlayerRuntime> players, const StrikeEvents& events) {
    if (!m_resources || index >= players.size()) {
        return 0;
    }
    ItemArchive* archive = moveEffectsOf(index, players);
    ItemArchive* textureArchive = nullptr;
    u32 textureSlot = 0;
    for (ItemArchive* candidate : {archive, &m_resources->weapons}) {
        if (candidate == nullptr) {
            continue;
        }
        const auto slot = candidate->textures.find(emission.descriptor.texture);
        if (slot && !candidate->textures.entry(*slot).external() &&
            !candidate->textures.entry(*slot).noPicture) {
            textureArchive = candidate;
            textureSlot = *slot;
            break;
        }
    }
    if (textureArchive == nullptr) {
        log::warn("Player particles: no texture {}", emission.descriptor.texture);
        return 0;
    }
    ParticleEffect particle;
    particle.actor = index;
    particle.parent = emission.previousEffect ? parent : 0;
    particle.node = emission.node;
    particle.offset = emission.offset;
    if (particle.parent == 0 && emission.partnerFallback) {
        const s32 partner = players[index].combo.partner;
        if (partner >= 0 && static_cast<usize>(partner) < players.size()) {
            particle.actor = static_cast<usize>(partner);
            particle.node.clear();
        }
    }
    const auto started = events.particle(*textureArchive, emission.descriptor, textureSlot);
    if (!started) {
        return 0;
    }
    particle.effect = *started;
    const u32 shown = particle.effect;
    m_sim.particleEffects.push_back(std::move(particle));
    updateParticles(players);
    return shown;
}

void PlayerAttacks::updateParticles(std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_sim.particleEffects, [&](const ParticleEffect& particle) {
        auto& effects = m_resources->effects;
        if (!effects.playing(particle.effect)) {
            return true;
        }
        if (particle.actor >= players.size() || players[particle.actor].figure == nullptr ||
            players[particle.actor].life == PlayerLife::InTower ||
            players[particle.actor].departed) {
            effects.stop(particle.effect);
            return true;
        }
        const auto& player = players[particle.actor];
        const auto& save = player.actor.save();
        Mat4 placement =
            PlayerFigure::bodyPlacement(player.capture.body().value_or(player.actor.transform()),
                                        save, PowerupEffects::of(save.progress().inventory));
        if (particle.parent != 0) {
            bool found = false;
            for (usize i = 0; i < effects.count(); ++i) {
                if (effects.effect(i).id == particle.parent) {
                    placement = effects.effect(i).transform();
                    found = true;
                    break;
                }
            }
            if (!found) {
                effects.stop(particle.effect);
                return true;
            }
        } else if (!particle.node.empty()) {
            placement = player.figure->attachment(placement, particle.node).value_or(placement);
        }
        effects.placeAt(particle.effect, glm::translate(placement, particle.offset));
        return false;
    });
}

/** All target families share the strike's cosine cone and swept cylinder contacts. */
void PlayerAttacks::presentStrikeHit(usize index, const MoveStrike& row, const Vec3& at,
                                     std::span<PlayerRuntime> players, bool sound) {
    if (!m_resources || index >= players.size()) {
        return;
    }
    ItemArchive* archive = moveEffectsOf(index, players);
    const ClassStats* stats = m_resources->classes.stats(players[index].actor.save().character);
    if (archive == nullptr || stats == nullptr || row.hitEffect < 0 ||
        static_cast<usize>(row.hitEffect) >= stats->moveEffects.size()) {
        return;
    }
    const MoveEffect& effect = stats->moveEffects[static_cast<usize>(row.hitEffect)];
    if ((effect.flags & effect_flags::kShakes) != 0 && m_resources->shake != nullptr) {
        m_resources->shake->start();
    }
    if (!effect.tree.empty() && archive->trees.find(effect.tree).has_value()) {
        m_resources->effects.start(m_resources->device, *archive, effect.tree, at, effect.scale);
    }
    if (sound && !effect.sound.empty()) {
        m_resources->audio.playNamed(effect.sound);
    }
}

void PlayerAttacks::updateStrikes(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    if (!m_resources) {
        return;
    }
    for (const auto& hit : advanceStrikes(seconds, players)) {
        dispatchStrikeHit(hit, players, targets);
    }
    targets.fixtures.settleBlasts(players, targets.fixtureHost);
    finishStrikes();
}

std::vector<StrikeHit> PlayerAttacks::advanceStrikes(f32 seconds,
                                                     std::span<PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return {};
    }
    updateParticles(players);
    updateMoveAttachments(players);
    for (auto& source : m_sim.strikeSources) {
        for (auto& contact : source.contacts) {
            contact.remaining -= seconds;
        }
    }
    return m_strikes.update(seconds, &m_resources->world.collision());
}

std::vector<MissileTarget> PlayerAttacks::strikeContacts(const StrikeHit& hit,
                                                         const StrikeSource& source,
                                                         const Targets& targets) const {
    std::vector<MissileTarget> result;
    for (const auto& target : strikeTargets(targets)) {
        MissileTarget contactTarget = target;
        if (!hit.swept && target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase) {
            // Stationary swarm contacts are planar; flying/node contacts retain height.
            contactTarget.base.y = hit.centre.y;
        }
        if (!contactTarget.reachedBy(hit)) {
            continue;
        }
        const auto contact =
            std::ranges::find(source.contacts, target.id, &StrikeSource::Contact::target);
        if (contact == source.contacts.end() || contact->remaining <= 0) {
            result.push_back(target);
        }
    }
    return result;
}

void PlayerAttacks::dispatchStrikeHit(const StrikeHit& hit, std::span<PlayerRuntime> players,
                                      const Targets& targets) {
    const auto source = std::ranges::find(m_sim.strikeSources, hit.strike, &StrikeSource::strike);
    if (source == m_sim.strikeSources.end() || source->actor >= players.size()) {
        return;
    }
    const auto& owner = players[source->actor].actor;
    const ClassStats* stats = m_resources->classes.stats(owner.save().character);
    if (stats == nullptr || source->row < 0 ||
        static_cast<usize>(source->row) >= stats->moveStrikes.size()) {
        return;
    }
    MoveStrike row = stats->moveStrikes[static_cast<usize>(source->row)];
    row.damageType = source->damageType;
    bool stoppedByPlayer = false;
    if (source->multiplayer == MultiplayerMode::Hurt && (row.damageType & damage::kMagic) == 0) {
        for (usize index = 0; index < players.size(); ++index) {
            auto& player = players[index];
            if (player.actor.player() == hit.owner || player.life != PlayerLife::Standing ||
                player.departed) {
                continue;
            }
            const Vec3 toward = player.actor.position() - hit.centre;
            const f32 across = portable::hypot(toward.x, toward.z);
            StrikeHit contact = hit;
            constexpr f32 kNearShare = 0.3f;
            constexpr f32 kNearCone = 0.85f;
            if (!hit.swept && across < kNearShare * (hit.radius + player.actor.radius())) {
                contact.arc *= kNearCone;
            }
            if (!contact.reaches(player.actor.position(), player.actor.radius(),
                                 player.actor.height())) {
                continue;
            }
            constexpr f32 kCoverFrom = 10.0f;
            if (!hit.swept && across > kCoverFrom &&
                playerAreaBlocked(hit.centre, player.actor.followPoint(), targets.fixtures,
                                  m_resources->world)) {
                continue;
            }
            const u32 armor = PowerupEffects::of(player.actor.save().progress().inventory).armor;
            if (hit.swept && (armor & armor::kReflects) != 0) {
                m_strikes.hitPlayer(hit.strike, true, hit.from);
                m_resources->audio.playNamed("S_RICOCHET");
                for (const auto& effect : m_sim.strikeEffects) {
                    if (effect.strike == hit.strike) {
                        m_resources->effects.redirect(effect.effect, hit.from,
                                                      -hit.facing * row.speed);
                        m_resources->effects.shortenLifetime(effect.effect, 1, 10);
                    }
                }
                stoppedByPlayer = true;
                break;
            }
            if (player.effectGap <= 0) {
                constexpr f32 kAreaPush = 0.25f;
                Vec3 direction = hit.swept ? hit.facing : Vec3{toward.x, 0, toward.z};
                if (!hit.swept && across > 0) {
                    direction *= kAreaPush / across;
                }
                u32 flags = row.damageType;
                if (!hit.swept && hit.damage < 5) {
                    flags = (flags & ~damage::kKnocks) | damage::kNoHitEffect;
                }
                targets.attackHost.hurtByAttack(index, hit.damage, HurtKind::Blow,
                                                {flags, direction});
                if ((row.damageType & damage::kGas) != 0) {
                    player.effectGap = kGasEffectGap;
                } else if (hit.damage > 2) {
                    f32 gap = hit.hitGap;
                    if (hit.swept) {
                        const bool passThrough = (row.damageType & damage::kSuperShot) != 0;
                        const std::string_view tree =
                            row.hitEffect >= 0 &&
                                    static_cast<usize>(row.hitEffect) < stats->moveEffects.size()
                                ? stats->moveEffects[static_cast<usize>(row.hitEffect)].tree
                                : std::string_view{};
                        gap = passThrough ? 1.0f
                                          : PlayerMissiles::hitGap(
                                                moveEffectsOf(source->actor, players), tree);
                    }
                    player.effectGap = gap;
                }
            }
            if (hit.swept && (row.damageType & damage::kSuperShot) != 0 &&
                (row.damageType & damage::kReflect) != 0 && row.hitEffect >= 0 && hit.damage > 2) {
                presentStrikeHit(source->actor, row, player.actor.position(), players, false);
                source->damageType &= ~damage::kSuperShot;
            }
            if (hit.swept && (row.damageType & damage::kSuperShot) == 0) {
                presentStrikeHit(source->actor, row, player.actor.followPoint(), players);
                m_strikes.hitPlayer(hit.strike, false, hit.from);
                stoppedByPlayer = true;
                break;
            }
        }
    }
    if (stoppedByPlayer) {
        return;
    }
    targets.fixtures.shootScenery(hit.centre, hit.radius);
    for (const MissileTarget& target : strikeContacts(hit, *source, targets)) {
        auto contact =
            std::ranges::find(source->contacts, target.id, &StrikeSource::Contact::target);
        // Several native body nodes may share one target ID. Earlier nodes
        // in this same batch can establish its cooldown.
        if (contact != source->contacts.end() && contact->remaining > 0) {
            continue;
        }
        // A pass-through wave must not deal its full damage every render frame.
        constexpr f32 kProjectileGap = 0.25f;
        constexpr f32 kPassThroughExtraGap = 3.0f;
        const f32 baseGap = hit.swept ? kProjectileGap : hit.hitGap;
        const bool passThrough = (row.damageType & damage::kSuperShot) != 0;
        const f32 gap =
            hit.damage > 2 ? baseGap + (passThrough ? kPassThroughExtraGap : 0.0f) : 0.0f;
        if (contact == source->contacts.end()) {
            source->contacts.push_back({target.id, gap});
        } else {
            contact->remaining = gap;
        }
        Vec3 effectPoint = target.base;
        if (target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase) {
            const s32 kind = targets.opponents.enemies().kindOf(target.id - kEnemyTargetBase);
            if (kind >= 0 && kind < kEnemyKindCount) {
                // ProcessEffects starts the impact at OBJGRP.attn_pos, not
                // the collision cylinder's floor. Acid Raid's rain hangs
                // below its cloud and otherwise disappears inside the body.
                effectPoint.y += enemyKind(kind).attentionHeight;
            }
        }
        strikeTarget(target, hit.damage, row.damageType, owner, players, targets,
                     BlowSource::Turbo);
        presentStrikeHit(source->actor, row, effectPoint, players);
    }
}

void PlayerAttacks::finishStrikes() {
    std::erase_if(m_sim.strikeSources, [this](const StrikeSource& source) {
        return m_strikes.find(source.strike) == nullptr;
    });
    std::erase_if(m_sim.strikeEffects, [this](const StrikeEffect& pair) {
        if (m_strikes.find(pair.strike) != nullptr) {
            return false;
        }
        m_resources->effects.stop(pair.effect);
        return true;
    });
}

/** A potion spent on a shield: its magic rings the character for a few seconds, going about
 * with them, to the potion's shield sound. */
void PlayerAttacks::shieldPotion(usize index, std::span<PlayerRuntime> players) {
    if (!m_resources.has_value() || index >= players.size()) {
        return;
    }
    PlayerActor& actor = players[index].actor;
    auto& inventory = actor.save().progress().inventory;
    if (inventory.potions.empty()) {
        return;
    }
    const s32 stored = inventory.takePotion();
    const s32 kind = m_resources->arsenal.resolvePotionKind(stored);
    const auto look = static_cast<usize>(std::clamp(kind, 0, 4));
    const f32 power = m_resources->arsenal.potionPowerOf(actor, stored);
    PotionShield shield;
    shield.actor = index;
    shield.radius = kShieldPotency * power;
    shield.damage =
        kShieldDamage * damage::colourBonus(actor.save().color, static_cast<u32>(stored));
    shield.flags = EnemyHit::kMagic | static_cast<u32>(kind) |
                   damage::magicHeal(experienceLevel(actor.save().experience()));
    shield.secondsLeft = kShieldSeconds;
    if (m_resources->weapons.loaded() &&
        m_resources->weapons.trees.find(kShieldTrees[look]).has_value()) {
        EffectTrees::Setting setting;
        setting.stretch = PotionEmission::shieldScale(shield.radius);
        setting.seconds = kShieldSeconds;
        setting.light = EffectTrees::Light{DynamicLights::ofPotion(static_cast<s32>(look)),
                                           DynamicLights::kMagicRadiusPerPower * shield.radius};
        shield.effect =
            m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                          kShieldTrees[look], actor.position(), setting);
    }
    m_resources->audio.playNamed(kShieldSounds[look]);
    m_resources->audio.playNamed("S_TURBODEFENSE", LevelSoundscape::kStepVolume);
    m_sim.shields.push_back(shield);
}

void PlayerAttacks::stopDeathSounds(std::span<PlayerRuntime> players) {
    if (!m_resources.has_value() || m_resources->audio.recordingSounds()) {
        return;
    }
    for (auto& player : players) {
        m_resources->audio.stop(player.deathHeldSuck);
        m_resources->audio.stop(player.deathHeldCry);
        player.deathHeldSuck = kNoSound;
        player.deathHeldCry = kNoSound;
    }
}

void PlayerAttacks::gatherLoops() const {
    if (!m_resources || !m_resources->audio.recordingSounds()) {
        return;
    }
    for (const auto& emitter : m_sim.deathSounds) {
        if (emitter) {
            m_resources->audio.desireNamedLoop(
                {*emitter, 1, 1}, kDeathCry, SceneSoundPlayback::RepeatWhileDesired, kDeathVolume);
            m_resources->audio.desireNamedLoop(
                {*emitter, 2, 1}, kDeathSuck, SceneSoundPlayback::RepeatWhileDesired, kDeathVolume);
        }
    }
}

std::optional<s32> PlayerAttacks::deathContactSlot(usize index,
                                                   std::span<const PlayerRuntime> players,
                                                   const Targets& targets) const {
    if (!m_resources.has_value() || index >= players.size()) {
        return std::nullopt;
    }
    const PlayerRuntime& runtime = players[index];
    const PlayerActor& actor = runtime.actor;
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    if (runtime.life != PlayerLife::Standing || (worn.armor & DeathRules::kProtection) == 0) {
        return std::nullopt;
    }
    const Vec3 facing = actor.facing();
    const f32 facingLength = portable::hypot(facing.x, facing.z);
    std::optional<MissileTarget> nearest;
    f32 best = kGrabReach;
    for (const MissileTarget& target : projectileTargets(targets)) {
        const Vec3 toward = target.base - actor.position();
        const f32 flat = portable::hypot(toward.x, toward.z);
        if (flat < 1e-4f || facingLength < 1e-4f) {
            continue;
        }
        const bool held =
            runtime.deathHeld >= 0 && target.id == kEnemyTargetBase + runtime.deathHeld;
        // PlayerGetTarget retains an acquired enemy with a 3D dot of 0.5.
        // closest_enemy instead narrows its acquisition cone toward maximum
        // range and rejects enemies more than ten units above or below.
        const f32 length = glm::length(toward);
        const f32 distance = length - target.radius;
        const f32 dot = (toward.x * facing.x + toward.z * facing.z) / facingLength;
        const f32 threshold = held ? length * kHeldCone
                                   : flat * (kGrabCone + distance * (1 - kGrabCone) / kGrabReach);
        constexpr f32 kAcquisitionHeight = 10;
        if ((!held && std::abs(toward.y) > kAcquisitionHeight) || dot < threshold) {
            continue;
        }
        if (distance < best) {
            best = distance;
            nearest = target;
        }
    }
    const Enemies& enemies = targets.opponents.enemies();
    if (!nearest || nearest->id < kEnemyTargetBase || nearest->id >= kGeneratorTargetBase) {
        return std::nullopt;
    }
    const s32 slot = nearest->id - kEnemyTargetBase;
    if (enemies.kindOf(slot) != kDeathKind || !enemies.alive(slot) || enemies.dying(slot)) {
        return std::nullopt;
    }
    const Vec3 separation = nearest->base - actor.position();
    constexpr f32 kContactTolerance = 0.001f;
    if (portable::hypot(separation.x, separation.z) >
            actor.radius() + nearest->radius + kContactTolerance ||
        nearest->base.y > actor.position().y + actor.height() ||
        nearest->base.y + nearest->height < actor.position().y) {
        return std::nullopt;
    }
    return slot;
}

std::optional<Vec3> PlayerAttacks::deathContact(usize index, std::span<const PlayerRuntime> players,
                                                const Targets& targets) const {
    if (const auto slot = deathContactSlot(index, players, targets)) {
        return targets.opponents.enemies().positionOf(*slot);
    }
    return std::nullopt;
}

/** The halo holds Death only while the bodies touch and he remains the target ahead.
 * Each 30 Hz frame draws one point: health back, or experience from his black form.
 * Ranged target acquisition alone never grants a remote drain. */
std::optional<Vec3> PlayerAttacks::grabDeath(usize index, s32 ticks, bool allowed,
                                             std::span<PlayerRuntime> players,
                                             const Targets& targets) {
    if (!m_resources.has_value() || index >= players.size()) {
        return std::nullopt;
    }
    PlayerRuntime& runtime = players[index];
    const s32 seat = runtime.actor.player();
    auto& audio = m_resources->audio;
    runtime.deathDrainTicks -= std::max(ticks, 0);
    const auto release = [&]() -> std::optional<Vec3> {
        runtime.deathHeld = -1;
        m_resources->effects.stop(runtime.deathHeldEffect);
        runtime.deathHeldEffect = 0;
        if (seat >= 0 && seat < FrameEventEmitter::kNoSeat) {
            m_sim.deathSounds[static_cast<usize>(seat)].reset();
        }
        if (!audio.recordingSounds()) {
            audio.stop(runtime.deathHeldSuck);
            runtime.deathHeldSuck = kNoSound;
            audio.stop(runtime.deathHeldCry);
            runtime.deathHeldCry = kNoSound;
        }
        return std::nullopt;
    };
    const PlayerActor& actor = runtime.actor;
    // PlayerMotion's speak_done latch survives losing contact (80085450).
    // PlayerProcessPowerups clears it only when the halo is no longer worn.
    if ((PowerupEffects::of(actor.save().progress().inventory).armor & DeathRules::kProtection) ==
        0) {
        runtime.deathHaloHeard = false;
    }
    const auto contact = allowed ? deathContactSlot(index, players, targets) : std::nullopt;
    if (!contact) {
        // Never bank damage while apart. Keep a pending cooldown, though, so
        // briefly losing contact cannot restart the 30 Hz damage cadence.
        runtime.deathDrainTicks = std::max(runtime.deathDrainTicks, 0);
        return release();
    }
    const s32 slot = *contact;
    Enemies& enemies = targets.opponents.enemies();
    const auto source = enemies.observe(slot);
    const FrameEventEmitter emitter{21, static_cast<u64>(slot), source ? source->instance : 0,
                                    static_cast<u8>(seat)};
    auto soundScope = audio.soundScope(emitter, 1);
    if (runtime.deathHeld != slot) {
        release();
        runtime.deathHeld = slot;
        if (!runtime.deathHaloHeard) {
            m_resources->audio.playNamed(kHaloSound, kHaloVolume);
            runtime.deathHaloHeard = true;
        }
    }
    const Vec3 at = enemies.positionOf(slot);
    // First contact drains immediately, as PlayerMotion does, including a
    // one-tick contact at 60 Hz. Accumulating two touching ticks before the
    // first point loses every short contact with a retreating Death.
    while (runtime.deathDrainTicks < 0 && enemies.alive(slot) && !enemies.dying(slot)) {
        runtime.deathDrainTicks += kTicksPerFrame;
        targets.opponents.strikeEnemy(slot, 0.0f, 0, at - actor.position(), actor.player(),
                                      players);
    }
    if (!enemies.alive(slot) || enemies.dying(slot)) {
        return release();
    }
    if (audio.recordingSounds()) {
        if (!emitter.valid() || seat < 0 || seat >= FrameEventEmitter::kNoSeat) {
            audio.recorder()->unsupported("anti-Death drain without its source lifetime");
        }
        m_sim.deathSounds[static_cast<usize>(seat)] = emitter;
    } else {
        SoundPlayer* sounds = m_resources->sounds;
        if (sounds == nullptr || !sounds->isPlaying(runtime.deathHeldCry)) {
            runtime.deathHeldCry = audio.playNamed(kDeathCry, kDeathVolume);
        }
        if (sounds == nullptr || !sounds->isPlaying(runtime.deathHeldSuck)) {
            runtime.deathHeldSuck = audio.playNamed(kDeathSuck, kDeathVolume);
        }
    }
    if (runtime.deathHeldEffect == 0) {
        if (ItemArchive* archive = enemies.archive(kDeathKind); archive != nullptr) {
            EffectTrees::Setting settings;
            settings.persistent = true;
            settings.depthWrite = false;
            settings.yaw = actor.yaw();
            runtime.deathHeldEffect = m_resources->effects.startSet(
                m_resources->device, *archive,
                DeathRules::effect(DeathRules::form(enemies.tierOf(slot))), actor.position(),
                settings);
        }
    } else {
        // StartDeathFX hangs the arc on the player's node: it goes where they go and turns
        // as they turn, toward Death, the way Death's own arc turns toward its victim.
        m_resources->effects.placeAt(runtime.deathHeldEffect,
                                     portable::rotate(glm::translate(Mat4{1.0f}, actor.position()),
                                                      actor.yaw(), Vec3{0.0f, 1.0f, 0.0f}));
    }
    return at;
}

/** The fire and lightning shields (PlayerMotion, pmotion.c 1641): against a creature within
 * a unit of the body, a bearer free to go about burns it 3 a 30 Hz frame, never waiting, or
 * shocks it for 20 (lightning, knocking down) at most once a second, the spark reaching
 * from the shield to it; the fire shield first. Doubled while grown. */
void PlayerAttacks::updateArmour(f32 seconds, std::span<PlayerRuntime> players,
                                 const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
    std::vector<MissileTarget> creatures;
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& runtime = players[i];
        for (PlayerRuntime::ShockGap& gap : runtime.shockGaps) {
            gap.seconds -= seconds;
        }
        std::erase_if(runtime.shockGaps,
                      [](const PlayerRuntime::ShockGap& gap) { return gap.seconds <= 0.0f; });
        if (runtime.life != PlayerLife::Standing || runtime.figure == nullptr) {
            continue;
        }
        const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
        const bool fire = (worn.armor & armor::kFireArmor) != 0;
        const bool lightning = (worn.armor & armor::kElectricArmor) != 0;
        const PlayerAnimator& body = runtime.figure->animator();
        if ((!fire && !lightning) || body.meleeing() || body.throwing() || body.conjuring() ||
            body.reacting() || body.turboing() || body.dying()) {
            continue;
        }
        if (creatures.empty()) {
            creatures = strikeTargets(targets);
            std::erase_if(creatures,
                          [](const MissileTarget& target) { return !isCreature(target.id); });
        }
        const PlayerActor& actor = runtime.actor;
        const auto target =
            TargetAssist::around(actor.position(), actor.height(), creatures,
                                 actor.radius() + kArmourReach, &m_resources->world.collision());
        if (!target) {
            continue;
        }
        const f32 grown = worn.grown() ? 2.0f : 1.0f;
        // The port's melee pulse marks a creature the shield kills as well: the blow is the
        // bearer's, however it lands, and nothing else tells the hand.
        const bool stood = creatureStanding(targets, target->id);
        const auto pulseKill = [&] {
            if (stood && !creatureStanding(targets, target->id)) {
                targets.attackHost.meleeHit(actor.player());
            }
        };
        if (fire) {
            strikeTarget(*target, kFireShieldDamage * seconds * kFramesPerSecond * grown,
                         kFireElement, actor, players, targets, BlowSource::Shield);
            pulseKill();
            continue;
        }
        if (std::ranges::any_of(runtime.shockGaps, [&](const PlayerRuntime::ShockGap& gap) {
                return gap.target == target->id;
            })) {
            continue;
        }
        strikeTarget(*target, kLightningShieldDamage * grown, kShockFlags, actor, players, targets,
                     BlowSource::Shield);
        pulseKill();
        runtime.shockGaps.push_back({target->id, kShockGap});
        // The spark leaves the shield on the arm and points at what it struck.
        const Mat4 placed = actor.transform();
        const Vec3 from = Vec3{runtime.figure->armAttachment(placed).value_or(placed)[3]};
        const Vec3 toward = target->base + Vec3{0.0f, target->height * 0.5f, 0.0f} - from;
        if (glm::length(toward) > 1e-4f) {
            const Vec3 forward = glm::normalize(toward);
            const Vec3 side = std::abs(forward.y) > 0.99f
                                  ? Vec3{1.0f, 0.0f, 0.0f}
                                  : glm::normalize(glm::cross(Vec3{0.0f, 1.0f, 0.0f}, forward));
            const Vec3 up = glm::cross(forward, side);
            if (const u32 spark = m_resources->effects.startSet(
                    m_resources->device, m_resources->weapons, kShockEffect, from, {});
                spark != 0) {
                m_resources->effects.placeAt(spark, Mat4{Vec4{side, 0.0f}, Vec4{up, 0.0f},
                                                         Vec4{forward, 0.0f}, Vec4{from, 1.0f}});
            }
        }
    }
}

void PlayerAttacks::updateShields(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
    for (PotionShield& shield : m_sim.shields) {
        shield.secondsLeft -= seconds;
        if (shield.secondsLeft <= 0 || shield.actor >= players.size() ||
            players[shield.actor].life != PlayerLife::Standing) {
            shield.secondsLeft = 0.0f;
            continue;
        }
        const Vec3 at = players[shield.actor].actor.position();
        m_resources->effects.moveTo(shield.effect, at);
        // ProcessEffects strikes whatever is within the ring at once and, per target, not
        // again for a quarter second; nothing it pushes away escapes the first blow.
        for (PotionShield::Mark& mark : shield.marks) {
            mark.wait -= seconds;
        }
        std::erase_if(shield.marks,
                      [](const PotionShield::Mark& mark) { return mark.wait <= 0.0f; });
        const auto fresh = [&shield](s32 kind, s32 id) {
            for (const PotionShield::Mark& mark : shield.marks) {
                if (mark.kind == kind && mark.id == id) {
                    return false;
                }
            }
            shield.marks.push_back({kind, id, kShieldHitAgain});
            return true;
        };
        constexpr s32 kEnemy = 0;
        constexpr s32 kCritter = 1;
        constexpr s32 kGenerator = 2;
        constexpr s32 kBoss = 3;
        constexpr s32 kBarrel = 4;
        const auto& actor = players[shield.actor].actor;
        for (const auto& statue : targets.opponents.statues().targets()) {
            const Vec3 distance = statue.base - at;
            if (portable::hypot(distance.x, distance.z) <= shield.radius + statue.radius &&
                std::abs(distance.y) <= shield.radius + statue.height) {
                targets.opponents.wakeStatue(static_cast<usize>(statue.id));
            }
        }
        // Its magic carries its bearer's class perk as a burst's does (start_magic's shield).
        if (const auto perk =
                MagicPerk::of(actor.save().character, experienceLevel(actor.save().experience()))) {
            targets.fixtures.bless(at, shield.radius, *perk, shield.actor, shield.blessed,
                                   targets.fixtureHost);
        }
        for (const s32 id : targets.opponents.enemies().reachedBy(
                 at, shield.radius, std::numbers::pi_v<f32>, {0, 0, 1})) {
            if (!fresh(kEnemy, id)) {
                continue;
            }
            const Enemies& enemies = targets.opponents.enemies();
            const Vec3 struck = enemies.positionOf(id);
            const bool stood = enemies.alive(id);
            const f32 credit = targets.opponents.strikeEnemy(id, shield.damage, shield.flags,
                                                             struck - at, actor.player(), players);
            countEnemyBlow(targets, players, actor.player(), BlowSource::Potion, enemies, id,
                           stood);
            healHit(actor.player(), shield.flags, credit, struck, players, targets);
        }
        for (const s32 id : targets.opponents.critters().reachedBy(
                 at, shield.radius, std::numbers::pi_v<f32>, {0, 0, 1})) {
            if (!fresh(kCritter, id)) {
                continue;
            }
            const Vec3 struck = targets.opponents.critters().positionOf(id);
            Vec3 direction{struck.x - at.x, 0, struck.z - at.z};
            if (const f32 length = glm::length(direction); length > 0) {
                direction /= length; // CritterExpCollide normalizes the radial XZ direction.
            }
            const Critters& critters = targets.opponents.critters();
            const bool stood = critters.alive(id) && !critters.dying(id);
            const f32 credit =
                targets.opponents.strikeCritter(id, shield.damage, shield.flags, direction,
                                                actor.player(), std::nullopt, false, players);
            countCritterBlow(targets, players, actor.player(), BlowSource::Potion, critters, id,
                             stood);
            healHit(actor.player(), shield.flags, credit, struck, players, targets);
        }
        for (const s32 id : targets.opponents.generators().within(at, shield.radius)) {
            if (fresh(kGenerator, id) && blowRulesOf(targets).generatorTakes(BlowSource::Potion)) {
                targets.opponents.strikeGenerator(id, shield.damage, actor.player());
            }
        }
        if (targets.opponents.bosses().within(at, shield.radius) && fresh(kBoss, 0)) {
            EnemyHit hit;
            hit.damage = shield.damage;
            hit.flags = shield.flags;
            hit.player = actor.player();
            hit.level = experienceLevel(actor.save().experience());
            hit.direction = *targets.opponents.bosses().position() - at;
            const Bosses& bosses = targets.opponents.bosses();
            const BossView before = bosses.view();
            const f32 credit = targets.opponents.bosses().hurt(hit);
            countBossBlow(targets, players, actor.player(), BlowSource::Potion, before, bosses);
            healHit(actor.player(), shield.flags, credit, *targets.opponents.bosses().position(),
                    players, targets);
        }
        // Magic leaves the walls, the rocks and every barrel but one holding something alone.
        for (const usize barrel : targets.fixtures.barrels().within(at, shield.radius)) {
            if (!immuneToMagic(static_cast<s32>(barrel), targets) &&
                fresh(kBarrel, static_cast<s32>(barrel))) {
                targets.fixtures.strikeBarrel(barrel, shield.damage,
                                              players[shield.actor].actor.player(), players,
                                              targets.fixtureHost);
            }
        }
        if (auto* imported = m_resources->world.imported()) {
            constexpr s32 kImported = 5;
            for (const auto& native : imported->targets()) {
                if (!native.valid() || !native.damageable || native.magicImmune) {
                    continue;
                }
                const Vec3 away = native.base - at;
                if (portable::hypot(away.x, away.z) <= shield.radius + native.radius &&
                    std::abs(away.y) <= shield.radius + native.height &&
                    fresh(kImported, native.id)) {
                    imported->strike(native.id, shield.damage, shield.flags, actor.player());
                }
            }
        }
    }
    targets.fixtures.settleBlasts(players, targets.fixtureHost);
    std::erase_if(m_sim.shields, [this](const PotionShield& shield) {
        if (shield.secondsLeft > 0.0f) {
            return false;
        }
        m_resources->effects.stop(shield.effect);
        return true;
    });
}

/** A guard that took enough off a hurt shows it: the block effect about the character, for
 * longer the more got through, and not again until that is over. */
void PlayerAttacks::showBlock(usize index, f32 taken, f32 left, std::span<PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    if (index >= players.size()) {
        return;
    }
    const auto duration = PartyDamage::block(players[index], taken, left);
    if (!duration) {
        return;
    }
    const f32 shown = *duration;
    if (m_resources->weapons.loaded() &&
        m_resources->weapons.trees.find(kBlockEffect).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = shown;
        // StartBlockFX (8009233C): the player's color, transparency 0x40,
        // unlit and parented to the player node rather than left at the impact point.
        setting.tint = LegendShow::chargeTint(players[index].actor.save().color);
        setting.alpha = 191.0f / 255.0f;
        setting.unlit = true;
        const u32 effect =
            m_resources->effects.startSet(m_resources->device, m_resources->weapons, kBlockEffect,
                                          players[index].actor.position(), setting);
        if (effect != 0) {
            m_sim.moveAttachments.push_back({index, effect, 0, Mat4{1}, true});
            updateMoveAttachments(players);
        }
    }
}

/** Runs a character's meter: a turbo attack is paid for as it first does harm, a shove runs
 * it down while it lasts, and otherwise it climbs while the character is free to act, the
 * narrator saying so when it comes full. */
void PlayerAttacks::updateTurbo(usize index, s32 ticks, f32 seconds,
                                std::span<PlayerRuntime> players,
                                const std::function<void(s32, usize)>& help) {
    if (!m_resources.has_value()) {
        return;
    }
    if (index >= players.size() || players[index].figure == nullptr) {
        return;
    }
    const TurboMove::Events events{
        .announce = [&help, index](s32 id) { help(id, index); },
        .dim =
            [this, index, players](f32 amount) {
                // The striker blazes against the dark it brings (PlyrSfxDoDamage).
                m_resources->dimmer.ask(amount);
                players[index].glow.raise(BodyGlow::kStrike);
            },
        .identifiedVolley =
            [this, index, players](const TurboMove::Volley& volley) {
                const auto& actor = players[index].actor;
                auto* figure = players[index].figure.get();
                if (const auto weapon = m_resources->arsenal.weaponThrow(
                        actor, figure->animator().attackSeconds())) {
                    const auto emission = TurboEmission::volley(
                        *weapon, volley.direction,
                        PowerupEffects::of(actor.save().progress().inventory).weapon);
                    std::optional<MissileRelease> release;
                    if (const u32 identity = volley.emissionIdentity();
                        identity != 0 &&
                        PlayerAnimator::rollbackTurboAction(figure->animator().action())) {
                        release = MissileRelease{actor.save().character,
                                                 figure->animator().player().generation(), false, 0,
                                                 identity};
                    }
                    m_resources->arsenal.launchWeapon(actor, figure, emission.weapon.direction,
                                                      emission.weapon.scale, emission.spreads,
                                                      emission.weapon.target, release, false,
                                                      PlayerArsenal::WeaponMode::Volley);
                }
            },
        .identifiedStrike =
            [this, index, players](const TurboMove::Strike& strike) {
                // Other native timelines (strong A and combo) keep their
                // normal effects, but do not claim the B/C replay domain.
                const auto action = players[index].figure->animator().action();
                fireStrike(index, strike.row, players, std::nullopt,
                           PlayerAnimator::rollbackTurboAction(action) ? std::optional{strike.slot}
                                                                       : std::nullopt);
            }};
    stepTurbo(index, ticks, seconds, players, events,
              [&](std::string_view voice) { cry(index, voice, players); });
}

void PlayerAttacks::stepTurbo(usize index, s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                              const TurboMove::Events& events,
                              const std::function<void(std::string_view)>& cry) {
    TurboMeter& meter = players[index].turbo;
    const PlayerAnimator& body = players[index].figure->animator();
    TurboMove& move = players[index].move;
    const ClassStats* stats = m_resources->classes.stats(players[index].actor.save().character);
    if (body.turboBegan()) {
        if (const std::string_view voice = move.begin(body.action(), stats, meter);
            !voice.empty()) {
            cry(voice);
        }
    }
    move.advance(body.action(), body.player().frame(), players[index].actor.facing(), stats, meter,
                 events);
    if (body.action() == PlayerAnimator::Action::Shove) {
        meter.drain(seconds);
    } else if (players[index].life == PlayerLife::Standing && !body.turboing() &&
               meter.fill(seconds)) {
        events.announce(HelpMessages::kUseTurbo);
    }
    // With two fifths of the meter, and less than all of it, the combo is suggested
    // (pmotion.c 1760; the scene waits for a party and a crowd).
    if (players[index].life == PlayerLife::Standing && !body.turboing() &&
        meter.held() >= TurboMeter::kStrongCost && meter.held() < TurboMeter::kFull) {
        events.announce(HelpMessages::kUseCombo);
    }
    meter.step(ticks);
}

/** One of a character's own cries, `which` being what follows its class in the name. */
void PlayerAttacks::cry(usize index, std::string_view which, std::span<PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    PlayerFigure* body = index < players.size() ? players[index].figure.get() : nullptr;
    if (body == nullptr) {
        return;
    }
    const bool pain = which.starts_with("PAIN");
    const bool poison = which == "POISON";
    const bool pojo =
        (PowerupEffects::of(players[index].actor.save().progress().inventory).special &
         powerup::kPojo) != 0;
    if (pojo && (pain || poison || which == "DIE1" || which == "DIE2")) {
        // GUNE5D AudioPlayerPain/Poison use COMMON 96, SeverePain/Dies use 98.
        // The latter is named POJOPOISON, including when it is the death cry.
        const std::string_view name = poison || which == "DIE2" ? "S_POJOPOISON" : "S_POJOPAIN";
        const f32 volume = poison ? LevelSoundscape::kBarkVolume : LevelSoundscape::kPainVolume;
        if (pain || poison) {
            m_resources->audio.barkNamed(name, volume);
        } else {
            m_resources->audio.playNamed(name, volume);
        }
        return;
    }
    const std::string_view voice =
        classCode(players[index].actor.save().character % kStartingClassCount);
    const std::string name = std::format("S_{}{}", voice, which);
    if (which.starts_with("PAIN") || which == "POISON") {
        m_resources->audio.bark(body->voice(), name,
                                which == "POISON" ? LevelSoundscape::kBarkVolume
                                                  : LevelSoundscape::kPainVolume);
    } else {
        m_resources->audio.playFrom(body->voice(), name,
                                    which == "DIE1" ? LevelSoundscape::kPainVolume : 1.0f);
    }
}

std::vector<MissileTarget> PlayerAttacks::strikeTargets(const Targets& targets) const {
    std::vector<MissileTarget> all = projectileTargets(targets);
    if (m_resources) {
        const LevelTriggers& triggers = m_resources->world.triggers();
        const auto& layout = m_resources->world.layout();
        for (usize i = 0; i < triggers.size(); ++i) {
            const LevelTrigger& trigger = triggers.trigger(i);
            if (trigger.enabled && trigger.shootable) {
                const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
                const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
                MissileTarget target{kSwitchTargetBase + static_cast<s32>(i), trigger.spot,
                                     trigger.radius, trigger.height};
                target.acquisition = TargetAssist::itemAcquisition(
                    trigger.placement, info.collisionOffset, info.radius, info.height,
                    TargetAssist::kItemDistanceScale);
                target.acquisition->enabled = info.armor != -1;
                all.push_back(target);
            }
        }
    }
    return all;
}

std::vector<MissileTarget> PlayerAttacks::acquisitionTargets(const Targets& targets) const {
    std::vector<MissileTarget> reachable = strikeTargets(targets);
    std::erase_if(reachable, [](const MissileTarget& target) {
        return (target.id >= kSafeRockTargetBase && target.id < kWallTargetBase) ||
               (target.id >= kImportedTargetBase + kImportedArenaBase &&
                target.id < kImportedTargetBase + kImportedSceneryBase);
    });
    return reachable;
}

bool PlayerAttacks::immuneToMagic(s32 id, const Targets& targets) const {
    if (id >= kImportedTargetBase) {
        if (m_resources && m_resources->world.imported()) {
            const auto native = m_resources->world.imported()->targets();
            const auto target =
                std::ranges::find(native, id - kImportedTargetBase, &ImportedTarget::id);
            return target == native.end() || !target->damageable || target->magicImmune;
        }
        return true;
    }
    if (id >= kSafeRockTargetBase) {
        return true; // rocks, walls and switches
    }
    if (id >= 0 && id < kEnemyTargetBase) {
        const auto barrel = static_cast<usize>(id);
        return barrel >= targets.fixtures.barrels().size() ||
               targets.fixtures.barrels().barrel(barrel).kind != BreakableStrike::Kind::Holding;
    }
    return false;
}

bool PlayerAttacks::strikeSwitch(s32 id, u32 flags) {
    if (id < kSwitchTargetBase || id >= kImportedTargetBase) {
        return false;
    }
    if (m_resources && (flags & damage::kGas) == 0) {
        m_resources->world.shootTrigger(static_cast<usize>(id - kSwitchTargetBase));
    }
    return true;
}

std::vector<MissileTarget> PlayerAttacks::projectileTargets(const Targets& targets) const {
    std::vector<MissileTarget> missileTargets;
    if (m_resources) {
        const auto& walls = m_resources->world.walls();
        for (usize i = 0; i < walls.size(); ++i) {
            if (walls.standing(i)) {
                missileTargets.push_back(walls.target(i, kWallTargetBase + static_cast<s32>(i)));
            }
        }
        if (const auto* imported = m_resources->world.imported()) {
            for (const auto& native : imported->targets()) {
                if (!native.valid()) {
                    continue;
                }
                MissileTarget target{kImportedTargetBase + native.id, native.base, native.radius,
                                     native.height, native.surface};
                if (native.surface.empty()) {
                    target.acquisition = TargetAssist::itemAcquisition(
                        glm::translate(Mat4{1}, native.base), Vec3{0}, native.radius, native.height,
                        TargetAssist::kItemDistanceScale);
                    target.acquisition->enabled = native.damageable;
                }
                missileTargets.push_back(target);
            }
        }
    }
    for (usize barrel = 0; barrel < targets.fixtures.barrels().size(); ++barrel) {
        if (targets.fixtures.barrels().standing(barrel)) {
            missileTargets.push_back(
                targets.fixtures.barrels().target(barrel, static_cast<s32>(barrel)));
        }
    }
    for (MissileTarget target : targets.opponents.enemies().targets()) {
        target.id += kEnemyTargetBase;
        missileTargets.push_back(target);
    }
    for (MissileTarget target : targets.opponents.critters().targets()) {
        target.id += kCritterTargetBase;
        missileTargets.push_back(target);
    }
    for (MissileTarget target : targets.opponents.bosses().targets()) {
        target.id += kBossTargetBase;
        missileTargets.push_back(target);
    }
    // A statue takes the blow that wakes it (fn_8005EE18 finds the placed enemies too).
    for (MissileTarget target : targets.opponents.statues().targets()) {
        target.id += kStatueTargetBase;
        missileTargets.push_back(target);
    }
    for (usize g = 0; g < targets.opponents.generators().count(); ++g) {
        if (targets.opponents.generators().standing(static_cast<s32>(g))) {
            missileTargets.push_back(targets.opponents.generators().target(
                static_cast<s32>(g), static_cast<s32>(g) + kGeneratorTargetBase));
        }
    }
    for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
        if (targets.fixtures.safeRocks().standing(rock)) {
            const Obstacle& cover = targets.fixtures.safeRocks().rock(rock).obstacle;
            missileTargets.push_back(MissileTarget{static_cast<s32>(rock) + kSafeRockTargetBase,
                                                   cover.centre, cover.cylinderRadius,
                                                   cover.height});
        }
    }
    return missileTargets;
}

std::optional<Vec3> PlayerAttacks::aim(const PlayerActor& actor, const Vec3& facing,
                                       const Targets& targets, bool fixedFacing) const {
    if (!m_resources) {
        return std::nullopt;
    }
    auto candidates = acquisitionTargets(targets);
    const f32 range =
        targets.opponents.bosses().view().alive ? TargetAssist::kBossRange : TargetAssist::kRange;
    Vec3 origin = actor.followPoint();
    if (fixedFacing) {
        // Acquisition must use the actual hand-offset line, not a parallel ray
        // through the player's feet which can miss a small creature entirely.
        if (auto weapon = m_resources->arsenal.weaponThrow(actor, 0)) {
            weapon->direction = facing;
            weapon->shots = 1;
            origin = PlayerMissiles::weaponLaunches(*weapon).front().position;
        }
    }
    const auto* imported = m_resources->world.imported();
    const auto* encounter = imported ? imported->importedBoss() : nullptr;
    if (encounter != nullptr && encounter->rangedAimFirst &&
        targets.opponents.bosses().view().awake) {
        // The encounter opts into a first query, not an unconditional target:
        // the ordinary facing, cursor-line, range and wall tests still apply.
        // Do not remove fixtures from the physical or melee target rosters.
        const auto bossTargets = targets.opponents.bosses().targets();
        if (const auto boss = TargetAssist::select(origin, facing, bossTargets, range,
                                                   &m_resources->world.collision(),
                                                   m_resources->acquisitionCone, fixedFacing)) {
            return boss;
        }
    }
    if (!fixedFacing) {
        if (const auto contact = contactEnemy(actor, targets, facing, range);
            contact && contact->acquisition) {
            return contact->acquisition->point;
        }
    }
    const auto ordinary =
        TargetAssist::select(origin, facing, candidates, range, &m_resources->world.collision(),
                             m_resources->acquisitionCone, fixedFacing);
    if (ordinary || targets.multiplayer != MultiplayerMode::Hurt) {
        return ordinary;
    }
    candidates.clear();
    {
        for (usize i = 0; i < targets.players.size(); ++i) {
            const PlayerRuntime& player = targets.players[i];
            if (player.actor.player() != actor.player() && player.life == PlayerLife::Standing &&
                !player.departed) {
                candidates.push_back({kPlayerTargetBase + static_cast<s32>(i),
                                      player.actor.position(), player.actor.radius(),
                                      player.actor.height()});
            }
        }
    }
    return TargetAssist::select(origin, facing, candidates, range, &m_resources->world.collision(),
                                TargetAssist::kFacingDot, fixedFacing);
}

/** Whether a target id is a creature: an enemy, a great one or a boss, not a thing. */
bool PlayerAttacks::isCreature(s32 id) {
    return id >= kEnemyTargetBase && id < kSafeRockTargetBase &&
           (id < kGeneratorTargetBase || id >= kCritterTargetBase);
}

bool PlayerAttacks::creatureStanding(const Targets& targets, s32 id) {
    if (id >= kBossTargetBase) {
        return targets.opponents.bosses().view().alive;
    }
    if (id >= kCritterTargetBase) {
        const Critters& critters = targets.opponents.critters();
        const s32 critter = id - kCritterTargetBase;
        return critters.alive(critter) && !critters.dying(critter);
    }
    return id >= kEnemyTargetBase && targets.opponents.enemies().alive(id - kEnemyTargetBase);
}

PlayerDeed PlayerAttacks::attackDeed(const PlayerActor& actor, bool strong, const Targets& targets,
                                     bool moved, s32 chain, std::optional<Vec3> facing) const {
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    if (const auto item = ItemAttack::select(worn)) {
        return item->deed;
    }
    // Pojo's native input selector maps the heavy button to the light attack
    // before choosing either a close strike or a fireball.
    strong = strong && (worn.special & powerup::kPojo) == 0;
    const PlayerDeed ranged = strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
    if (!m_resources) {
        return ranged;
    }
    const MeleeSense sense = meleeSense(actor, true, targets, facing);
    return MeleeSelection::deed(sense, strong, moved, chain);
}

bool PlayerAttacks::rangedOnly(const PlayerActor& actor, const Targets& targets) const {
    if (ItemAttack::select(PowerupEffects::of(actor.save().progress().inventory))) {
        return false;
    }
    if (!m_resources) {
        return true;
    }
    const f32 reach = actor.reach() + kStepReach + kHeldReach;
    const auto candidates = acquisitionTargets(targets);
    // ahead() narrows its cone with distance, leaving gaps between cardinal
    // heading probes. Its angle-independent around() precondition is
    // a conservative superset of every possible swing/step selection instead.
    // Deliberately retain low and acquisition-disabled bodies here: accepting
    // fewer predicted throws is safer than predicting a native melee action.
    if (TargetAssist::around(actor.position(), actor.height(), candidates, reach,
                             &m_resources->world.collision())) {
        return false;
    }
    // A same-step swarm contact precedes fresh acquisition and can bypass its
    // visibility/vertical-overlap checks. Preserve that path's 3D surface range
    // as well, without depending on the last authoritative facing/contact seed.
    for (const auto& target : candidates) {
        if (target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase &&
            target.acquisition &&
            glm::distance(target.acquisition->point, actor.followPoint()) - target.radius < reach) {
            return false;
        }
    }
    // Party fallback uses centre distance minus radius, not around()'s vertical
    // overlap test. As with meleePlayer(), only live Hurt-mode partners qualify.
    if (targets.multiplayer == MultiplayerMode::Hurt) {
        for (const auto& player : targets.players) {
            if (player.actor.player() != actor.player() && player.life == PlayerLife::Standing &&
                !player.departed &&
                glm::distance(player.actor.position(), actor.position()) - player.actor.radius() <
                    reach) {
                return false;
            }
        }
    }
    return true;
}

PlayerDeed PlayerAttacks::automaticMeleeDeed(const PlayerActor& actor, const Targets& targets,
                                             const Vec3& facing) const {
    if (!m_resources) {
        return PlayerDeed::None;
    }
    // Contact shields need locomotion to stay active. An unpressed swing must not
    // lock the wearer into melee before the shield reaches its target.
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    if ((worn.armor & (armor::kFireArmor | armor::kElectricArmor)) != 0) {
        return PlayerDeed::None;
    }
    const auto target = meleeTarget(actor, targets, facing, actor.reach() + kStepReach);
    if (!target || target->body.id < kEnemyTargetBase || target->body.id >= kSafeRockTargetBase) {
        return PlayerDeed::None;
    }
    if (target->body.id < kGeneratorTargetBase &&
        targets.opponents.enemies().kindOf(target->body.id - kEnemyTargetBase) == kDeathKind) {
        return PlayerDeed::None;
    }
    if (target->body.id >= kBossTargetBase) {
        // PlayerMotion's boss-family exception permits only the two mobile
        // encounters, DRIDER and LICH, to trigger an unpressed close attack.
        const BossStage* stage = bossStageOf(targets.opponents.bosses().view().kind);
        if (stage == nullptr || !stage->roams) {
            return PlayerDeed::None;
        }
    }
    const MeleeSense sense = senseOf(actor, false, *target);
    if (sense.range != MeleeRange::Swing) {
        return PlayerDeed::None;
    }
    // Forced melee bypasses ranged item attacks and does not spend ammunition.
    return sense.low ? PlayerDeed::AutoMeleeLow : PlayerDeed::AutoMelee;
}

std::optional<MissileTarget> PlayerAttacks::meleePlayer(const PlayerActor& actor,
                                                        const Targets& targets, f32 reach,
                                                        const Vec3& facing) {
    std::optional<MissileTarget> nearest;
    if (targets.multiplayer != MultiplayerMode::Hurt) {
        return nearest;
    }
    for (usize i = 0; i < targets.players.size(); ++i) {
        const PlayerRuntime& player = targets.players[i];
        if (player.actor.player() == actor.player() || player.life != PlayerLife::Standing ||
            player.departed) {
            continue;
        }
        const Vec3 toward = player.actor.position() - actor.position();
        const f32 length = glm::length(toward);
        const f32 distance = length - player.actor.radius();
        if (length > 0 && distance < reach && glm::dot(toward / length, facing) >= kGrabCone) {
            nearest =
                MissileTarget{kPlayerTargetBase + static_cast<s32>(i), player.actor.position(),
                              player.actor.radius(), player.actor.height()};
            reach = distance;
        }
    }
    return nearest;
}

std::optional<PlayerAttacks::MeleeTarget> PlayerAttacks::meleeTarget(const PlayerActor& actor,
                                                                     const Targets& targets,
                                                                     const Vec3& facing,
                                                                     f32 reach) const {
    if (!m_resources) {
        return std::nullopt;
    }
    const f32 range =
        targets.opponents.bosses().present() ? TargetAssist::kBossRange : TargetAssist::kRange;
    if (const auto contact = contactEnemy(actor, targets, facing, range)) {
        const auto acquisition = contact->acquisition;
        if (!acquisition) {
            return std::nullopt;
        }
        // Keep PlayerGetTarget's 3D surface distance through the swing/step
        // decision. A valid contact outside melee reach still owns acquisition;
        // it must not give way to a nearer barrel from the fresh search.
        const f32 distance =
            glm::distance(acquisition->point, actor.followPoint()) - contact->radius;
        if (distance < reach) {
            return MeleeTarget{*contact, distance};
        }
        return std::nullopt;
    }
    const auto query = meleeQuery(actor, targets, facing);
    const auto selected = MeleeSelection::select(query.selection, facing, reach);
    std::optional<MissileTarget> target;
    if (selected) {
        target = query.bodies[selected->index];
    }
    if (!target) {
        target = meleePlayer(actor, targets, reach, facing);
    }
    if (target) {
        return MeleeTarget{*target,
                           TargetAssist::distanceTo(actor.position(), actor.height(), *target)};
    }
    return std::nullopt;
}

std::optional<MissileTarget> PlayerAttacks::contactEnemy(const PlayerActor& actor,
                                                         const Targets& targets, const Vec3& facing,
                                                         f32 range) {
    const auto body = contactSeed(actor, targets, false);
    if (body && MeleeSelection::accepts(meleeContact(actor, *body, targets), facing, range)) {
        return body;
    }
    return std::nullopt;
}

std::optional<MissileTarget> PlayerAttacks::contactSeed(const PlayerActor& actor,
                                                        const Targets& targets, bool stationary) {
    const Vec3 origin = actor.followPoint();
    Vec3 from = origin;
    Vec3 to = origin;
    for (const PlayerRuntime& player : targets.players) {
        if (!stationary && &player.actor == &actor && player.attackStep) {
            from = player.attackStep->from;
            to = player.attackStep->to;
            break;
        }
    }
    const auto& enemies = targets.opponents.enemies();
    f32 endpointDistance = 0;
    const auto id =
        enemies.playerContact(from, to, actor.reach(), actor.height() * 0.5f, &endpointDistance);
    if (!id) {
        return std::nullopt;
    }
    // Critter contacts replace a swarm seed only for a strictly smaller endpoint
    // distance. Zero cannot lose that comparison, including stationary overlaps.
    // Nonzero mixed contacts still use fresh selection until the shared native
    // critter/root contact ordering is represented by the separate actor pools.
    if (endpointDistance > 0 && (targets.opponents.critters().struckBy(from, to, actor.reach()) ||
                                 targets.opponents.bosses().struckBy(from, to, actor.reach()))) {
        return std::nullopt;
    }
    const auto bodies = enemies.targets();
    const auto body = std::ranges::find(bodies, *id, &MissileTarget::id);
    if (body == bodies.end()) {
        return std::nullopt;
    }
    const auto acquisition = body->acquisition;
    if (!acquisition) {
        return std::nullopt;
    }
    MissileTarget selected = *body;
    selected.id += kEnemyTargetBase;
    return selected;
}

MeleeSelection::Target PlayerAttacks::meleeCandidate(const PlayerActor& actor,
                                                     const MissileTarget& body) {
    const bool swarm = body.id >= kEnemyTargetBase && body.id < kGeneratorTargetBase;
    const bool thing =
        !isCreature(body.id) && (body.id < kPlayerTargetBase || body.id >= kImportedTargetBase);
    return MeleeGeometry::candidate(actor.position(), actor.height(), body,
                                    (swarm && body.height <= kLowEnemy) ||
                                        (thing && body.height <= kLowThing));
}

MeleeSelection::Contact PlayerAttacks::meleeContact(const PlayerActor& actor,
                                                    const MissileTarget& body,
                                                    const Targets& targets) {
    auto target = meleeCandidate(actor, body);
    const Vec3 origin = actor.followPoint();
    const auto& acquisition = body.acquisition;
    target.offset = (acquisition ? acquisition->point : body.pointNear(origin)) - origin;
    target.distance = glm::length(target.offset) - body.radius;
    return {target, targets.opponents.enemies().kindOf(body.id - kEnemyTargetBase) == kDeathKind};
}

PlayerAttacks::MeleeQuery PlayerAttacks::meleeQuery(const PlayerActor& actor,
                                                    const Targets& targets,
                                                    std::optional<Vec3> facing) const {
    MeleeQuery query;
    if (!m_resources) {
        return query;
    }
    const auto& resources = *m_resources;
    auto& snapshot = query.selection;
    snapshot.feet = actor.position();
    snapshot.reach = actor.reach();
    snapshot.range =
        targets.opponents.bosses().present() ? TargetAssist::kBossRange : TargetAssist::kRange;
    snapshot.facingDot = resources.acquisitionCone;
    const f32 reach = actor.reach() + kStepReach + kHeldReach;
    for (const auto& body : acquisitionTargets(targets)) {
        const auto target = meleeCandidate(actor, body);
        if ((body.acquisition && (!body.acquisition->enabled ||
                                  std::abs(target.offset.y) > body.acquisition->maxHeight)) ||
            target.distance >= reach || target.score >= snapshot.range ||
            (facing &&
             !MeleeSelection::accepts(target, *facing, snapshot.range, snapshot.facingDot))) {
            continue;
        }
        // around() owns the native vertical/body validity and line-of-sight tests.
        // They are independent of facing at these fixed feet, including surface meshes.
        if (!TargetAssist::around(actor.position(), actor.height(), std::span{&body, 1}, reach,
                                  &resources.world.collision())) {
            continue;
        }
        query.bodies.push_back(body);
        snapshot.targets.push_back(target);
    }
    return query;
}

std::optional<MeleeSelection::Snapshot>
PlayerAttacks::meleeAcquisition(const PlayerActor& actor, const Targets& targets) const {
    if (!m_resources || targets.multiplayer == MultiplayerMode::Hurt) {
        return std::nullopt;
    }
    auto snapshot = meleeQuery(actor, targets, std::nullopt).selection;
    if (const auto contact = contactSeed(actor, targets, true)) {
        snapshot.contact = meleeContact(actor, *contact, targets);
    }
    return snapshot;
}

std::optional<std::vector<MeleeGeometry::Target>>
PlayerAttacks::meleeGeometry(const PlayerActor& actor, const Targets& targets, f32 movementRadius,
                             bool requireClose) const {
    if (!m_resources || targets.multiplayer != MultiplayerMode::Normal ||
        targets.opponents.bosses().present() || !std::isfinite(movementRadius) ||
        movementRadius <= 0) {
        return std::nullopt;
    }
    // Dying critter roots leave acquisition queries before their solid movement
    // bodies disappear. Replay does not carry those bodies, so keep the same
    // movement query as LevelOpponents when excluding an unsupported contact.
    for (const auto& body : targets.opponents.critters().targets(true)) {
        const Vec3 offset = body.base - actor.position();
        if (portable::hypot(offset.x, offset.z) <= movementRadius + actor.radius() + body.radius) {
            return std::nullopt;
        }
    }
    const auto enemies = targets.opponents.enemies().meleeGeometry();
    std::vector<MeleeGeometry::Target> result;
    const f32 closeReach = actor.reach() + kStepReach + kHeldReach + movementRadius;
    bool close = false;
    for (const auto& body : acquisitionTargets(targets)) {
        // Capture the whole reachable disk, not just today's cone or line of
        // sight. A later turn or wall slide can expose any of these candidates.
        const f32 distance = TargetAssist::distanceTo(actor.position(), actor.height(), body);
        const f32 physical =
            glm::length(Vec2{body.base.x - actor.position().x, body.base.z - actor.position().z}) -
            body.radius;
        const bool nearby = distance < closeReach || physical < closeReach;
        const auto& acquisition = body.acquisition;
        const bool aimable =
            acquisition
                ? acquisition->enabled &&
                      (glm::distance(acquisition->point, actor.followPoint()) - movementRadius) *
                                  acquisition->distanceScale -
                              acquisition->radius <
                          TargetAssist::kRange
                : glm::distance(body.pointNear(actor.followPoint()), actor.followPoint()) -
                          (body.surface.empty() ? body.radius : 0) - movementRadius <
                      TargetAssist::kRange;
        if (!nearby && !aimable) {
            continue;
        }
        if (!body.surface.empty() || body.node >= 0 || body.potionBottle || !acquisition) {
            return std::nullopt;
        }
        if (body.id >= kEnemyTargetBase && body.id < kGeneratorTargetBase) {
            const auto found =
                std::ranges::find(enemies, body.id - kEnemyTargetBase, &MeleeGeometry::Target::id);
            if (found == enemies.end()) {
                return std::nullopt;
            }
            result.push_back(*found);
            result.back().id = body.id;
        } else if (body.id >= kGeneratorTargetBase && body.id < kCritterTargetBase) {
            MeleeGeometry::Target target;
            target.id = body.id;
            target.base = body.base;
            target.radius = body.radius;
            target.height = body.height;
            target.acquisition = *acquisition;
            target.low = body.height <= kLowThing;
            // Native movement uses generators' rotated obstacle boxes, not
            // their broader acquisition cylinders.
            target.blocksMovement = false;
            result.push_back(target);
        } else {
            // Mixed critter contacts, mesh items and multipart bosses need
            // their own contact ordering and geometry before local replay.
            return std::nullopt;
        }
        close = close || nearby;
    }
    // Leave open areas on the ordinary ranged path; a melee context must not
    // suppress local projectile previews when no close attack can be acquired.
    return close || !requireClose ? std::optional{std::move(result)} : std::nullopt;
}

MeleeSense PlayerAttacks::meleeSense(const PlayerActor& actor, bool held, const Targets& targets,
                                     std::optional<Vec3> facing) const {
    MeleeSense sense;
    sense.range = MeleeRange::Beyond;
    if (!m_resources) {
        return sense;
    }
    const f32 bias = held ? kHeldReach : 0.0f;
    const auto target = meleeTarget(actor, targets, facing.value_or(actor.facing()),
                                    actor.reach() + kStepReach + bias);
    if (!target) {
        return sense;
    }
    return senseOf(actor, held, *target);
}

MeleeSense PlayerAttacks::senseOf(const PlayerActor& actor, bool held,
                                  const MeleeTarget& selected) {
    auto target = meleeCandidate(actor, selected.body);
    target.distance = selected.distance;
    return MeleeSelection::senseOf(target, actor.position(), actor.reach(), actor.yaw(), held);
}

void PlayerAttacks::melee(usize index, std::span<PlayerRuntime> players, const Targets& targets) {
    if (!m_resources || index >= players.size() || players[index].figure == nullptr) {
        return;
    }
    const PlayerActor& actor = players[index].actor;
    const PlayerAnimator& animator = players[index].figure->animator();
    // PlayerMotion consumes this frame's PlayerGetTarget result when the blow
    // lands, not a new all-bearing query. Desired heading may still point
    // behind the body's yaw during a backward combo.
    const auto selected =
        meleeTarget(actor, targets, players[index].meleeFacing.value_or(actor.facing()),
                    actor.reach() + kStepReach);
    // The swing's sweep brings down the SHOOTFALL scenery within it (combat.c's item query).
    targets.fixtures.shootScenery(actor.position(), actor.reach() + kStepReach);
    if (!selected) {
        return;
    }
    const MissileTarget* target = &selected->body;
    f32 damage = meleeDamageOf(index, players);
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    u32 flags = worn.weapon;
    if (worn.grown()) {
        damage *= 2;
    }
    constexpr f32 kHeavyScale = 2.0f;
    constexpr f32 kPowerScale = 3.0f;
    switch (animator.meleeBlow()) {
    case MeleeBlow::Heavy:
        damage *= kHeavyScale;
        flags |= EnemyHit::kKnockBack;
        break;
    case MeleeBlow::Kick:
        if (isCreature(target->id) && target->height <= kLowEnemy) {
            flags |= EnemyHit::kKnockDown;
        }
        break;
    case MeleeBlow::Power:
        damage *= kPowerScale;
        flags |= EnemyHit::kKnockDown;
        break;
    default: break;
    }
    const Vec3 point = target->base + Vec3{0, target->height * 0.5f, 0};
    const s32 id = target->id;
    if (id >= kImportedTargetBase) {
        if (auto* imported = m_resources->world.imported()) {
            imported->strike(id - kImportedTargetBase, damage, flags, actor.player());
        }
        return;
    }
    if (id >= kPlayerTargetBase) {
        targets.attackHost.hurtByAttack(static_cast<usize>(id - kPlayerTargetBase), damage,
                                        HurtKind::Burn,
                                        {flags, glm::normalize(target->base - actor.position())});
        return;
    }
    if (id >= kStatueTargetBase) {
        // fn_8005C1DC handles placed-enemy items separately from switches:
        // the blow sets their wake flag, without damaging the future creature.
        targets.opponents.wakeStatue(static_cast<usize>(id - kStatueTargetBase));
        return;
    }
    if (strikeSwitch(id, flags)) {
        return;
    }
    // PlayerMotion_DamageTarget (80086A24): use the player's forward basis
    // and a damage-scaled lift, not a vector toward an animated collision node.
    // Large gargoyles can overlap the player or have nodes below their feet.
    Vec3 direction = actor.facing();
    direction.y = std::min(0.05f * damage, 2.0f);
    f32 credit = 0.0f;
    if (id >= kWallTargetBase) {
        targets.fixtures.strikeWall(static_cast<usize>(id - kWallTargetBase), damage, flags);
        targets.fixtureHost.lesson(HelpMessages::kSecretWalls, index);
    } else if (id >= kBossTargetBase) {
        EnemyHit hit{
            damage, flags, direction, actor.player(), experienceLevel(actor.save().experience()),
            point,  true};
        hit.node = target->node;
        Bosses& bosses = targets.opponents.bosses();
        const BossView before = bosses.view();
        credit = bosses.hurt(hit, id - kBossTargetBase);
        countBossBlow(targets, players, actor.player(), BlowSource::Swing, before, bosses);
    } else if (id >= kCritterTargetBase) {
        const s32 critter = id - kCritterTargetBase;
        const Critters& critters = targets.opponents.critters();
        const bool standing = critters.alive(critter) && !critters.dying(critter);
        credit = targets.opponents.strikeCritter(critter, damage, flags, direction, actor.player(),
                                                 point, true, players, target->node);
        countCritterBlow(targets, players, actor.player(), BlowSource::Swing, critters, critter,
                         standing);
    } else if (id >= kGeneratorTargetBase) {
        const s32 hut = id - kGeneratorTargetBase;
        const Generators& huts = targets.opponents.generators();
        const bool stood = huts.standing(hut);
        targets.opponents.strikeGenerator(hut, damage, actor.player(), players);
        countGeneratorBlow(targets, players, actor.player(), BlowSource::Swing, huts, hut, stood);
    } else if (id >= kEnemyTargetBase) {
        const s32 enemy = id - kEnemyTargetBase;
        const Enemies& enemies = targets.opponents.enemies();
        const bool standing = enemies.alive(enemy);
        credit = targets.opponents.strikeEnemy(enemy, damage, flags, direction, actor.player(),
                                               players, true, point);
        countEnemyBlow(targets, players, actor.player(), BlowSource::Swing, enemies, enemy,
                       standing);
    } else {
        targets.fixtures.strikeBarrel(static_cast<usize>(id), damage, actor.player(), players,
                                      targets.fixtureHost);
        targets.fixtures.settleBlasts(players, targets.fixtureHost);
    }
    if (credit > 0.0f) {
        targets.attackHost.meleeHit(actor.player());
    }
    healHit(actor.player(), flags, credit, point, players, targets);
}

void PlayerAttacks::glowWeapons(std::span<PlayerRuntime> players, s32 occupiedHand) {
    if (!m_resources) {
        return;
    }
    for (PlayerRuntime& runtime : players) {
        const CharacterSave& save = runtime.actor.save();
        const PowerupEffects worn = PowerupEffects::of(save.progress().inventory);
        std::optional<Mat4> hand;
        if (runtime.figure != nullptr && runtime.life != PlayerLife::InTower) {
            const Mat4 body = PlayerFigure::bodyPlacement(
                runtime.capture.body().value_or(runtime.actor.transform()), save, worn);
            hand = runtime.figure->handAttachment(body);
        }
        const u32 element = hand.has_value() && runtime.actor.player() != occupiedHand &&
                                    (worn.special & powerup::kPojo) == 0
                                ? WeaponGlow::elementOf(worn)
                                : 0;
        Vec3 offset{0.0f};
        Vec3 scale{0.0f};
        if (const ClassStats* stats = m_resources->classes.stats(save.character);
            stats != nullptr) {
            const usize tier = WeaponGlow::tierOf(experienceLevel(save.experience()));
            offset = stats->weaponGlowOffsets[tier];
            scale = stats->weaponGlowScales[tier];
        }
        // The effects archive is asked for only with an element, so that a missing one is
        // not tried again every frame.
        ItemArchive* archive =
            element != 0 && runtime.figure != nullptr ? runtime.figure->effects() : nullptr;
        runtime.weaponGlow.update(m_resources->device, m_resources->effects, archive, element,
                                  hand.value_or(Mat4{1.0f}), offset, scale);
    }
}

void PlayerAttacks::updateProjectiles(f32 seconds, std::span<PlayerRuntime> players,
                                      const Targets& targets) {
    if (!m_resources) {
        return;
    }
    for (const MissileImpact& impact : advanceProjectileFlights(seconds, players, targets)) {
        dispatchProjectileImpact(impact, players, targets);
    }
    updatePotions(seconds, players, targets);
    updateItems(seconds, players, targets);
}

std::vector<MissileImpact> PlayerAttacks::advanceProjectileFlights(f32 seconds,
                                                                   std::span<PlayerRuntime> players,
                                                                   const Targets& targets) {
    if (!m_resources) {
        return {};
    }
    stepProjectileFlights(seconds, players, targets);
    // A weapon still flying brings down the SHOOTFALL scenery it passes (fn_8005EE18).
    const PlayerMissiles& flying = m_resources->arsenal.missiles();
    for (usize i = 0; i < flying.count(); ++i) {
        const PlayerMissiles::Missile& missile = flying.missile(i);
        targets.fixtures.shootScenery(missile.position,
                                      missile.spec != nullptr ? missile.spec->radius : 1.0f);
    }
    return m_resources->arsenal.missiles().takeImpacts();
}

void PlayerAttacks::stepProjectileFlights(f32 seconds, std::span<PlayerRuntime> players,
                                          const Targets& targets) {
    if (!m_resources) {
        return;
    }
    glowWeapons(players, targets.occupiedHand);
    std::vector<MissileTarget> missileTargets = strikeTargets(targets);
    // A bottle is authored two units tall, but a thrown weapon leaves the hand up to four
    // and a third units up and flies level, so the shot counts along the column a throw
    // passes through; its footprint stays the bottle's own.
    constexpr f32 kBottleShotHeight = 5.0f;
    const PlacedItems& lying = m_resources->world.placedItems();
    for (const usize bottle : lying.shootablePotions()) {
        const PlacedItems::Item& item = lying.item(bottle);
        missileTargets.push_back(MissileTarget{kPotionTargetBase + static_cast<s32>(bottle),
                                               item.position, std::max(item.radius, 0.5f),
                                               std::max(item.height, kBottleShotHeight)});
        missileTargets.back().potionBottle = true;
    }
    // Chests, shut gates and raised tent walls stop it and take nothing (SfxSkipItem).
    s32 stop = kItemStopBase;
    for (const Obstacle& box : targets.fixtures.inertStops()) {
        missileTargets.push_back(
            MissileTarget{stop++, box.centre, std::max(box.halfAcross, box.halfAlong), box.height});
    }
    std::vector<MissilePlayer> missilePlayers;
    for (const auto& player : players) {
        m_resources->arsenal.followCaster(player.actor);
        if (player.life == PlayerLife::Standing && !player.departed) {
            const u32 armor = PowerupEffects::of(player.actor.save().progress().inventory).armor;
            missilePlayers.push_back(
                {player.actor.player(),
                 {0, player.actor.position(), player.actor.radius(), player.actor.height()},
                 (armor & armor::kReflects) != 0});
        }
    }
    m_resources->arsenal.missiles().update(seconds, &m_resources->world.collision(), missileTargets,
                                           missilePlayers);
}

void PlayerAttacks::dispatchProjectileImpact(
    const MissileImpact& impact, std::span<PlayerRuntime> players, const Targets& targets,
    const std::function<void(const MissileImpact&, f32)>& presentation) {
    if (!m_resources) {
        return;
    }
    if (impact.worldObject >= 0) {
        if (auto* imported = m_resources->world.imported()) {
            imported->strikeWorldObject(impact.worldObject, impact.damage, impact.flags,
                                        impact.owner);
        } else if (m_resources->world.explodeObject(impact.worldObject, impact.position)) {
            for (const Vec3& position : m_resources->world.takeWorldExplosions()) {
                targets.fixtures.worldExplosion(position, players, targets.fixtureHost);
            }
        }
    }
    f32 playerDistance = std::numeric_limits<f32>::max();
    for (const PlayerRuntime& player : players) {
        if (player.life == PlayerLife::Standing) {
            playerDistance = std::min(playerDistance,
                                      glm::distance(player.actor.followPoint(), impact.position));
        }
    }
    if (presentation) {
        presentation(impact, playerDistance);
    } else {
        m_resources->arsenal.presentImpact(impact, playerDistance);
    }
    if (impact.player >= 0) {
        const auto player = std::ranges::find_if(players, [&impact](const PlayerRuntime& p) {
            return p.actor.player() == impact.player;
        });
        if (player != players.end() && player->effectGap <= 0) {
            targets.attackHost.hurtByAttack(
                static_cast<usize>(player - players.begin()), impact.stun ? 0 : impact.damage,
                impact.stun ? HurtKind::QuietBlow : HurtKind::Blow,
                {impact.flags | (impact.stun ? damage::kStun : 0), impact.direction});
            if (impact.damage > 2) {
                player->effectGap = impact.playerHitGap;
            }
        }
        return;
    }
    if (impact.potion != 0) {
        beginPotion(impact);
        return;
    }
    if (impact.target >= kImportedTargetBase) {
        if (auto* imported = m_resources->world.imported()) {
            imported->strike(impact.target - kImportedTargetBase, impact.damage, impact.flags,
                             impact.owner);
        }
        return;
    }
    if (impact.target >= kStatueTargetBase) {
        targets.opponents.wakeStatue(static_cast<usize>(impact.target - kStatueTargetBase));
        return;
    }
    if (impact.target >= kPotionTargetBase) {
        shootPotion(impact, players, targets);
        return;
    }
    if (impact.target >= kItemStopBase) {
        return;
    }
    if (strikeSwitch(impact.target, impact.flags)) {
        return;
    }
    const BlowSource source =
        (impact.flags & damage::kSuperShot) != 0 ? BlowSource::SuperShot : BlowSource::Shot;
    f32 credit = 0.0f;
    if (impact.target >= kWallTargetBase) {
        targets.fixtures.strikeWall(static_cast<usize>(impact.target - kWallTargetBase),
                                    impact.damage, impact.flags);
    } else if (impact.target >= kSafeRockTargetBase) {
        targets.fixtures.strikeSafeRock(static_cast<usize>(impact.target - kSafeRockTargetBase),
                                        impact.damage);
    } else if (impact.target >= kBossTargetBase) {
        EnemyHit hit;
        hit.damage = impact.damage;
        hit.flags = impact.flags;
        hit.player = impact.owner;
        hit.where = impact.position;
        hit.node = impact.node;
        hit.direction = impact.direction;
        for (const PlayerRuntime& runtime : players) {
            const PlayerActor& actor = runtime.actor;
            if (actor.player() == impact.owner) {
                hit.level = experienceLevel(actor.save().experience());
            }
        }
        const Bosses& bosses = targets.opponents.bosses();
        const BossView before = bosses.view();
        credit = targets.opponents.bosses().hurt(hit, impact.target - kBossTargetBase);
        countBossBlow(targets, players, impact.owner, source, before, bosses);
    } else if (impact.target >= kCritterTargetBase) {
        const s32 critter = impact.target - kCritterTargetBase;
        const Critters& critters = targets.opponents.critters();
        const bool stood = critters.alive(critter) && !critters.dying(critter);
        credit = targets.opponents.strikeCritter(critter, impact.damage, impact.flags,
                                                 impact.direction, impact.owner, impact.position,
                                                 false, players, impact.node);
        countCritterBlow(targets, players, impact.owner, source, critters, critter, stood);
    } else if (impact.target >= kGeneratorTargetBase) {
        const s32 hut = impact.target - kGeneratorTargetBase;
        const Generators& huts = targets.opponents.generators();
        const bool stood = huts.standing(hut);
        if (blowRulesOf(targets).generatorTakes(source)) {
            targets.opponents.strikeGenerator(hut, impact.damage, impact.owner, players);
            countGeneratorBlow(targets, players, impact.owner, source, huts, hut, stood);
        }
    } else if (impact.target >= kEnemyTargetBase) {
        const s32 enemy = impact.target - kEnemyTargetBase;
        const Enemies& enemies = targets.opponents.enemies();
        const bool stood = enemies.alive(enemy);
        credit = targets.opponents.strikeEnemy(enemy, impact.damage, impact.flags, impact.direction,
                                               impact.owner, players, false, impact.position);
        countEnemyBlow(targets, players, impact.owner, source, enemies, enemy, stood);
    } else if (impact.target >= 0) {
        targets.fixtures.strikeBarrel(static_cast<usize>(impact.target), impact.damage,
                                      impact.owner, players, targets.fixtureHost);
        targets.fixtures.settleBlasts(players, targets.fixtureHost);
    }
    healHit(impact.owner, impact.flags, credit, impact.position, players, targets);
}

/** A thrown weapon breaks a bottle lying about (fn_8005C1DC, PlayerDamagedItem): its own magic
 * goes off with nobody's power, then the thrower's at four fifths of theirs, and the thrower is
 * told that shooting magic does less. */
void PlayerAttacks::shootPotion(const MissileImpact& impact, std::span<PlayerRuntime> players,
                                const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
    const auto kind = m_resources->world.strikePotion(
        static_cast<usize>(impact.target - kPotionTargetBase), impact.damage);
    if (!kind.has_value()) {
        return;
    }
    shatterPotion(*kind, impact.position);
    for (usize i = 0; i < players.size(); ++i) {
        if (players[i].actor.player() != impact.owner) {
            continue;
        }
        const s32 colour = m_resources->arsenal.resolvePotionKind(*kind);
        MissileImpact own;
        own.owner = impact.owner;
        own.position = impact.position;
        own.potion = colour;
        own.potency = kShotMagicShare * m_resources->arsenal.potionPowerOf(players[i].actor, *kind);
        own.damage = kPotionDamage *
                     damage::colourBonus(players[i].actor.save().color, static_cast<u32>(*kind));
        own.flags = damage::magicHeal(experienceLevel(players[i].actor.save().experience()));
        m_resources->arsenal.burstPotion(colour, impact.position, own.potency, true);
        beginPotion(own);
        targets.fixtureHost.lesson(HelpMessages::kShotMagic, i);
    }
}

void PlayerAttacks::usePotion(usize index, std::span<PlayerRuntime> players) {
    if (m_resources && index < players.size()) {
        if (const auto burst = m_resources->arsenal.usePotion(players[index].actor)) {
            beginPotion(*burst);
        }
    }
}

void PlayerAttacks::shatterPotion(s32 kind, const Vec3& position) {
    if (!m_resources) {
        return;
    }
    // start_magic(-1, ..., 0.8): power = 20 * 0.8, damage = 40 * 0.8.
    // Ownerless magic has neither a player's color/level bonus nor AudioPotion.
    kind = m_resources->arsenal.resolvePotionKind(kind);
    MissileImpact impact;
    impact.owner = -1;
    impact.position = position;
    impact.potion = kind;
    impact.potency = 16;
    impact.damage = 32;
    m_resources->arsenal.burstPotion(kind, position, impact.potency, false);
    beginPotion(impact);
}

void PlayerAttacks::beginPotion(const MissileImpact& impact) {
    if (!m_resources) {
        return;
    }
    static constexpr std::array<std::string_view, 5> kTrees{"MP_FIRE", "MP_FIRE", "MP_ELEC",
                                                            "MP_LIGHT", "MP_ACID"};
    PotionBurst burst;
    burst.impact = impact;
    const auto kind = static_cast<usize>(std::clamp(impact.potion, 0, 4));
    if (const auto tree = m_resources->weapons.trees.find(kTrees[kind])) {
        const auto& sequences = m_resources->weapons.trees.tree(*tree).sequences;
        if (!sequences.empty()) {
            AnimationPlayer animation;
            animation.start(sequences[0], 0);
            burst.duration = std::max(1.0f / 30, animation.secondsPerFrame() *
                                                     static_cast<f32>(sequences[0].frames));
        }
    }
    m_sim.potions.push_back(std::move(burst));
}

/** A wave of magic reaches the shut chests too, each once: one holding Death gives him up. */
void PlayerAttacks::enchantChests(PotionBurst& burst, f32 radius, f32 power,
                                  const Targets& targets) {
    const Chests& chests = targets.fixtures.chests();
    for (usize index = 0; index < chests.size(); ++index) {
        const Chests::Chest& chest = chests.chest(index);
        const s32 id = kChestTargetBase + static_cast<s32>(index);
        if (!chest.shown || chest.gone || chest.state != Chests::kShut ||
            std::ranges::find(burst.hit, id) != burst.hit.end()) {
            continue;
        }
        const Vec3 offset = chest.box.centre - burst.impact.position;
        const f32 reach = radius + std::max(chest.box.halfAcross, chest.box.halfAlong);
        if (portable::hypot(offset.x, offset.z) > reach ||
            std::abs(offset.y) > radius + chest.box.height) {
            continue;
        }
        burst.hit.push_back(id);
        targets.fixtures.enchantChest(index, power);
    }
}

/** The magic of a caster from level 25 carries their class family's perk (start_magic's
 * DMG_HEAL, fn_8005BA1C); a bottle broken by a blast has no caster and none. */
void PlayerAttacks::bless(PotionBurst& burst, f32 radius, std::span<const PlayerRuntime> players,
                          const Targets& targets) {
    for (usize i = 0; i < players.size(); ++i) {
        const PlayerActor& actor = players[i].actor;
        if (actor.player() != burst.impact.owner) {
            continue;
        }
        if (const auto perk =
                MagicPerk::of(actor.save().character, experienceLevel(actor.save().experience()))) {
            targets.fixtures.bless(burst.impact.position, radius, *perk, i, burst.blessed,
                                   targets.fixtureHost);
        }
        return;
    }
}

void PlayerAttacks::updatePotions(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    for (PotionBurst& burst : m_sim.potions) {
        // ProcessEffects expands the magic wave while its damage falls. Each
        // target is struck once, not once per frame while it tries to get up.
        burst.elapsed += seconds;
        const f32 phase = 1.0f - burst.elapsed / burst.duration;
        if (phase <= 0.33f) {
            continue;
        }
        const f32 radius = burst.impact.potency * (1.33f - phase);
        const f32 power = burst.impact.damage * 1.5f * (phase - 0.33f);
        targets.fixtures.shootScenery(burst.impact.position, radius);
        const u32 flags = EnemyHit::kMagic | static_cast<u32>(burst.impact.potion) |
                          (burst.impact.flags & damage::kHeal);
        // The perk goes first: a barrel it cleanses may break under the same wave.
        bless(burst, radius, players, targets);
        for (const MissileTarget& target : strikeTargets(targets)) {
            Vec3 direction =
                (target.surface.empty() ? target.base : target.pointNear(burst.impact.position)) -
                burst.impact.position;
            const bool outside =
                target.surface.empty()
                    ? portable::hypot(direction.x, direction.z) > radius + target.radius ||
                          std::abs(target.base.y - burst.impact.position.y) > radius + target.height
                    : !target.touches(burst.impact.position, radius);
            if (outside || std::ranges::find(burst.hit, target.id) != burst.hit.end()) {
                continue;
            }
            burst.hit.push_back(target.id);
            direction.y = 0;
            if (const f32 length = glm::length(direction); length > 0) {
                direction /= length;
            }
            const s32 byPlayer = burst.impact.owner;
            if (target.id >= kStatueTargetBase && target.id < kPlayerTargetBase) {
                // Placed enemies are still items: fn_8005C1DC wakes them before
                // their native ACTIVE sequence releases the actual creature.
                // A continuing potion wave can then strike that new enemy too.
                targets.opponents.wakeStatue(static_cast<usize>(target.id - kStatueTargetBase));
                burst.struck = true;
                continue;
            }
            if (immuneToMagic(target.id, targets)) {
                continue;
            }
            burst.struck = true;
            if (target.id >= kImportedTargetBase) {
                if (auto* imported = m_resources->world.imported()) {
                    imported->strike(target.id - kImportedTargetBase, power, flags, byPlayer);
                }
            } else if (target.id >= kWallTargetBase) {
                targets.fixtures.strikeWall(static_cast<usize>(target.id - kWallTargetBase), power,
                                            flags);
            } else if (target.id >= kSafeRockTargetBase) {
                targets.fixtures.strikeSafeRock(static_cast<usize>(target.id - kSafeRockTargetBase),
                                                power);
            } else if (target.id >= kBossTargetBase) {
                EnemyHit hit;
                hit.damage = power;
                hit.flags = flags;
                hit.player = byPlayer;
                hit.direction = direction;
                for (const auto& player : players) {
                    if (player.actor.player() == byPlayer) {
                        hit.level = experienceLevel(player.actor.save().experience());
                    }
                }
                const Bosses& bosses = targets.opponents.bosses();
                const BossView before = bosses.view();
                const f32 credit =
                    targets.opponents.bosses().hurt(hit, target.id - kBossTargetBase);
                countBossBlow(targets, players, byPlayer, BlowSource::Potion, before, bosses);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else if (target.id >= kCritterTargetBase) {
                const s32 id = target.id - kCritterTargetBase;
                const Critters& critters = targets.opponents.critters();
                const bool stood = critters.alive(id) && !critters.dying(id);
                const f32 credit = targets.opponents.strikeCritter(
                    id, power, flags, direction, byPlayer, target.base, false, players);
                countCritterBlow(targets, players, byPlayer, BlowSource::Potion, critters, id,
                                 stood);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else if (target.id >= kGeneratorTargetBase) {
                if (blowRulesOf(targets).generatorTakes(BlowSource::Potion)) {
                    targets.opponents.strikeGenerator(target.id - kGeneratorTargetBase, power,
                                                      byPlayer, players);
                }
            } else if (target.id >= kEnemyTargetBase) {
                const s32 id = target.id - kEnemyTargetBase;
                const Enemies& enemies = targets.opponents.enemies();
                const bool stood = enemies.alive(id);
                const f32 credit =
                    targets.opponents.strikeEnemy(id, power, flags, direction, byPlayer, players);
                countEnemyBlow(targets, players, byPlayer, BlowSource::Potion, enemies, id, stood);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else {
                targets.fixtures.strikeBarrel(static_cast<usize>(target.id), power, byPlayer,
                                              players, targets.fixtureHost);
            }
        }
        enchantChests(burst, radius, power, targets);
    }
    // Broken bottles may append a new wave through the fixtures' host, so
    // resolve barrel chains only after iteration over existing waves finishes.
    targets.fixtures.settleBlasts(players, targets.fixtureHost);
    // A caster's magic that struck nothing at all is wasted, and they are told so.
    for (const PotionBurst& burst : m_sim.potions) {
        if (burst.elapsed < burst.duration || burst.struck) {
            continue;
        }
        for (usize i = 0; i < players.size(); ++i) {
            if (players[i].actor.player() == burst.impact.owner) {
                targets.fixtureHost.lesson(HelpMessages::kWastedMagic, i);
            }
        }
    }
    std::erase_if(m_sim.potions,
                  [](const PotionBurst& burst) { return burst.elapsed >= burst.duration; });
}

void PlayerAttacks::healHit(s32 owner, u32 flags, f32 credit, const Vec3& at,
                            std::span<PlayerRuntime> players, const Targets& targets) {
    if (damage::heals(flags)) {
        healFrom(owner, credit, at, players, targets);
    }
}

/** From level 75 a hit that carries DMG_HEAL (a caster's magic from 25, the healing weapon)
 * heals from its family's credited damage (do_heal_players, damage_enemy and CritterDamage),
 * not the final health loss: a tenth, more by 0.016 a level past 75. Standing partners within
 * the caster's magic power get half that, none past their most; it shows over the target and
 * teaches them so. */
void PlayerAttacks::healFrom(s32 owner, f32 harm, const Vec3& at, std::span<PlayerRuntime> players,
                             const Targets& targets) {
    if (!m_resources.has_value() || harm <= 0.0f) {
        return;
    }
    const auto caster = std::ranges::find_if(players, [owner](const PlayerRuntime& runtime) {
        return runtime.actor.player() == owner && runtime.life == PlayerLife::Standing;
    });
    if (caster == players.end()) {
        return;
    }
    const s32 level = experienceLevel(caster->actor.save().experience());
    if (level < kHealingLevel) {
        return;
    }
    const auto heal = [](PlayerRuntime& runtime, f32 amount) {
        CharacterSave& save = runtime.actor.save();
        const s32 most = mostHealth(experienceLevel(save.experience()));
        if (save.health() < most) {
            save.progress().health =
                std::min(most, save.health() + static_cast<s32>(std::lround(amount)));
        }
    };
    const f32 given =
        harm * (kHealingShare + kHealingShareALevel * static_cast<f32>(level - kHealingLevel));
    heal(*caster, given);
    const f32 reach = m_resources->arsenal.magicPowerOf(caster->actor);
    for (PlayerRuntime& other : players) {
        const Vec3 apart = other.actor.position() - caster->actor.position();
        if (&other != &*caster && other.life == PlayerLife::Standing && !other.departed &&
            portable::hypot(apart.x, apart.z) < reach) {
            heal(other, given * kHealingOthers);
        }
    }
    if (m_resources->weapons.loaded()) {
        m_resources->effects.start(m_resources->device, m_resources->weapons, kHealingEffect, at);
    }
    targets.fixtureHost.lesson(HelpMessages::kHealingMagic,
                               static_cast<usize>(std::distance(players.begin(), caster)));
}

} // namespace gdl::game
