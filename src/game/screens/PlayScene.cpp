#include "game/screens/PlayScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/core/Log.h"
#include "engine/world/WorldCamera.h"

#include "game/enemies/BossCoins.h"
#include "game/enemies/LegendItems.h"
#include "game/players/ItemPickup.h"
#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kBeamObject = "L1XPLIGHTRAY01"; ///< the light on Sumner's lectern
constexpr std::string_view kWeaponsArchive = "WEAPONS";
constexpr std::string_view kSpawnEffect = "STARTFX"; ///< the tree the party materialises in
/** The stained-glass light through the window over the door: the Desecrated Temple's, lit once
 * its shards are all found. */
constexpr std::array<std::string_view, 2> kTempleLights{"L1XPLOWERLIGHTR", "L1XPUPPERLIGHTR"};
constexpr f32 kCutBarTop = 48.0f / 384.0f;    ///< the cut's black bars, as the original's trigger
constexpr f32 kCutBarBottom = 80.0f / 384.0f; ///< cameras draw them: shares of the height
constexpr std::string_view kNeedCrystals = "NEEDCRYSTALS";
constexpr std::string_view kNeedIcons = "NEEDGARGITEMS";
constexpr std::string_view kUnlockLevel = "UNLOCKLEVEL";
constexpr s32 kIconTierBase = 101; ///< a gargoyle gate's trigger id less this is its tier

constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";
constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";
constexpr std::string_view kScrollBurnSound = "S_OPTMENUSCROLL"; ///< the options menu's, too
constexpr s32 kOgre = 12;
constexpr f32 kOgreScale = 1.6f;
constexpr f32 kMasterScale = 1.2f; ///< at level 99
constexpr std::string_view kChestSound = "S_CHEST";
constexpr std::string_view kFirstRuneVoice = "S_RUNEFOUND1";
constexpr std::string_view kRuneVoicePrefix = "S_RUNE";    ///< then S_RUNE2 to S_RUNE12
constexpr std::string_view kLevelScrollPrefix = "SCROLLS"; ///< a level's scroll pages
constexpr std::string_view kBossKeyTree = "BOSSKEY"; ///< the key that rises where the boss fell
constexpr std::string_view kBossKeyLaterTree = "BOSSKEY2";
constexpr f32 kBossKeySeconds = 30.0f;
constexpr std::string_view kBossKeySoundPrefix = "S_BOSSKEY"; ///< then the level's letter
constexpr std::string_view kNoEffectTree = "NULLFX"; ///< a move's effect row that shows nothing
constexpr std::array<std::string_view, 5> kShieldTrees{"MS_FIRE", "MS_FIRE", "MS_ELEC", "MS_LIGHT",
                                                       "MS_ACID"};
constexpr std::array<std::string_view, 5> kShieldSounds{"S_SHIELD2", "S_SHIELD2", "S_SHIELD1",
                                                        "S_SHIELD3", "S_SHIELD4"};
constexpr f32 kShieldSeconds = 3.0f;   ///< how long a potion's ring lasts
constexpr f32 kShieldRadius = 25.0f;   ///< at full size; it is sized by the magic, as a burst is
constexpr f32 kShieldPotency = 0.25f;  ///< of the character's magic power, its harm
constexpr f32 kShieldHarmEvery = 0.5f; ///< seconds between its harming what it touches
constexpr f32 kLevelUpEffectSeconds = 3.0f; ///< the fanfare's ring about the character
constexpr f32 kKnockdownFrom = 1.0f;        ///< a blast must do more than this to floor anyone
constexpr f32 kBehind = 1.5707964f;         ///< a blow from further round than this is from behind
constexpr f32 kStrongThrowScale = 2.0f; ///< a strong throw's weapon: twice the size and the harm
constexpr std::string_view kBlockEffect = "BLOCKFX";
constexpr f32 kBlockWorth = 2.0f;      ///< what a guard must take off a hurt for it to show
constexpr f32 kBlockPerDamage = 0.01f; ///< seconds it shows for each point left
constexpr f32 kBlockLeast = 0.333f;
constexpr f32 kBlockMost = 1.0f;
constexpr f32 kRamDamage = 3.0f;    ///< what a charge does to what it runs into
constexpr f32 kRamReach = 0.3f;     ///< how near counts as run into
constexpr s32 kSpecialPowerup = 9;  ///< the pickup subtype of the specials
constexpr u32 kTurboFlag = 0x80000; ///< of them, the one that fills the turbo meter
constexpr std::string_view kWoodHitSound = "S_WEAPONHITWOOD";
constexpr std::string_view kBarrelBreakSound = "S_BARREL_WOOD"; ///< with the realm's letter
constexpr std::string_view kBarrelBlastSound = "S_BARREL_EXPLO";
constexpr std::string_view kBarrelGasSound = "S_BARREL_GAS";
constexpr std::string_view kFireTrapSound = "S_FIREHOLE";
constexpr std::string_view kBarrelBlast = "EXPLOSION";
constexpr std::string_view kBarrelGas = "POISONEXP1";
constexpr std::string_view kBarrelSmoke = "DESTSMOKE";
constexpr f32 kBlastRadius = 12.0f;      ///< of a chest or a barrel blowing up
constexpr f32 kChestBlastDamage = 50.0f; ///< each times the level's trap damage
constexpr f32 kBarrelBlastDamage = 30.0f;
constexpr f32 kGasDamage = 10.0f;
constexpr f32 kGasRadius = 6.5f;
constexpr f32 kGasSeconds = 4.0f;
constexpr f32 kGasGapSeconds = 0.5f;
constexpr f32 kFallenSeconds = 3.0f; ///< from the last death to the tower
constexpr s32 kFireTrap = 1;
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};
constexpr std::string_view kChestBlast = "EXPCHEST"; ///< a trapped chest going up

constexpr std::string_view kMenuMoveSound = "S_OPTMENUMOVVRT";
constexpr std::string_view kMenuSelectSound = "S_OPTMENUSEL";
constexpr std::string_view kMenuExitSound = "S_OPTMENUEXIT";
constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr s32 kMinTicks = 1; ///< a frame advances the clock by at least one tick
constexpr s32 kMaxTicks = 4; ///< and, however late, by at most four

} // namespace

bool PlayScene::open(RenderDevice& device, const GameContext& context, LevelWorld& world,
                     std::span<const PartyMember> party, const PlayOptions& options) {
    close();
    m_context = context;
    m_device = &device;
    m_world = &world;
    if (!world.built() && !world.load(device, context.unpackedRoot)) {
        return false;
    }
    if (!m_hud.load(device, context.unpackedRoot, context.strings)) {
        return false;
    }
    m_classes.load(context.unpackedRoot / kClassDataDirectory);
    m_audio.open(context.unpackedRoot, context.sounds, world.audio());
    m_messages.load(device, m_staticTextures, m_context.unpackedRoot, m_context.strings);
    // Sumner, his hints and his welcome belong to the tower alone.
    if (world.isTower()) {
        m_hud.setGlow(m_sumnerVisit.load(device, m_staticTextures, m_world->powerups(),
                                         m_context.unpackedRoot, m_context.strings));
        m_sumner.load(device, world.items(), world.layout());
    }
    if (context.levels != nullptr) {
        m_portals.bind(device, world.layout(), world.items(), *context.levels, &world.collision());
    }
    m_chests.bind(device, world.layout(), world.items(), &world.collision());
    m_gates.bind(device, world.layout(), world.items(), &world.collision());
    {
        const LevelInfo* level = world.level();
        const f32 gain = context.config != nullptr ? context.config->difficulty.gain() : 1.0f;
        m_traps.bind(device, world.layout(), world.items(), &world.collision(), 1,
                     level != nullptr ? level->tuning.trapTimeScale(gain) : 1.0f,
                     trapDamageScale());
    }
    m_barrels.bind(device, world.layout(), world.items(), &world.collision());
    m_safeRocks.bind(device, world.layout(), world.items());
    m_refusedPortal = -1;
    m_leaving = false;
    m_transition.load(device, context.unpackedRoot);
    if (options.arriving) {
        m_transition.cover();
        m_transition.clearAway();
    }
    // Sumner's beam waits unseen until the party comes to him; the temple's light waits for
    // shards the save does not keep yet.
    m_beam = -1;
    m_beamAlpha = 0.0f;
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        const std::string& name = world.layout().objects()[i].name;
        if (name == kBeamObject) {
            m_beam = static_cast<s32>(i);
            world.setObjectAlpha(i, 0.0f);
        } else if (std::ranges::find(kTempleLights, name) != kTempleLights.end()) {
            world.setObjectAlpha(i, 0.0f);
        }
    }
    spawnParty(party, options);
    for (const DroppedItem& item : options.items) {
        world.placeItem(device, item.name, item.position);
    }
    world.setPlayerCount(static_cast<s32>(m_players.size()));
    m_chests.setPlayerCount(static_cast<s32>(m_players.size()));
    m_gates.setPlayerCount(static_cast<s32>(m_players.size()));
    m_traps.setPlayerCount(static_cast<s32>(m_players.size()));
    m_barrels.setPlayerCount(static_cast<s32>(m_players.size()));
    m_safeRocks.setPlayerCount(static_cast<s32>(m_players.size()));
    bindEnemies(device, world, context);
    // The levels the party comes in at: what is gained from here is news.
    m_levels.clear();
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        m_levels.observe(actor.player(), experienceLevel(actor.save().experience()));
    }
    world.startTriggers(visitors());
    m_audio.bindAmbience(world.layout());
    std::vector<CameraSubject> subjects;
    subjects.reserve(m_players.size());
    for (PlayerRuntime& runtime : m_players) {
        runtime.figure = PlayerFigure::load(device, m_context.unpackedRoot, runtime.actor.save());
        subjects.push_back(CameraSubject{runtime.actor.position(), runtime.actor.followPoint()});
    }
    m_camera.reset(subjects, world.cameraMarkers(), world.cameraRange(), cameraView());
    if (const LevelInfo* level = world.level();
        level != nullptr && level->bossCamera.has_value() && bossCameraOn()) {
        m_bossCamera.reset(bossSubject(), subjects, *level->bossCamera, cameraView());
    }
    m_audio.startMusic(context.assets,
                       world.level() != nullptr ? world.level()->musicVolume : 1.0f);
    m_intro = Intro::None;
    // The party materialises first; Sumner's welcome, when it is due, follows.
    m_welcomePending = world.isTower() && options.welcome.value_or(freshParty(party));
    if (m_welcomePending) {
        m_world->hideCrystals(); // Sumner reveals them once the scroll has gone
    }
    // The start camera holds at the level's entrance and rides in to a party that stands
    // there; one back among a realm's portals (as when it has fallen, or come out of a level)
    // materialises with the follow camera already on it.
    const bool atEntrance = world.arrivalPoint(options.arrivalWorld) == world.startPoint(0);
    beginSpawn(device, !options.position.has_value() && atEntrance);
    m_arsenal.bind(
        {device, m_classes, m_weapons, world.collision(), m_effects, m_audio, context.sounds});
    m_legend = std::make_unique<LegendPresentation>(
        m_effects, LegendPresentation::Assets{device, world.items(), m_weapons, m_staticTextures},
        LegendPresentation::Audio{[this](std::string_view name) { return m_audio.playNamed(name); },
                                  [this](SoundHandle handle) { m_audio.stop(handle); }});
    m_open = true;
    log::info("Tower: {} in the party", m_players.size());
    log::info("Level {} ({}): {} exit portals", world.ref().name, world.ref().title,
              m_portals.size());
    return true;
}

void PlayScene::close() {
    m_audio.stopCues();
    m_sumnerVisit.clear();
    m_messages.clear();
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_portals.clear();
    m_chests.clear();
    m_gates.clear();
    m_traps.clear();
    m_transition.release();
    m_leaving = false;
    m_arsenal.clear(); // before the figures whose models they fly
    m_critterEffects.clear();
    m_legend.reset();
    m_effects.clear(); // and before the archive whose trees they play
    m_generators.clear();
    m_enemyMissiles.clear(); // before the archives whose trees they fly
    m_critters.close();
    m_bossMeter.clear(); // before the archive whose textures it draws
    m_victory.clear();
    m_bosses.close();
    m_enemies.close();
    m_playSeconds = 0.0f;
    m_hud.clear(); // before the static texture borrowed for selector glow
    m_staticTextures.releaseTextures();
    m_intro = Intro::None;
    m_strikes.clear();
    m_strikeEffects.clear();
    m_strikeSources.clear();
    m_shields.clear();
    m_dimmer.reset();
    if (m_world != nullptr) {
        m_world->setAmbientOffset(0.0f);
    }
    m_clouds.clear();
    m_blasts.clear();
    m_fallenSeconds = 0.0f;
    m_barrels.clear();
    m_safeRocks.clear();
    m_audio.close(); // before the figures whose class voices it can play
    m_players.clear();
    m_arrival.clear();
    m_weapons.release(); // its textures must go before the device does
    m_welcomePending = false;
    m_world = nullptr;
    m_device = nullptr;
    m_open = false;
}

