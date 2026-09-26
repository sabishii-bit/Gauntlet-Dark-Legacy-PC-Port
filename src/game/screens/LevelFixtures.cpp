#include "game/screens/LevelFixtures.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/HelpMessages.h"
namespace gdl::game {
namespace {
constexpr std::string_view kChestSound = "S_CHEST";
constexpr f32 kKnockdownFrom = 1.0f; ///< a blast must do more than this to floor anyone
constexpr f32 kBehind = 1.5707964f;  ///< a blow from further round than this is from behind
constexpr std::string_view kWoodHitSound = "S_WEAPONHITWOOD";
constexpr std::string_view kBarrelBreakSound = "S_BARREL_WOOD"; ///< with the realm's letter
constexpr std::string_view kBarrelBlastSound = "S_BARREL_EXPLO";
constexpr std::string_view kBarrelGasSound = "S_BARREL_GAS";
constexpr std::string_view kFireTrapSound = "S_FIREHOLE";
constexpr std::string_view kBarrelBlast = "EXPLOSION";
constexpr std::string_view kBarrelGas = "POISONEXP1";
constexpr std::string_view kBarrelSmoke = "DESTSMOKE";
constexpr f32 kChestBlastDamage = 50.0f; ///< each times the level's trap damage
constexpr f32 kBarrelBlastDamage = 30.0f;
constexpr f32 kGasDamage = 10.0f;
constexpr f32 kGasRadius = 6.5f;
constexpr f32 kGasSeconds = 4.0f;
constexpr f32 kGasGapSeconds = 0.5f;
constexpr s32 kFireTrap = 1;
constexpr std::string_view kChestBlast = "EXPCHEST"; ///< a trapped chest going up
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";

} // namespace
void LevelFixtures::bind(const Resources& resources) {
    clear();
    m_resources.emplace(resources);
    LevelWorld& world = resources.world;
    RenderDevice& device = resources.device;
    m_chests.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems());
    m_gates.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems());
    const LevelInfo* level = world.level();
    m_traps.bind(device, world.layout(), world.items(), &world.collision(), 1,
                 level != nullptr ? level->tuning.trapTimeScale(resources.difficultyGain) : 1.0f,
                 trapDamageScale(), &world.realmItems());
    m_barrels.bind(device, world.layout(), world.items(), &world.collision(), &world.realmItems());
    m_safeRocks.bind(device, world.layout(), world.items());
}
void LevelFixtures::clear() {
    if (m_resources) {
        for (const GasCloud& cloud : m_clouds) {
            m_resources->effects.stop(cloud.effect);
        }
    }
    m_chests.clear();
    m_gates.clear();
    m_traps.clear();
    m_barrels.clear();
    m_safeRocks.clear();
    m_clouds.clear();
    m_blasts.clear();
    m_resources.reset();
}
void LevelFixtures::setPlayerCount(s32 count) {
    m_chests.setPlayerCount(count);
    m_gates.setPlayerCount(count);
    m_traps.setPlayerCount(count);
    m_barrels.setPlayerCount(count);
    m_safeRocks.setPlayerCount(count);
}
std::vector<Obstacle> LevelFixtures::obstacles() const {
    std::vector<Obstacle> result = m_chests.obstacles();
    for (const auto& group :
         {m_gates.obstacles(), m_barrels.obstacles(), m_safeRocks.obstacles()}) {
        result.insert(result.end(), group.begin(), group.end());
    }
    return result;
}
void LevelFixtures::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera) const {
    m_chests.draw(device, clip, lighting);
    m_gates.draw(device, clip, lighting);
    m_traps.draw(device, clip, lighting, camera);
    m_barrels.draw(device, clip, lighting);
    m_safeRocks.draw(device, clip, lighting);
}
/** What the level's traps and blasts are scaled by: its own trap damage and the
 * difficulty's gain. */
