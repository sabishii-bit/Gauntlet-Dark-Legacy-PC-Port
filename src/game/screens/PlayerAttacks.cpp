#include "game/screens/PlayerAttacks.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/AnimationPlayer.h"

#include "game/combat/Damage.h"
#include "game/combat/DamageTypes.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/LegendItems.h"
#include "game/players/ItemPickup.h"
#include "game/players/MagicPerks.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/world/Chests.h"
#include "game/world/DynamicLights.h"
#include "game/world/TargetAssist.h"
#include "game/world/WeaponGlow.h"
namespace gdl::game {
namespace {
constexpr std::string_view kNoEffectTree = "NULLFX"; ///< a move's effect row that shows nothing
constexpr std::array<std::string_view, 5> kShieldTrees{"MS_FIRE", "MS_FIRE", "MS_ELEC", "MS_LIGHT",
                                                       "MS_ACID"};
constexpr std::array<std::string_view, 5> kShieldSounds{"S_SHIELD2", "S_SHIELD2", "S_SHIELD1",
                                                        "S_SHIELD3", "S_SHIELD4"};
constexpr f32 kShieldSeconds = 3.0f; ///< how long a potion's ring lasts
constexpr f32 kShieldDamage = 25.0f;
constexpr f32 kShieldPotency = 0.25f;  ///< of the character's magic power, its radius
constexpr f32 kShieldHarmEvery = 0.5f; ///< seconds between its harming what it touches
constexpr std::string_view kBlockEffect = "BLOCKFX";
constexpr f32 kBlockWorth = 2.0f;      ///< what a guard must take off a hurt for it to show
constexpr f32 kBlockPerDamage = 0.01f; ///< seconds it shows for each point left
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1.0f;
constexpr f32 kRamDamage = 3.0f;   ///< what a charge does to what it runs into
constexpr f32 kChargeBlow = 32.0f; ///< and to an enemy or a great one (PlayerMotion's anim 8)
constexpr f32 kRamReach = 0.3f;    ///< how near counts as run into
constexpr u32 kFireElement = 1;    ///< the fire shield's harm
constexpr u32 kShockFlags = 0x22;  ///< the lightning shield's: lightning, knocking down
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
void PlayerAttacks::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
}
void PlayerAttacks::clear() {
    if (m_resources.has_value()) {
        for (const auto& particle : m_particleEffects) {
            m_resources->effects.stop(particle.effect);
        }
        for (const auto& item : m_items) {
            m_resources->effects.stop(item.effect);
        }
        for (const StrikeEffect& effect : m_strikeEffects) {
            m_resources->effects.stop(effect.effect);
        }
        for (const MoveAttachment& attachment : m_moveAttachments) {
            m_resources->effects.stop(attachment.effect);
        }
        for (const PotionShield& shield : m_shields) {
            m_resources->effects.stop(shield.effect);
        }
    }
    m_strikes.clear();
    m_particleEffects.clear();
    m_strikeEffects.clear();
    m_moveAttachments.clear();
    m_strikeSources.clear();
    m_shields.clear();
    m_potions.clear();
    m_items.clear();
    m_resources.reset();
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
                                      targets.fixtureEvents);
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
            targets.opponents.strikeEnemy(hit.id, blow, EnemyHit::kKnockDown, facing,
                                          actor.player(), players, true, hit.base);
        });
    }
    for (const MissileTarget& target : targets.opponents.critters().targets()) {
        ram(target, kCritterTargetBase, [&](const MissileTarget& hit) {
            targets.opponents.strikeCritter(hit.id, blow, EnemyHit::kKnockDown, facing,
                                            actor.player(), hit.base, true, players);
        });
    }
    targets.fixtures.settleBlasts(players, targets.fixtureEvents);
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
    const auto once = [&](usize key, const auto& strike) {
        if (std::ranges::find(struck, key) != struck.end()) {
            return;
        }
        struck.push_back(key);
        strike();
        hit = true;
    };
    const auto& walls = m_resources->world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        if (walls.standing(i) &&
            walls.target(i, 0).touches(actor.followPoint(), actor.radius() + kRamReach)) {
            once(static_cast<usize>(kWallTargetBase) + i,
                 [&] { targets.fixtures.strikeWall(i, blow, EnemyHit::kKnockDown); });
        }
    }
    for (usize barrel = 0; barrel < targets.fixtures.barrels().size(); ++barrel) {
        if (targets.fixtures.barrels().standing(barrel) &&
            targets.fixtures.barrels().barrel(barrel).box.touchedBy(actor.position(),
                                                                    actor.radius(), kRamReach)) {
            once(barrel, [&] {
                targets.fixtures.strikeBarrel(barrel, blow, actor.player(), players,
                                              targets.fixtureEvents);
            });
        }
    }
    for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
        if (targets.fixtures.safeRocks().standing(rock) &&
            targets.fixtures.safeRocks().rock(rock).obstacle.touchedBy(actor.position(),
                                                                       actor.radius(), kRamReach)) {
            once(rock + static_cast<usize>(kSafeRockTargetBase),
                 [&] { targets.fixtures.strikeSafeRock(rock, blow); });
        }
    }
    const Generators& generators = targets.opponents.generators();
    for (usize g = 0; g < generators.count(); ++g) {
        const auto id = static_cast<s32>(g);
        if (generators.standing(id) &&
            generators.boxOf(id).touchedBy(actor.position(), actor.radius(), kRamReach)) {
            once(static_cast<usize>(kGeneratorTargetBase) + g,
                 [&] { targets.opponents.strikeGenerator(id, blow, actor.player(), players); });
        }
    }
    if (hit && thrower < players.size()) {
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

/** What a character's own blows do, which a strike with a negative amount multiplies. */
f32 PlayerAttacks::ownDamageOf(usize index, std::span<PlayerRuntime> players) const {
    if (!m_resources.has_value()) {
        return PlayerMissiles::kLeastDamage;
    }
    const CharacterSave& save = players[index].actor.save();
    const ClassStats* stats = m_resources->classes.stats(save.character);
    if (stats == nullptr) {
        return PlayerMissiles::kLeastDamage;
    }
    const StatBlock block =
        displayStats(*stats, experienceLevel(save.experience()), save.progress());
    return PlayerMissiles::damageFor(MissileSpec::byMagic(save.character) ? block.magic()
                                                                          : block.strength());
}

/** One strike of a move: its effects show and sound where the character stands, the meter
 * pays what the move still owes if the strike does harm, and the harm is set going. */
void PlayerAttacks::fireStrike(usize index, s32 strikeIndex, std::span<PlayerRuntime> players,
                               std::optional<Vec3> at) {
    if (!m_resources.has_value() || index >= players.size() || players[index].figure == nullptr) {
        return;
    }
    const ClassStats* stats = m_resources->classes.stats(players[index].actor.save().character);
    if (stats == nullptr || strikeIndex < 0 ||
        static_cast<usize>(strikeIndex) >= stats->moveStrikes.size()) {
        return;
    }
    const MoveStrike& strike = stats->moveStrikes[static_cast<usize>(strikeIndex)];
    const PlayerActor& actor = players[index].actor;
    const Vec3 facing = actor.facing();
    const Vec3 base = at.value_or(actor.position());
    f32 effectSeconds = 0;
    if (strike.effect >= 0 && static_cast<usize>(strike.effect) < stats->moveEffects.size()) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(strike.effect)];
        if (const auto* archive = moveEffectArchive(index, players, effect.tree)) {
            const auto& sequences =
                archive->trees.tree(*archive->trees.find(effect.tree)).sequences;
            // DoPlyrSfx gives the primary tree's authored maxlen to the same effect
            // that carries damage. Animation length is only its untimed fallback.
            if (effect.lifetime > 0) {
                effectSeconds = effect.lifetime;
            } else if (!sequences.empty()) {
                const auto& sequence = sequences.front();
                constexpr s32 kEmptyFrames = 30;
                const auto frames =
                    static_cast<f32>(sequence.frames > 0 ? sequence.frames : kEmptyFrames);
                const f32 rate = sequence.frameRate > 0 ? static_cast<f32>(sequence.frameRate)
                                                        : AnimationPlayer::kDefaultRate;
                effectSeconds = frames * rate * AnimationPlayer::kRateUnit;
            }
        }
    }
    // A span that only lasts, or a volley, harms nothing of itself; the rest are set going.
    u32 id = 0;
    if (strike.harms() && MoveStrikes::damageOf(strike, ownDamageOf(index, players)) > 0) {
        MoveStrike volume = strike;
        if (strike.effect >= 0 && static_cast<usize>(strike.effect) < stats->moveEffects.size()) {
            volume.offset += stats->moveEffects[static_cast<usize>(strike.effect)].offset;
        }
        id = m_strikes.start(volume, actor.player(), base, facing, ownDamageOf(index, players),
                             effectSeconds);
        m_strikeSources.push_back(StrikeSource{id,
                                               index,
                                               strikeIndex,
                                               {},
                                               m_resources->multiplayer != nullptr
                                                   ? *m_resources->multiplayer
                                                   : MultiplayerMode::Normal,
                                               strike.damageType});
    }
    const Vec3 origin = MoveStrikes::originOf(strike, base, facing);
    const MoveStrikes::Strike* started = m_strikes.find(id);
    // An effect may bring another with it.
    usize followed = 0;
    u32 previousEffect = 0;
    for (s32 effectIndex = strike.effect;
         effectIndex >= 0 && static_cast<usize>(effectIndex) < stats->moveEffects.size() &&
         followed < stats->moveEffects.size();
         effectIndex = stats->moveEffects[static_cast<usize>(effectIndex)].next, ++followed) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(effectIndex)];
        if ((effect.flags & 2U) != 0 && m_resources->shake != nullptr) {
            m_resources->shake->start();
        }
        if (effect.particle()) {
            startParticles(index, effect, previousEffect, players);
            previousEffect = 0;
            continue;
        }
        previousEffect = 0;
        if (!effect.sound.empty()) {
            if (const auto sound = players[index].figure->voice().find(effect.sound);
                sound.has_value() && m_resources->sounds != nullptr) {
                m_resources->sounds->play(players[index].figure->voice().sequence(*sound), 1.0f,
                                          SoundCategory::Effects);
            } else {
                m_resources->audio.playNamed(effect.sound);
            }
        }
        std::optional<MoveAttachment> attachment;
        // Stationary area trees follow the player's first posed node. Saved matrices,
        // floor placement and stage/effect parents have separate SFXX policies.
        constexpr u32 kOtherParents = 0x10U | 0x40U | 0x80U | 0x800U | 0x40000U;
        if (!at && strike.type != MoveStrike::kFlies && strike.speed == 0 &&
            (effect.flags & kOtherParents) == 0) {
            MoveAttachment parent;
            parent.actor = index;
            parent.strike = effectIndex == strike.effect ? id : 0;
            parent.bodyParent = (effect.flags & 1U) != 0;
            if ((effect.flags & 0x2000U) != 0) {
                const s32 partner = players[index].combo.partner;
                if (partner >= 0 && static_cast<usize>(partner) < players.size()) {
                    parent.actor = static_cast<usize>(partner);
                }
            }
            const Vec3 offset = effect.offset + (parent.bodyParent ? Vec3{0} : strike.offset);
            parent.local = glm::translate(Mat4{1}, offset);
            if (effectIndex == strike.effect) {
                parent.local = glm::rotate(parent.local, strike.angle, Vec3{0, 1, 0});
            }
            attachment = parent;
        }
        ItemArchive* archive = moveEffectArchive(index, players, effect.tree);
        if (effect.tree.empty() || effect.tree == kNoEffectTree || archive == nullptr) {
            if (attachment && attachment->strike != 0) {
                m_moveAttachments.push_back(*attachment);
            }
            continue;
        }
        EffectTrees::Setting setting;
        setting.scale = effect.scale;
        setting.tint = effect.tint();
        setting.yaw = std::atan2(facing.x, facing.z) + strike.angle;
        setting.seconds = effect.lifetime;
        setting.shrinks = (effect.flags & 0x10000U) != 0 && strike.hitEffect < 0;
        if (effectIndex == strike.effect && started != nullptr && started->flies) {
            setting.velocity = started->facing * started->speed;
            setting.seconds = started->secondsLeft;
            // What flies launches once, then its looping tree carries it on.
            if (strike.loopEffect >= 0 &&
                static_cast<usize>(strike.loopEffect) < stats->moveEffects.size()) {
                setting.then = stats->moveEffects[static_cast<usize>(strike.loopEffect)].tree;
                setting.morphIn = started->secondsLeft - strike.maxTime;
                setting.holdForMorph = (strike.flags & 0x800) != 0;
            }
        }
        const Vec3 side{facing.z, 0.0f, -facing.x};
        const Vec3 at3 = origin + side * effect.offset.x + Vec3{0.0f, effect.offset.y, 0.0f} +
                         facing * effect.offset.z;
        // The strike's first effect gives off a light twice its reach in the class's colour,
        // swelling over a burst's life and steady on what flies (PlyrSfxDoDamageSub).
        if (effectIndex == strike.effect && started != nullptr) {
            const f32 reach = strike.radius > 0.0f ? strike.radius : strike.hitRadius;
            setting.light =
                EffectTrees::Light{DynamicLights::ofClass(players[index].actor.save().character),
                                   DynamicLights::kBlastRadiusScale * reach, !started->flies};
        }
        const u32 shown =
            m_resources->effects.startSet(m_resources->device, *archive, effect.tree, at3, setting);
        previousEffect = shown;
        if (attachment && (shown != 0 || attachment->strike != 0)) {
            attachment->effect = shown;
            m_moveAttachments.push_back(*attachment);
        }
        if (shown != 0 && effectIndex == strike.effect && started != nullptr && started->flies) {
            m_strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
    updateMoveAttachments(players);
}