/** Stands the party side by side at the entrance, facing into the tower: the start marker's
 * heading points back out of the door. */
void PlayScene::spawnParty(std::span<const PartyMember> party, const PlayOptions& options) {
    // Arriving from a realm the party stands where the level marks that realm's way in.
    const WorldLocator* start = m_world->arrivalPoint(options.arrivalWorld);
    Vec3 origin{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    if (start != nullptr) {
        origin = start->position;
        yaw = start->rotation.y + kPi;
    } else if (!options.position.has_value()) {
        log::warn("Tower: no entrance start point; the party stands at the origin");
    }
    origin = options.position.value_or(origin);
    yaw = options.yaw.value_or(yaw);
    const Vec3 sideways{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const f32 first = -0.5f * static_cast<f32>(party.size() - 1) * kSpawnSpacing;
    m_players.reserve(party.size());
    for (usize i = 0; i < party.size(); ++i) {
        const PartyMember& member = party[i];
        PlayerRuntime runtime;
        const Vec3 position = origin + sideways * (first + static_cast<f32>(i) * kSpawnSpacing);
        runtime.actor.spawn(member.player, member.save, m_classes.stats(member.save.character),
                            position, yaw);
        runtime.actor.settle(m_world->collision());
        runtime.slot = member.slot;
        // Someone who fell stands again in the tower; elsewhere they wait there still.
        runtime.life =
            member.fallen && !m_world->isTower() ? PlayerLife::InTower : PlayerLife::Standing;
        runtime.entrySave = member.save;
        runtime.turbo.add(member.turbo);
        runtime.helpHeard = member.helpHeard;
        std::ranges::sort(runtime.helpHeard);
        m_players.push_back(std::move(runtime));
    }
}

std::filesystem::path PlayScene::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                  const CharacterSave& save) {
    return PlayerFigure::costumeDirectory(unpackedRoot, save);
}

/** Lets the weapon go: from the body's centre, out by the class's hand and a little ahead,
 * along the facing, as fast as the character's strength (or magic) throws. */
void PlayScene::throwWeapon(const PlayerActor& actor) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (&m_players[i].actor == &actor) {
            launchWeapon(i, actor.facing(), 1.0f, true);
        }
    }
}

void PlayScene::launchWeapon(usize index, const Vec3& direction, f32 scale, bool spreads) {
    if (index < m_players.size()) {
        m_arsenal.launchWeapon(m_players[index].actor, m_players[index].figure.get(), direction,
                               scale, spreads);
    }
}

std::optional<std::filesystem::path> PlayScene::figureDirectory(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player && runtime.figure != nullptr) {
            return runtime.figure->directory();
        }
    }
    return std::nullopt;
}

bool PlayScene::weaponHeld(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player && runtime.figure != nullptr) {
            // A move may empty the hand for a while.
            const bool hidden = runtime.move.weaponHidden();
            return runtime.figure->heldWeaponBound() && !hidden;
        }
    }
    return false;
}

/** Sumner's beam comes up over three seconds while a player is near him and goes again once
 * they leave. */
void PlayScene::updateBeam(s32 ticks) {
    if (m_beam < 0) {
        return;
    }
    bool near = false;
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        near = near || glm::distance(actor.position(), m_sumner.position()) <= kBeamRadius;
    }
    const f32 step = static_cast<f32>(ticks) / static_cast<f32>(kBeamFadeTicks);
    const f32 alpha = std::clamp(m_beamAlpha + (near ? step : -step), 0.0f, 1.0f);
    if (alpha != m_beamAlpha) {
        m_beamAlpha = alpha;
        m_world->setObjectAlpha(static_cast<usize>(m_beam), alpha);
    }
}

/** Takes what the party stands on: a crystal counts for everyone, towards its realm's gate,
 * up to what the gate wants; the taker's box gets the card, every box the count. */
void PlayScene::collectItems() {
    if (m_device == nullptr) {
        return;
    }
    std::vector<Collector> collectors;
    collectors.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        // Against an open chest, a character reaches what lies in it.
        const Vec3 here = presenceOf(collectors.size());
        const s32 chest = m_chests.holdingTouchedBy(ChestVisitor{here, actor.radius()});
        const Vec3 from =
            chest >= 0 ? m_chests.chest(static_cast<usize>(chest)).figure.position() : here;
        collectors.push_back(Collector{from, actor.reach(), actor.height() * 0.5f});
    }
    const std::vector<Pickup> pickups = m_world->collect(
        *m_device, collectors, [this](const Pickup& pickup) { return takePickup(pickup); });
    for (usize chest = 0; chest < m_chests.size(); ++chest) {
        const s32 held = m_chests.chest(chest).held;
        if (held >= 0 && m_world->placedItems().item(static_cast<usize>(held)).taken) {
            m_chests.remove(chest);
        }
    }
    for (const Pickup& pickup : pickups) {
        if (pickup.realm <= 0) {
            continue; // handed over as it was judged
        }
        if (pickup.realm > 0 && static_cast<usize>(pickup.realm) < kRealmCount) {
            const s32 wanted = LevelTriggers::crystalsNeeded(pickup.realm);
            bool enough = wanted > 0;
            for (PlayerRuntime& runtime : m_players) {
                PlayerActor& actor = runtime.actor;
                s32& count = actor.save().progress().crystals[static_cast<usize>(pickup.realm)];
                if (wanted <= 0 || count < wanted) {
                    ++count;
                }
                enough = enough && count >= wanted;
                m_hud.pickups().showCount(actor.player(), PickupHud::crystalIcon(pickup.realm),
                                          count, wanted);
            }
            if (pickup.collector < m_players.size()) {
                m_hud.pickups().addCard(m_players[pickup.collector].actor.player(),
                                        PickupHud::kCrystalCard);
            }
            if (enough) {
                announceUnlock(pickup.realm);
            }
        }
        m_audio.playPickup();
    }
}

/** The level's chests, gates and traps under the party: nobody walks through a chest or a
 * gate that is shut; against one, a key carried is spent and it opens (a chest's sound is
 * the common one, a gate's its realm's); an opened chest drops what it held, pays its gold
 * to its opener or blows up; a trap that is out hurts whoever is in it. */
void PlayScene::updateFixtures(s32 ticks, f32 seconds) {
    std::vector<Obstacle> boxes = m_chests.obstacles();
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    m_barrels.update(seconds);
    const auto cover = m_safeRocks.obstacles();
    boxes.insert(boxes.end(), cover.begin(), cover.end());
    std::vector<ChestVisitor> visitors;
    std::vector<TrapVictim> victims;
    visitors.reserve(m_players.size());
    victims.reserve(m_players.size());
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerActor& actor = m_players[i].actor;
        if (isDown(i)) {
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
            ChestVisitor{position, actor.radius(), actor.save().progress().inventory.keys});
        victims.push_back(TrapVictim{position, actor.radius()});
    }
    for (const ChestEvent& event : m_chests.update(seconds, visitors)) {
        if (event.visitor >= m_players.size()) {
            continue;
        }
        PlayerActor& actor = m_players[event.visitor].actor;
        switch (event.kind) {
        case ChestEvent::Kind::Unlocked:
            if (m_chests.chest(event.chest).locked) {
                actor.save().progress().inventory.spendKey();
            }
            m_audio.playNamed(kChestSound);
            break;
        case ChestEvent::Kind::Opened:
            if (event.explodes) {
                if (m_device != nullptr && m_weapons.loaded()) {
                    m_effects.start(*m_device, m_weapons, kChestBlast, event.position);
                }
                playRealmSound(kBarrelBlastSound);
                m_chests.remove(event.chest);
                postHelp(HelpMessages::kChestsExplode, event.visitor);
                blast(event.position, kBlastRadius, kChestBlastDamage * trapDamageScale());
            } else if (event.gold > 0) {
                takeItem(actor.save(), ItemOffer{static_cast<s32>(ItemKind::Gold), event.gold});
                m_hud.pickups().addCard(actor.player(), "GOLD");
                m_audio.playNamed(kPickupSound);
            } else if (event.contents >= 0 && m_device != nullptr) {
                // It lies in the open chest, for whoever touches the chest next.
                const s32 count = m_chests.chest(event.chest).count;
                if (m_world->placeItemRecord(*m_device, event.contents, event.position, count)) {
                    m_chests.hold(event.chest, static_cast<s32>(m_world->placedItems().size()) - 1);
                }
            } else {
                m_chests.remove(event.chest);
            }
            break;
        case ChestEvent::Kind::Refused:
            postHelp(HelpMessages::kChestNeedsKey, event.visitor);
            break;
        }
    }
    for (const GateEvent& event : m_gates.update(ticks, seconds, visitors)) {
        if (event.visitor >= m_players.size()) {
            continue;
        }
        if (event.kind == GateEvent::Kind::Unlocked) {
            m_players[event.visitor].actor.save().progress().inventory.spendKey();
            playGateSound(0);
        } else if (event.kind == GateEvent::Kind::Refused) {
            postHelp(HelpMessages::kDoorNeedsKey, event.visitor);
        }
    }
    for (const TrapHit& hit : m_traps.update(ticks, seconds, victims)) {
        if (hit.victim >= m_players.size()) {
            continue;
        }
        if (hit.subtype == kFireTrap) {
            playRealmSound(kFireTrapSound);
        }
        // Every trap stuns: spikes and blades make their victim flinch, the rest reel.
        if (guarded(hit.victim, hit.damage, false) > 1.0f && !isDown(hit.victim)) {
            m_players[hit.victim].reaction = hit.pierces ? PlayerDeed::Flinch : PlayerDeed::Reel;
        }
        hurt(hit.victim, hit.damage, hit.pierces ? HurtKind::Pierce : HurtKind::Burn);
        postHelp(HelpMessages::kTrapsHurt, hit.victim);
    }
    updateClouds(seconds);
    for (PlayerRuntime& runtime : m_players) {
        runtime.hitSoundGap = std::max(runtime.hitSoundGap - ticks, 0);
    }
}

/** What the level's traps and blasts are scaled by: its own trap damage and the
 * difficulty's gain. */
f32 PlayScene::trapDamageScale() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    const f32 gain = m_context.config != nullptr ? m_context.config->difficulty.gain() : 1.0f;
    return level != nullptr ? level->tuning.trapDamageScale(gain) : gain;
}

Vec3 PlayScene::presenceOf(usize index) const {
    return index < m_players.size() && !isDown(index) ? m_players[index].actor.position()
                                                      : kNowhere;
}

bool PlayScene::fallen(s32 player) const {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() == player) {
            return isDown(i);
        }
    }
    return false;
}

void PlayScene::hurtPlayer(s32 player, f32 damage, HurtKind kind, bool directed) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() == player) {
            hurt(i, damage, kind, directed);
        }
    }
}

/** What a guard or a shove leaves of a hurt over a point, by the original's rules as it
 * shipped: a raised guard halves what comes from somewhere and takes all of what comes from
 * nowhere in particular (a trap underfoot); a shove halves either. */
f32 PlayScene::guarded(usize index, f32 damage, bool directed) const {
    return index < m_players.size() ? PlayerHealth::guarded(m_players[index], damage, directed)
                                    : damage;
}

/** A sound of the realm's bank, whose names end in the realm's letter. */
SoundHandle PlayScene::playRealmSound(std::string_view stem) {
    const std::string& level = m_world->ref().name;
    const char letter = level.empty() ? 'G' : level.front();
    return m_audio.playNamed(std::format("{}{}", stem, letter));
}

const TurboMeter* PlayScene::turboMeter(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            return &runtime.turbo;
        }
    }
    return nullptr;
}

