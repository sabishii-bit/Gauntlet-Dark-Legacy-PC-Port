#include "game/screens/LevelFixtures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

#include "engine/core/Types.h"
#include "engine/world/TreeModel.h"

#include "game/enemies/EnemyKinds.h"
#include "game/players/ItemPickup.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PlayerPowerups.h"
#include "game/world/DynamicLights.h"
namespace gdl::game {
namespace {
constexpr std::string_view kChestSound = "S_CHEST";
constexpr std::string_view kChestFuseSound = "S_TICKY";
constexpr f32 kChestFuseVolume = 224.0f / 255.0f;
constexpr std::string_view kApple = "APPLE"; ///< what magic makes of Death in a chest
constexpr std::string_view kChicken = "CHICKEN";
constexpr std::string_view kTreasureGold = "TREAS_GOLD";
constexpr std::string_view kTreasureSilver = "TREAS_SILVER";
constexpr std::string_view kGoldChestGreater = "CHESTG3"; ///< a chest the treasure perk gilds
constexpr std::string_view kGoldChestLesser = "CHESTG1";
constexpr s32 kChestPerkBase = 0; ///< ids of what a wave's perk has reached
constexpr s32 kBarrelPerkBase = 1000;
constexpr s32 kTrapPerkBase = 2000;
constexpr s32 kWallPerkBase = 3000;
constexpr f32 kWallRuin = 9999.0f;  ///< the greater archer perk's blow
constexpr f32 kSmallestStop = 0.1f; ///< an item's reach for a missile, at the least
constexpr s32 kTentRaising = 1;     ///< a tent wall's moves while it stops missiles
constexpr s32 kTentRaised = 2;
constexpr std::string_view kDeathDies = "S_DEATHDIE"; ///< his cry as he goes
constexpr f32 kKnockdownFrom = 1.0f; ///< a blast must do more than this to floor anyone
constexpr f32 kBehind = 1.5707964f;  ///< a blow from further round than this is from behind
constexpr f32 kBlastPush = 0.25f;    ///< how hard a blast throws its victim, as a push's length
constexpr f32 kRingFade = 0.33f;     ///< a blast's ring stops a third of its life from the end
constexpr f32 kRingGrowth = 1.5f;    ///< and deals this times the phase past that
constexpr f32 kOneFrame = 1.0f / 30.0f;
constexpr f32 kRingHeldFrom = 2.0f;  ///< a blow over this is not dealt the same thing again
constexpr f32 kRingKnockFrom = 5.0f; ///< under this a blast floors nobody
constexpr f32 kShelterFrom = 10.0f;  ///< past this a wall between shelters a player
constexpr f32 kShelterProbe = 0.1f;
constexpr f32 kChestBlastWidth = 2.5f;
constexpr f32 kChestBlastHeight = 3.0f;
constexpr s32 kBarrelReached = 0; ///< a blast's own ids for what it has reached
constexpr s32 kWallReached = 1000;
constexpr s32 kRockReached = 2000;
constexpr s32 kTriggerReached = 3000;
constexpr std::string_view kWoodHitSound = "S_WEAPONHITWOOD";
constexpr std::string_view kBarrelBreakSound = "S_BARREL_WOOD"; ///< with the realm's letter
constexpr std::string_view kBarrelBlastSound = "S_BARREL_EXPLO";
constexpr std::string_view kBarrelGasSound = "S_BARREL_GAS";
constexpr std::string_view kFireTrapSound = "S_FIREHOLE";
constexpr std::string_view kBarrelBlast = "EXPLOSION";
constexpr std::string_view kBarrelGas = "POISONEXP1";
constexpr std::string_view kBarrelSmoke = "DESTSMOKE";
constexpr std::string_view kChestDestroyed = "CHESTDEST";
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
constexpr u32 kPoisonDamage = 0x800;
constexpr u32 kExplosionDamage = 0x400;

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
    m_rubble.clear();
    m_doomedChests.clear();
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
std::vector<Obstacle> LevelFixtures::inertStops() const {
    std::vector<Obstacle> stops = m_chests.obstacles();
    const std::vector<Obstacle> gates = m_gates.obstacles();
    stops.insert(stops.end(), gates.begin(), gates.end());
    for (usize i = 0; i < m_traps.size(); ++i) {
        const Traps::Trap& trap = m_traps.trap(i);
        if (trap.shown && !trap.disarmed && trap.subtype == Traps::kTentWall &&
            (trap.action == kTentRaising || trap.action == kTentRaised)) {
            Obstacle box = trap.box;
            box.solid = true;
            stops.push_back(box);
        }
    }
    return stops;
}

std::vector<MissileStop> LevelFixtures::missileStops() const {
    std::vector<MissileStop> stops;
    for (const Obstacle& box : inertStops()) {
        stops.push_back(MissileStop::of(box));
    }
    for (const Obstacle& box : m_barrels.obstacles()) {
        stops.push_back(MissileStop::of(box));
    }
    for (usize i = 0; i < m_safeRocks.size(); ++i) {
        if (m_safeRocks.standing(i)) {
            const SafeRocks::Rock& rock = m_safeRocks.rock(i);
            stops.push_back(MissileStop{.box = rock.obstacle,
                                        .rock = static_cast<s32>(i),
                                        .rockHealth = rock.health,
                                        .rockArmor = rock.armor});
        }
    }
    const auto upright = [](const Vec3& base, f32 radius, f32 height) {
        Obstacle box;
        box.centre = base;
        box.cylinderRadius = std::max(radius, kSmallestStop);
        box.halfAcross = box.cylinderRadius;
        box.halfAlong = box.cylinderRadius;
        box.height = height;
        return MissileStop::of(box);
    };
    if (m_resources.has_value()) {
        const PlacedItems& items = m_resources->world.placedItems();
        for (const usize bottle : items.shootablePotions()) {
            const PlacedItems::Item& item = items.item(bottle);
            stops.push_back(upright(item.position, item.radius, item.height));
        }
        const LevelTriggers& triggers = m_resources->world.triggers();
        for (usize i = 0; i < triggers.size(); ++i) {
            const LevelTrigger& trigger = triggers.trigger(i);
            if (trigger.shootable) {
                stops.push_back(upright(trigger.spot, trigger.radius, trigger.height));
            }
        }
    }
    return stops;
}

std::vector<CombatantObstacle> LevelFixtures::critterObstacles() const {
    std::vector<CombatantObstacle> items;
    const auto add = [&](const Obstacle& box, CombatantObstacle::Kind kind) {
        CombatantObstacle item;
        item.box = box;
        item.kind = kind;
        items.push_back(item);
    };
    for (const Obstacle& box : m_chests.obstacles()) {
        add(box, CombatantObstacle::Kind::Chest);
    }
    for (const auto& group : {m_gates.obstacles(), m_safeRocks.obstacles()}) {
        for (const Obstacle& box : group) {
            add(box, CombatantObstacle::Kind::Blocks);
        }
    }
    for (usize i = 0; i < m_barrels.size(); ++i) {
        if (!m_barrels.standing(i)) {
            continue;
        }
        const Breakables::Barrel& cask = m_barrels.barrel(i);
        CombatantObstacle item;
        item.box = cask.box;
        item.kind = CombatantObstacle::Kind::Breakable;
        item.id = static_cast<s32>(i);
        item.health = cask.health;
        item.armor = cask.armor;
        item.explodes = cask.kind == BreakableStrike::Kind::Exploding;
        items.push_back(item);
    }
    return items;
}

void LevelFixtures::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                         const CameraFrame* camera) const {
    m_chests.draw(device, clip, lighting);
    m_gates.draw(device, clip, lighting);
    m_traps.draw(device, clip, lighting, camera, TreeModel::Pass::DepthWriting);
    m_barrels.draw(device, clip, lighting);
    m_safeRocks.draw(device, clip, lighting);
    m_rubble.draw(device, clip, lighting);
}

void LevelFixtures::drawEffects(RenderDevice& device, const Mat4& clip,
                                const WorldLighting& lighting, const CameraFrame* camera) const {
    m_traps.draw(device, clip, lighting, camera, TreeModel::Pass::Effects);
}

void LevelFixtures::leaveRubble(std::string_view object, const Mat4& transform) {
    if (!m_resources.has_value()) {
        return;
    }
    const std::array<ItemArchive*, 2> archives{&m_resources->world.items(),
                                               &m_resources->world.realmItems()};
    m_rubble.leave(m_resources->device, archives, object, transform);
}

std::optional<usize> LevelFixtures::nearestStanding(std::span<const PlayerRuntime> players,
                                                    const Vec3& position, f32 reach) {
    std::optional<usize> nearest;
    f32 best = reach;
    for (usize i = 0; i < players.size(); ++i) {
        if (players[i].life != PlayerLife::Standing) {
            continue;
        }
        const f32 distance = glm::distance(players[i].actor.position(), position);
        if (distance <= best) {
            best = distance;
            nearest = i;
        }
    }
    return nearest;
}

void LevelFixtures::detonateChest(usize chest, std::optional<usize> opener,
                                  std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value() || chest >= m_chests.size() || m_chests.chest(chest).gone) {
        return;
    }
    const Vec3 position = m_chests.chest(chest).figure.position();
    const Vec3 blastPosition = position + Vec3{0, kChestBlastHeight, 0};
    f32 seconds = kExplosionSeconds;
    if (m_resources->weapons.loaded()) {
        EffectTrees::Setting setting;
        setting.stretch = Vec3{kChestBlastWidth, 1, kChestBlastWidth};
        setting.light = EffectTrees::Light{DynamicLights::blast(),
                                           DynamicLights::kBlastRadiusScale * kBlastRadius};
        const u32 id = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                     kBarrelBlast, blastPosition, setting);
        seconds = m_resources->effects.remaining(id).value_or(seconds);
        // Flying fragments retain the chest's orientation and outlive its fireball.
        const u32 debris = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                         kChestBlast, position, {});
        m_resources->effects.placeAt(debris, m_chests.chest(chest).figure.transform());
    }
    playRealmSound(kBarrelBlastSound);
    m_chests.remove(chest);
    const std::optional<usize> told =
        opener.has_value() ? opener
                           : nearestStanding(players, position, std::numeric_limits<f32>::max());
    if (told.has_value() && events.help) {
        events.help(HelpMessages::kChestsExplode, *told);
    }
    blast(blastPosition, kBlastRadius, kChestBlastDamage * trapDamageScale(), players, events,
          seconds);
}

