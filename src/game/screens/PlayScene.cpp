#include "game/screens/PlayScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "game/players/ItemPickup.h"
#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kBeamObject = "L1XPLIGHTRAY01"; ///< the light on Sumner's lectern
constexpr std::string_view kWeaponsArchive = "WEAPONS";
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
constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";
constexpr std::string_view kScrollBurnSound = "S_OPTMENUSCROLL"; ///< the options menu's, too
constexpr std::string_view kFirstRuneVoice = "S_RUNEFOUND1";
constexpr std::string_view kRuneVoicePrefix = "S_RUNE";    ///< then S_RUNE2 to S_RUNE12
constexpr std::string_view kLevelScrollPrefix = "SCROLLS"; ///< a level's scroll pages
constexpr f32 kLevelUpEffectSeconds = 3.0f; ///< the fanfare's ring about the character
constexpr f32 kStrongThrowScale = 2.0f; ///< a strong throw's weapon: twice the size and the harm
constexpr s32 kSpecialPowerup = 9;      ///< the pickup subtype of the specials
constexpr u32 kTurboFlag = 0x80000;     ///< of them, the one that fills the turbo meter
constexpr f32 kFallenSeconds = 3.0f;    ///< from the last death to the tower
const Vec3 kNowhere{0.0f, -1.0e6f, 0.0f};

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
    m_fixtures.bind({device, world, m_weapons, m_effects, m_audio,
                     context.config != nullptr ? context.config->difficulty.gain() : 1.0f});
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
    m_fixtures.setPlayerCount(static_cast<s32>(m_players.size()));
    m_opponents.open({device, world, m_weapons, m_effects, m_audio, context.unpackedRoot,
                      context.config != nullptr ? context.config->difficulty.gain() : 1.0f},
                     m_players);
    if (m_opponents.bosses().raisesArenaRocks()) {
        m_fixtures.safeRocks().hideForEruptions();
    }
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
    m_attacks.bind({device, m_classes, world, m_weapons, m_effects, m_audio, context.sounds,
                    m_arsenal, m_dimmer});
    m_bossSequence.bind(
        {device, world, m_weapons, m_staticTextures, m_effects, m_audio, context.levels});
    m_open = true;
    log::info("Tower: {} in the party", m_players.size());
    log::info("Level {} ({}): {} exit portals", world.ref().name, world.ref().title,
              m_portals.size());
    return true;
}