/** What a charge runs into is struck, once each charge. */
void PlayScene::ramBarrels(usize index) {
    const PlayerActor& actor = m_players[index].actor;
    std::vector<usize>& rammed = m_players[index].rammed;
    for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
        if (!m_barrels.standing(barrel) || std::ranges::find(rammed, barrel) != rammed.end() ||
            !m_barrels.barrel(barrel).box.touchedBy(actor.position(), actor.radius(), kRamReach)) {
            continue;
        }
        rammed.push_back(barrel);
        strikeBarrel(barrel, kRamDamage, actor.player());
    }
    for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
        // Keep the shared per-charge hit ledger disjoint from barrel indices.
        const usize key = rock + static_cast<usize>(kSafeRockTargetBase);
        if (m_safeRocks.standing(rock) && std::ranges::find(rammed, key) == rammed.end() &&
            m_safeRocks.rock(rock).obstacle.touchedBy(actor.position(), actor.radius(),
                                                      kRamReach)) {
            rammed.push_back(key);
            strikeSafeRock(rock, kRamDamage);
        }
    }
    settleBlasts();
}

/** The costume colour's effects, which hold the trees a class's moves show; loaded when
 * first wanted. */
ItemArchive* PlayScene::moveEffectsOf(usize index) {
    PlayerFigure* figure = index < m_players.size() ? m_players[index].figure.get() : nullptr;
    return figure != nullptr ? figure->effects() : nullptr;
}

/** What a character's own blows do, which a strike with a negative amount multiplies. */
f32 PlayScene::ownDamageOf(usize index) const {
    const CharacterSave& save = m_players[index].actor.save();
    const ClassStats* stats = m_classes.stats(save.character);
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
void PlayScene::fireStrike(usize index, s32 strikeIndex) {
    const ClassStats* stats = m_classes.stats(m_players[index].actor.save().character);
    if (stats == nullptr || strikeIndex < 0 ||
        static_cast<usize>(strikeIndex) >= stats->moveStrikes.size()) {
        return;
    }
    const MoveStrike& strike = stats->moveStrikes[static_cast<usize>(strikeIndex)];
    const PlayerActor& actor = m_players[index].actor;
    const Vec3 facing = actor.facing();
    // A span that only lasts, or a volley, harms nothing of itself; the rest are set going.
    u32 id = 0;
    if (strike.harms()) {
        id = m_strikes.start(strike, actor.player(), actor.position(), facing, ownDamageOf(index));
        m_strikeSources.push_back(StrikeSource{id, index, strikeIndex});
    }
    const Vec3 origin = MoveStrikes::originOf(strike, actor.position(), facing);
    const MoveStrikes::Strike* started = m_strikes.find(id);
    ItemArchive* archive = moveEffectsOf(index);
    // An effect may bring another with it.
    usize followed = 0;
    for (s32 at = strike.effect; at >= 0 && static_cast<usize>(at) < stats->moveEffects.size() &&
                                 followed < stats->moveEffects.size();
         at = stats->moveEffects[static_cast<usize>(at)].next, ++followed) {
        const MoveEffect& effect = stats->moveEffects[static_cast<usize>(at)];
        if (!effect.sound.empty()) {
            if (const auto sound = m_players[index].figure->voice().find(effect.sound);
                sound.has_value() && m_context.sounds != nullptr) {
                m_context.sounds->play(m_players[index].figure->voice().sequence(*sound), 1.0f,
                                       SoundCategory::Effects);
            } else {
                m_audio.playNamed(effect.sound);
            }
        }
        if (effect.tree.empty() || effect.tree == kNoEffectTree || archive == nullptr ||
            m_device == nullptr || !archive->trees.find(effect.tree).has_value()) {
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
        const u32 shown = m_effects.startSet(*m_device, *archive, effect.tree, at3, setting);
        if (shown != 0 && started != nullptr && started->flies) {
            m_strikeEffects.push_back(StrikeEffect{id, shown});
        }
    }
}

/** The strikes under way harm what they reach: the barrels, for now. What flies takes its
 * effect along, and the effect ends with it. */
void PlayScene::updateStrikes(f32 seconds) {
    for (const StrikeHit& hit : m_strikes.update(seconds, &m_world->collision())) {
        const auto source = std::ranges::find(m_strikeSources, hit.strike, &StrikeSource::strike);
        // The swarm and the generators in its reach take it, with the row's damage type.
        u32 flags = 0;
        if (source != m_strikeSources.end() && source->actor < m_players.size()) {
            const ClassStats* stats =
                m_classes.stats(m_players[source->actor].actor.save().character);
            if (stats != nullptr && source->row >= 0 &&
                static_cast<usize>(source->row) < stats->moveStrikes.size()) {
                flags = static_cast<u32>(
                    stats->moveStrikes[static_cast<usize>(source->row)].damageType);
            }
        }
        for (const s32 enemy : m_enemies.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = m_enemies.positionOf(enemy) - hit.centre;
            strikeEnemy(enemy, hit.damage, flags, Vec3{direction.x, 0.0f, direction.z}, hit.owner);
        }
        for (const s32 generator : m_generators.within(hit.centre, hit.radius)) {
            strikeGenerator(generator, hit.damage, hit.owner);
        }
        if (m_bosses.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            EnemyHit struck;
            struck.damage = hit.damage;
            struck.flags = flags;
            struck.player = hit.owner;
            struck.close = true;
            if (const Vec3* at = m_bosses.position(); at != nullptr) {
                struck.direction = Vec3{at->x - hit.centre.x, 0.0f, at->z - hit.centre.z};
                struck.where = hit.centre + glm::normalize(struck.direction) * hit.radius;
            }
            m_bosses.hurt(struck);
        }
        for (const s32 critter :
             m_critters.reachedBy(hit.centre, hit.radius, hit.arc, hit.facing)) {
            const Vec3 direction = m_critters.positionOf(critter) - hit.centre;
            strikeCritter(critter, hit.damage, flags, Vec3{direction.x, 0.0f, direction.z},
                          hit.owner, std::nullopt, true);
        }
        for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
            const auto& cover = m_safeRocks.rock(rock).obstacle;
            if (m_safeRocks.standing(rock) &&
                hit.reaches(cover.centre, cover.cylinderRadius, cover.height)) {
                strikeSafeRock(rock, hit.damage);
            }
        }
        for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
            if (!m_barrels.standing(barrel)) {
                continue;
            }
            const Breakables::Barrel& cask = m_barrels.barrel(barrel);
            if (!hit.reaches(cask.figure.position(), cask.radius, cask.height)) {
                continue;
            }
            strikeBarrel(barrel, hit.damage, hit.owner);
            // What it harms shows the strike's own mark, when its class gives it one.
            if (source == m_strikeSources.end() || source->actor >= m_players.size()) {
                continue;
            }
            const ClassStats* stats =
                m_classes.stats(m_players[source->actor].actor.save().character);
            ItemArchive* archive = moveEffectsOf(source->actor);
            if (stats == nullptr || archive == nullptr || m_device == nullptr) {
                continue;
            }
            const s32 mark = stats->moveStrikes[static_cast<usize>(source->row)].hitEffect;
            if (mark >= 0 && static_cast<usize>(mark) < stats->moveEffects.size()) {
                const MoveEffect& effect = stats->moveEffects[static_cast<usize>(mark)];
                if (!effect.tree.empty() && archive->trees.find(effect.tree).has_value()) {
                    m_effects.start(*m_device, *archive, effect.tree, cask.figure.position(),
                                    effect.scale);
                }
                if (!effect.sound.empty()) {
                    m_audio.playNamed(effect.sound);
                }
            }
        }
    }
    settleBlasts();
    std::erase_if(m_strikeSources, [this](const StrikeSource& source) {
        return m_strikes.find(source.strike) == nullptr;
    });
    std::erase_if(m_strikeEffects, [this](const StrikeEffect& pair) {
        if (m_strikes.find(pair.strike) != nullptr) {
            return false;
        }
        m_effects.stop(pair.effect);
        return true;
    });
}

/** Experience won, as the original awards it: scaled by the level (its own scale, less the
 * further the character is past the level the place is meant for); what a kill wins also
 * feeds the turbo meter, unless the character is in the middle of a turbo move. */
void PlayScene::harm(s32 player, f32 damage, HurtKind kind) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() == player) {
            hurt(i, damage, kind);
        }
    }
}

void PlayScene::awardExperience(s32 player, s32 amount, bool kill) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() != player || isDown(i) || amount <= 0) {
            continue;
        }
        CharacterSave& save = m_players[i].actor.save();
        const LevelInfo* level = m_world->level();
        const f32 scale = level != nullptr
                              ? level->tuning.experienceScale(experienceLevel(save.experience()))
                              : 1.0f;
        const auto won = static_cast<s32>(static_cast<f32>(amount) * scale);
        save.progress().experience += won;
        const bool busy =
            m_players[i].figure != nullptr && m_players[i].figure->animator().turboing();
        if (kill && !busy) {
            m_players[i].turbo.add(TurboMeter::kPerExperience * static_cast<f32>(won));
        }
    }
}

/** A potion spent on a shield: its magic rings the character for a few seconds, going about
 * with them, to the potion's shield sound. */
void PlayScene::shieldPotion(usize index) {
    PlayerActor& actor = m_players[index].actor;
    const s32 kind = actor.save().progress().inventory.takePotion();
    if (kind == 0) {
        return;
    }
    const auto look = static_cast<usize>(std::clamp(kind, 0, 4));
    const f32 power = m_arsenal.magicPowerOf(actor);
    const f32 size = std::min(PlayerArsenal::kBurstPerPower * power, 1.0f);
    PotionShield shield;
    shield.actor = index;
    shield.radius = kShieldRadius * size;
    shield.damage = kShieldPotency * power;
    shield.secondsLeft = kShieldSeconds;
    if (m_device != nullptr && m_weapons.loaded() &&
        m_weapons.trees.find(kShieldTrees[look]).has_value()) {
        EffectTrees::Setting setting;
        setting.scale = size;
        setting.seconds = kShieldSeconds;
        shield.effect =
            m_effects.startSet(*m_device, m_weapons, kShieldTrees[look], actor.position(), setting);
    }
    m_audio.playNamed(kShieldSounds[look]);
    m_shields.push_back(shield);
}

/** The rings go about with their characters and harm the barrels they touch. */
void PlayScene::updateShields(f32 seconds) {
    for (PotionShield& shield : m_shields) {
        shield.secondsLeft -= seconds;
        if (shield.actor >= m_players.size() || isDown(shield.actor)) {
            shield.secondsLeft = 0.0f;
            continue;
        }
        const Vec3 at = m_players[shield.actor].actor.position();
        m_effects.moveTo(shield.effect, at);
        shield.harmIn -= seconds;
        if (shield.harmIn > 0.0f) {
            continue;
        }
        shield.harmIn = kShieldHarmEvery;
        for (const usize barrel : m_barrels.within(at, shield.radius)) {
            strikeBarrel(barrel, shield.damage, m_players[shield.actor].actor.player());
        }
        for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
            if (m_safeRocks.rock(rock).obstacle.touchedBy(at, shield.radius, 0.0f)) {
                strikeSafeRock(rock, shield.damage);
            }
        }
    }
    settleBlasts();
    std::erase_if(m_shields, [this](const PotionShield& shield) {
        if (shield.secondsLeft > 0.0f) {
            return false;
        }
        m_effects.stop(shield.effect);
        return true;
    });
}

/** A guard that took enough off a hurt shows it: the block effect about the character, for
 * longer the more got through, and not again until that is over. */
void PlayScene::showBlock(usize index, f32 taken, f32 left) {
    if (index >= m_players.size() || m_players[index].blockLeft > 0.0f || taken <= kBlockWorth) {
        return;
    }
    const f32 shown = std::clamp(kBlockPerDamage * left, kBlockLeast, kBlockMost);
    m_players[index].blockLeft = shown;
    if (m_device != nullptr && m_weapons.loaded() &&
        m_weapons.trees.find(kBlockEffect).has_value()) {
        EffectTrees::Setting setting;
        setting.seconds = shown;
        m_effects.startSet(*m_device, m_weapons, kBlockEffect, m_players[index].actor.followPoint(),
                           setting);
    }
}

/** Runs a character's meter: a turbo attack is paid for as it first does harm, a shove runs
 * it down while it lasts, and otherwise it climbs while the character is free to act, the
 * narrator saying so when it comes full. */