f32 LevelFixtures::trapDamageScale() const {
    if (!m_resources.has_value()) {
        return 1;
    }
    const LevelInfo* level = m_resources->world.level();
    return level != nullptr ? level->tuning.trapDamageScale(m_resources->difficultyGain)
                            : m_resources->difficultyGain;
}
void LevelFixtures::playRealmSound(std::string_view stem) {
    if (!m_resources.has_value()) {
        return;
    }
    const std::string& name = m_resources->world.ref().name;
    m_resources->audio.playNamed(std::format("{}{}", stem, name.empty() ? 'G' : name.front()));
}
/** The level's chests, gates and traps under the party: nobody walks through a chest or a
 * gate that is shut; against one, a key carried is spent and it opens (a chest's sound is
 * the common one, a gate's its realm's); an opened chest drops what it held, pays its gold
 * to its opener or blows up; a trap that is out hurts whoever is in it. */
void LevelFixtures::update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                           const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    std::vector<Obstacle> boxes = m_chests.obstacles();
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    m_barrels.update(seconds);
    m_safeRocks.update(seconds);
    const auto cover = m_safeRocks.obstacles();
    boxes.insert(boxes.end(), cover.begin(), cover.end());
    std::vector<ChestVisitor> visitors;
    std::vector<TrapVictim> victims;
    visitors.reserve(players.size());
    victims.reserve(players.size());
    for (PlayerRuntime& player : players) {
        PlayerActor& actor = player.actor;
        if (player.life != PlayerLife::Standing || player.capture.held()) {
            visitors.push_back(ChestVisitor{kNowhere, actor.radius(), 0});
            victims.push_back(TrapVictim{kNowhere, actor.radius()});
            continue;
        }
        Vec3 position = actor.position();
        for (const Obstacle& box : boxes) {
            position = box.pushOut(position, actor.radius());
        }
        actor.place(position);
        visitors.push_back(
            ChestVisitor{position, actor.radius(), actor.save().progress().inventory.keys,
                         PowerupEffects::of(actor.save().progress().inventory).xray()});
        victims.push_back(TrapVictim{position, actor.radius()});
    }
    for (const ChestEvent& event : m_chests.update(seconds, visitors)) {
        if (event.visitor >= players.size()) {
            continue;
        }
        PlayerActor& actor = players[event.visitor].actor;
        switch (event.kind) {
        case ChestEvent::Kind::Unlocked:
            if (m_chests.chest(event.chest).locked) {
                actor.save().progress().inventory.spendKey();
            }
            m_resources->audio.playNamed(kChestSound);
            if (events.releaseEnemy && events.releaseEnemy(event.contents, event.position,
                                                           m_chests.chest(event.chest).count)) {
                break;
            }
            if (event.contents >= 0 && m_resources->world.placeItemRecord(
                                           m_resources->device, event.contents, event.position,
                                           m_chests.chest(event.chest).count)) {
                m_chests.hold(event.chest,
                              static_cast<s32>(m_resources->world.placedItems().size()) - 1);
            }
            break;
        case ChestEvent::Kind::Opened:
            if (event.explodes) {
                if (m_resources->weapons.loaded()) {
                    m_resources->effects.start(m_resources->device, m_resources->weapons,
                                               kChestBlast, event.position);
                }
                playRealmSound(kBarrelBlastSound);
                m_chests.remove(event.chest);
                events.help(HelpMessages::kChestsExplode, event.visitor);
                blast(event.position, kBlastRadius, kChestBlastDamage * trapDamageScale(), players,
                      events);
            } else if (event.gold > 0) {
                takeItem(actor.save(), ItemOffer{static_cast<s32>(ItemKind::Gold), event.gold});
                events.card(actor.player(), "GOLD");
                m_resources->audio.playNamed(kPickupSound);
            } else if (event.contents >= 0) {
                // Already visible on the opening lid's attachment; now collectible.
            } else {
                m_chests.remove(event.chest);
            }
            break;
        case ChestEvent::Kind::Refused:
            events.help(HelpMessages::kChestNeedsKey, event.visitor);
            break;
        }
    }
    const usize reveals = m_chests.updateXray(m_resources->device, m_resources->world.items(),
                                              m_resources->world.powerups(), seconds, visitors,
                                              &m_resources->world.realmItems());
    for (usize i = 0; i < reveals; ++i) {
        m_resources->audio.playNamed("S_XRAY");
    }
    for (usize i = 0; i < m_chests.size(); ++i) {
        const auto& chest = m_chests.chest(i);
        if (chest.gone || chest.held < 0) {
            continue;
        }
        const Mat4 socket = chest.figure.nodeTransform("NULL1").value_or(
            glm::translate(Mat4{1}, chest.figure.position()));
        // DoItems grows the child from 20% to full size during OPEN.
        const f32 scale =
            chest.state == Chests::kOpening ? 0.2f + 0.8f * chest.figure.progress() : 1;
        m_resources->world.attachItem(static_cast<usize>(chest.held),
                                      glm::scale(socket, Vec3{scale}),
                                      chest.state != Chests::kOpen);
    }
    for (const GateEvent& event : m_gates.update(ticks, seconds, visitors)) {
        if (event.visitor >= players.size()) {
            continue;
        }
        if (event.kind == GateEvent::Kind::Unlocked) {
            players[event.visitor].actor.save().progress().inventory.spendKey();
            playGateSound(0);
        } else if (event.kind == GateEvent::Kind::Refused) {
            events.help(HelpMessages::kDoorNeedsKey, event.visitor);
        }
    }
    for (const TrapHit& hit : m_traps.update(ticks, seconds, victims)) {
        if (hit.victim >= players.size()) {
            continue;
        }
        if (hit.subtype == kFireTrap) {
            playRealmSound(kFireTrapSound);
        }
        // Every trap stuns: spikes and blades make their victim flinch, the rest reel.
        if (PlayerHealth::guarded(players[hit.victim], hit.damage, false) > 1.0f &&
            !PowerupEffects::of(players[hit.victim].actor.save().progress().inventory)
                 .preventsKnockback() &&
            players[hit.victim].life == PlayerLife::Standing) {
            players[hit.victim].reaction = PlayerImpact::combine(
                players[hit.victim].reaction, hit.pierces ? PlayerDeed::Spike : PlayerDeed::Reel);
        }
        events.hurt(hit.victim, hit.damage, hit.pierces ? HurtKind::Pierce : HurtKind::Burn, false);
        events.help(HelpMessages::kTrapsHurt, hit.victim);
    }
    updateClouds(seconds, players, events);
    for (PlayerRuntime& runtime : players) {
        runtime.hitSoundGap = std::max(runtime.hitSoundGap - ticks, 0);
    }
}