void PlayScene::close() {
    m_shake.clear();
    m_audio.stopCues();
    m_sumnerVisit.clear();
    m_messages.clear();
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_portals.clear();
    m_fixtures.clear();
    m_transition.release();
    m_leaving = false;
    m_attacks.clear();
    m_arsenal.clear(); // before the figures whose models they fly
    m_bossSequence.clear();
    m_effects.clear(); // and before the archive whose trees they play
    m_opponents.close();
    m_playSeconds = 0.0f;
    m_hud.clear(); // before the static texture borrowed for selector glow
    m_staticTextures.releaseTextures();
    m_intro = Intro::None;
    m_dimmer.reset();
    if (m_world != nullptr) {
        m_world->setAmbientOffset(0.0f);
    }
    m_fallenSeconds = 0.0f;
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
                               scale, spreads,
                               m_attacks.aim(m_players[index].actor, direction, attackTargets()));
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
        const s32 chest = m_fixtures.chests().holdingTouchedBy(ChestVisitor{here, actor.radius()});
        const Vec3 from =
            chest >= 0 ? m_fixtures.chests().chest(static_cast<usize>(chest)).figure.position()
                       : here;
        collectors.push_back(Collector{from, actor.reach(), actor.height() * 0.5f});
    }
    const std::vector<Pickup> pickups = m_world->collect(
        *m_device, collectors, [this](const Pickup& pickup) { return takePickup(pickup); });
    for (usize chest = 0; chest < m_fixtures.chests().size(); ++chest) {
        const s32 held = m_fixtures.chests().chest(chest).held;
        if (held >= 0 && m_world->placedItems().item(static_cast<usize>(held)).taken) {
            m_fixtures.chests().remove(chest);
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

const TurboMeter* PlayScene::turboMeter(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            return &runtime.turbo;
        }
    }
    return nullptr;
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

/** Puts a help message up over a character, the narrator saying it, unless the party has
 * seen it. */

bool PlayScene::postHelp(s32 id, usize index, s32 number) {
    return m_hud.postHelp(id, index, m_players, m_audio, number);
}

PlayerAttacks::Targets PlayScene::attackTargets() {
    return {m_opponents, m_fixtures, fixtureEvents()};
}

LevelFixtures::Events PlayScene::fixtureEvents() {
    return {.hurt = [this](usize i, f32 damage, HurtKind kind,
                           bool directed) { hurt(i, damage, kind, directed); },
            .help = [this](s32 id, usize i) { postHelp(id, i); },
            .card = [this](s32 player,
                           std::string_view name) { m_hud.pickups().addCard(player, name); },
            .opponents = [this](const Vec3& position, f32 radius,
                                f32 damage) { hurtOpponentsByBlast(position, radius, damage); }};
}
void PlayScene::updateFixtures(s32 ticks, f32 seconds) {
    m_fixtures.update(ticks, seconds, m_players, fixtureEvents());
}
void PlayScene::blast(const Vec3& position, f32 radius, f32 damage) {
    m_fixtures.blast(position, radius, damage, m_players, fixtureEvents());
}
void PlayScene::settleBlasts() {
    m_fixtures.settleBlasts(m_players, fixtureEvents());
}
void PlayScene::hurtOpponentsByBlast(const Vec3& position, f32 radius, f32 damage) {
    for (const s32 enemy : m_opponents.enemies().within(position, radius)) {
        const Vec3 away = m_opponents.enemies().positionOf(enemy) - position;
        strikeEnemy(enemy, damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
    }
    for (const s32 generator : m_opponents.generators().within(position, radius)) {
        strikeGenerator(generator, damage, -1);
    }
    if (m_opponents.bosses().within(position, radius)) {
        EnemyHit struck;
        struck.damage = damage;
        struck.flags = EnemyHit::kKnockDown;
        if (const Vec3* at = m_opponents.bosses().position(); at != nullptr) {
            struck.direction = Vec3{at->x - position.x, 0.0f, at->z - position.z};
        }
        m_opponents.bosses().hurt(struck);
    }
    for (const s32 critter : m_opponents.critters().within(position, radius)) {
        const Vec3 away = m_opponents.critters().positionOf(critter) - position;
        strikeCritter(critter, damage, EnemyHit::kKnockDown, Vec3{away.x, 0.0f, away.z}, -1);
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
        // The name borrows clips from the figure's voice bank. Start it after
        // replacing a milestone costume, not from the figure being destroyed.
        postHelp(HelpMessages::kLevelUp, i, change->to);
    }
}

void PlayScene::updateVictory(s32 ticks, f32 seconds) {
    if (m_bossSequence.advanceVictory(ticks, seconds, m_players, m_hud.strings()) && !m_leaving) {
        m_destination = LevelRef::tower();
        m_leaving = true;
        m_transition.comeUp();
    }
}

void PlayScene::updateEnemies(s32 ticks, f32 seconds) {
    m_opponents.update(
        ticks, seconds, m_players, m_fixtures.obstacles(),
        {.hurt = [this](usize i, f32 damage, HurtKind kind, bool directed,
                        const PlayerImpact& impact) { hurt(i, damage, kind, directed, impact); },
         .blast = [this](const Vec3& position, f32 radius,
                         f32 damage) { blast(position, radius, damage); },
         .settleBlasts = [this] { settleBlasts(); },
         .legend =
             [this](const LegendEvent& event) {
                 m_bossSequence.showLegend(event, m_opponents.bosses(), m_players);
             },
         .advanceLegend =
             [this](f32 duration) {
                 m_bossSequence.advanceLegend(duration, m_opponents.bosses(), m_players);
             },
         .fallen =
             [this](const Vec3& position) {
                 m_bossSequence.fallen(position, m_opponents.bosses(), m_players);
             },
         .spew =
             [this](const CombatSpew& spew) {
                 m_bossSequence.spewCoins(spew, m_opponents, m_players);
             },
         .advanceVictory = [this](s32 elapsed, f32 duration) { updateVictory(elapsed, duration); },
         .levels = [this] { updateLevels(); },
         .award = [this](s32 player, s32 amount,
                         bool kill) { awardExperience(player, amount, kill); },
         .blocksBreath =
             [this](const Vec3& from, const Vec3& to) {
                 return m_fixtures.safeRocks().blocksBreath(from, to);
             },
         .blocksArea =
             [this](const Vec3& from, const Vec3& to) {
                 constexpr f32 kAreaProbeRadius = 0.1f;
                 return m_fixtures.safeRocks().blocksSegment(from, to, kAreaProbeRadius);
             },
         .arenaAnchors = [this] { return m_fixtures.safeRocks().attackAnchors(); },
         .arenaTargets = [this] { return m_fixtures.safeRocks().arenaTargets(); },
         .activateArena =
             [this](const CombatArenaActivation& activation) {
                 m_fixtures.safeRocks().scheduleActivation(activation.index, activation.delay);
             },
         .shake = [this] { m_shake.start(); }});
}
void PlayScene::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer) {
    m_opponents.strikeEnemy(id, power, flags, direction, byPlayer, m_players);
}
void PlayScene::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                              std::optional<Vec3> where, bool close) {
    m_opponents.strikeCritter(id, power, flags, direction, byPlayer, where, close, m_players);
}
void PlayScene::strikeGenerator(s32 id, f32 power, s32 byPlayer) {
    m_opponents.strikeGenerator(id, power, byPlayer);
}