void PlayScene::updateTurbo(usize index, s32 ticks, f32 seconds) {
    if (index >= m_players.size() || m_players[index].figure == nullptr) {
        return;
    }
    TurboMeter& meter = m_players[index].turbo;
    const PlayerAnimator& body = m_players[index].figure->animator();
    TurboMove& move = m_players[index].move;
    const ClassStats* stats = m_classes.stats(m_players[index].actor.save().character);
    if (body.turboBegan()) {
        if (const std::string_view voice = move.begin(body.action(), stats, meter);
            !voice.empty()) {
            cry(index, voice);
        }
    }
    move.advance(
        body.action(), body.player().frame(), m_players[index].actor.facing(), stats, meter,
        {.announce = [this, index](s32 help) { postHelp(help, index); },
         .dim = [this](f32 amount) { m_dimmer.ask(amount); },
         .volley = [this,
                    index](const Vec3& direction) { launchWeapon(index, direction, 1.0f, false); },
         .strike = [this, index](s32 row) { fireStrike(index, row); }});
    if (body.action() == PlayerAnimator::Action::Shove) {
        meter.drain(seconds);
    } else if (!isDown(index) && !body.turboing() && meter.fill(seconds)) {
        postHelp(HelpMessages::kUseTurbo, index);
    }
    meter.step(ticks);
}

/** One of a character's own cries, `which` being what follows its class in the name. */
void PlayScene::cry(usize index, std::string_view which) {
    PlayerFigure* body = index < m_players.size() ? m_players[index].figure.get() : nullptr;
    if (body == nullptr || m_context.sounds == nullptr) {
        return;
    }
    const std::string_view voice =
        classCode(m_players[index].actor.save().character % kStartingClassCount);
    if (const auto sound = body->voice().find(std::format("S_{}{}", voice, which));
        sound.has_value()) {
        m_context.sounds->play(body->voice().sequence(*sound), 1.0f, SoundCategory::Effects);
    }
}

/** Puts a help message up over a character, the narrator saying it, unless the party has
 * seen it. */

bool PlayScene::postHelp(s32 id, usize index, s32 number) {
    return m_hud.postHelp(id, index, m_players, m_audio, number);
}

void PlayScene::strikeSafeRock(usize index, f32 power) {
    if (m_safeRocks.strike(index, power) && m_device != nullptr && m_world != nullptr) {
        m_effects.start(*m_device, m_world->items(), "SAFEREXP", m_safeRocks.rock(index).position);
    }
}

/** A blow on a barrel: wood sounds under it until it breaks, when what it held is left
 * lying, or it blows up, or its gas hangs where it stood. */
void PlayScene::strikeBarrel(usize barrel, f32 power, s32 byPlayer) {
    const auto struck = m_barrels.strike(barrel, power);
    if (!struck.has_value()) {
        return;
    }
    if (!struck->broken) {
        m_audio.playNamed(kWoodHitSound);
        return;
    }
    const auto effect = [&](std::string_view tree) {
        if (m_device != nullptr && m_weapons.loaded()) {
            m_effects.start(*m_device, m_weapons, tree, struck->position);
        }
    };
    switch (struck->kind) {
    case BreakableStrike::Kind::Plain:
    case BreakableStrike::Kind::Holding:
        playRealmSound(kBarrelBreakSound);
        effect(kBarrelSmoke);
        if (struck->contents >= 0 && m_device != nullptr &&
            m_world->placeItemRecord(*m_device, struck->contents, struck->position,
                                     struck->count)) {
            for (usize i = 0; i < m_players.size(); ++i) {
                if (m_players[i].actor.player() == byPlayer) {
                    postHelp(HelpMessages::kBarrelsHold, i);
                }
            }
        }
        break;
    case BreakableStrike::Kind::Exploding:
        playRealmSound(kBarrelBlastSound);
        effect(kBarrelBlast);
        m_blasts.push_back(
            Blast{struck->position, kBlastRadius, kBarrelBlastDamage * trapDamageScale()});
        break;
    case BreakableStrike::Kind::Poison:
        playRealmSound(kBarrelGasSound);
        effect(kBarrelGas);
        m_clouds.push_back(GasCloud{struck->position, kGasDamage * trapDamageScale(), kGasSeconds});
        break;
    }
}

/** Whoever is within a blast is hurt by it, and the barrels within it are struck by it (so
 * one that blows up sets off its neighbours). */
void PlayScene::blast(const Vec3& position, f32 radius, f32 damage) {
    m_blasts.push_back(Blast{position, radius, damage});
    settleBlasts();
}

/** Feels out every blast waiting, and those they set off in turn. */
void PlayScene::settleBlasts() {
    while (!m_blasts.empty()) {
        const Blast felt = m_blasts.back();
        m_blasts.pop_back();
        for (usize i = 0; i < m_players.size(); ++i) {
            if (isDown(i)) {
                continue;
            }
            const PlayerActor& actor = m_players[i].actor;
            const Vec3 offset = actor.followPoint() - felt.position;
            if (std::hypot(offset.x, offset.z) <= felt.radius + actor.radius() &&
                std::abs(offset.y) <= actor.height() * 0.5f + felt.radius) {
                // A blast that gets through knocks its victim off their feet: onto their face
                // when it came from behind them, onto their back otherwise.
                const bool guarding =
                    m_players[i].figure != nullptr && m_players[i].figure->animator().guarding();
                if (guarded(i, felt.damage, true) > kKnockdownFrom && !guarding &&
                    !m_world->isTower()) {
                    const Vec3 push = actor.position() - felt.position;
                    f32 round = std::atan2(push.x, push.z) - actor.yaw();
                    round = std::remainder(round, 2.0f * kBehind * 2.0f);
                    m_players[i].reaction =
                        std::abs(round) > kBehind ? PlayerDeed::FallBack : PlayerDeed::FallForward;
                }
                hurt(i, felt.damage, HurtKind::Blow, true);
            }
        }
        for (const usize barrel : m_barrels.within(felt.position, felt.radius)) {
            strikeBarrel(barrel, felt.damage, -1);
        }
        for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
            if (m_safeRocks.rock(rock).obstacle.touchedBy(felt.position, felt.radius, 0.0f)) {
                strikeSafeRock(rock, felt.damage);
            }
        }
        for (const s32 enemy : m_enemies.within(felt.position, felt.radius)) {
            const Vec3 away = m_enemies.positionOf(enemy) - felt.position;
            strikeEnemy(enemy, felt.damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
        }
        for (const s32 generator : m_generators.within(felt.position, felt.radius)) {
            strikeGenerator(generator, felt.damage, -1);
        }
        if (m_bosses.within(felt.position, felt.radius)) {
            EnemyHit struck;
            struck.damage = felt.damage;
            struck.flags = EnemyHit::kKnockDown;
            if (const Vec3* at = m_bosses.position(); at != nullptr) {
                struck.direction = Vec3{at->x - felt.position.x, 0.0f, at->z - felt.position.z};
            }
            m_bosses.hurt(struck);
        }
        for (const s32 critter : m_critters.within(felt.position, felt.radius)) {
            const Vec3 away = m_critters.positionOf(critter) - felt.position;
            strikeCritter(critter, felt.damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z},
                          -1);
        }
    }
}

/** As the original's AddExp has it: a level gained posts "LEVEL n", plays the class's
 * fanfare about the character and heals a hundred; a tenth level besides changes the costume
 * and has the class say its piece (`S_EXP10WAR`, up to `S_EXP99`). */
void PlayScene::updateLevels() {
    for (usize i = 0; i < m_players.size(); ++i) {
        CharacterSave& save = m_players[i].actor.save();
        const auto change =
            m_levels.observe(m_players[i].actor.player(), experienceLevel(save.experience()));
        if (!change.has_value() || !change->gained()) {
            continue;
        }
        postHelp(HelpMessages::kLevelUp, i, change->to);
        save.progress().health += static_cast<s32>(kLevelUpHealth);
        if (m_device != nullptr && m_weapons.loaded()) {
            const std::string tree = std::format("LEVELUP_{}", colorCode(save.color));
            if (m_weapons.trees.find(tree).has_value()) {
                EffectTrees::Setting setting;
                setting.seconds = kLevelUpEffectSeconds;
                const u32 effect = m_effects.startSet(*m_device, m_weapons, tree,
                                                      m_players[i].actor.position(), setting);
                m_effects.moveTo(effect, m_players[i].actor.position());
            }
        }
        if (change->milestone() && m_device != nullptr) {
            const s32 tier = std::min(change->to / LevelChange::kLevelsPerTier, 9);
            const std::string_view cls = classCode(save.character);
            if (m_audio.playNamed(std::format("S_EXP{}0{}", tier, cls.substr(0, 3))) == kNoSound) {
                m_audio.playNamed("S_EXP99ALL");
            }
            // The costume of the new tier, weapon and all, where the character stands.
            if (auto figure = PlayerFigure::load(*m_device, m_context.unpackedRoot, save);
                figure != nullptr) {
                m_players[i].figure = std::move(figure);
            }
        }
    }
}

/** A hit on one of the great ones. */
void PlayScene::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                              std::optional<Vec3> where, bool close) {
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    hit.where = where;
    hit.close = close;
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_critters.hurt(id, hit);
}

/** Plays what one of the great ones (the boss with `ofBoss`) has set off: its tree from its
 * own archive (or the weapons', which holds the common marks of a hit) where it happened,
 * riding along with it when it follows, and its sound. */
void PlayScene::showCritterCue(const CritterCue& cue, ItemArchive* archive, bool ofBoss) {
    if (archive == nullptr || !archive->trees.find(cue.tree).has_value()) {
        archive =
            m_weapons.loaded() && m_weapons.trees.find(cue.tree).has_value() ? &m_weapons : nullptr;
    }
    if (!cue.tree.empty() && archive != nullptr && m_device != nullptr) {
        EffectTrees::Setting setting;
        setting.scale = cue.scale;
        setting.yaw = cue.yaw;
        setting.seconds = cue.life;
        if (const u32 effect =
                m_effects.startSet(*m_device, *archive, cue.tree, cue.position, setting);
            effect != 0 && cue.follows) {
            const Vec3* at = ofBoss ? m_bosses.position() : &m_critters.positionOf(cue.critter);
            m_critterEffects.push_back(
                CritterEffect{effect, cue.critter, ofBoss,
                              at != nullptr ? cue.position - *at : Vec3{0.0f, 0.0f, 0.0f}});
        }
    }
    if (!cue.sound.empty()) {
        m_audio.playNamed(cue.sound);
    }
}

