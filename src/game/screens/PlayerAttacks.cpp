#include "game/screens/PlayerAttacks.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/Types.h"

#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/world/TargetAssist.h"
namespace gdl::game {
namespace {
constexpr std::string_view kNoEffectTree = "NULLFX"; ///< a move's effect row that shows nothing
constexpr std::array<std::string_view, 5> kShieldTrees{"MS_FIRE", "MS_FIRE", "MS_ELEC", "MS_LIGHT",
                                                       "MS_ACID"};
constexpr std::array<std::string_view, 5> kShieldSounds{"S_SHIELD2", "S_SHIELD2", "S_SHIELD1",
                                                        "S_SHIELD3", "S_SHIELD4"};
constexpr f32 kShieldSeconds = 3.0f;   ///< how long a potion's ring lasts
constexpr f32 kShieldRadius = 25.0f;   ///< at full size; it is sized by the magic, as a burst is
constexpr f32 kShieldPotency = 0.25f;  ///< of the character's magic power, its harm
constexpr f32 kShieldHarmEvery = 0.5f; ///< seconds between its harming what it touches
constexpr std::string_view kBlockEffect = "BLOCKFX";
constexpr f32 kBlockWorth = 2.0f;      ///< what a guard must take off a hurt for it to show
constexpr f32 kBlockPerDamage = 0.01f; ///< seconds it shows for each point left
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1.0f;
constexpr f32 kRamDamage = 3.0f; ///< what a charge does to what it runs into
constexpr f32 kRamReach = 0.3f;  ///< how near counts as run into

} // namespace
void PlayerAttacks::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
}
void PlayerAttacks::clear() {
    if (m_resources.has_value()) {
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
        id = m_strikes.start(strike, actor.player(), actor.position(), facing,
                             ownDamageOf(index, players));
        m_strikeSources.push_back(StrikeSource{id, index, strikeIndex});
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
        setting.yaw = std::atan2(facing.x, facing.z);
        if (started != nullptr && started->flies) {
            setting.velocity = facing * started->speed;
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
        const u32 shown =
            m_resources->effects.startSet(m_resources->device, *archive, effect.tree, at3, setting);
        if (shown != 0 && started != nullptr && started->flies) {
            m_strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
}

/** The strikes under way harm what they reach: the barrels, for now. What flies takes its
 * effect along, and the effect ends with it. */
void PlayerAttacks::updateStrikes(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const StrikeHit& hit : m_strikes.update(seconds, &m_resources->world.collision())) {
        const auto source = std::ranges::find(m_strikeSources, hit.strike, &StrikeSource::strike);
        // The swarm and the generators in its reach take it, with the row's damage type.
        u32 flags = 0;
        if (source != m_strikeSources.end() && source->actor < players.size()) {
            const ClassStats* stats =
                m_resources->classes.stats(players[source->actor].actor.save().character);
            if (stats != nullptr && source->row >= 0 &&
                static_cast<usize>(source->row) < stats->moveStrikes.size()) {
                flags = static_cast<u32>(
                    stats->moveStrikes[static_cast<usize>(source->row)].damageType);
            }
        }
        for (const s32 enemy :
             targets.opponents.enemies().reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = targets.opponents.enemies().positionOf(enemy) - hit.centre;
            targets.opponents.strikeEnemy(enemy, hit.damage, flags,
                                          Vec3{direction.x, 0.0f, direction.z}, hit.owner, players);
        }
        for (const s32 generator : targets.opponents.generators().within(hit.centre, hit.radius)) {
            targets.opponents.strikeGenerator(generator, hit.damage, hit.owner);
        }
        if (targets.opponents.bosses().reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            EnemyHit struck;
            struck.damage = hit.damage;
            struck.flags = flags;
            struck.player = hit.owner;
            struck.close = true;
            if (const Vec3* at = targets.opponents.bosses().position(); at != nullptr) {
                struck.direction = Vec3{at->x - hit.centre.x, 0.0f, at->z - hit.centre.z};
                struck.where = hit.centre + glm::normalize(struck.direction) * hit.radius;
            }
            targets.opponents.bosses().hurt(struck);
        }
        for (const s32 critter :
             targets.opponents.critters().reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = targets.opponents.critters().positionOf(critter) - hit.centre;
            targets.opponents.strikeCritter(critter, hit.damage, flags,
                                            Vec3{direction.x, 0.0f, direction.z}, hit.owner,
                                            std::nullopt, true, players);
        }
        for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
            const auto& cover = targets.fixtures.safeRocks().rock(rock).obstacle;
            if (targets.fixtures.safeRocks().standing(rock) &&
                hit.reaches(cover.centre, cover.cylinderRadius, cover.height)) {
                targets.fixtures.strikeSafeRock(rock, hit.damage);
            }
        }
        for (usize barrel = 0; barrel < targets.fixtures.barrels().size(); ++barrel) {
            if (!targets.fixtures.barrels().standing(barrel)) {
                continue;
            }
            const Breakables::Barrel& cask = targets.fixtures.barrels().barrel(barrel);
            if (!hit.reaches(cask.figure.position(), cask.radius, cask.height)) {
                continue;
            }
            targets.fixtures.strikeBarrel(barrel, hit.damage, hit.owner, players,
                                          targets.fixtureEvents);
            // What it harms shows the strike's own mark, when its class gives it one.
            if (source == m_strikeSources.end() || source->actor >= players.size()) {
                continue;
            }
            const ClassStats* stats =
                m_resources->classes.stats(players[source->actor].actor.save().character);
            ItemArchive* archive = moveEffectsOf(source->actor, players);
            if (stats == nullptr || archive == nullptr) {
                continue;
            }
            const s32 mark = stats->moveStrikes[static_cast<usize>(source->row)].hitEffect;
            if (mark >= 0 && static_cast<usize>(mark) < stats->moveEffects.size()) {
                const MoveEffect& effect = stats->moveEffects[static_cast<usize>(mark)];
                if (!effect.tree.empty() && archive->trees.find(effect.tree).has_value()) {
                    m_resources->effects.start(m_resources->device, *archive, effect.tree,
                                               cask.figure.position(), effect.scale);
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
    shield.radius = kShieldRadius * size;
    shield.damage = kShieldPotency * power;
    shield.secondsLeft = kShieldSeconds;
    if (m_resources->weapons.loaded() &&
        m_resources->weapons.trees.find(kShieldTrees[look]).has_value()) {
        EffectTrees::Setting setting;
        setting.scale = size;
        setting.seconds = kShieldSeconds;
        shield.effect =
            m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                          kShieldTrees[look], actor.position(), setting);
    }
    m_resources->audio.playNamed(kShieldSounds[look]);
    m_shields.push_back(shield);
}

/** The rings go about with their characters and harm the barrels they touch. */
void PlayerAttacks::updateShields(f32 seconds, std::span<PlayerRuntime> players,
                                  const Targets& targets) {
    if (!m_resources.has_value()) {
        return;
    }
    for (PotionShield& shield : m_shields) {
        shield.secondsLeft -= seconds;
        if (shield.actor >= players.size() || players[shield.actor].life != PlayerLife::Standing) {
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
        for (const usize barrel : targets.fixtures.barrels().within(at, shield.radius)) {
            targets.fixtures.strikeBarrel(barrel, shield.damage,
                                          players[shield.actor].actor.player(), players,
                                          targets.fixtureEvents);
        }
        for (usize rock = 0; rock < targets.fixtures.safeRocks().size(); ++rock) {
            if (targets.fixtures.safeRocks().rock(rock).obstacle.touchedBy(at, shield.radius,
                                                                           0.0f)) {
                targets.fixtures.strikeSafeRock(rock, shield.damage);
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

std::vector<MissileTarget> PlayerAttacks::projectileTargets(const Targets& targets) {
    std::vector<MissileTarget> missileTargets;
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

PlayerDeed PlayerAttacks::attackDeed(const PlayerActor& actor, bool strong,
                                     const Targets& targets) const {
    const PlayerDeed ranged = strong ? PlayerDeed::StrongAttack : PlayerDeed::Attack;
    if (!m_resources) {
        return ranged;
    }
    constexpr f32 kCloseReach = 1.0f;
    const auto target = TargetAssist::melee(
        actor.position(), actor.height(), actor.facing(), projectileTargets(targets),
        actor.radius() + kCloseReach, &m_resources->world.collision());
    if (!target) {
        return ranged;
    }
    constexpr f32 kLowHeight = 4.0f;
    const bool low = target->height <= kLowHeight;
    if (strong) {
        return low ? PlayerDeed::MeleeSlowLow : PlayerDeed::MeleeSlow;
    }
    return low ? PlayerDeed::MeleeLow : PlayerDeed::Melee;
}

void PlayerAttacks::melee(usize index, std::span<PlayerRuntime> players, const Targets& targets) {
    if (!m_resources || index >= players.size() || players[index].figure == nullptr) {
        return;
    }
    const PlayerActor& actor = players[index].actor;
    const PlayerAnimator& animator = players[index].figure->animator();
    constexpr f32 kHitReach = 2.0f;
    const auto target = TargetAssist::melee(actor.position(), actor.height(), actor.facing(),
                                            projectileTargets(targets), actor.radius() + kHitReach,
                                            &m_resources->world.collision());
    if (!target) {
        return;
    }
    f32 damage = PlayerMissiles::kLeastDamage;
    if (const ClassStats* stats = m_resources->classes.stats(actor.save().character)) {
        const StatBlock block = displayStats(*stats, experienceLevel(actor.save().experience()),
                                             actor.save().progress());
        damage = PlayerMissiles::damageFor(block.strength());
    }
    u32 flags = 0;
    if (animator.meleePower()) {
        damage *= 2;
        flags |= EnemyHit::kKnockBack;
    } else if (animator.meleeKick() && target->height <= 4.0f) {
        flags |= EnemyHit::kKnockDown;
    }
    const Vec3 point = target->base + Vec3{0, target->height * 0.5f, 0};
    const Vec3 direction = target->base - actor.position();
    const s32 id = target->id;
    if (id >= kSafeRockTargetBase) {
        targets.fixtures.strikeSafeRock(static_cast<usize>(id - kSafeRockTargetBase), damage);
    } else if (id >= kBossTargetBase) {
        const EnemyHit hit{
            damage, flags, direction, actor.player(), experienceLevel(actor.save().experience()),
            point,  true};
        targets.opponents.bosses().hurt(hit);
    } else if (id >= kCritterTargetBase) {
        targets.opponents.strikeCritter(id - kCritterTargetBase, damage, flags, direction,
                                        actor.player(), point, true, players);
    } else if (id >= kGeneratorTargetBase) {
        targets.opponents.strikeGenerator(id - kGeneratorTargetBase, damage, actor.player());
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
    const auto missileTargets = projectileTargets(targets);
    m_resources->arsenal.missiles().update(seconds, &m_resources->world.collision(),
                                           missileTargets);
    for (const MissileImpact& impact : m_resources->arsenal.missiles().takeImpacts()) {
        if (impact.potion != 0) {
            m_resources->arsenal.burstPotion(impact.potion, impact.position,
                                             impact.potency); // weapons leave no mark yet
        }
        if (impact.target >= kSafeRockTargetBase) {
            targets.fixtures.strikeSafeRock(static_cast<usize>(impact.target - kSafeRockTargetBase),
                                            impact.damage);
        } else if (impact.target >= kBossTargetBase) {
            EnemyHit hit;
            hit.damage = impact.damage;
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
            targets.opponents.bosses().hurt(hit);
        } else if (impact.target >= kCritterTargetBase) {
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            targets.opponents.strikeCritter(impact.target - kCritterTargetBase, impact.damage, 0,
                                            direction, impact.owner, impact.position, false,
                                            players);
        } else if (impact.target >= kGeneratorTargetBase) {
            targets.opponents.strikeGenerator(impact.target - kGeneratorTargetBase, impact.damage,
                                              impact.owner);
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
            targets.opponents.strikeEnemy(impact.target - kEnemyTargetBase, impact.damage, 0,
                                          direction, impact.owner, players, false, impact.position);
        } else if (impact.target >= 0) {
            targets.fixtures.strikeBarrel(static_cast<usize>(impact.target), impact.damage,
                                          impact.owner, players, targets.fixtureEvents);
            targets.fixtures.settleBlasts(players, targets.fixtureEvents);
        }
    }
}

} // namespace gdl::game