void LevelFixtures::strikeSafeRock(usize index, f32 power) {
    if (!m_resources.has_value()) {
        return;
    }
    if (m_safeRocks.strike(index, power)) {
        m_resources->effects.start(m_resources->device, m_resources->world.items(), "SAFEREXP",
                                   m_safeRocks.rock(index).position);
    }
}

void LevelFixtures::strikeWall(usize index, f32 power, u32 flags) {
    if (!m_resources) {
        return;
    }
    const auto health = m_resources->world.strikeWall(index, power, flags);
    if (health) {
        m_resources->audio.playNamed(*health == 0 ? "S_SECRETWALL"
                                                  : m_resources->world.wallHitSound());
    }
}

/** A blow on a barrel: wood sounds under it until it breaks, when what it held is left
 * lying, or it blows up, or its gas hangs where it stood. */
void LevelFixtures::strikeBarrel(usize barrel, f32 power, s32 byPlayer,
                                 std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    const auto struck = m_barrels.strike(barrel, power);
    if (!struck.has_value()) {
        return;
    }
    if (!struck->broken) {
        m_resources->audio.playNamed(kWoodHitSound);
        return;
    }
    const auto effect = [&](std::string_view tree) {
        if (m_resources->weapons.loaded()) {
            m_resources->effects.start(m_resources->device, m_resources->weapons, tree,
                                       struck->position);
        }
    };
    switch (struck->kind) {
    case BreakableStrike::Kind::Plain:
    case BreakableStrike::Kind::Holding:
        playRealmSound(kBarrelBreakSound);
        if (struck->contents >= 0 &&
            m_resources->world.placeItemRecord(m_resources->device, struck->contents,
                                               struck->position, struck->count)) {
            for (usize i = 0; i < players.size(); ++i) {
                if (players[i].actor.player() == byPlayer) {
                    events.help(HelpMessages::kBarrelsHold, i);
                }
            }
        }
        break;
    case BreakableStrike::Kind::Exploding:
        playRealmSound(kBarrelBlastSound);
        effect(kBarrelBlast);
        effect(kBarrelSmoke);
        m_blasts.push_back(
            Blast{struck->position, kBlastRadius, kBarrelBlastDamage * trapDamageScale()});
        break;
    case BreakableStrike::Kind::Poison: {
        playRealmSound(kBarrelGasSound);
        // StartExplosion(25): entrance -> sustained gas -> dispersal,
        // with horizontal scale 3.5 and a four-second sustained hazard.
        EffectTrees::Setting setting;
        setting.persistent = true;
        setting.then = "POISONEXP2";
        setting.unlit = true;
        setting.depthWrite = false;
        const u32 id = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                     kBarrelGas, struck->position, setting);
        m_resources->effects.placeAt(
            id, glm::scale(glm::translate(Mat4{1}, struck->position), Vec3{3.5f, 1, 3.5f}));
        m_clouds.push_back(
            GasCloud{struck->position, kGasDamage * trapDamageScale(), kGasSeconds, id});
        break;
    }
    }
}