/** Effects riding on the great ones go where they go, and are let go of when they end. */
void PlayScene::followCritterEffects() {
    for (usize i = 0; i < m_critterEffects.size();) {
        const CritterEffect& riding = m_critterEffects[i];
        const bool alive =
            riding.ofBoss ? m_bosses.present()
                          : m_critters.alive(riding.critter) || m_critters.dying(riding.critter);
        if (!m_effects.playing(riding.effect) || !alive) {
            m_critterEffects.erase(m_critterEffects.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        const Vec3* at =
            riding.ofBoss ? m_bosses.position() : &m_critters.positionOf(riding.critter);
        if (at != nullptr) {
            m_effects.moveTo(riding.effect, *at + riding.offset);
        }
        ++i;
    }
}

/** Translate scene-owned poses into the presentation's small, read-only snapshot. */
std::optional<LegendPresentation::Bearer> PlayScene::legendBearer(s32 player, s32 kind) const {
    for (usize i = 0; i < m_players.size(); ++i) {
        const PlayerActor& actor = m_players[i].actor;
        if (actor.player() != player) {
            continue;
        }
        const PlayerFigure* figure = i < m_players.size() ? m_players[i].figure.get() : nullptr;
        LegendPresentation::Bearer bearer;
        bearer.player = player;
        bearer.color = actor.save().color;
        bearer.position = actor.position();
        bearer.facing = actor.facing();
        bearer.holdPoint = actor.position() + Vec3{0.0f, LegendShow::kHeldLift, 0.0f};
        bearer.canGesture = figure != nullptr && !isDown(i);
        bearer.casting = figure != nullptr && figure->animator().castingLegend();
        bearer.released = figure != nullptr && figure->animator().legendReleased();
        if (LegendShow::heldInHand(kind) && figure != nullptr) {
            const f32 size =
                bodyScale(actor.save(), PowerupEffects::of(actor.save().progress().inventory));
            const Mat4 body = glm::scale(actor.transform(), Vec3{size, size, size});
            if (const auto hand = figure->handPosition(body); hand.has_value()) {
                bearer.holdPoint = *hand;
            }
        }
        return bearer;
    }
    return std::nullopt;
}

void PlayScene::showLegendEvent(const LegendEvent& event) {
    if (m_legend != nullptr) {
        const s32 kind = m_bosses.view().kind;
        m_legend->show(event.cue, event.player, event.realm, kind,
                       legendBearer(event.player, kind));
    }
}

void PlayScene::updateLegend(f32 seconds) {
    if (m_legend == nullptr) {
        return;
    }
    std::optional<LegendPresentation::Target> target;
    if (const Vec3* at = m_bosses.position(); at != nullptr) {
        target = LegendPresentation::Target{*at, m_bosses.height()};
    }
    const LegendPresentation::Update result =
        m_legend->update(seconds, legendBearer(m_legend->player(), m_legend->kind()), target);
    if (result.gesture != PlayerDeed::None) {
        for (PlayerRuntime& runtime : m_players) {
            if (runtime.actor.player() == m_legend->player()) {
                runtime.reaction = result.gesture;
            }
        }
    }
    if (result.landed) {
        m_bosses.landLegend();
    }
}

/** The boss has fallen: everyone in play gets its shard, its key rises where it fell, the
 * meter goes, and the wizard's visit is set going. */
void PlayScene::bossFallen(const Vec3& where) {
    const LevelInfo* level = m_world->level();
    if (level == nullptr || m_victory.state().running() || m_victory.state().finished()) {
        return;
    }
    const s32 order = LevelRef::orderOf(m_world->ref().realmId);
    u16 found = 0;
    for (PlayerRuntime& runtime : m_players) {
        PlayerActor& actor = runtime.actor;
        actor.save().progress().relics.addShard(order);
        found |= actor.save().progress().relics.runes;
    }
    // The realm's runestones are those its levels' records number, from one.
    u16 inRealm = 0;
    if (m_context.levels != nullptr) {
        for (const s32 rune : m_context.levels->runesOf(m_world->ref().realm)) {
            if (rune > 0 && rune <= Relics::kRuneCount) {
                inRealm |= static_cast<u16>(1U << static_cast<u32>(rune - 1));
            }
        }
    }
    const char letter = m_world->ref().name.empty() ? 'G' : m_world->ref().name.front();
    m_victory.begin(level->bossType, letter, inRealm, found, false);
    if (m_device != nullptr && m_world->items().loaded()) {
        EffectTrees::Setting setting;
        setting.seconds = kBossKeySeconds;
        setting.then = kBossKeyLaterTree;
        m_effects.startSet(*m_device, m_world->items(), kBossKeyTree, where, setting);
        std::vector<Vec3> standing;
        for (usize i = 0; i < m_players.size(); ++i) {
            if (!isDown(i)) {
                standing.push_back(m_players[i].actor.position());
            }
        }
        const Vec3* boss = m_bosses.position();
        m_victory.bindWizard(*m_device, m_world->items(), boss != nullptr ? *boss : Vec3{0.0f},
                             standing);
    }
    m_audio.playNamed(std::format("{}{}", kBossKeySoundPrefix, letter));
}

/** The wizard's visit runs on: he fades in, says his piece (typed out under the view, his
 * lines from the level's bank), then the party sparkles and is taken to the tower. */
void PlayScene::updateVictory(s32 ticks, f32 seconds) {
    if (!m_victory.state().running()) {
        return;
    }
    const auto result = m_victory.update(ticks, seconds, m_world->goldLeft(), m_hud.strings());
    for (const VictoryVoice& voice : result.voices) {
        m_audio.playNamed(voice.sound);
    }
    if (result.sparkle) {
        if (m_device != nullptr && m_weapons.loaded()) {
            for (usize i = 0; i < m_players.size(); ++i) {
                if (!isDown(i)) {
                    m_effects.start(*m_device, m_weapons, kSpawnEffect,
                                    m_players[i].actor.position());
                }
            }
        }
    }
    if (m_victory.state().finished() && !m_leaving) {
        m_destination = LevelRef::tower();
        m_leaving = true;
        m_transition.comeUp();
    }
}

/** As the death throws them out, the boss's coins for the party fly from where it stands
 * and its blast takes the rest of the level's enemies with it. */
void PlayScene::spewBossCoins(const CritterSpew& spew) {
    for (const s32 enemy : m_enemies.within(spew.origin, kBossDeathBlastRadius)) {
        const Vec3 away = m_enemies.positionOf(enemy) - spew.origin;
        strikeEnemy(enemy, kBossDeathBlast, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
    }
    for (const s32 generator : m_generators.within(spew.origin, kBossDeathBlastRadius)) {
        strikeGenerator(generator, kBossDeathBlast, -1);
    }
    if (m_device == nullptr) {
        return;
    }
    const auto players = static_cast<s32>(m_players.size());
    for (const SpewedCoin& coin : BossCoins::spray(m_world->ref().realmId, players, spew.velocity,
                                                   spew.halfAngle, m_coinRandom)) {
        m_world->throwItem(*m_device, coin.name, spew.origin, coin.velocity,
                           BossCoins::kNoGrabSeconds);
    }
}

/** The boss's worth goes the great ones' way: shares to the hitter, a kill's to everyone. */
void PlayScene::awardBossLosses() {
    for (const CritterSpew& spew : m_bosses.takeSpews()) {
        spewBossCoins(spew);
    }
    for (const CritterLoss& loss : m_bosses.takeLosses()) {
        if (loss.killed) {
            bossFallen(loss.position);
        }
        for (const PlayerRuntime& runtime : m_players) {
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
                awardExperience(player, whole, loss.killed);
            }
        }
    }
}

/** What the great ones are worth: a share to whoever hurt one, whole points as they add
 * up, and a kill's share to everyone. */
void PlayScene::awardCritterLosses() {
    for (const CritterLoss& loss : m_critters.takeLosses()) {
        // A gargoyle slain leaves the key its form is named by where it fell.
        if (loss.killed && loss.kind == kGargoyleCritter && !loss.form.empty() &&
            m_device != nullptr) {
            m_world->placeItem(*m_device, "GARG" + loss.form, loss.position);
        }
        for (const PlayerRuntime& runtime : m_players) {
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
                awardExperience(player, whole, loss.killed);
            }
        }
    }
}

/** The level's swarm and the generators that breed it, at the level's scales: what they can
 * take and deal is the level's own, how fast they go and see and how a generator breeds grow
 * with the difficulty setting. Placements of ordinary strength stand where the level puts
 * them, asleep when of no strength; the archer, bomber and suicide variants wait for their
 * missiles. */
void PlayScene::bindEnemies(RenderDevice& device, LevelWorld& world, const GameContext& context) {
    const LevelInfo* level = world.level();
    const f32 gain = context.config != nullptr ? context.config->difficulty.gain() : 1.0f;
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
    m_enemies.open(device, context.unpackedRoot, &world.collision(), most, scales, seed);
    const std::string& levelName = world.ref().name;
    m_critters.open(device, context.unpackedRoot, &world.collision(), scales,
                    levelName.empty() ? 'G' : levelName.front());
    m_bosses.open(device, context.unpackedRoot, &world.collision(), scales,
                  levelName.empty() ? 'G' : levelName.front());
    m_critterExperienceOwed.fill(0.0f);
    const auto players = static_cast<s32>(m_players.size());
    const std::span<const LevelEnemy> roster = level != nullptr
                                                   ? std::span<const LevelEnemy>(level->enemies)
                                                   : std::span<const LevelEnemy>{};
    m_generators.bind(device, world.layout(), m_enemies, &world.collision(), breeding, players,
                      roster);
    // The level's boss, at its boss mark.
    if (level != nullptr && !bossNameOf(level->bossType).empty()) {
        if (const WorldLocator* mark = world.layout().findLocator(LocatorKind::Boss);
            mark != nullptr) {
            m_bosses.spawn(level->bossType, mark->position, mark->rotation.y);
            if (const CritterMeter* meter = m_bosses.meter(); meter != nullptr) {
                ItemArchive* archive = m_bosses.archive();
                m_bossMeter.bind(*meter, archive != nullptr ? &archive->textures : nullptr);
                const BossView boss = m_bosses.view();
                m_bossMeter.update(0, boss.health, boss.maxHealth, boss.alive, false);
            }
            // The first of the party carrying its legend item brings it to the fight.
            for (const PlayerRuntime& runtime : m_players) {
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
        if (info.type != ItemInfo::kPlacedEnemy || !shownToParty(instance.minPlayers, players)) {
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
            m_critters.spawn(kGolemCritter, instance.position, facing);
            continue;
        }
        if (*kind == kGeneralEnemyKind) {
            m_critters.spawn(kGeneralCritter, instance.position, facing);
            continue;
        }
        if (*kind == kGargoyleEnemyKind) {
            m_critters.spawn(kGargoyleCritter, instance.position, facing);
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
std::vector<EnemyView> PlayScene::enemyViews() const {
    std::vector<EnemyView> views;
    views.reserve(m_players.size());
    for (usize i = 0; i < m_players.size(); ++i) {
        const PlayerActor& actor = m_players[i].actor;
        EnemyView view;
        view.player = actor.player();
        view.position = actor.position();
        view.radius = actor.radius();
        view.height = actor.height();
        view.level = experienceLevel(actor.save().experience());
        view.hidden = isDown(i);
        views.push_back(view);
    }
    return views;
}

/** The generators breed, the swarm goes about its business, and what it lands on the party
 * is taken: a power blow from a tall one is a knock that makes its victim flinch. What the
 * party has done to it is paid in experience. */
void PlayScene::updateEnemies(s32 ticks, f32 seconds) {
    const std::vector<EnemyView> views = enemyViews();
    std::vector<Obstacle> boxes = m_generators.obstacles();
    const std::vector<Obstacle> chests = m_chests.obstacles();
    boxes.insert(boxes.end(), chests.begin(), chests.end());
    const std::vector<Obstacle> barred = m_gates.obstacles();
    boxes.insert(boxes.end(), barred.begin(), barred.end());
    const std::vector<Obstacle> casks = m_barrels.obstacles();
    boxes.insert(boxes.end(), casks.begin(), casks.end());
    const LevelInfo* level = m_world->level();
    const f32 missileSpeed = level != nullptr ? level->tuning.enemyMissileSpeed : 1.0f;
    const auto cover = m_safeRocks.obstacles();
    boxes.insert(boxes.end(), cover.begin(), cover.end());
    m_generators.update(ticks, m_enemies, views, boxes);
    m_enemies.update(ticks, seconds, views, boxes, &m_enemyMissiles, missileSpeed);
    m_enemyMissiles.update(seconds, &m_world->collision(), views);
    // What the throwers let fly lands on the party, or bursts where it fell; what blows
    // itself up blasts everything about it.
    for (const EnemyMissileHit& hit : m_enemyMissiles.takeHits()) {
        for (usize i = 0; i < m_players.size(); ++i) {
            if (hit.player >= 0 && m_players[i].actor.player() == hit.player && !isDown(i)) {
                const bool guarding =
                    m_players[i].figure != nullptr && m_players[i].figure->animator().guarding();
                if ((hit.flags & EnemyMissileKind::kKnockBack) != 0 && !guarding &&
                    m_players[i].reaction == PlayerDeed::None) {
                    m_players[i].reaction = PlayerDeed::Flinch;
                }
                hurt(i, hit.damage, HurtKind::Pierce, true);
            }
        }
        if (hit.burstRadius > 0.0f) {
            blast(hit.position, hit.burstRadius, hit.damage);
        }
    }
    for (const EnemyBurst& burst : m_enemies.takeBursts()) {
        blast(burst.position, kBlastRadius, burst.damage);
    }
    settleBlasts();
    m_critters.update(ticks, seconds, views);
    m_bosses.update(ticks, seconds, views);
    if (m_bossMeter.bound()) {
        const BossView boss = m_bosses.view();
        m_bossMeter.update(ticks, boss.health, boss.maxHealth, m_bosses.present() && boss.alive,
                           m_bosses.frozen());
    }
    // The legend item held up is the bearer's no more; the rite is shown as it goes.
    for (const LegendEvent& event : m_bosses.takeLegendEvents()) {
        if (event.cue == LegendCue::Brandished) {
            for (PlayerRuntime& runtime : m_players) {
                PlayerActor& actor = runtime.actor;
                if (actor.player() == event.player) {
                    actor.save().progress().relics.spendLegend(event.realm);
                }
            }
        }
        showLegendEvent(event);
    }
    updateLegend(seconds);
    for (const CritterBlow& blow : m_bosses.takeBlows()) {
        for (usize i = 0; i < m_players.size(); ++i) {
            if (m_players[i].actor.player() == blow.player && !isDown(i)) {
                hurt(i, blow.damage, HurtKind::Blow, true);
            }
        }
    }
    awardBossLosses();
    updateVictory(ticks, seconds);
    // The great ones' effects and sounds: a move's, a strike's, a hit's.
    for (const CritterCue& cue : m_bosses.takeCues()) {
        showCritterCue(cue, m_bosses.archive(), true);
    }
    for (const CritterCue& cue : m_critters.takeCues()) {
        showCritterCue(cue, m_critters.archiveOf(cue.critter), false);
    }
    followCritterEffects();
    for (const CritterBlow& blow : m_critters.takeBlows()) {
        for (usize i = 0; i < m_players.size(); ++i) {
            if (m_players[i].actor.player() == blow.player && !isDown(i)) {
                hurt(i, blow.damage, HurtKind::Blow, true);
            }
        }
    }
    awardCritterLosses();
    updateLevels();
    for (const EnemyBlow& blow : m_enemies.takeBlows()) {
        for (usize i = 0; i < m_players.size(); ++i) {
            if (m_players[i].actor.player() != blow.player || isDown(i)) {
                continue;
            }
            const bool guarding =
                m_players[i].figure != nullptr && m_players[i].figure->animator().guarding();
            if (blow.knocksDown && !guarding && m_players[i].reaction == PlayerDeed::None) {
                m_players[i].reaction = PlayerDeed::Flinch;
            }
            hurt(i, blow.damage, HurtKind::Blow, true);
        }
    }
    for (const EnemyLoss& loss : m_enemies.takeLosses()) {
        awardExperience(loss.player, loss.experience, loss.killed);
    }
}

/** A hit on one of the swarm, from a player or the world. */
void PlayScene::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer) {
    EnemyHit hit;
    hit.damage = power;
    hit.flags = flags;
    hit.direction = direction;
    hit.player = byPlayer;
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == byPlayer) {
            hit.level = experienceLevel(actor.save().experience());
        }
    }
    m_enemies.hurt(id, hit);
}

/** A hit on a generator: as it crumbles a state its kind's hit or death effect plays over
 * it to the realm's own sound (`S_GENDAMG`, `S_GENKILLG`), and, gone, its brood is freed of
 * it. */
void PlayScene::strikeGenerator(s32 id, f32 power, s32 byPlayer) {
    const auto event = m_generators.strike(id, power, byPlayer);
    if (!event.has_value()) {
        return;
    }
    if (ItemArchive* archive = m_enemies.archive(event->kind);
        archive != nullptr && m_device != nullptr) {
        const std::string_view tree = event->destroyed ? "GENDIE" : "GENHIT";
        if (archive->trees.find(tree).has_value()) {
            m_effects.start(*m_device, *archive, tree, event->position);
        }
    }
    playRealmSound(event->destroyed ? "S_GENKILL" : "S_GENDAM");
    if (event->destroyed) {
        m_enemies.generatorGone(id);
    }
}

/** Gas hangs for a while and hurts whoever stands in it, every half second. */
void PlayScene::updateClouds(f32 seconds) {
    for (PlayerRuntime& runtime : m_players) {
        runtime.cloudGap = std::max(runtime.cloudGap - seconds, 0.0f);
    }
    for (GasCloud& cloud : m_clouds) {
        cloud.secondsLeft -= seconds;
        for (usize i = 0; i < m_players.size(); ++i) {
            if (isDown(i) || m_players[i].cloudGap > 0.0f) {
                continue;
            }
            const Vec3 offset = m_players[i].actor.followPoint() - cloud.position;
            if (std::hypot(offset.x, offset.z) <= kGasRadius + m_players[i].actor.radius() &&
                std::abs(offset.y) <= m_players[i].actor.height() * 0.5f + kGasRadius) {
                m_players[i].cloudGap = kGasGapSeconds;
                hurt(i, cloud.damage, HurtKind::Gas, true);
            }
        }
    }
    std::erase_if(m_clouds, [](const GasCloud& cloud) { return cloud.secondsLeft <= 0.0f; });
}

/** A gate's opening sounds from the realm's own bank, named after the level's letter. */
void PlayScene::playGateSound(s32 /*subtype*/) {
    const std::string& level = m_world->ref().name;
    const char letter = level.empty() ? 'G' : level.front();
    for (const std::string_view stem : {"S_GATEMET", "S_GATEWOOD", "S_GATE"}) {
        for (const std::string_view tail : {"", "1"}) {
            if (m_audio.playNamed(std::format("{}{}{}", stem, letter, tail)) != kNoSound) {
                return;
            }
        }
    }
}

/** Takes health from a character the way the original does: harm over a point is scaled by
 * the level's damage, the cry depends on what did it, and with under a point of health left
 * the character dies, to the dying sound and its own last cry. Nobody is hurt in the tower,
 * nor once they have fallen. */
void PlayScene::hurt(usize index, f32 damage, HurtKind kind, bool directed) {
    if (index >= m_players.size()) {
        return;
    }
    const LevelInfo* level = m_world->level();
    m_health.hurt(m_players[index], damage, kind, directed, m_world->isTower(),
                  level != nullptr ? level->tuning.damage : 1.0f,
                  {.block = [this, index](f32 taken, f32 left) { showBlock(index, taken, left); },
                   .sound = [this](std::string_view sound) { m_audio.playNamed(sound); },
                   .cry = [this, index](std::string_view voice) { cry(index, voice); },
                   .named = [this, index](std::string_view line) { sayWithName(index, line); }});
}

/** The narrator names the character ("Red Warrior", from the class's own bank) and says
 * `line` after: what the original's announcements by name do. */
void PlayScene::sayWithName(usize index, std::string_view line) {
    PlayerFigure* body = index < m_players.size() ? m_players[index].figure.get() : nullptr;
    if (body == nullptr || m_context.sounds == nullptr) {
        return;
    }
    const CharacterSave& save = m_players[index].actor.save();
    const std::string name = std::format("S_{}{}2", colorCode(save.color),
                                         classCode(save.character % kStartingClassCount));
    const SoundHandle spoken = m_audio.playFrom(body->voice(), name);
    m_audio.narrate(line, LevelSoundscape::Narrator::Either, spoken);
}

/** The whole party has gone through a portal: where to? Its own level when that is unpacked;
 * from a realm's level whose next is not, back to the tower, so that no one is stranded;
 * from the tower, nowhere, with a word in the log. */
bool PlayScene::leaveBy(usize portal) {
    const ExitPortals::Portal& exit = m_portals.portal(portal);
    const bool reachable = exit.destination.has_value() &&
                           LevelCatalog::unpacked(m_context.unpackedRoot, *exit.destination);
    if (reachable) {
        m_destination = *exit.destination;
        log::info("Portal {}: on to {} ({})", exit.tag, m_destination.name, m_destination.title);
        return true;
    }
    if (!m_world->isTower()) {
        m_destination = LevelRef::tower();
        log::info("Portal {}: its level is not unpacked; back to the tower", exit.tag);
        return true;
    }
    if (m_refusedPortal != static_cast<s32>(portal)) {
        m_refusedPortal = static_cast<s32>(portal);
        log::warn("Portal {}: its level is not unpacked; unpack it with gdlunpack --only "
                  "<level> (and its realm's items)",
                  exit.tag);
    }
    return false;
}

std::vector<PartyMember> PlayScene::party() const {
    std::vector<PartyMember> members;
    members.reserve(m_players.size());
    // The fallen go on as they came into the level, less what it gave them (but what they
    // were taught stays taught).
    for (usize i = 0; i < m_players.size(); ++i) {
        const PlayerRuntime& runtime = m_players[i];
        const bool down = isDown(i);
        PartyMember member{runtime.actor.player(), down ? runtime.entrySave : runtime.actor.save(),
                           runtime.slot, down};
        member.save.helpSeen = runtime.actor.save().helpSeen;
        member.helpHeard = runtime.helpHeard;
        members.push_back(std::move(member));
    }
    return members;
}

f32 PlayScene::bodyScale(const CharacterSave& save, const PowerupEffects& effects) {
    if (save.character == kOgre) {
        return kOgreScale;
    }
    if (effects.grown()) {
        return PowerupEffects::kGrowthScale;
    }
    return experienceLevel(save.experience()) >= kMaxLevel ? kMasterScale : 1.0f;
}

/** Hands a touched item to whoever touched it, by the original's rules: their card slides
 * up, the item's sound (or their own eating) plays, and what they cannot carry stays lying
 * where it is. Crystals are the party's and are dealt with once taken. */
std::optional<s32> PlayScene::takePickup(const Pickup& pickup) {
    if (pickup.realm > 0) {
        return 0;
    }
    if (pickup.collector >= m_players.size()) {
        return std::nullopt;
    }
    PlayerActor& actor = m_players[pickup.collector].actor;
    const ClassStats* stats = m_classes.stats(actor.save().character);
    const ItemTaking taking = takeItem(
        actor.save(), ItemOffer{pickup.subtype, pickup.amount, pickup.flags, pickup.strength},
        stats != nullptr ? stats->powerupTime : 1.0f);
    if (!taking.took()) {
        if (taking.outcome == ItemTaking::Outcome::KeysFull) {
            postHelp(HelpMessages::kKeysFull, pickup.collector);
        } else if (taking.outcome == ItemTaking::Outcome::HealthFull) {
            postHelp(HelpMessages::kHealthFull, pickup.collector);
        } else if (taking.outcome == ItemTaking::Outcome::AlreadyHeld) {
            postHelp(HelpMessages::kAlreadyHaveRune, pickup.collector);
        }
        return std::nullopt;
    }
    if (pickup.subtype == kSpecialPowerup && (static_cast<u32>(pickup.flags) & kTurboFlag) != 0) {
        m_players[pickup.collector].turbo.add(TurboMeter::kFull);
    }
    switch (static_cast<ItemKind>(pickup.subtype)) {
    case ItemKind::Runestone: shareRune(pickup.amount); break;
    case ItemKind::Legend:
        postHelp(HelpMessages::kFirstLegendName + taking.count, pickup.collector);
        break;
    case ItemKind::Scroll:
        if (const LevelInfo* level = m_world->level(); level != nullptr && taking.count >= 0) {
            openMessage(std::format("{}{}", kLevelScrollPrefix, level->name),
                        static_cast<usize>(taking.count));
        }
        break;
    default: break;
    }
    if (!taking.card.empty()) {
        m_hud.pickups().addCard(actor.player(), taking.card);
    }
    if (!taking.sound.empty()) {
        m_audio.playNamed(taking.sound);
    } else if (PlayerFigure* figure = m_players[pickup.collector].figure.get();
               figure != nullptr && m_context.sounds != nullptr) {
        const std::string_view voice = classCode(actor.save().character % kStartingClassCount);
        const auto sound =
            figure->voice().find(std::format("S_{}{}", voice, taking.hurt ? "PAIN1" : "EATSFX"));
        if (sound.has_value()) {
            m_context.sounds->play(figure->voice().sequence(*sound), 1.0f, SoundCategory::Effects);
        }
    }
    return taking.left;
}

/** A runestone found is everyone's: each character in play gets it, and the narrator counts
 * what the party holds. */
void PlayScene::shareRune(s32 rune) {
    u16 held = 0;
    for (PlayerRuntime& runtime : m_players) {
        PlayerActor& actor = runtime.actor;
        Relics& relics = actor.save().progress().relics;
        relics.addRune(rune);
        held |= relics.runes;
    }
    const s32 count = std::popcount(held);
    if (count <= 0) {
        return;
    }
    const std::string voice =
        count == 1 ? std::string(kFirstRuneVoice) : std::format("{}{}", kRuneVoicePrefix, count);
    m_audio.narrate(voice, LevelSoundscape::Narrator::Primary);
}

/** The first of the party standing in the spot before Sumner, or null. */
const PlayerActor* PlayScene::visitorOfSumner() const {
    const LevelTriggers& triggers = m_world->triggers();
    for (usize i = 0; i < triggers.size(); ++i) {
        const LevelTrigger& spot = triggers.trigger(i);
        if (spot.id != kSumnerSpot) {
            continue;
        }
        for (const PlayerRuntime& runtime : m_players) {
            const PlayerActor& actor = runtime.actor;
            const Vec3 away = actor.position() - spot.spot;
            const f32 reach = spot.radius + actor.radius();
            if (away.x * away.x + away.z * away.z <= reach * reach &&
                std::abs(away.y) <= LevelTriggers::kReach) {
                return &actor;
            }
        }
    }
    return nullptr;
}

void PlayScene::updateSumnerVisit(f32 seconds) {
    const PlayerActor* visitor = visitorOfSumner();
    const std::optional<s32> player =
        visitor != nullptr ? std::optional<s32>{visitor->player()} : std::nullopt;
    if (m_sumnerVisit.visit(seconds, player, m_sumner.loaded(), m_messages.text(), m_context.config,
                            m_context.strings)) {
        m_sumner.play(SumnerFigure::kWelcomeIndex);
    }
}

/** The scene routes the scroll owner's input and applies its sound/gesture cues. */
void PlayScene::updateHints(const Inputs& inputs, s32 ticks) {
    const auto player = static_cast<usize>(std::max(m_sumnerVisit.owner(), 0));
    const MenuInput input = player < inputs.size() ? inputs[player].menu : MenuInput{};
    const HintMenuEvent event = m_sumnerVisit.update(*m_device, input, ticks);
    switch (event.kind) {
    case HintMenuEvent::Kind::Moved: m_audio.playNamed(kMenuMoveSound); break;
    case HintMenuEvent::Kind::Asked: {
        m_audio.playNamed(kMenuSelectSound);
        std::vector<ClassProgress> party;
        party.reserve(m_players.size());
        for (const PlayerRuntime& runtime : m_players) {
            party.push_back(runtime.actor.save().progress());
        }
        m_sumnerVisit.answer(event.topic, m_messages.text(), m_context.strings,
                             HintKnowledge::ofParty(party));
        break;
    }
    case HintMenuEvent::Kind::Returned: m_audio.playNamed(kMenuExitSound); break;
    case HintMenuEvent::Kind::Left:
        m_audio.playNamed(m_sumnerVisit.menu().burning() ? kScrollBurnSound : kMenuExitSound);
        m_sumner.play(SumnerFigure::kGoAwayIndex);
        break;
    case HintMenuEvent::Kind::None: break;
    }
}

/** A party is new to the tower while no class of any of its characters has experience. */
bool PlayScene::freshParty(std::span<const PartyMember> party) {
    return !party.empty() && std::ranges::all_of(party, [](const PartyMember& member) {
        return std::ranges::none_of(member.save.classes, [](const ClassProgress& progress) {
            return progress.experience > 0;
        });
    });
}

/** Opens Sumner's welcome scroll; without it the welcome goes straight to the crystals. */
void PlayScene::beginIntro(RenderDevice& device) {
    if (m_messages.open(device, kWelcomeMessage, m_context.strings)) {
        m_intro = Intro::Scroll;
        return;
    }
    log::warn("Tower: no welcome scroll to show; on to the crystals");
    startCrystalCut();
}

/** Sumner gestures at the crystals while the camera cuts to them from the level's marker. */
void PlayScene::startCrystalCut() {
    m_sumner.gesture();
    const WorldLocator* marker =
        m_world->layout().findLocator(LocatorKind::TriggerCamera, kCrystalCamera);
    if (marker == nullptr) {
        log::warn("Tower: no crystal camera marker {}", kCrystalCamera);
        m_intro = Intro::Done;
        return;
    }
    m_cutCamera = WorldCamera{};
    m_cutCamera.position = marker->position;
    m_cutCamera.pitch = marker->rotation.x;
    m_cutCamera.yaw = marker->rotation.y;
    m_cutTicks = kCrystalTicks;
    m_intro = Intro::Crystal;
}

/** A bit per party member whose player pressed their button this frame. */
u32 PlayScene::acceptedPlayers(const Inputs& inputs) const {
    u32 accepted = 0;
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        const auto player = static_cast<usize>(actor.player());
        if (player < inputs.size() && inputs[player].menu.select) {
            accepted |= 1U << player;
        }
    }
    return accepted;
}

const WorldCamera& PlayScene::viewCamera() const {
    if (m_arrival.camera().active()) {
        return m_arrival.camera().camera();
    }
    if (m_intro == Intro::Crystal) {
        return m_cutCamera;
    }
    return bossCameraOn() ? m_bossCamera.camera() : m_camera.camera();
}

/** A boss level with a boss camera record frames the fight with it while the boss stands. */
bool PlayScene::bossCameraOn() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    return level != nullptr && level->bossCamera.has_value() &&
           (m_bosses.present() || m_victory.state().running());
}

/** The boss as the camera sees it; once it has fallen, the wizard in its place. */
BossCameraSubject PlayScene::bossSubject() const {
    BossCameraSubject subject;
    if (m_victory.state().running() && !m_bosses.present()) {
        return m_victory.wizardSubject();
    }
    if (const Vec3* at = m_bosses.position(); at != nullptr) {
        subject.position = *at;
    }
    subject.facing = m_bosses.facing();
    subject.radius = m_bosses.radius();
    subject.height = m_bosses.height();
    subject.attentionOffset = m_bosses.cameraOffset();
    subject.awake = m_bosses.view().awake;
    return subject;
}

/** Whether any party member's player pressed a button this frame. */
bool PlayScene::anyButton(const Inputs& inputs) const {
    return std::ranges::any_of(m_players, [&inputs](const PlayerRuntime& runtime) {
        const PlayerActor& actor = runtime.actor;
        const auto player = static_cast<usize>(actor.player());
        return player < inputs.size() && (inputs[player].menu.select || inputs[player].menu.back ||
                                          inputs[player].menu.start);
    });
}

PlayOutcome PlayScene::update(f64 deltaSeconds, const Inputs& inputs) {
    if (!m_open) {
        return PlayOutcome::Running;
    }
    // The clock advances in whole ticks, two per frame at the 30 frames per second the game
    // runs at, so a late frame moves everything further rather than smoother.
    const f32 tickRate =
        m_context.config != nullptr ? static_cast<f32>(m_context.config->timing.tickRate) : 60.0f;
    const auto ticks =
        std::clamp(static_cast<s32>(std::lround(deltaSeconds * tickRate)), kMinTicks, kMaxTicks);
    const f32 seconds = static_cast<f32>(ticks) / tickRate;
    // A scroll holds everything else still until it has burnt away; the welcome's leads on
    // to the crystals. Leaving one burns it to the options menu's note and cuts off whatever
    // Sumner was saying over it.
    if (m_messages.active()) {
        const LevelMessages::Cues cues = m_messages.step(ticks, acceptedPlayers(inputs));
        if (cues.stopVoice) {
            m_audio.stopVoice();
        }
        if (cues.burnSound) {
            m_audio.playNamed(kScrollBurnSound);
        }
        if (!m_messages.active() && m_intro == Intro::Scroll) {
            startCrystalCut();
        }
        return PlayOutcome::Running;
    }
    // Gone through a portal, the party is out of play while the transition picture comes
    // up over the level, which goes on around it; once it covers the view they travel.
    m_transition.update(seconds);
    if (m_leaving) {
        m_world->update(seconds);
        m_effects.update(seconds);
        updateAmbience();
        return m_transition.covering() ? PlayOutcome::Travel : PlayOutcome::Running;
    }
    // Sumner's scroll of hints holds play the same way, while he goes on moving behind it.
    if (m_sumnerVisit.active()) {
        updateHints(inputs, ticks);
        m_sumner.update(seconds);
        return PlayOutcome::Running;
    }
    // Materialising, the party stands still, playing its entrance, while the level runs on
    // around it and the start camera holds, then rides in; the title slides up until the
    // ride, when it sits.
    if (spawning()) {
        m_arrival.animate(seconds);
        for (const PlayerRuntime& runtime : m_players) {
            const std::unique_ptr<PlayerFigure>& figure = runtime.figure;
            if (figure != nullptr) {
                figure->animate(0.0f, ticks, seconds);
            }
        }
        m_world->update(seconds);
        updateAmbience();
        updateBeam(ticks);
        m_arrival.advance(ticks, anyButton(inputs), m_camera.camera().position,
                          m_camera.attention());
        if (!spawning() && m_welcomePending) {
            m_welcomePending = false;
            beginIntro(*m_device);
        }
        return PlayOutcome::Running;
    }
    const bool held = m_intro == Intro::Crystal;
    if (held) {
        m_cutTicks -= ticks;
        if (m_cutTicks <= 0) {
            m_intro = Intro::Done;
        }
    }
    m_world->update(seconds);
    m_world->revealCrystals(seconds);
    m_hud.pickups().step(ticks, seconds);
    m_sumner.update(seconds);
    const PartyMotion::Events movementEvents{
        .perform =
            [this](usize i, PartyMotion::Action action) {
                switch (action) {
                case PartyMotion::Action::NoPotion: postHelp(HelpMessages::kNoPotion, i); break;
                case PartyMotion::Action::Ram: ramBarrels(i); break;
                case PartyMotion::Action::ThrowWeapon: throwWeapon(m_players[i].actor); break;
                case PartyMotion::Action::StrongThrow:
                    launchWeapon(i, m_players[i].actor.facing(), kStrongThrowScale, true);
                    break;
                case PartyMotion::Action::ShieldPotion: shieldPotion(i); break;
                case PartyMotion::Action::UsePotion: m_arsenal.usePotion(m_players[i].actor); break;
                case PartyMotion::Action::ThrowPotion:
                    m_arsenal.throwPotion(m_players[i].actor);
                    break;
                case PartyMotion::Action::FirstFoot: m_audio.playFootstep(false); break;
                case PartyMotion::Action::SecondFoot: m_audio.playFootstep(true); break;
                }
            },
        .select =
            [this](usize i, const SelectorInput& input, s32 elapsed) {
                m_hud.stepSelector(m_players[i].actor, input, elapsed, m_audio);
            },
        .advanceTurbo = [this](usize i, s32 elapsed,
                               f32 duration) { updateTurbo(i, elapsed, duration); }};
    const std::vector<CameraSubject> subjects =
        PartyMotion::step(m_players, inputs, held, m_camera.yaw(), ticks, seconds,
                          m_world->collision(), movementEvents);
    m_playSeconds += seconds;
    m_hud.help().update(ticks);
    updateFixtures(ticks, seconds);
    updateEnemies(ticks, seconds);
    std::vector<MissileTarget> targets;
    for (usize barrel = 0; barrel < m_barrels.size(); ++barrel) {
        if (m_barrels.standing(barrel)) {
            const Breakables::Barrel& cask = m_barrels.barrel(barrel);
            targets.push_back(MissileTarget{static_cast<s32>(barrel), cask.figure.position(),
                                            cask.radius, cask.height});
        }
    }
    for (MissileTarget target : m_enemies.targets()) {
        target.id += kEnemyTargetBase;
        targets.push_back(target);
    }
    for (MissileTarget target : m_critters.targets()) {
        target.id += kCritterTargetBase;
        targets.push_back(target);
    }
    for (MissileTarget target : m_bosses.targets()) {
        target.id += kBossTargetBase;
        targets.push_back(target);
    }
    for (usize g = 0; g < m_generators.count(); ++g) {
        if (m_generators.standing(static_cast<s32>(g))) {
            const Obstacle& box = m_generators.boxOf(static_cast<s32>(g));
            targets.push_back(MissileTarget{static_cast<s32>(g) + kGeneratorTargetBase,
                                            m_generators.positionOf(static_cast<s32>(g)),
                                            std::max(box.halfAcross, box.halfAlong), box.height});
        }
    }
    for (usize rock = 0; rock < m_safeRocks.size(); ++rock) {
        if (m_safeRocks.standing(rock)) {
            const Obstacle& cover = m_safeRocks.rock(rock).obstacle;
            targets.push_back(MissileTarget{static_cast<s32>(rock) + kSafeRockTargetBase,
                                            cover.centre, cover.cylinderRadius, cover.height});
        }
    }
    m_arsenal.missiles().update(seconds, &m_world->collision(), targets);
    for (const MissileImpact& impact : m_arsenal.missiles().takeImpacts()) {
        if (impact.potion != 0) {
            m_arsenal.burstPotion(impact.potion, impact.position,
                                  impact.potency); // weapons leave no mark yet
        }
        if (impact.target >= kSafeRockTargetBase) {
            strikeSafeRock(static_cast<usize>(impact.target - kSafeRockTargetBase), impact.damage);
        } else if (impact.target >= kBossTargetBase) {
            EnemyHit hit;
            hit.damage = impact.damage;
            hit.player = impact.owner;
            hit.where = impact.position;
            for (const PlayerRuntime& runtime : m_players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    hit.direction = impact.position - actor.position();
                    hit.direction.y = 0.0f;
                    hit.level = experienceLevel(actor.save().experience());
                }
            }
            m_bosses.hurt(hit);
        } else if (impact.target >= kCritterTargetBase) {
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : m_players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            strikeCritter(impact.target - kCritterTargetBase, impact.damage, 0, direction,
                          impact.owner, impact.position);
        } else if (impact.target >= kGeneratorTargetBase) {
            strikeGenerator(impact.target - kGeneratorTargetBase, impact.damage, impact.owner);
        } else if (impact.target >= kEnemyTargetBase) {
            // The hit travels the way the weapon flew: out from whoever threw it.
            Vec3 direction{0.0f, 0.0f, 1.0f};
            for (const PlayerRuntime& runtime : m_players) {
                const PlayerActor& actor = runtime.actor;
                if (actor.player() == impact.owner) {
                    direction = impact.position - actor.position();
                    direction.y = 0.0f;
                }
            }
            strikeEnemy(impact.target - kEnemyTargetBase, impact.damage, 0, direction,
                        impact.owner);
        } else if (impact.target >= 0) {
            strikeBarrel(static_cast<usize>(impact.target), impact.damage, impact.owner);
            settleBlasts();
        }
    }
    updateStrikes(seconds);
    updateShields(seconds);
    // The level goes dark for the legend item's rite, as for a great move.
    if (m_bosses.legend().darkens()) {
        m_dimmer.ask(LegendRite::kDarkening);
    }
    m_dimmer.update(seconds);
    m_world->setAmbientOffset(m_dimmer.offset());
    m_effects.update(seconds);
    collectItems();
    updateBeam(ticks);
    m_world->updateTriggers(seconds, visitors());
    handleTriggerEvents();
    if (!held && !m_messages.active() && m_world->isTower()) {
        updateSumnerVisit(seconds);
    }
    if (!held) {
        // A portal waits for everyone still on their feet.
        std::vector<PortalVisitor> standing;
        standing.reserve(m_players.size());
        for (usize i = 0; i < m_players.size(); ++i) {
            if (!isDown(i)) {
                standing.push_back(
                    PortalVisitor{m_players[i].actor.position(), m_players[i].actor.radius()});
            }
        }
        // With nobody left, and the last of them done falling, the party is taken back.
        const bool falling = std::ranges::any_of(m_players, [](const PlayerRuntime& runtime) {
            return runtime.life == PlayerLife::Dying;
        });
        if (standing.empty() && !m_players.empty() && !falling) {
            m_fallenSeconds += seconds;
            if (m_fallenSeconds >= kFallenSeconds) {
                return PlayOutcome::Fallen;
            }
        }
        if (const auto portal = m_portals.update(ticks, seconds, standing);
            portal.has_value() && leaveBy(*portal)) {
            m_leaving = true;
            m_transition.comeUp();
        }
    }
    // The camera keeps to those still standing, while anyone is.
    std::vector<CameraSubject> followed;
    for (usize i = 0; i < subjects.size(); ++i) {
        if (!isDown(i)) {
            followed.push_back(subjects[i]);
        }
    }
    if (const LevelInfo* level = m_world->level();
        level != nullptr && level->bossCamera.has_value() && bossCameraOn()) {
        m_bossCamera.update(bossSubject(), followed.empty() ? subjects : followed,
                            *level->bossCamera, cameraView(), seconds);
    } else {
        m_camera.update(followed.empty() ? subjects : followed, m_world->cameraMarkers(),
                        m_world->cameraRange(), cameraView(), seconds);
    }
    updateAmbience();
    return PlayOutcome::Running;
}

