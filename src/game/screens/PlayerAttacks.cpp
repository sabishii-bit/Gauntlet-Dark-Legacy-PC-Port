#include "game/screens/PlayerAttacks.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "game/combat/Damage.h"
#include "game/enemies/DeathRules.h"
#include "game/enemies/EnemyKinds.h"
#include "game/players/ItemPickup.h"
#include "game/players/MagicPerks.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/world/Chests.h"
#include "game/world/DynamicLights.h"
#include "game/world/TargetAssist.h"
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

} // namespace
void PlayerAttacks::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
}
void PlayerAttacks::clear() {
    if (m_resources.has_value()) {
        for (const auto& item : m_items) {
            m_resources->effects.stop(item.effect);
        }
        for (const StrikeEffect& effect : m_strikeEffects) {
            m_resources->effects.stop(effect.effect);
        }
        for (const PotionShield& shield : m_shields) {
            m_resources->effects.stop(shield.effect);
        }
    }
    m_strikes.clear();
    m_strikeEffects.clear();
    m_strikeSources.clear();
    m_shields.clear();
    m_potions.clear();
    m_nextPotionKind = 1;
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

/** The costume colour's effects, which hold the trees a class's moves show; loaded when
 * first wanted. */
ItemArchive* PlayerAttacks::moveEffectsOf(usize index, std::span<PlayerRuntime> players) {
    PlayerFigure* figure = index < players.size() ? players[index].figure.get() : nullptr;
    return figure != nullptr ? figure->effects() : nullptr;
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
void PlayerAttacks::fireStrike(usize index, s32 strikeIndex, std::span<PlayerRuntime> players) {
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
    // A span that only lasts, or a volley, harms nothing of itself; the rest are set going.
    u32 id = 0;
    if (strike.harms()) {
        MoveStrike volume = strike;
        if (strike.effect >= 0 && static_cast<usize>(strike.effect) < stats->moveEffects.size()) {
            volume.offset += stats->moveEffects[static_cast<usize>(strike.effect)].offset;
        }
        id = m_strikes.start(volume, actor.player(), actor.position(), facing,
                             ownDamageOf(index, players));
        m_strikeSources.push_back(StrikeSource{id, index, strikeIndex, {}});
    }
    const Vec3 origin = MoveStrikes::originOf(strike, actor.position(), facing);
    const MoveStrikes::Strike* started = m_strikes.find(id);
    ItemArchive* archive = moveEffectsOf(index, players);
    // An effect may bring another with it.
    usize followed = 0;
    for (s32 at = strike.effect; at >= 0 && static_cast<usize>(at) < stats->moveEffects.size() &&
                                 followed < stats->moveEffects.size();
         at = stats->moveEffects[static_cast<usize>(at)].next, ++followed) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(at)];
        // A particle record's names are a texture and a node, neither a tree nor a sound;
        // its emitter is not drawn yet.
        if (effect.particle()) {
            continue;
        }
        if (!effect.sound.empty()) {
            if (const auto sound = players[index].figure->voice().find(effect.sound);
                sound.has_value() && m_resources->sounds != nullptr) {
                m_resources->sounds->play(players[index].figure->voice().sequence(*sound), 1.0f,
                                          SoundCategory::Effects);
            } else {
                m_resources->audio.playNamed(effect.sound);
            }
        }
        if (effect.tree.empty() || effect.tree == kNoEffectTree || archive == nullptr ||
            !archive->trees.find(effect.tree).has_value()) {
            continue;
        }
        EffectTrees::Setting setting;
        setting.scale = effect.scale;
        setting.yaw = std::atan2(facing.x, facing.z) + strike.angle;
        if (started != nullptr && started->flies) {
            setting.velocity = started->facing * started->speed;
            setting.seconds = started->secondsLeft;
            // What flies launches once, then its looping tree carries it on.
            if (at == strike.effect && strike.loopEffect >= 0 &&
                static_cast<usize>(strike.loopEffect) < stats->moveEffects.size()) {
                setting.then = stats->moveEffects[static_cast<usize>(strike.loopEffect)].tree;
            }
        }
        const Vec3 side{facing.z, 0.0f, -facing.x};
        const Vec3 at3 = origin + side * effect.offset.x + Vec3{0.0f, effect.offset.y, 0.0f} +
                         facing * effect.offset.z;
        // The strike's first effect gives off a light twice its reach in the class's colour,
        // swelling over a burst's life and steady on what flies (PlyrSfxDoDamageSub).
        if (at == strike.effect && strike.harms()) {
            const f32 reach = strike.radius > 0.0f ? strike.radius : strike.hitRadius;
            setting.light = EffectTrees::Light{
                DynamicLights::ofClass(players[index].actor.save().character),
                DynamicLights::kBlastRadiusScale * reach, started == nullptr || !started->flies};
        }
        const u32 shown =
            m_resources->effects.startSet(m_resources->device, *archive, effect.tree, at3, setting);
        if (shown != 0 && started != nullptr && started->flies) {
            m_strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
}

/** All target families share the strike's cosine cone and swept cylinder contacts. */
void PlayerAttacks::updateStrikes(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
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
        const MoveStrike& row = stats->moveStrikes[static_cast<usize>(source->row)];
        for (const MissileTarget& target : strikeTargets(targets)) {
            if (!target.reachedBy(hit)) {
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
            const f32 gap = hit.damage > 2 ? kProjectileGap + ((row.damageType & kPassThrough) != 0
                                                                   ? kPassThroughExtraGap
                                                                   : 0.0f)
                                           : 0.0f;
            if (contact == source->contacts.end()) {
                source->contacts.push_back({target.id, gap});
            } else {
                contact->remaining = gap;
            }
            strikeTarget(target, hit.damage, row.damageType, owner, players, targets);
            ItemArchive* archive = moveEffectsOf(source->actor, players);
            if (archive == nullptr) {
                continue;
            }
            const s32 mark = row.hitEffect;
            if (mark >= 0 && static_cast<usize>(mark) < stats->moveEffects.size()) {
                const MoveEffect& effect = stats->moveEffects[static_cast<usize>(mark)];
                if (!effect.tree.empty() && archive->trees.find(effect.tree).has_value()) {
                    m_resources->effects.start(m_resources->device, *archive, effect.tree,
                                               target.base, effect.scale);
                }
                if (!effect.sound.empty()) {
                    m_resources->audio.playNamed(effect.sound);
                }
            }
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
    const s32 kind = actor.save().progress().inventory.takePotion();
    if (kind == 0) {
        return;
    }
    const auto look = static_cast<usize>(std::clamp(kind, 0, 4));
    const f32 power = m_resources->arsenal.magicPowerOf(actor);
    const f32 size = std::min(PlayerArsenal::kBurstPerPower * power, 1.0f);
    PotionShield shield;
    shield.actor = index;
    shield.radius = kShieldPotency * power;
    shield.damage = kShieldDamage;
    shield.flags = EnemyHit::kMagic | static_cast<u32>(kind);
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
    m_shields.push_back(shield);
}

/** The rings follow their bearers and harm nearby creatures and breakable fixtures. */
/** The halo's hold on Death (PlayerMotion, pmotion.c 1621): with Death the nearest thing
 * ahead within thirty (PlayerGetTarget; one already held while he is anywhere in the half
 * ahead), the wearer stands facing him and, each 30 Hz frame, draws a point off him as a hit
 * would (damage_enemy's halo branch: health back, or experience from the black form).
 * S_HALO sounds as the hold begins, S_DEATHDIE while he is held, and his drain effect and
 * S_DEATHSUCK follow the one holding him (player.c 5869). */
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
        const bool held = target.id == kEnemyTargetBase + runtime.deathHeld;
        const f32 dot = (toward.x * facing.x + toward.z * facing.z) / (flat * facingLength);
        if (dot < (held ? kHeldCone : kGrabCone)) {
            continue;
        }
        if (const f32 distance = TargetAssist::distanceTo(actor.position(), actor.height(), target);
            distance < best) {
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
            targets.opponents.strikeEnemy(id, shield.damage, shield.flags,
                                          targets.opponents.enemies().positionOf(id) - at,
                                          actor.player(), players);
        }
        for (const s32 id : targets.opponents.critters().reachedBy(
                 at, shield.radius, std::numbers::pi_v<f32>, {0, 0, 1})) {
            targets.opponents.strikeCritter(id, shield.damage, shield.flags,
                                            targets.opponents.critters().positionOf(id) - at,
                                            actor.player(), std::nullopt, false, players);
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
            targets.opponents.bosses().hurt(hit);
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
                  .dim = [this](f32 amount) { m_resources->dimmer.ask(amount); },
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
    if (const auto sound = body->voice().find(std::format("S_{}{}", voice, which));
        sound.has_value()) {
        m_resources->sounds->play(body->voice().sequence(*sound), 1.0f, SoundCategory::Effects);
    }
}

std::vector<MissileTarget> PlayerAttacks::strikeTargets(const Targets& targets) const {
    std::vector<MissileTarget> all = projectileTargets(targets);
    if (m_resources) {
        const LevelTriggers& triggers = m_resources->world.triggers();
        for (usize i = 0; i < triggers.size(); ++i) {
            const LevelTrigger& trigger = triggers.trigger(i);
            if (trigger.shootable) {
                all.push_back(MissileTarget{kSwitchTargetBase + static_cast<s32>(i), trigger.spot,
                                            trigger.radius, trigger.height});
            }
        }
    }
    return all;
}

std::vector<MissileTarget> PlayerAttacks::meleeTargets(const Targets& targets) const {
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
            const Breakables::Barrel& cask = targets.fixtures.barrels().barrel(barrel);
            missileTargets.push_back(MissileTarget{static_cast<s32>(barrel), cask.figure.position(),
                                                   cask.radius, cask.height});
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
    for (usize g = 0; g < targets.opponents.generators().count(); ++g) {
        if (targets.opponents.generators().standing(static_cast<s32>(g))) {
            const Obstacle& box = targets.opponents.generators().boxOf(static_cast<s32>(g));
            missileTargets.push_back(
                MissileTarget{static_cast<s32>(g) + kGeneratorTargetBase,
                              targets.opponents.generators().positionOf(static_cast<s32>(g)),
                              std::max(box.halfAcross, box.halfAlong), box.height});
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
    return TargetAssist::select(actor.followPoint(), facing, projectileTargets(targets),
                                targets.opponents.bosses().view().alive ? TargetAssist::kBossRange
                                                                        : TargetAssist::kRange,
                                &m_resources->world.collision());
}

/** Whether a target id is a creature: an enemy, a great one or a boss, not a thing. */
bool PlayerAttacks::isCreature(s32 id) {
    return id >= kEnemyTargetBase && id < kSafeRockTargetBase &&
           (id < kGeneratorTargetBase || id >= kCritterTargetBase);
}

PlayerDeed PlayerAttacks::attackDeed(const PlayerActor& actor, bool strong, const Targets& targets,
                                     bool moved, s32 chain) const {
    if (const auto item =
            ItemAttack::select(PowerupEffects::of(actor.save().progress().inventory))) {
        return item->deed;
    }
    const PlayerDeed ranged = strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
    if (!m_resources) {
        return ranged;
    }
    const MeleeSense sense = meleeSense(actor, true, targets);
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

MeleeSense PlayerAttacks::meleeSense(const PlayerActor& actor, bool held,
                                     const Targets& targets) const {
    MeleeSense sense;
    sense.range = MeleeRange::Beyond;
    if (!m_resources) {
        return sense;
    }
    const f32 bias = held ? kHeldReach : 0.0f;
    const auto target =
        TargetAssist::around(actor.position(), actor.height(), meleeTargets(targets),
                             actor.radius() + kStepReach + bias, &m_resources->world.collision());
    if (!target) {
        return sense;
    }
    const f32 distance = TargetAssist::distanceTo(actor.position(), actor.height(), *target);
    sense.range =
        distance < actor.radius() + kSwingReach + bias ? MeleeRange::Swing : MeleeRange::Step;
    sense.low = distance < actor.radius() + kStepReach &&
                target->height <= (isCreature(target->id) ? kLowEnemy : kLowThing);
    const Vec3 toward = target->base - actor.position();
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
    // The blow lands on whatever is nearest within a step, whichever way it lies: the
    // swing has already turned to it.
    const auto target =
        TargetAssist::around(actor.position(), actor.height(), meleeTargets(targets),
                             actor.radius() + kStepReach, &m_resources->world.collision());
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
    if (strikeSwitch(id, flags)) {
        return;
    }
    if (id >= kWallTargetBase) {
        targets.fixtures.strikeWall(static_cast<usize>(id - kWallTargetBase), damage, flags);
        if (targets.fixtureEvents.help) {
            targets.fixtureEvents.help(HelpMessages::kSecretWalls, index);
        }
    } else if (id >= kBossTargetBase) {
        const EnemyHit hit{
            damage, flags, direction, actor.player(), experienceLevel(actor.save().experience()),
            point,  true};
        targets.opponents.bosses().hurt(hit, id - kBossTargetBase);
    } else if (id >= kCritterTargetBase) {
        targets.opponents.strikeCritter(id - kCritterTargetBase, damage, flags, direction,
                                        actor.player(), point, true, players);
    } else if (id >= kGeneratorTargetBase) {
        targets.opponents.strikeGenerator(id - kGeneratorTargetBase, damage, actor.player(),
                                          players);
    } else if (id >= kEnemyTargetBase) {
        targets.opponents.strikeEnemy(id - kEnemyTargetBase, damage, flags, direction,
                                      actor.player(), players, true, point);
    } else {
        targets.fixtures.strikeBarrel(static_cast<usize>(id), damage, actor.player(), players,
                                      targets.fixtureEvents);
        targets.fixtures.settleBlasts(players, targets.fixtureEvents);
    }
}

void PlayerAttacks::updateProjectiles(f32 seconds, std::span<PlayerRuntime> players,
                                      const Targets& targets) {
    if (!m_resources) {
        return;
    }
    std::vector<MissileTarget> missileTargets = strikeTargets(targets);
    const PlacedItems& lying = m_resources->world.placedItems();
    for (const usize bottle : lying.shootablePotions()) {
        const PlacedItems::Item& item = lying.item(bottle);
        missileTargets.push_back(MissileTarget{kPotionTargetBase + static_cast<s32>(bottle),
                                               item.position, std::max(item.radius, 0.5f),
                                               std::max(item.height, 1.0f)});
    }
    // Chests, shut gates and raised tent walls stop it and take nothing (SfxSkipItem).
    s32 stop = kItemStopBase;
    for (const Obstacle& box : targets.fixtures.inertStops()) {
        missileTargets.push_back(
            MissileTarget{stop++, box.centre, std::max(box.halfAcross, box.halfAlong), box.height});
    }
    m_resources->arsenal.missiles().update(seconds, &m_resources->world.collision(),
                                           missileTargets);
    for (const MissileImpact& impact : m_resources->arsenal.missiles().takeImpacts()) {
        m_resources->arsenal.presentImpact(impact);
        if (impact.potion != 0) {
            beginPotion(impact);
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
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    hit.direction = impact.position - actor.position();
                    hit.direction.y = 0.0f;
                    hit.level = experienceLevel(actor.save().experience());
                }
            }
            targets.opponents.bosses().hurt(hit, impact.target - kBossTargetBase);
        } else if (impact.target >= kCritterTargetBase) {
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            targets.opponents.strikeCritter(impact.target - kCritterTargetBase, impact.damage,
                                            impact.flags, direction, impact.owner, impact.position,
                                            false, players);
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
            targets.opponents.strikeEnemy(impact.target - kEnemyTargetBase, impact.damage,
                                          impact.flags, direction, impact.owner, players, false,
                                          impact.position);
        } else if (impact.target >= 0) {
            targets.fixtures.strikeBarrel(static_cast<usize>(impact.target), impact.damage,
                                          impact.owner, players, targets.fixtureEvents);
            targets.fixtures.settleBlasts(players, targets.fixtureEvents);
        }
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
        const s32 colour = *kind != 0 ? *kind : m_nextPotionKind;
        MissileImpact own;
        own.owner = impact.owner;
        own.position = impact.position;
        own.potion = colour;
        own.potency = kShotMagicShare * m_resources->arsenal.magicPowerOf(players[i].actor);
        own.damage = kShotMagicShare * kPotionDamage;
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
    if (kind == 0) {
        kind = m_nextPotionKind;
        m_nextPotionKind = m_nextPotionKind % 4 + 1;
    }
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
        const u32 flags = EnemyHit::kMagic | static_cast<u32>(burst.impact.potion);
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
                const f32 before = targets.opponents.bosses().view().health;
                targets.opponents.bosses().hurt(hit, target.id - kBossTargetBase);
                healFrom(byPlayer, before - targets.opponents.bosses().view().health, target.base,
                         players, targets);
            } else if (target.id >= kCritterTargetBase) {
                const s32 id = target.id - kCritterTargetBase;
                const f32 before = targets.opponents.critters().healthOf(id);
                targets.opponents.strikeCritter(id, power, flags, direction, byPlayer, target.base,
                                                false, players);
                healFrom(byPlayer, before - targets.opponents.critters().healthOf(id), target.base,
                         players, targets);
            } else if (target.id >= kGeneratorTargetBase) {
                targets.opponents.strikeGenerator(target.id - kGeneratorTargetBase, power, byPlayer,
                                                  players);
            } else if (target.id >= kEnemyTargetBase) {
                const s32 id = target.id - kEnemyTargetBase;
                const f32 before = targets.opponents.enemies().healthOf(id);
                targets.opponents.strikeEnemy(id, power, flags, direction, byPlayer, players);
                healFrom(byPlayer, before - targets.opponents.enemies().healthOf(id), target.base,
                         players, targets);
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

/** From level 75 a caster's magic heals as it harms (do_heal_players, reached through
 * DMG_HEAL): they take a tenth of what it took, more by 0.016 a level past 75, and every other
 * standing player within their magic's power half that, none past their most; it shows over
 * what was harmed and teaches them so. */
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
        if (&other != &*caster && other.life == PlayerLife::Standing &&
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