/** An explosion breaks the chests about it (fn_8005C1DC's container case, at a power of five
 * or more): a trapped one goes up on the next update, one holding Death lets him out, and the
 * rest are blown apart, what lay in them with them, leaving their rubble. It also fires the
 * shootable triggers it reaches. */
void LevelFixtures::blastFixtures(const Vec3& position, f32 reach, f32 damage, const Events& events,
                                  std::vector<s32>& reached, bool destroysContainers) {
    if (!m_resources.has_value()) {
        return;
    }
    const LevelTriggers& triggers = m_resources->world.triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& trigger = triggers.trigger(i);
        const Vec3 away = trigger.spot - position;
        const s32 id = kTriggerReached + static_cast<s32>(i);
        if (trigger.shootable && std::hypot(away.x, away.z) <= reach + trigger.radius &&
            std::ranges::find(reached, id) == reached.end()) {
            reached.push_back(id);
            m_resources->world.shootTrigger(i);
        }
    }
    if (!destroysContainers || damage < kItemBlastPower) {
        return;
    }
    for (usize i = 0; i < m_chests.size(); ++i) {
        const Chests::Chest& chest = m_chests.chest(i);
        if (chest.gone || !chest.shown || !chest.box.touchedBy(position, reach, 0.0f) ||
            std::ranges::find(m_doomedChests, i) != m_doomedChests.end()) {
            continue;
        }
        if (chest.subtype == Chests::kTrappedChest) {
            m_doomedChests.push_back(i);
            continue;
        }
        const Vec3 at = chest.figure.position();
        if (chest.state == Chests::kShut && chest.contents >= 0 && events.releaseEnemy) {
            events.releaseEnemy(chest.contents, at, chest.count);
        }
        if (m_resources->weapons.loaded()) {
            for (const std::string_view tree : {kChestDestroyed, kBarrelSmoke}) {
                m_resources->effects.start(m_resources->device, m_resources->weapons, tree, at);
            }
        }
        leaveRubble(chest.subtype == Chests::kRandomChest ? Rubble::kSilverChest : Rubble::kChest,
                    chest.figure.transform());
        if (chest.held >= 0) {
            m_resources->world.discardItem(static_cast<usize>(chest.held));
        }
        m_chests.remove(i);
    }
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
void LevelFixtures::playFallingCues(std::span<const FallingCue> cues) {
    if (!m_resources.has_value()) {
        return;
    }
    for (const FallingCue& cue : cues) {
        if (!cue.sound.empty()) {
            m_resources->audio.playNamed(cue.sound, FallingScenery::kSoundVolume);
        }
    }
}
void LevelFixtures::shootScenery(const Vec3& position, f32 radius) {
    if (m_resources.has_value()) {
        playFallingCues(m_resources->world.fallingScenery().shoot(position, radius));
    }
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
    for (const Vec3& position : m_resources->world.takeWorldExplosions()) {
        worldExplosion(position, players, events);
    }
    std::vector<Obstacle> boxes = m_chests.obstacles();
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    // A barrel that blew up or gassed is gone once it has broken; a player near where it stood
    // is told to shoot such barrels from afar (items.c's action 2).
    for (const usize spent : m_barrels.update(seconds)) {
        const Breakables::Barrel& barrel = m_barrels.barrel(spent);
        if (const auto near = nearestStanding(players, barrel.figure.position(), kBarrelWarning);
            near.has_value() && events.help) {
            events.help(barrel.kind == BreakableStrike::Kind::Exploding
                            ? HelpMessages::kRedBarrels
                            : HelpMessages::kGreenBarrels,
                        *near);
        }
    }
    // Trapped chests a blast set off go up now, one setting off the next in turn.
    for (const usize chest : std::exchange(m_doomedChests, {})) {
        detonateChest(chest, std::nullopt, players, events);
    }
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
            player.fixtureSpot.reset();
            visitors.push_back(ChestVisitor{kNowhere, actor.radius(), 0});
            victims.push_back(TrapVictim{kNowhere, actor.radius()});
            continue;
        }
        Vec3 position = actor.position();
        for (const Obstacle& box : boxes) {
            const Vec3 resolved = box.pushOut(position, actor.radius());
            actor.slide(resolved - position, &m_resources->world.collision());
            position = actor.position();
        }
        // The rocks and leaves that give way to a body brushing them (fn_8005D730's case 10).
        playFallingCues(m_resources->world.fallingScenery().touch(position, actor.radius()));
        const PowerupEffects worn = PowerupEffects::of(actor.save().progress().inventory);
        const Vec3 step = player.fixtureSpot ? position - *player.fixtureSpot : Vec3{0.0f};
        player.fixtureSpot = position;
        visitors.push_back(ChestVisitor{position, actor.radius(),
                                        actor.save().progress().inventory.keys, worn.xray(), step});
        // Traps pass under a levitating body (ItemTouch's trap case).
        const bool floating = (worn.special & powerup::kLevitation) != 0;
        victims.push_back(TrapVictim{floating ? kNowhere : position, actor.radius()});
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
            if (m_chests.chest(event.chest).subtype == Chests::kTrappedChest) {
                m_resources->audio.playNamed(kChestFuseSound, kChestFuseVolume);
            }
            if (events.releaseEnemy && events.releaseEnemy(event.contents, event.position,
                                                           m_chests.chest(event.chest).count)) {
                break;
            }
            if (event.contents >= 0 && m_resources->world.placeItemRecord(
                                           m_resources->device, event.contents, event.position,
                                           m_chests.chest(event.chest).count)) {
                const usize held = m_resources->world.placedItems().size() - 1;
                m_chests.hold(event.chest, static_cast<s32>(held));
                m_resources->world.setItemOpener(held, actor.player());
            }
            break;
        case ChestEvent::Kind::Opened:
            if (event.explodes) {
                detonateChest(event.chest, event.visitor, players, events);
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
            // A silver chest's own hint is given once the key lesson has nothing to add.
            if (!events.help(HelpMessages::kChestNeedsKey, event.visitor) &&
                m_chests.chest(event.chest).subtype == Chests::kRandomChest) {
                events.help(HelpMessages::kRandomChest, event.visitor);
            }
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
    for (const TrapHit& hit :
         m_traps.update(ticks, seconds, victims, PlayerPowerups::timeStopped(players))) {
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
    advanceBlasts(seconds, players, events);
    for (PlayerRuntime& runtime : players) {
        runtime.hitSoundGap = std::max(runtime.hitSoundGap - ticks, 0);
    }
}

bool LevelFixtures::enchantChest(usize index, f32 power) {
    if (!m_resources.has_value() || index >= m_chests.size()) {
        return false;
    }
    const auto& infos = m_resources->world.layout().itemInfos();
    const s32 inside = m_chests.chest(index).contents;
    if (inside < 0 || static_cast<usize>(inside) >= infos.size()) {
        return false;
    }
    const ItemInfo& held = infos[static_cast<usize>(inside)];
    if (held.type != ItemInfo::kPlacedEnemy || enemyKindOf(held.name) != kDeathKind) {
        return false;
    }
    // The record found by name, as a pickup of food; without one the chest is left empty.
    const auto apple = std::ranges::find_if(infos, [](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup &&
               info.subtype == static_cast<s32>(ItemKind::Food) && info.name == kApple;
    });
    const s32 record =
        apple != infos.end() ? static_cast<s32>(std::distance(infos.begin(), apple)) : -1;
    if (!m_chests.transmute(index, record, power)) {
        return false;
    }
    m_resources->audio.playNamed(kDeathDies);
    return true;
}

namespace {
/** The first item record of `name` among the pickups of `subtype`, or -1 (fn_8005BA1C looks
 * its replacements up by name). */
s32 pickupRecord(std::span<const ItemInfo> infos, std::string_view name, ItemKind subtype) {
    const auto found = std::ranges::find_if(infos, [&](const ItemInfo& info) {
        return info.type == ItemInfo::kPowerup && info.subtype == static_cast<s32>(subtype) &&
               info.name == name;
    });
    return found != infos.end() ? static_cast<s32>(std::distance(infos.begin(), found)) : -1;
}

/** What a perk makes of a container's contents record: the new record and what it did. */
struct ContentsTurn {
    s32 record = -1;
    MagicPerkDeed deed = MagicPerkDeed::JunkToSilver;
};

std::optional<ContentsTurn> blessContents(std::span<const ItemInfo> infos, s32 contents,
                                          MagicPerk perk) {
    constexpr s32 kMostJunk = 10;
    constexpr s32 kRottenMeat = -100;
    if (contents < 0 || static_cast<usize>(contents) >= infos.size()) {
        return std::nullopt;
    }
    const ItemInfo& held = infos[static_cast<usize>(contents)];
    if (held.type != ItemInfo::kPowerup) {
        return std::nullopt;
    }
    if (perk.family == MagicPerkFamily::Treasure &&
        held.subtype == static_cast<s32>(ItemKind::Gold) && held.value <= kMostJunk) {
        return perk.greater ? ContentsTurn{pickupRecord(infos, kTreasureGold, ItemKind::Gold),
                                           MagicPerkDeed::JunkToGold}
                            : ContentsTurn{pickupRecord(infos, kTreasureSilver, ItemKind::Gold),
                                           MagicPerkDeed::JunkToSilver};
    }
    if (perk.family == MagicPerkFamily::Food && held.subtype == static_cast<s32>(ItemKind::Food) &&
        held.value < 0) {
        if (held.value > kRottenMeat) {
            return ContentsTurn{pickupRecord(infos, kApple, ItemKind::Food),
                                MagicPerkDeed::CleanseFruit};
        }
        if (perk.greater) {
            return ContentsTurn{pickupRecord(infos, kChicken, ItemKind::Food),
                                MagicPerkDeed::CleanseMeat};
        }
    }
    return std::nullopt;
}

/** The LEVELUP tree a change shows (fn_8009190C's FX_HEALGOLD, FX_HEALTRAP, FX_HEALFOOD);
 * the walls show none. */
std::string_view perkEffectOf(MagicPerkDeed deed) {
    switch (deed) {
    case MagicPerkDeed::JunkToSilver:
    case MagicPerkDeed::JunkToGold: return "LEVELUP_YEL";
    case MagicPerkDeed::StopTrap:
    case MagicPerkDeed::DestroyTrap: return "LEVELUP_BLU";
    case MagicPerkDeed::CleanseFruit:
    case MagicPerkDeed::CleanseMeat: return "LEVELUP_RED";
    default: return {};
    }
}
} // namespace

void LevelFixtures::bless(const Vec3& position, f32 radius, MagicPerk perk, usize caster,
                          std::vector<s32>& reached, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    auto& resources = *m_resources;
    const auto firstTouch = [&](s32 id) {
        if (std::ranges::find(reached, id) != reached.end()) {
            return false;
        }
        reached.push_back(id);
        return true;
    };
    const auto within = [&](const Vec3& centre, f32 across, f32 height) {
        const Vec3 offset = centre - position;
        return std::hypot(offset.x, offset.z) <= radius + across &&
               std::abs(offset.y) <= radius + height;
    };
    std::vector<PlacedItems::PerkChange> changes =
        resources.world.blessItems(resources.device, position, radius, perk);
    const auto& infos = resources.world.layout().itemInfos();
    switch (perk.family) {
    case MagicPerkFamily::Treasure:
    case MagicPerkFamily::Food:
        for (usize i = 0; i < m_chests.size(); ++i) {
            const Chests::Chest& chest = m_chests.chest(i);
            if (!chest.shown || chest.gone || chest.state != Chests::kShut ||
                !within(chest.box.centre, std::max(chest.box.halfAcross, chest.box.halfAlong),
                        chest.box.height) ||
                !firstTouch(kChestPerkBase + static_cast<s32>(i))) {
                continue;
            }
            const auto turn = blessContents(infos, chest.contents, perk);
            if (!turn.has_value()) {
                continue;
            }
            // The treasure perk dresses the chest in gold as well.
            std::string_view figure;
            if (turn->deed == MagicPerkDeed::JunkToGold) {
                figure = kGoldChestGreater;
            } else if (turn->deed == MagicPerkDeed::JunkToSilver) {
                figure = kGoldChestLesser;
            }
            ItemArchive& source =
                figure.empty() || resources.world.items().trees.find(figure).has_value()
                    ? resources.world.items()
                    : resources.world.realmItems();
            if (m_chests.changeContents(i, turn->record, resources.device, resources.world.layout(),
                                        source, &resources.world.collision(), figure)) {
                changes.push_back({turn->deed, chest.figure.transform()});
            }
        }
        for (usize i = 0; i < m_barrels.size(); ++i) {
            const Breakables::Barrel& cask = m_barrels.barrel(i);
            if (!m_barrels.standing(i) || cask.kind != BreakableStrike::Kind::Holding ||
                !within(cask.figure.position(), cask.radius, cask.height) ||
                !firstTouch(kBarrelPerkBase + static_cast<s32>(i))) {
                continue;
            }
            if (const auto turn = blessContents(infos, cask.contents, perk)) {
                m_barrels.changeContents(i, turn->record);
                changes.push_back({turn->deed, cask.figure.transform()});
            }
        }
        break;
    case MagicPerkFamily::Traps:
        for (usize i = 0; i < m_traps.size(); ++i) {
            const Traps::Trap& trap = m_traps.trap(i);
            if (!trap.shown || trap.disarmed ||
                !within(trap.box.centre, std::max(trap.box.halfAcross, trap.box.halfAlong),
                        trap.box.height) ||
                !firstTouch(kTrapPerkBase + static_cast<s32>(i))) {
                continue;
            }
            if (perk.greater) {
                const Mat4 at = trap.figure.transform();
                if (m_traps.disarm(i, resources.device, resources.world.layout(),
                                   resources.world.items(), &resources.world.collision(),
                                   &resources.world.realmItems())) {
                    changes.push_back({MagicPerkDeed::DestroyTrap, at});
                }
            } else {
                const bool shows = m_traps.stop(i);
                changes.push_back({MagicPerkDeed::StopTrap, trap.figure.transform(), shows});
            }
        }
        break;
    case MagicPerkFamily::Walls: {
        const auto& walls = resources.world.walls();
        for (usize i = 0; i < walls.size(); ++i) {
            if (!walls.standing(i) || !walls.target(i, 0).touches(position, radius) ||
                !firstTouch(kWallPerkBase + static_cast<s32>(i))) {
                continue;
            }
            if (perk.greater) {
                strikeWall(i, kWallRuin);
                changes.push_back({MagicPerkDeed::DestroyWall, walls.wall(i).transform, false});
            } else {
                resources.world.revealWall(i);
                changes.push_back({MagicPerkDeed::RevealWall, walls.wall(i).transform, false});
            }
        }
        break;
    }
    }
    for (const PlacedItems::PerkChange& change : changes) {
        const std::string_view tree = perkEffectOf(change.deed);
        if (change.shows && !tree.empty() && resources.weapons.loaded()) {
            resources.effects.start(resources.device, resources.weapons, tree,
                                    Vec3{change.transform[3]});
        }
        if (events.help) {
            events.help(HelpMessages::kFirstMagicPerk + static_cast<s32>(change.deed), caster);
        }
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
    const auto effect = [&](std::string_view tree) -> std::optional<f32> {
        if (!m_resources->weapons.loaded()) {
            return std::nullopt;
        }
        EffectTrees::Setting setting;
        if (tree == kBarrelBlast) {
            setting.light = EffectTrees::Light{DynamicLights::blast(),
                                               DynamicLights::kBlastRadiusScale * kBlastRadius};
        }
        const u32 id = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                     tree, struck->position, setting);
        return m_resources->effects.remaining(id);
    };
    switch (struck->kind) {
    case BreakableStrike::Kind::Plain:
    case BreakableStrike::Kind::Holding:
        playRealmSound(kBarrelBreakSound);
        // What it held comes out: Death himself, or a pickup (fn_8005E90C).
        if (struck->contents >= 0 &&
            ((events.releaseEnemy &&
              events.releaseEnemy(struck->contents, struck->position, struck->count)) ||
             m_resources->world.placeItemRecord(m_resources->device, struck->contents,
                                                struck->position, struck->count))) {
            for (usize i = 0; i < players.size(); ++i) {
                if (players[i].actor.player() == byPlayer) {
                    events.help(HelpMessages::kBarrelsHold, i);
                }
            }
        }
        break;
    case BreakableStrike::Kind::Exploding: {
        playRealmSound(kBarrelBlastSound);
        const f32 seconds = effect(kBarrelBlast).value_or(kExplosionSeconds);
        effect(kBarrelSmoke);
        leaveRubble(Rubble::kBlownBarrel, m_barrels.transformOf(barrel));
        m_blasts.push_back(Blast{.position = struck->position,
                                 .radius = kBlastRadius,
                                 .damage = kBarrelBlastDamage * trapDamageScale(),
                                 .seconds = seconds,
                                 .players = {},
                                 .reached = {},
                                 .opponents = {},
                                 .playerReady = {},
                                 .opponentReady = {}});
        break;
    }
    case BreakableStrike::Kind::Poison: {
        playRealmSound(kBarrelGasSound);
        leaveRubble(Rubble::kGasBarrel, m_barrels.transformOf(barrel));
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
                          std::span<PlayerRuntime> players, const Events& events, f32 seconds) {
    if (!m_resources.has_value()) {
        return;
    }
    m_blasts.push_back(Blast{.position = position,
                             .radius = radius,
                             .damage = damage,
                             .seconds = seconds,
                             .players = {},
                             .reached = {},
                             .opponents = {},
                             .playerReady = {},
                             .opponentReady = {}});
    settleBlasts(players, events);
}

/** Starts every blast waiting, and those they set off in turn: each ring's first step, as
 * the effect's first frame (ProcessEffects, remaining = its whole life). */
void LevelFixtures::settleBlasts(std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    // A step may set more off, appended to be started in their turn.
    for (auto waiting = std::ranges::find(m_blasts, false, &Blast::started);
         waiting != m_blasts.end(); waiting = std::ranges::find(m_blasts, false, &Blast::started)) {
        const auto index = static_cast<usize>(std::distance(m_blasts.begin(), waiting));
        Blast ring = *waiting;
        ring.started = true;
        feel(ring, players, events);
        m_blasts[index] = std::move(ring);
    }
}

void LevelFixtures::worldExplosion(const Vec3& position, std::span<PlayerRuntime> players,
                                   const Events& events) {
    if (!m_resources) {
        return;
    }
    auto& resources = *m_resources;
    const auto realm = resources.world.ref().realmId;
    const bool customRealm = realm == 9 || realm == 11;
    ItemArchive* archive = &resources.weapons;
    std::string_view tree = "EXPLOSION";
    if (customRealm) {
        for (ItemArchive* candidate : {&resources.world.items(), &resources.world.realmItems()}) {
            if (candidate->trees.find("WORLD_EXP").has_value()) {
                archive = candidate;
                tree = "WORLD_EXP";
                break;
            }
        }
    }
    EffectTrees::Setting setting;
    setting.stretch = customRealm ? Vec3{1} : Vec3{1.5f, 1, 1.5f};
    const u32 effect =
        resources.effects.startSet(resources.device, *archive, tree, position, setting);
    const f32 lifetime = resources.effects.remaining(effect).value_or(kExplosionSeconds);
    Blast ring;
    ring.position = position;
    ring.radius = customRealm ? 6.0f : 5.0f;
    ring.damage = 50.0f;
    ring.seconds = lifetime;
    ring.flags = tree == "WORLD_EXP" ? kPoisonDamage : 0x21;
    m_blasts.push_back(std::move(ring));
    if (realm == 9) {
        f32 distance = std::numeric_limits<f32>::max();
        for (const PlayerRuntime& player : players) {
            if (player.life == PlayerLife::Standing) {
                distance = std::min(distance, glm::distance(position, player.actor.position()));
            }
        }
        resources.audio.playAt("S_MINECAREXPLO", position, distance, 127.0f / 255.0f);
    }
    settleBlasts(players, events);
}

/** Grows the rings under way by `seconds`, then starts what they set off; spent ones go. */
void LevelFixtures::advanceBlasts(f32 seconds, std::span<PlayerRuntime> players,
                                  const Events& events) {
    // By index: a step may append what it sets off, which starts below.
    for (const usize i : std::views::iota(usize{0}, m_blasts.size())) {
        if (!m_blasts[i].started || m_blasts[i].done) {
            continue;
        }
        Blast ring = m_blasts[i];
        ring.elapsed += seconds;
        feel(ring, players, events);
        m_blasts[i] = std::move(ring);
    }
    settleBlasts(players, events);
    std::erase_if(m_blasts, [](const Blast& ring) { return ring.done; });
}

void LevelFixtures::feel(Blast& ring, std::span<PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value()) {
        return;
    }
    const f32 phase = ring.seconds <= kOneFrame
                          ? 1.0f
                          : std::clamp(1.0f - ring.elapsed / ring.seconds, 0.0f, 1.0f);
    if (phase <= kRingFade) {
        ring.done = true;
        return;
    }
    struct Step {
        Vec3 position;
        f32 radius = 0.0f;
        f32 damage = 0.0f;
    };
    const Step felt{ring.position, ring.radius * (kRingFade + (1.0f - phase)),
                    ring.damage * kRingGrowth * (phase - kRingFade)};
    const bool holds = felt.damage > kRingHeldFrom;
    const bool poison = (ring.flags & kPoisonDamage) != 0;
    if (poison) {
        const auto expire = [&](auto& reached, auto& ready) {
            std::erase_if(ready, [&](const auto& hit) { return hit.second <= ring.elapsed; });
            std::erase_if(reached, [&](auto id) {
                return std::ranges::none_of(ready,
                                            [&](const auto& hit) { return hit.first == id; });
            });
        };
        expire(ring.players, ring.playerReady);
        expire(ring.opponents, ring.opponentReady);
    }
    const auto first = [&](std::vector<s32>& reached, s32 id) {
        if (std::ranges::find(reached, id) != reached.end()) {
            return false;
        }
        if (holds) {
            reached.push_back(id);
        }
        return true;
    };
    for (usize i = 0; i < players.size(); ++i) {
        if (players[i].life != PlayerLife::Standing ||
            std::ranges::find(ring.players, i) != ring.players.end()) {
            continue;
        }
        const PlayerActor& actor = players[i].actor;
        const Vec3 offset = actor.followPoint() - felt.position;
        const f32 across = std::hypot(offset.x, offset.z);
        if (across <= felt.radius + actor.radius() &&
            std::abs(offset.y) <= actor.height() * 0.5f + felt.radius) {
            // Far enough out, a wall between shelters them from it.
            if (across > kShelterFrom &&
                EnemyMissiles::walled(m_resources->world.collision(), felt.position,
                                      actor.followPoint(), kShelterProbe)) {
                continue;
            }
            if (holds) {
                ring.players.push_back(i);
                if (poison) {
                    ring.playerReady.emplace_back(i, ring.elapsed + 0.5f);
                }
            }
            // A blast that gets through knocks its victim off their feet: onto their face
            // when it came from behind them, onto their back otherwise; one of under five
            // floors nobody.
            const bool guarding =
                players[i].figure != nullptr && players[i].figure->animator().guarding();
            if ((ring.flags & EnemyHit::kKnockDown) != 0 && felt.damage >= kRingKnockFrom &&
                PlayerHealth::guarded(players[i], felt.damage, true) > kKnockdownFrom &&
                !PowerupEffects::of(actor.save().progress().inventory).preventsKnockback() &&
                !guarding && !m_resources->world.isTower()) {
                const Vec3 push = actor.position() - felt.position;
                f32 round = std::atan2(push.x, push.z) - actor.yaw();
                round = std::remainder(round, 2.0f * kBehind * 2.0f);
                players[i].reaction =
                    std::abs(round) > kBehind ? PlayerDeed::FallBack : PlayerDeed::FallForward;
                // And throws them a little way from it (ProcessEffects: a quarter of the
                // way out along the ground).
                const Vec2 out{push.x, push.z};
                if (glm::length(out) > 0.0f) {
                    const Vec2 away = glm::normalize(out) * kBlastPush;
                    players[i].knockback.queue(
                        Vec3{away.x, 0.0f, away.y}, PlayerImpact::kKnockDown,
                        PlayerHealth::guarded(players[i], felt.damage, true));
                }
            }
            events.hurt(i, felt.damage, poison ? HurtKind::Gas : HurtKind::Blow, true);
        }
    }
    // ProcessEffects shortens the item query by 1.5 for DMG_EXPLODE: barrels, walls and rocks
    // are items too.
    const f32 reach = std::max(
        0.0f, felt.radius - ((ring.flags & kExplosionDamage) != 0 ? kItemBlastInset : 0.0f));
    const auto opponents = [&] {
        if (!events.opponents) {
            return;
        }
        const usize previous = ring.opponents.size();
        events.opponents(felt.position, felt.radius, felt.damage, ring.opponents, ring.flags);
        for (usize i = previous; poison && i < ring.opponents.size(); ++i) {
            ring.opponentReady.emplace_back(ring.opponents[i], ring.elapsed + 0.5f);
        }
    };
    if (poison) {
        opponents();
        spoilFood(felt.position, reach, felt.damage, players, events);
        return; // fn_8005C1DC: gas does not subtract item health or detonate containers.
    }
    for (const usize barrel : m_barrels.within(felt.position, reach)) {
        if (first(ring.reached, kBarrelReached + static_cast<s32>(barrel))) {
            strikeBarrel(barrel, felt.damage, -1, players, events);
        }
    }
    const auto& walls = m_resources->world.walls();
    for (usize i = 0; i < walls.size(); ++i) {
        if (walls.standing(i) && walls.target(i, 0).touches(felt.position, reach) &&
            first(ring.reached, kWallReached + static_cast<s32>(i))) {
            strikeWall(i, felt.damage);
        }
    }
    for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
        if (m_safeRocks.rock(rock).obstacle.touchedBy(felt.position, reach, 0.0f) &&
            first(ring.reached, kRockReached + static_cast<s32>(rock))) {
            strikeSafeRock(rock, felt.damage);
        }
    }
    opponents();
    const bool destructive = (ring.flags & kExplosionDamage) != 0;
    blastFixtures(felt.position, reach, felt.damage, events, ring.reached, destructive);
    blastPickups(felt.position, felt.radius, felt.damage, ring.flags, players, events);
}

void LevelFixtures::blastPickups(const Vec3& position, f32 radius, f32 damage, u32 flags,
                                 std::span<const PlayerRuntime> players, const Events& events) {
    if (!m_resources) {
        return;
    }
    const bool destructive = (flags & kExplosionDamage) != 0;
    const f32 reach = std::max(0.0f, radius - (destructive ? kItemBlastInset : 0.0f));
    if ((flags & kPoisonDamage) != 0) {
        spoilFood(position, reach, damage, players, events);
        return;
    }
    const auto changes =
        m_resources->world.blastItems(m_resources->device, position, reach, damage, destructive);
    bool destroyed = false;
    for (const auto& change : changes) {
        if (change.potion) {
            if (events.shatterPotion) {
                events.shatterPotion(*change.potion, change.position);
            }
            continue; // Magic replaces the bottle; no food-destruction smoke or help.
        }
        // The retail effect table maps both CHESTDEST and ITEMDEST to this tree.
        for (const std::string_view tree : {kChestDestroyed, kBarrelSmoke}) {
            const u32 id = m_resources->effects.startSet(m_resources->device, m_resources->weapons,
                                                         tree, change.position, {});
            m_resources->effects.placeAt(id, change.transform);
        }
        if (change.destroyed) {
            leaveRubble(Rubble::kItem, change.transform);
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

void LevelFixtures::spoilFood(const Vec3& position, f32 radius, f32 damage,
                              std::span<const PlayerRuntime> players, const Events& events) {
    if (!m_resources.has_value() ||
        m_resources->world.poisonFood(m_resources->device, position, radius, damage) == 0 ||
        !events.help) {
        return;
    }
    // Retail posts msgPost(0x88, -1, 0): party-wide help, not credited to whoever let the gas
    // out. Present it over the first standing player.
    for (usize i = 0; i < players.size(); ++i) {
        if (players[i].life == PlayerLife::Standing) {
            events.help(HelpMessages::kGasSpoils, i);
            return;
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
        spoilFood(cloud.position, kGasRadius, cloud.damage, players, events);
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