/** Places the level's loops for the party, heard from the camera. */
void PlayScene::updateAmbience() {
    if (m_context.sounds == nullptr) {
        return;
    }
    std::vector<Vec3> listeners;
    listeners.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        listeners.push_back(actor.position());
    }
    const CameraFrame frame = CameraFrame::of(viewCamera());
    const LevelInfo* level = m_world->level();
    m_audio.updateAmbience(listeners, AmbientEar{frame.position, frame.right},
                           level != nullptr ? level->soundVolume : 1.0f);
}

/** The party as the level's triggers see it. */
std::vector<TriggerVisitor> PlayScene::visitors() const {
    std::vector<TriggerVisitor> out;
    for (usize i = 0; i < m_players.size(); ++i) {
        const PlayerActor& actor = m_players[i].actor;
        TriggerVisitor visitor;
        visitor.position = presenceOf(i);
        visitor.radius = actor.radius();
        visitor.crystals = actor.save().progress().crystals;
        out.push_back(visitor);
    }
    return out;
}

void PlayScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                       f32 frameHeight) {
    if (!m_open || m_context.config == nullptr) {
        return;
    }
    const GameConfig& config = *m_context.config;
    m_messages.prepare(device);
    m_sumnerVisit.prepare(device);
    const Mat4 clip = viewCamera().clipTransform(config.horizontalFovRadians(), frameWidth,
                                                 frameHeight, frameProjection);
    m_world->draw(device, clip, viewCamera());
    m_sumner.draw(device, clip, m_world->lighting());
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.figure != nullptr && runtime.life != PlayerLife::InTower) {
            const PlayerFigure& figure = *runtime.figure;
            const PowerupEffects worn =
                PowerupEffects::of(runtime.actor.save().progress().inventory);
            const f32 size = bodyScale(runtime.actor.save(), worn);
            const Mat4 body = glm::scale(runtime.actor.transform(), Vec3{size, size, size});
            figure.draw(device, clip, body, m_world->lighting(), worn.bodyAlpha(m_playSeconds),
                        runtime.move.weaponHidden());
        }
    }
    m_portals.draw(device, clip, m_world->lighting());
    m_chests.draw(device, clip, m_world->lighting());
    m_gates.draw(device, clip, m_world->lighting());
    m_traps.draw(device, clip, m_world->lighting());
    m_barrels.draw(device, clip, m_world->lighting());
    m_safeRocks.draw(device, clip, m_world->lighting());
    m_generators.draw(device, clip, m_world->lighting());
    m_enemies.draw(device, clip, m_world->lighting());
    m_critters.draw(device, clip, m_world->lighting());
    // The boss stands out in the level's own light while the rite darkens the rest.
    m_bosses.draw(device, clip,
                  m_bosses.legend().darkens() ? m_world->fullLighting() : m_world->lighting(),
                  m_legend != nullptr ? m_legend->frozenTexture() : nullptr);
    m_victory.drawWizard(device, clip, m_world->lighting());
    m_enemyMissiles.draw(device, clip, m_world->lighting());
    m_arsenal.missiles().draw(device, clip, m_world->lighting());
    const CameraFrame effectCamera = CameraFrame::of(viewCamera());
    m_effects.draw(device, clip, m_world->fullLighting(), &effectCamera);
    m_arrival.drawEffects(device, clip, m_world->lighting());
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    // The welcome's cut is letterboxed the way the original's trigger cameras are: black
    // bars top and bottom, the status boxes hidden beneath the lower one.
    const bool cut = m_intro == Intro::Crystal;
    m_transition.draw(m_canvas, width); // over the view, under the boxes
    if (!cut) {
        m_hud.drawStatus(m_canvas, m_players);
        m_bossMeter.draw(m_canvas, device);
        m_victory.drawCaption(m_canvas, m_messages.text(), m_hud.strings(), width, height);
    }
    if (const LevelInfo* level = m_world->level(); level != nullptr) {
        m_arrival.drawTitle(m_canvas, m_messages.text(), level->title, width);
    }
    if (cut) {
        m_canvas.fill(Rect{0.0f, 0.0f, width, height * kCutBarTop}, Color::black());
        m_canvas.fill(Rect{0.0f, height * (1.0f - kCutBarBottom), width, height * kCutBarBottom},
                      Color::black());
    }
    if (!cut) {
        m_hud.drawSelectors(m_canvas, m_messages.text(), m_context.strings, m_players);
        m_hud.drawHelp(m_canvas, device, m_staticTextures, m_players, clip, width, height);
    }
    m_messages.draw(m_canvas);
    m_sumnerVisit.draw(m_canvas, m_messages.text());
    m_canvas.end();
}