void PlayScene::hurt(usize index, f32 damage, HurtKind kind, bool directed,
                     const PlayerImpact& impact) {
    if (index >= m_players.size()) {
        return;
    }
    const LevelInfo* level = m_world->level();
    m_health.hurt(
        m_players[index], damage, kind, directed, m_world->isTower(),
        level != nullptr ? level->tuning.damage : 1.0f,
        {.block = [this, index](f32 taken,
                                f32 left) { m_attacks.showBlock(index, taken, left, m_players); },
         .sound = [this](std::string_view sound) { m_audio.playNamed(sound); },
         .cry = [this, index](std::string_view voice) { m_attacks.cry(index, voice, m_players); },
         .named = [this, index](std::string_view line) { sayWithName(index, line); }},
        impact);
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
    return PlayerFigure::bodyScale(save, effects);
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

WorldCamera PlayScene::viewCamera() const {
    if (m_arrival.camera().active()) {
        return m_arrival.camera().camera();
    }
    if (m_intro == Intro::Crystal) {
        return m_cutCamera;
    }
    return bossCameraOn() ? m_shake.apply(m_bossCamera.camera(), m_bossCamera.attention())
                          : m_shake.apply(m_camera.camera(), m_camera.attention());
}

/** A boss level with a boss camera record frames the fight with it while the boss stands. */
bool PlayScene::bossCameraOn() const {
    const LevelInfo* level = m_world != nullptr ? m_world->level() : nullptr;
    return level != nullptr && level->bossCamera.has_value() &&
           (m_opponents.bosses().present() || m_bossSequence.victory().state().running());
}

/** The boss as the camera sees it; once it has fallen, the wizard in its place. */
BossCameraSubject PlayScene::bossSubject() const {
    BossCameraSubject subject;
    if (m_bossSequence.victory().state().running()) {
        return m_bossSequence.victorySubject();
    }
    if (const Vec3* at = m_opponents.bosses().position(); at != nullptr) {
        subject.position = *at;
    }
    subject.facing = m_opponents.bosses().facing();
    subject.radius = m_opponents.bosses().radius();
    subject.height = m_opponents.bosses().height();
    subject.attentionOffset = m_opponents.bosses().cameraOffset();
    subject.baseAttention = m_opponents.bosses().cameraBase();
    subject.awake = m_opponents.bosses().view().awake;
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
                case PartyMotion::Action::Ram:
                    m_attacks.ramBarrels(i, m_players, attackTargets());
                    break;
                case PartyMotion::Action::ThrowWeapon: throwWeapon(m_players[i].actor); break;
                case PartyMotion::Action::StrongThrow:
                    launchWeapon(i, m_players[i].actor.facing(), kStrongThrowScale, true);
                    break;
                case PartyMotion::Action::ShieldPotion: m_attacks.shieldPotion(i, m_players); break;
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
        .advanceTurbo =
            [this](usize i, s32 elapsed, f32 duration) {
                m_attacks.updateTurbo(i, elapsed, duration, m_players,
                                      [this](s32 id, usize index) { postHelp(id, index); });
            },
        .thrownImpact = [this](usize i, f32 damage) { hurt(i, damage, HurtKind::Blow, true); },
        .aim =
            [this](usize i) {
                const PlayerActor& actor = m_players[i].actor;
                return m_attacks.aim(actor, actor.facing(), attackTargets());
            }};
    const std::vector<CameraSubject> subjects = PartyMotion::step(
        m_players, inputs, held, bossCameraOn() ? m_bossCamera.yaw() : m_camera.yaw(), ticks,
        seconds, m_world->collision(), movementEvents);
    m_playSeconds += seconds;
    m_shake.update(ticks);
    m_hud.help().update(ticks);
    updateFixtures(ticks, seconds);
    updateEnemies(ticks, seconds);
    m_attacks.updateProjectiles(seconds, m_players, attackTargets());
    m_attacks.updateStrikes(seconds, m_players, attackTargets());
    m_attacks.updateShields(seconds, m_players, attackTargets());
    // The level goes dark for the legend item's rite, as for a great move.
    if (m_opponents.bosses().legend().darkens()) {
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
            const Mat4 body = glm::scale(runtime.capture.body().value_or(runtime.actor.transform()),
                                         Vec3{size, size, size});
            figure.draw(device, clip, body, m_world->lighting(), worn.bodyAlpha(m_playSeconds),
                        runtime.move.weaponHidden());
        }
    }
    m_portals.draw(device, clip, m_world->lighting());
    m_fixtures.draw(device, clip, m_world->lighting());
    m_opponents.generators().draw(device, clip, m_world->lighting());
    m_opponents.enemies().draw(device, clip, m_world->lighting());
    m_opponents.critters().draw(device, clip, m_world->lighting());
    // The boss stands out in the level's own light while the rite darkens the rest.
    m_opponents.bosses().draw(device, clip,
                              m_opponents.bosses().legend().darkens() ? m_world->fullLighting()
                                                                      : m_world->lighting(),
                              m_bossSequence.frozenTexture());
    const CameraFrame effectCamera = CameraFrame::of(viewCamera());
    m_bossSequence.victory().drawWizard(device, clip, m_world->lighting(), &effectCamera);
    m_opponents.missiles().draw(device, clip, m_world->lighting());
    m_arsenal.missiles().draw(device, clip, m_world->lighting());
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
        m_opponents.meter().draw(m_canvas, device);
        m_bossSequence.victory().drawCaption(m_canvas, m_messages.text(), m_hud.strings(), width,
                                             height);
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