/** Whoever is within a blast is hurt by it, and the barrels within it are struck by it (so
 * one that blows up sets off its neighbours). */
void LevelFixtures::blast(const Vec3& position, f32 radius, f32 damage,
                          std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    m_blasts.push_back(Blast{position, radius, damage});
    settleBlasts(players, events);
}

/** Feels out every blast waiting, and those they set off in turn. */
void LevelFixtures::settleBlasts(std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    while (!m_blasts.empty()) {
        const Blast felt = m_blasts.back();
        m_blasts.pop_back();
        for (usize i = 0; i < players.size(); ++i) {
            if (players[i].life != PlayerLife::Standing) {
                continue;
            }
            const PlayerActor& actor = players[i].actor;
            const Vec3 offset = actor.followPoint() - felt.position;
            if (std::hypot(offset.x, offset.z) <= felt.radius + actor.radius() &&
                std::abs(offset.y) <= actor.height() * 0.5f + felt.radius) {
                // A blast that gets through knocks its victim off their feet: onto their face
                // when it came from behind them, onto their back otherwise.
                const bool guarding =
                    players[i].figure != nullptr && players[i].figure->animator().guarding();
                if (PlayerHealth::guarded(players[i], felt.damage, true) > kKnockdownFrom &&
                    !PowerupEffects::of(actor.save().progress().inventory).preventsKnockback() &&
                    !guarding && !m_resources->world.isTower()) {
                    const Vec3 push = actor.position() - felt.position;
                    f32 round = std::atan2(push.x, push.z) - actor.yaw();
                    round = std::remainder(round, 2.0f * kBehind * 2.0f);
                    players[i].reaction =
                        std::abs(round) > kBehind ? PlayerDeed::FallBack : PlayerDeed::FallForward;
                }
                events.hurt(i, felt.damage, HurtKind::Blow, true);
            }
        }
        for (const usize barrel : m_barrels.within(felt.position, felt.radius)) {
            strikeBarrel(barrel, felt.damage, -1, players, events);
        }
        const auto& walls = m_resources->world.walls();
        for (usize i = 0; i < walls.size(); ++i) {
            if (walls.standing(i) && walls.target(i, 0).touches(felt.position, felt.radius)) {
                strikeWall(i, felt.damage);
            }
        }
        for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
            if (m_safeRocks.rock(rock).obstacle.touchedBy(felt.position, felt.radius, 0.0f)) {
                strikeSafeRock(rock, felt.damage);
            }
        }
        events.opponents(felt.position, felt.radius, felt.damage);
        // ProcessEffects shortens the item query by 1.5 for DMG_EXPLODE.
        constexpr f32 kItemBlastInset = 1.5f;
        const auto changes = m_resources->world.blastItems(
            m_resources->device, felt.position, std::max(0.0f, felt.radius - kItemBlastInset),
            felt.damage);
        bool destroyed = false;
        for (const auto& change : changes) {
            if (change.potion) {
                if (events.shatterPotion) {
                    events.shatterPotion(*change.potion, change.position);
                }
                continue; // Magic replaces the bottle; no food-destruction smoke or help.
            }
            // The retail effect table maps both CHESTDEST and ITEMDEST to this tree.
            for (const auto* tree : {"CHESTDEST", "DESTSMOKE"}) {
                m_resources->effects.start(m_resources->device, m_resources->weapons, tree,
                                           change.position);
            }
            destroyed |= change.destroyed;
        }
        if (destroyed && events.help) {
            for (usize i = 0; i < players.size(); ++i) {
                if (players[i].life == PlayerLife::Standing) {
                    events.help(HelpMessages::kBlastsDestroy, i);
                    break;
                }
            }
        }
    }
}