void PlayerAttacks::updateMoveAttachments(std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_moveAttachments, [&](const MoveAttachment& attachment) {
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

void PlayerAttacks::startParticles(usize index, const MoveEffect& effect, u32 parent,
                                   std::span<PlayerRuntime> players) {
    if (!m_resources || index >= players.size()) {
        return;
    }
    ItemArchive* archive = moveEffectsOf(index, players);
    ItemArchive* textureArchive = nullptr;
    u32 textureSlot = 0;
    for (ItemArchive* candidate : {archive, &m_resources->weapons}) {
        if (candidate == nullptr) {
            continue;
        }
        const auto slot = candidate->textures.find(effect.tree);
        if (slot && !candidate->textures.entry(*slot).external() &&
            !candidate->textures.entry(*slot).noPicture) {
            textureArchive = candidate;
            textureSlot = *slot;
            break;
        }
    }
    if (textureArchive == nullptr) {
        log::warn("Player particles: no texture {}", effect.tree);
        return;
    }
    // PsfxDoParticle starts a default emitter, not one of the authored presets.
    // Its named setters configure a spherical cone and particle life/fade, not a volume.
    ParticleDescriptor descriptor;
    descriptor.texture = effect.tree;
    descriptor.emitFrames =
        effect.lifetime < 0.0f
            ? ParticleDescriptor::kEndless
            : static_cast<u32>(
                  std::clamp(effect.lifetime * ParticleDescriptor::kFrameRate, 1.0f, 65535.0f));
    descriptor.fadeFrames = 1; // 0.034 seconds, truncated at 30 Hz
    descriptor.angle = ParticleDescriptor::kSphere;
    descriptor.rate.fill(effect.radius);
    descriptor.speed = static_cast<f32>(effect.alphaMod) * 0.01f / ParticleDescriptor::kFrameRate;
    const bool shortLife = (effect.flags & MoveEffect::kParticleFlags) == 0x02000000U;
    descriptor.particleLife = shortLife ? 6 : 15;
    descriptor.particleFade = shortLife ? 0 : 15;
    descriptor.red = descriptor.green = descriptor.blue = descriptor.alpha =
        ParticleEnvelope{255, 255, 255, 255};
    const f32 width = 0.5f * effect.scale;
    descriptor.width = ParticleEnvelope{width, width, width, width};
    ParticleEffect particle;
    particle.actor = index;
    particle.parent = (effect.flags & 0x40000U) != 0 ? parent : 0;
    particle.node = effect.sound;
    particle.offset = effect.offset;
    if (particle.parent == 0 && (effect.flags & 0x2000U) != 0) {
        const s32 partner = players[index].combo.partner;
        if (partner >= 0 && static_cast<usize>(partner) < players.size()) {
            particle.actor = static_cast<usize>(partner);
            particle.node.clear();
        }
    }
    particle.effect = m_resources->effects.startParticles(m_resources->device, *textureArchive,
                                                          descriptor, textureSlot, Mat4{1});
    m_particleEffects.push_back(std::move(particle));
    updateParticles(players);
}

void PlayerAttacks::updateParticles(std::span<PlayerRuntime> players) {
    if (!m_resources) {
        return;
    }
    std::erase_if(m_particleEffects, [&](const ParticleEffect& particle) {
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
    if ((effect.flags & 2U) != 0 && m_resources->shake != nullptr) {
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
    if (!m_resources.has_value()) {
        return;
    }
    updateParticles(players);
    updateMoveAttachments(players);
    for (auto& source : m_strikeSources) {
        for (auto& contact : source.contacts) {
            contact.remaining -= seconds;
        }
    }
    for (const StrikeHit& hit : m_strikes.update(seconds, &m_resources->world.collision())) {
        const auto source = std::ranges::find(m_strikeSources, hit.strike, &StrikeSource::strike);
        if (source == m_strikeSources.end() || source->actor >= players.size()) {
            continue;
        }
        const auto& owner = players[source->actor].actor;
        const ClassStats* stats = m_resources->classes.stats(owner.save().character);
        if (stats == nullptr || source->row < 0 ||
            static_cast<usize>(source->row) >= stats->moveStrikes.size()) {
            continue;
        }
        MoveStrike row = stats->moveStrikes[static_cast<usize>(source->row)];
        row.damageType = source->damageType;
        bool stoppedByPlayer = false;
        if (source->multiplayer == MultiplayerMode::Hurt && targets.hurt &&
            (row.damageType & Damage::kMagic) == 0) {
            for (usize index = 0; index < players.size(); ++index) {
                auto& player = players[index];
                if (player.actor.player() == hit.owner || player.life != PlayerLife::Standing ||
                    player.departed) {
                    continue;
                }
                const Vec3 toward = player.actor.position() - hit.centre;
                const f32 across = std::hypot(toward.x, toward.z);
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
                constexpr u32 kReflectiveArmor = 0x01020000;
                const u32 armor =
                    PowerupEffects::of(player.actor.save().progress().inventory).armor;
                if (hit.swept && (armor & kReflectiveArmor) != 0) {
                    m_strikes.hitPlayer(hit.strike, true, hit.from);
                    m_resources->audio.playNamed("S_RICOCHET");
                    for (const auto& effect : m_strikeEffects) {
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
                        constexpr u32 kLightAreaMask = 0x170;
                        constexpr u32 kNoHitEffect = 0x1000000;
                        flags = (flags & ~kLightAreaMask) | kNoHitEffect;
                    }
                    targets.hurt(index, hit.damage, HurtKind::Blow, {flags, direction});
                    if ((row.damageType & Damage::kGas) != 0) {
                        player.effectGap = kShieldHarmEvery;
                    } else if (hit.damage > 2) {
                        f32 gap = hit.hitGap;
                        if (hit.swept) {
                            const bool passThrough = (row.damageType & powerup::kSuperShot) != 0;
                            const std::string_view tree =
                                row.hitEffect >= 0 && static_cast<usize>(row.hitEffect) <
                                                          stats->moveEffects.size()
                                    ? stats->moveEffects[static_cast<usize>(row.hitEffect)].tree
                                    : std::string_view{};
                            gap = passThrough ? 1.0f
                                              : PlayerMissiles::hitGap(
                                                    moveEffectsOf(source->actor, players), tree);
                        }
                        player.effectGap = gap;
                    }
                }
                if (hit.swept && (row.damageType & powerup::kSuperShot) != 0 &&
                    (row.damageType & powerup::kReflect) != 0 && row.hitEffect >= 0 &&
                    hit.damage > 2) {
                    presentStrikeHit(source->actor, row, player.actor.position(), players, false);
                    source->damageType &= ~powerup::kSuperShot;
                }
                if (hit.swept && (row.damageType & powerup::kSuperShot) == 0) {
                    presentStrikeHit(source->actor, row, player.actor.followPoint(), players);
                    m_strikes.hitPlayer(hit.strike, false, hit.from);
                    stoppedByPlayer = true;
                    break;
                }
            }
        }
        if (stoppedByPlayer) {
            continue;
        }
        targets.fixtures.shootScenery(hit.centre, hit.radius);
        for (const MissileTarget& target : strikeTargets(targets)) {
            MissileTarget contactTarget = target;
            if (!hit.swept && target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase) {
                // ProcessEffects' stationary swarm pass uses NormalVector2D;
                // flying effects and item/node collision retain their height tests.
                contactTarget.base.y = hit.centre.y;
            }
            if (!contactTarget.reachedBy(hit)) {
                continue;
            }
            auto contact =
                std::ranges::find(source->contacts, target.id, &StrikeSource::Contact::target);
            if (contact != source->contacts.end() && contact->remaining > 0) {
                continue;
            }
            // A pass-through wave must not deal its full damage every render frame.
            constexpr u32 kPassThrough = 0x100000;
            constexpr f32 kProjectileGap = 0.25f;
            constexpr f32 kPassThroughExtraGap = 3.0f;
            const f32 baseGap = hit.swept ? kProjectileGap : hit.hitGap;
            const f32 gap =
                hit.damage > 2
                    ? baseGap + ((row.damageType & kPassThrough) != 0 ? kPassThroughExtraGap : 0.0f)
                    : 0.0f;
            if (contact == source->contacts.end()) {
                source->contacts.push_back({target.id, gap});
            } else {
                contact->remaining = gap;
            }
            strikeTarget(target, hit.damage, row.damageType, owner, players, targets);
            presentStrikeHit(source->actor, row, target.base, players);
        }
    }
    targets.fixtures.settleBlasts(players, targets.fixtureEvents);
    std::erase_if(m_strikeSources, [this](const StrikeSource& source) {
        return m_strikes.find(source.strike) == nullptr;
    });
    std::erase_if(m_strikeEffects, [this](const StrikeEffect& pair) {
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
    const f32 size = std::min(PlayerArsenal::kBurstPerPower * power, 1.0f);
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
        setting.scale = size;
        setting.seconds = kShieldSeconds;
        setting.light = EffectTrees::Light{DynamicLights::ofPotion(static_cast<s32>(look)),
                                           DynamicLights::kMagicRadiusPerPower * power};
        shield.effect =
            m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                          kShieldTrees[look], actor.position(), setting);
    }
    m_resources->audio.playNamed(kShieldSounds[look]);
    m_resources->audio.playNamed("S_TURBODEFENSE", LevelSoundscape::kStepVolume);
    m_shields.push_back(shield);
}

void PlayerAttacks::stopDeathSounds(std::span<PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    for (auto& player : players) {
        m_resources->audio.stop(player.deathHeldSuck);
        m_resources->audio.stop(player.deathHeldCry);
        player.deathHeldSuck = kNoSound;
        player.deathHeldCry = kNoSound;
    }
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
    const auto release = [&]() -> std::optional<Vec3> {
        runtime.deathHeld = -1;
        runtime.deathHeldTicks = 0;
        m_resources->effects.stop(runtime.deathHeldEffect);
        runtime.deathHeldEffect = 0;
        m_resources->audio.stop(runtime.deathHeldSuck);
        runtime.deathHeldSuck = kNoSound;
        m_resources->audio.stop(runtime.deathHeldCry);
        runtime.deathHeldCry = kNoSound;
        return std::nullopt;
    };
    const PlayerActor& actor = runtime.actor;
    const auto worn = PowerupEffects::of(actor.save().progress().inventory);
    if (!allowed || runtime.life != PlayerLife::Standing ||
        (worn.armor & DeathRules::kProtection) == 0) {
        return release();
    }
    const Vec3 facing = actor.facing();
    const f32 facingLength = std::hypot(facing.x, facing.z);
    std::optional<MissileTarget> nearest;
    f32 best = kGrabReach;
    for (const MissileTarget& target : projectileTargets(targets)) {
        const Vec3 toward = target.base - actor.position();
        const f32 flat = std::hypot(toward.x, toward.z);
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
    Enemies& enemies = targets.opponents.enemies();
    if (!nearest || nearest->id < kEnemyTargetBase || nearest->id >= kGeneratorTargetBase) {
        return release();
    }
    const s32 slot = nearest->id - kEnemyTargetBase;
    if (enemies.kindOf(slot) != kDeathKind || !enemies.alive(slot) || enemies.dying(slot)) {
        return release();
    }
    const Vec3 separation = nearest->base - actor.position();
    constexpr f32 kContactTolerance = 0.001f;
    if (std::hypot(separation.x, separation.z) >
            actor.radius() + nearest->radius + kContactTolerance ||
        nearest->base.y > actor.position().y + actor.height() ||
        nearest->base.y + nearest->height < actor.position().y) {
        return release();
    }
    if (runtime.deathHeld != slot) {
        release();
        runtime.deathHeld = slot;
        m_resources->audio.playNamed(kHaloSound, kHaloVolume);
    }
    const Vec3 at = enemies.positionOf(slot);
    runtime.deathHeldTicks += ticks;
    while (runtime.deathHeldTicks >= kTicksPerFrame && enemies.alive(slot) &&
           !enemies.dying(slot)) {
        runtime.deathHeldTicks -= kTicksPerFrame;
        targets.opponents.strikeEnemy(slot, 0.0f, 0, at - actor.position(), actor.player(),
                                      players);
    }
    SoundPlayer* sounds = m_resources->sounds;
    if (sounds == nullptr || !sounds->isPlaying(runtime.deathHeldCry)) {
        runtime.deathHeldCry = m_resources->audio.playNamed(kDeathCry, kDeathVolume);
    }
    if (sounds == nullptr || !sounds->isPlaying(runtime.deathHeldSuck)) {
        runtime.deathHeldSuck = m_resources->audio.playNamed(kDeathSuck, kDeathVolume);
    }
    if (runtime.deathHeldEffect == 0) {
        if (ItemArchive* archive = enemies.archive(kDeathKind); archive != nullptr) {
            EffectTrees::Setting settings;
            settings.persistent = true;
            settings.depthWrite = false;
            runtime.deathHeldEffect = m_resources->effects.startSet(
                m_resources->device, *archive,
                DeathRules::effect(DeathRules::form(enemies.tierOf(slot))), actor.position(),
                settings);
        }
    } else {
        m_resources->effects.moveTo(runtime.deathHeldEffect, actor.position());
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
        const bool fire = (worn.armor & powerup::kFireShield) != 0;
        const bool lightning = (worn.armor & powerup::kLightningShield) != 0;
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
        if (fire) {
            strikeTarget(*target, kFireShieldDamage * seconds * kFramesPerSecond * grown,
                         kFireElement, actor, players, targets);
            continue;
        }
        if (std::ranges::any_of(runtime.shockGaps, [&](const PlayerRuntime::ShockGap& gap) {
                return gap.target == target->id;
            })) {
            continue;
        }
        strikeTarget(*target, kLightningShieldDamage * grown, kShockFlags, actor, players, targets);
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
    for (PotionShield& shield : m_shields) {
        shield.secondsLeft -= seconds;
        if (shield.secondsLeft <= 0 || shield.actor >= players.size() ||
            players[shield.actor].life != PlayerLife::Standing) {
            shield.secondsLeft = 0.0f;
            continue;
        }
        const Vec3 at = players[shield.actor].actor.position();
        m_resources->effects.moveTo(shield.effect, at);
        shield.harmIn -= seconds;
        if (shield.harmIn > 0.0f) {
            continue;
        }
        shield.harmIn = kShieldHarmEvery;
        const auto& actor = players[shield.actor].actor;
        // Its magic carries its bearer's class perk as a burst's does (start_magic's shield).
        if (const auto perk =
                MagicPerk::of(actor.save().character, experienceLevel(actor.save().experience()))) {
            targets.fixtures.bless(at, shield.radius, *perk, shield.actor, shield.blessed,
                                   targets.fixtureEvents);
        }
        for (const s32 id : targets.opponents.enemies().reachedBy(
                 at, shield.radius, std::numbers::pi_v<f32>, {0, 0, 1})) {
            const Vec3 struck = targets.opponents.enemies().positionOf(id);
            const f32 credit = targets.opponents.strikeEnemy(id, shield.damage, shield.flags,
                                                             struck - at, actor.player(), players);
            healHit(actor.player(), shield.flags, credit, struck, players, targets);
        }
        for (const s32 id : targets.opponents.critters().reachedBy(
                 at, shield.radius, std::numbers::pi_v<f32>, {0, 0, 1})) {
            const Vec3 struck = targets.opponents.critters().positionOf(id);
            const f32 credit =
                targets.opponents.strikeCritter(id, shield.damage, shield.flags, struck - at,
                                                actor.player(), std::nullopt, false, players);
            healHit(actor.player(), shield.flags, credit, struck, players, targets);
        }
        for (const s32 id : targets.opponents.generators().within(at, shield.radius)) {
            targets.opponents.strikeGenerator(id, shield.damage, actor.player());
        }
        if (targets.opponents.bosses().within(at, shield.radius)) {
            EnemyHit hit;
            hit.damage = shield.damage;
            hit.flags = shield.flags;
            hit.player = actor.player();
            hit.level = experienceLevel(actor.save().experience());
            hit.direction = *targets.opponents.bosses().position() - at;
            const f32 credit = targets.opponents.bosses().hurt(hit);
            healHit(actor.player(), shield.flags, credit, *targets.opponents.bosses().position(),
                    players, targets);
        }
        // Magic leaves the walls, the rocks and every barrel but one holding something alone.
        for (const usize barrel : targets.fixtures.barrels().within(at, shield.radius)) {
            if (!immuneToMagic(static_cast<s32>(barrel), targets)) {
                targets.fixtures.strikeBarrel(barrel, shield.damage,
                                              players[shield.actor].actor.player(), players,
                                              targets.fixtureEvents);
            }
        }
    }
    targets.fixtures.settleBlasts(players, targets.fixtureEvents);
    std::erase_if(m_shields, [this](const PotionShield& shield) {
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
    if (index >= players.size() || players[index].blockLeft > 0.0f || taken <= kBlockWorth) {
        return;
    }
    const f32 shown = std::clamp(kBlockPerDamage * left, kBlockLeast, kBlockMost);
    players[index].blockLeft = shown;
    players[index].blocked = true;
    if (m_resources->weapons.loaded() &&
        m_resources->weapons.trees.find(kBlockEffect).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = shown;
        m_resources->effects.startSet(m_resources->device, m_resources->weapons, kBlockEffect,
                                      players[index].actor.followPoint(), setting);
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
    TurboMeter& meter = players[index].turbo;
    const PlayerAnimator& body = players[index].figure->animator();
    TurboMove& move = players[index].move;
    const ClassStats* stats = m_resources->classes.stats(players[index].actor.save().character);
    if (body.turboBegan()) {
        if (const std::string_view voice = move.begin(body.action(), stats, meter);
            !voice.empty()) {
            cry(index, voice, players);
        }
    }
    move.advance(body.action(), body.player().frame(), players[index].actor.facing(), stats, meter,
                 {.announce = [&help, index](s32 id) { help(id, index); },
                  .dim =
                      [this, index, players](f32 amount) {
                          // The striker blazes against the dark it brings (PlyrSfxDoDamage).
                          m_resources->dimmer.ask(amount);
                          players[index].glow.raise(BodyGlow::kStrike);
                      },
                  .volley =
                      [this, index, players](const Vec3& direction) {
                          m_resources->arsenal.launchWeapon(players[index].actor,
                                                            players[index].figure.get(), direction,
                                                            1.0f, false);
                      },
                  .strike = [this, index, players](s32 row) { fireStrike(index, row, players); }});
    if (body.action() == PlayerAnimator::Action::Shove) {
        meter.drain(seconds);
    } else if (players[index].life == PlayerLife::Standing && !body.turboing() &&
               meter.fill(seconds)) {
        help(HelpMessages::kUseTurbo, index);
    }
    // With two fifths of the meter, and less than all of it, the combo is suggested
    // (pmotion.c 1760; the scene waits for a party and a crowd).
    if (players[index].life == PlayerLife::Standing && !body.turboing() &&
        meter.held() >= TurboMeter::kStrongCost && meter.held() < TurboMeter::kFull) {
        help(HelpMessages::kUseCombo, index);
    }
    meter.step(ticks);
}

/** One of a character's own cries, `which` being what follows its class in the name. */
void PlayerAttacks::cry(usize index, std::string_view which, std::span<PlayerRuntime> players) {
    if (!m_resources.has_value()) {
        return;
    }
    PlayerFigure* body = index < players.size() ? players[index].figure.get() : nullptr;
    if (body == nullptr || m_resources->sounds == nullptr) {
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
        return target.id >= kSafeRockTargetBase && target.id < kWallTargetBase;
    });
    return reachable;
}

bool PlayerAttacks::immuneToMagic(s32 id, const Targets& targets) {
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
    if (id < kSwitchTargetBase) {
        return false;
    }
    if (m_resources && (flags & Damage::kGas) == 0) {
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
                                       const Targets& targets) const {
    if (!m_resources) {
        return std::nullopt;
    }
    auto candidates = acquisitionTargets(targets);
    const f32 range =
        targets.opponents.bosses().view().alive ? TargetAssist::kBossRange : TargetAssist::kRange;
    const auto ordinary =
        TargetAssist::select(actor.followPoint(), facing, candidates, range,
                             &m_resources->world.collision(), m_resources->acquisitionCone);
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
    return TargetAssist::select(actor.followPoint(), facing, candidates, range,
                                &m_resources->world.collision());
}

/** Whether a target id is a creature: an enemy, a great one or a boss, not a thing. */
bool PlayerAttacks::isCreature(s32 id) {
    return id >= kEnemyTargetBase && id < kSafeRockTargetBase &&
           (id < kGeneratorTargetBase || id >= kCritterTargetBase);
}

PlayerDeed PlayerAttacks::attackDeed(const PlayerActor& actor, bool strong, const Targets& targets,
                                     bool moved, s32 chain, std::optional<Vec3> facing) const {
    if (const auto item =
            ItemAttack::select(PowerupEffects::of(actor.save().progress().inventory))) {
        return item->deed;
    }
    const PlayerDeed ranged = strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
    if (!m_resources) {
        return ranged;
    }
    const MeleeSense sense = meleeSense(actor, true, targets, facing);
    // What is a step away is struck only by stepping to it (never at something low), or
    // mid-chain by the slow swing.
    const bool steps = sense.range == MeleeRange::Step && moved && !sense.low;
    const bool slowInChain = strong && chain != 0 && moved && sense.range != MeleeRange::Beyond;
    if (sense.range != MeleeRange::Swing && !steps && !slowInChain) {
        return ranged;
    }
    if (strong) {
        return sense.low ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow;
    }
    return sense.low ? PlayerDeed::MeleeLow : PlayerDeed::Melee;
}

PlayerDeed PlayerAttacks::automaticMeleeDeed(const PlayerActor& actor, const Targets& targets,
                                             const Vec3& facing) const {
    if (!m_resources) {
        return PlayerDeed::None;
    }
    const auto target = meleeTarget(actor, targets, facing, actor.reach() + kStepReach);
    if (!target || target->id < kEnemyTargetBase || target->id >= kSafeRockTargetBase) {
        return PlayerDeed::None;
    }
    if (target->id >= kBossTargetBase) {
        // PlayerMotion's boss-family exception permits only the two mobile
        // encounters, DRIDER and LICH, to trigger an unpressed close attack.
        constexpr s32 kSpiderQueen = 37;
        constexpr s32 kLich = 41;
        const s32 kind = targets.opponents.bosses().view().kind;
        if (kind != kSpiderQueen && kind != kLich) {
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

std::optional<MissileTarget> PlayerAttacks::meleeTarget(const PlayerActor& actor,
                                                        const Targets& targets, const Vec3& facing,
                                                        f32 reach) const {
    if (!m_resources) {
        return std::nullopt;
    }
    const f32 range =
        targets.opponents.bosses().present() ? TargetAssist::kBossRange : TargetAssist::kRange;
    auto target = TargetAssist::ahead(
        actor.position(), actor.height(), facing, acquisitionTargets(targets), reach, range,
        &m_resources->world.collision(), m_resources->acquisitionCone);
    if (!target) {
        target = meleePlayer(actor, targets, reach, facing);
    }
    return target;
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
                                  const MissileTarget& target) {
    MeleeSense sense;
    const f32 distance = TargetAssist::distanceTo(actor.position(), actor.height(), target);
    const f32 bias = held ? kHeldReach : 0.0f;
    // PlayerMotion uses col_radius (the full PDAT width), not the half-width
    // cylinder used for horizontal movement. Target distance already excludes
    // the target's radius, so only the player's reach and authored margin remain.
    sense.range =
        distance < actor.reach() + kSwingReach + bias ? MeleeRange::Swing : MeleeRange::Step;
    const bool swarm = target.id >= kEnemyTargetBase && target.id < kGeneratorTargetBase;
    const bool thing = !isCreature(target.id) && target.id < kPlayerTargetBase;
    // PlayerMotion tests the swarm's height or the item's height, but never a
    // critter's small NODE collision part, when setting the low-attack bit.
    sense.low = distance < actor.reach() + kStepReach &&
                ((swarm && target.height <= kLowEnemy) || (thing && target.height <= kLowThing));
    const Vec3 toward = target.base - actor.position();
    if (std::hypot(toward.x, toward.z) > 1e-5f) {
        const f32 bearing = std::atan2(toward.x, toward.z) - actor.yaw();
        sense.yaw = std::remainder(bearing, 2.0f * std::numbers::pi_v<f32>);
    }
    return sense;
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
    const auto target =
        meleeTarget(actor, targets, players[index].meleeFacing.value_or(actor.facing()),
                    actor.reach() + kStepReach);
    // The swing's sweep brings down the SHOOTFALL scenery within it (combat.c's item query).
    targets.fixtures.shootScenery(actor.position(), actor.reach() + kStepReach);
    if (!target) {
        return;
    }
    f32 damage = PlayerMissiles::kLeastDamage;
    if (const ClassStats* stats = m_resources->classes.stats(actor.save().character)) {
        const StatBlock block = displayStats(*stats, experienceLevel(actor.save().experience()),
                                             actor.save().progress());
        damage = PlayerMissiles::damageFor(block.strength());
    }
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
    const Vec3 direction = target->base - actor.position();
    const s32 id = target->id;
    if (id >= kPlayerTargetBase) {
        if (targets.hurt) {
            targets.hurt(static_cast<usize>(id - kPlayerTargetBase), damage, HurtKind::Burn,
                         {flags, glm::normalize(direction)});
        }
        return;
    }
    if (strikeSwitch(id, flags)) {
        return;
    }
    f32 credit = 0.0f;
    if (id >= kWallTargetBase) {
        targets.fixtures.strikeWall(static_cast<usize>(id - kWallTargetBase), damage, flags);
        if (targets.fixtureEvents.help) {
            targets.fixtureEvents.help(HelpMessages::kSecretWalls, index);
        }
    } else if (id >= kBossTargetBase) {
        EnemyHit hit{
            damage, flags, direction, actor.player(), experienceLevel(actor.save().experience()),
            point,  true};
        hit.node = target->node;
        Bosses& bosses = targets.opponents.bosses();
        const BossView before = bosses.view();
        credit = bosses.hurt(hit, id - kBossTargetBase);
        // A blow a sleeping boss does not take, or the last, tells for three.
        players[index].streak.record(!before.alive || !before.awake || !bosses.view().alive);
    } else if (id >= kCritterTargetBase) {
        const s32 critter = id - kCritterTargetBase;
        const Critters& critters = targets.opponents.critters();
        const bool standing = critters.alive(critter) && !critters.dying(critter);
        credit = targets.opponents.strikeCritter(critter, damage, flags, direction, actor.player(),
                                                 point, true, players, target->node);
        players[index].streak.record(!standing || !critters.alive(critter) ||
                                     critters.dying(critter));
    } else if (id >= kGeneratorTargetBase) {
        targets.opponents.strikeGenerator(id - kGeneratorTargetBase, damage, actor.player(),
                                          players);
    } else if (id >= kEnemyTargetBase) {
        const s32 enemy = id - kEnemyTargetBase;
        const Enemies& enemies = targets.opponents.enemies();
        const bool standing = enemies.alive(enemy);
        credit = targets.opponents.strikeEnemy(enemy, damage, flags, direction, actor.player(),
                                               players, true, point);
        // Only a blow on something taller than the short counts towards the run (hht > 2).
        if (target->height > kLowEnemy) {
            players[index].streak.record(!standing || !enemies.alive(enemy));
        }
    } else {
        targets.fixtures.strikeBarrel(static_cast<usize>(id), damage, actor.player(), players,
                                      targets.fixtureEvents);
        targets.fixtures.settleBlasts(players, targets.fixtureEvents);
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
        const u32 element = hand.has_value() && runtime.actor.player() != occupiedHand
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
    glowWeapons(players, targets.occupiedHand);
    std::vector<MissileTarget> missileTargets = strikeTargets(targets);
    const PlacedItems& lying = m_resources->world.placedItems();
    for (const usize bottle : lying.shootablePotions()) {
        const PlacedItems::Item& item = lying.item(bottle);
        missileTargets.push_back(MissileTarget{kPotionTargetBase + static_cast<s32>(bottle),
                                               item.position, std::max(item.radius, 0.5f),
                                               std::max(item.height, 1.0f)});
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
            constexpr u32 kReflectiveArmor = 0x01020000;
            const u32 armor = PowerupEffects::of(player.actor.save().progress().inventory).armor;
            missilePlayers.push_back(
                {player.actor.player(),
                 {0, player.actor.position(), player.actor.radius(), player.actor.height()},
                 (armor & kReflectiveArmor) != 0});
        }
    }
    m_resources->arsenal.missiles().update(seconds, &m_resources->world.collision(), missileTargets,
                                           missilePlayers);
    // A weapon still flying brings down the SHOOTFALL scenery it passes (fn_8005EE18).
    const PlayerMissiles& flying = m_resources->arsenal.missiles();
    for (usize i = 0; i < flying.count(); ++i) {
        const PlayerMissiles::Missile& missile = flying.missile(i);
        targets.fixtures.shootScenery(missile.position,
                                      missile.spec != nullptr ? missile.spec->radius : 1.0f);
    }
    for (const MissileImpact& impact : m_resources->arsenal.missiles().takeImpacts()) {
        if (impact.worldObject >= 0) {
            if (m_resources->world.explodeObject(impact.worldObject, impact.position)) {
                for (const Vec3& position : m_resources->world.takeWorldExplosions()) {
                    targets.fixtures.worldExplosion(position, players, targets.fixtureEvents);
                }
            }
        }
        f32 playerDistance = std::numeric_limits<f32>::max();
        for (const PlayerRuntime& player : players) {
            if (player.life == PlayerLife::Standing) {
                playerDistance = std::min(
                    playerDistance, glm::distance(player.actor.followPoint(), impact.position));
            }
        }
        m_resources->arsenal.presentImpact(impact, playerDistance);
        if (impact.player >= 0) {
            const auto player = std::ranges::find_if(players, [&impact](const PlayerRuntime& p) {
                return p.actor.player() == impact.player;
            });
            if (player != players.end() && player->effectGap <= 0 && targets.hurt) {
                targets.hurt(
                    static_cast<usize>(player - players.begin()), impact.stun ? 0 : impact.damage,
                    impact.stun ? HurtKind::QuietBlow : HurtKind::Blow,
                    {impact.flags | (impact.stun ? PlayerImpact::kStun : 0), impact.direction});
                if (impact.damage > 2) {
                    player->effectGap = impact.playerHitGap;
                }
            }
            continue;
        }
        if (impact.potion != 0) {
            beginPotion(impact);
            continue;
        }
        if (impact.target >= kStatueTargetBase) {
            targets.opponents.wakeStatue(static_cast<usize>(impact.target - kStatueTargetBase));
            continue;
        }
        if (impact.target >= kPotionTargetBase) {
            shootPotion(impact, players, targets);
            continue;
        }
        if (impact.target >= kItemStopBase) {
            continue;
        }
        if (strikeSwitch(impact.target, impact.flags)) {
            continue;
        }
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
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    hit.direction = impact.position - actor.position();
                    hit.direction.y = 0.0f;
                    hit.level = experienceLevel(actor.save().experience());
                }
            }
            credit = targets.opponents.bosses().hurt(hit, impact.target - kBossTargetBase);
        } else if (impact.target >= kCritterTargetBase) {
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            credit = targets.opponents.strikeCritter(
                impact.target - kCritterTargetBase, impact.damage, impact.flags, direction,
                impact.owner, impact.position, false, players, impact.node);
        } else if (impact.target >= kGeneratorTargetBase) {
            targets.opponents.strikeGenerator(impact.target - kGeneratorTargetBase, impact.damage,
                                              impact.owner, players);
        } else if (impact.target >= kEnemyTargetBase) {
            // The hit travels the way the weapon flew: out from whoever threw it.
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            credit = targets.opponents.strikeEnemy(impact.target - kEnemyTargetBase, impact.damage,
                                                   impact.flags, direction, impact.owner, players,
                                                   false, impact.position);
        } else if (impact.target >= 0) {
            targets.fixtures.strikeBarrel(static_cast<usize>(impact.target), impact.damage,
                                          impact.owner, players, targets.fixtureEvents);
            targets.fixtures.settleBlasts(players, targets.fixtureEvents);
        }
        healHit(impact.owner, impact.flags, credit, impact.position, players, targets);
    }
    updatePotions(seconds, players, targets);
    updateItems(seconds, players, targets);
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
        if (targets.fixtureEvents.help) {
            targets.fixtureEvents.help(HelpMessages::kShotMagic, i);
        }
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
    m_potions.push_back(std::move(burst));
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
        if (std::hypot(offset.x, offset.z) > reach ||
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
                                   targets.fixtureEvents);
        }
        return;
    }
}

void PlayerAttacks::updatePotions(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    for (PotionBurst& burst : m_potions) {
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
                    ? std::hypot(direction.x, direction.z) > radius + target.radius ||
                          std::abs(target.base.y - burst.impact.position.y) > radius + target.height
                    : !target.touches(burst.impact.position, radius);
            if (outside || std::ranges::find(burst.hit, target.id) != burst.hit.end()) {
                continue;
            }
            burst.hit.push_back(target.id);
            direction.y = 0;
            const s32 byPlayer = burst.impact.owner;
            if (immuneToMagic(target.id, targets)) {
                continue;
            }
            burst.struck = true;
            if (target.id >= kWallTargetBase) {
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
                const f32 credit =
                    targets.opponents.bosses().hurt(hit, target.id - kBossTargetBase);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else if (target.id >= kCritterTargetBase) {
                const s32 id = target.id - kCritterTargetBase;
                const f32 credit = targets.opponents.strikeCritter(
                    id, power, flags, direction, byPlayer, target.base, false, players);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else if (target.id >= kGeneratorTargetBase) {
                targets.opponents.strikeGenerator(target.id - kGeneratorTargetBase, power, byPlayer,
                                                  players);
            } else if (target.id >= kEnemyTargetBase) {
                const s32 id = target.id - kEnemyTargetBase;
                const f32 credit =
                    targets.opponents.strikeEnemy(id, power, flags, direction, byPlayer, players);
                healHit(byPlayer, flags, credit, target.base, players, targets);
            } else {
                targets.fixtures.strikeBarrel(static_cast<usize>(target.id), power, byPlayer,
                                              players, targets.fixtureEvents);
            }
        }
        enchantChests(burst, radius, power, targets);
    }
    // Broken bottles may append a new wave through the fixture callback, so
    // resolve barrel chains only after iteration over existing waves finishes.
    targets.fixtures.settleBlasts(players, targets.fixtureEvents);
    // A caster's magic that struck nothing at all is wasted, and they are told so.
    for (const PotionBurst& burst : m_potions) {
        if (burst.elapsed < burst.duration || burst.struck || !targets.fixtureEvents.help) {
            continue;
        }
        for (usize i = 0; i < players.size(); ++i) {
            if (players[i].actor.player() == burst.impact.owner) {
                targets.fixtureEvents.help(HelpMessages::kWastedMagic, i);
            }
        }
    }
    std::erase_if(m_potions,
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
            std::hypot(apart.x, apart.z) < reach) {
            heal(other, given * kHealingOthers);
        }
    }
    if (m_resources->weapons.loaded()) {
        m_resources->effects.start(m_resources->device, m_resources->weapons, kHealingEffect, at);
    }
    if (targets.fixtureEvents.help) {
        targets.fixtureEvents.help(HelpMessages::kHealingMagic,
                                   static_cast<usize>(std::distance(players.begin(), caster)));
    }
}

} // namespace gdl::game