CameraView PlayScene::cameraView() const {
    CameraView view;
    if (m_context.config != nullptr) {
        view.horizontalFov = m_context.config->horizontalFovRadians();
        view.aspect = static_cast<f32>(m_context.config->display.frameWidth) /
                      static_cast<f32>(m_context.config->display.frameHeight);
    }
    return view;
}

const PlayerActor* PlayScene::actor(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        if (actor.player() == player) {
            return &actor;
        }
    }
    return nullptr;
}

const PlayerAnimator* PlayScene::animator(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            return runtime.figure != nullptr && runtime.figure->animator().bound()
                       ? &runtime.figure->animator()
                       : nullptr;
        }
    }
    return nullptr;
}

/** Stands the materialising effect at every character's feet and, with `ride`, the start
 * camera at the entrance marker to hold and ride in; the party holds still for it. (The
 * realm's entering sound belongs to the loading screen, not to this.) */
void PlayScene::beginSpawn(RenderDevice& device, bool ride) {
    const auto marker = ride ? m_world->entranceCamera() : std::nullopt;
    if (ride && !marker.has_value()) {
        log::warn("Tower: no start camera; the party appears under the follow camera");
    }
    if (!m_weapons.loaded()) {
        m_weapons.load(m_context.unpackedRoot / kWeaponsArchive);
    }
    std::vector<Vec3> positions;
    positions.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        positions.push_back(runtime.actor.position());
    }
    m_arrival.begin(device, m_weapons, positions, marker);
}