/** Gas hangs for a while and hurts whoever stands in it, every half second. */
void LevelFixtures::updateClouds(f32 seconds, std::span<PlayerRuntime> players,
                                 const Events& events) {
    for (PlayerRuntime& runtime : players) {
        runtime.cloudGap = std::max(runtime.cloudGap - seconds, 0.0f);
    }
    for (GasCloud& cloud : m_clouds) {
        cloud.secondsLeft -= seconds;
        if (cloud.secondsLeft <= 0) {
            m_resources->effects.stop(cloud.effect);
            EffectTrees::Setting setting;
            setting.unlit = true;
            setting.depthWrite = false;
            const u32 id = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                         "POISONEXP3", cloud.position, setting);
            m_resources->effects.placeAt(
                id, glm::scale(glm::translate(Mat4{1}, cloud.position), Vec3{3.5f, 1, 3.5f}));
            continue;
        }
        if (m_resources->world.poisonFood(m_resources->device, cloud.position, kGasRadius,
                                          cloud.damage) > 0 &&
            events.help) {
            // Retail posts msgPost(0x88, -1, 0): party-wide help, not credited to
            // the barrel's attacker. Present it over the first standing player.
            for (usize i = 0; i < players.size(); ++i) {
                if (players[i].life == PlayerLife::Standing) {
                    events.help(HelpMessages::kGasSpoils, i);
                    break;
                }
            }
        }
        for (usize i = 0; i < players.size(); ++i) {
            if ((players[i].life != PlayerLife::Standing) || players[i].cloudGap > 0.0f) {
                continue;
            }
            const Vec3 offset = players[i].actor.followPoint() - cloud.position;
            if (std::hypot(offset.x, offset.z) <= kGasRadius + players[i].actor.radius() &&
                std::abs(offset.y) <= players[i].actor.height() * 0.5f + kGasRadius) {
                players[i].cloudGap = kGasGapSeconds;
                events.hurt(i, cloud.damage, HurtKind::Gas, true);
            }
        }
    }
    std::erase_if(m_clouds, [](const GasCloud& cloud) { return cloud.secondsLeft <= 0.0f; });
}

/** A gate's opening sounds from the realm's own bank, named after the level's letter. */
void LevelFixtures::playGateSound(s32 /*subtype*/) {
    if (!m_resources.has_value()) {
        return;
    }
    const std::string& level = m_resources->world.ref().name;
    const char letter = level.empty() ? 'G' : level.front();
    for (const std::string_view stem : {"S_GATEMET", "S_GATEWOOD", "S_GATE"}) {
        for (const std::string_view tail : {"", "1"}) {
            if (m_resources->audio.playNamed(std::format("{}{}{}", stem, letter, tail)) !=
                kNoSound) {
                return;
            }
        }
    }
}

} // namespace gdl::game