/** Opens one page of a scroll message over the tower: the party reads it and presses on. */
bool PlayScene::openMessage(std::string_view name, usize page) {
    return m_device != nullptr && m_messages.open(*m_device, name, m_context.strings, page);
}

/** Congratulates the party once its crystals open a realm's gate: the scroll for the realm,
 * its announcing voice, and the save remembers so it is not said twice. */
void PlayScene::announceUnlock(s32 realm) {
    if (realm <= 0 || static_cast<usize>(realm) >= kRealmCount) {
        return;
    }
    const u32 bit = 1U << static_cast<u32>(realm);
    bool fresh = false;
    for (PlayerRuntime& runtime : m_players) {
        PlayerActor& actor = runtime.actor;
        ClassProgress& progress = actor.save().progress();
        fresh = fresh || (progress.unlocked & bit) == 0;
        progress.unlocked |= bit;
    }
    if (!fresh) {
        return;
    }
    openMessage(kUnlockLevel, static_cast<usize>(realm));
    if (static_cast<usize>(realm) < kUnlockVoices.size()) {
        m_audio.speakOverScroll(kUnlockVoices[static_cast<usize>(realm)]);
    }
}

/** Tells a refused party what a gate wants; a target opening before them (a gate's field, a
 * lift, a gate) sounds its slot's note until it is done, then the note of its end. */
void PlayScene::handleTriggerEvents() {
    // One scroll at a time: the frame's first refusal.
    if (const std::vector<TriggerRefusal> refusals = m_world->takeTriggerRefusals();
        !refusals.empty()) {
        const TriggerRefusal& refusal = refusals.front();
        if (refusal.crystals) {
            openMessage(kNeedCrystals, static_cast<usize>(refusal.id));
        } else if (const s32 tier = refusal.id - kIconTierBase; tier >= 0) {
            openMessage(kNeedIcons, static_cast<usize>(tier));
        }
    }
    for (const TriggerOpening& opening : m_world->takeTriggerOpenings()) {
        m_audio.opening(opening);
    }
    for (const TriggerOpening& settled : m_world->takeTriggerSettled()) {
        m_audio.settled(settled);
    }
}

} // namespace gdl::game
