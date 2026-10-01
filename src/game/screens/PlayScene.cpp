#include "game/screens/PlayScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "game/menu/CompassHud.h"
#include "game/players/ClassData.h"
#include "game/players/Progression.h"
#include "game/screens/PlayerPowerups.h"

namespace gdl::game {

namespace {

constexpr std::string_view kWeaponsArchive = "WEAPONS";

/** The stained-glass light through the window over the door: the Desecrated Temple's, lit once
 * its shards are all found. */
constexpr std::string_view kTempleLight = "L1XPLIGHTRAY01";
constexpr f32 kCutBarTop = 48.0f / 384.0f;    ///< the cut's black bars, as the original's trigger
constexpr f32 kCutBarBottom = 80.0f / 384.0f; ///< cameras draw them: shares of the height

constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kScrollBurnSound = "S_OPTMENUSCROLL"; ///< the options menu's, too
constexpr f32 kLevelUpEffectSeconds = 3.0f; ///< the fanfare's ring about the character
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
    m_audio.open(context.unpackedRoot, context.sounds, world.audio(),
                 world.ref().name.empty() ? 'L' : world.ref().name.front(),
                 world.level() != nullptr && world.level()->bossType >= 0);
    m_messages.load(device, m_staticTextures, m_context.unpackedRoot, m_context.strings);
    m_names.load(device, m_context.unpackedRoot, m_staticTextures);
    m_weapons.load(context.unpackedRoot / kWeaponsArchive);
    m_figures.loadSkins(device, world.powerups(), m_weapons);
    const std::array<TextureSet*, 5> effectTextures{&m_weapons.textures, &world.items().textures,
                                                    &world.realmItems().textures,
                                                    &world.powerups().textures, &m_staticTextures};
    m_effects.setTextureLenders(effectTextures);
    if (const auto glow = m_staticTextures.find("FONT32_GLOW")) {
        m_hud.setGlow(&m_staticTextures.texture(device, *glow));
    }
    // Sumner, his hints and his welcome belong to the tower alone.
    if (world.isTower()) {
        m_sumnerVisit.load(device, m_staticTextures, m_world->powerups(), m_context.unpackedRoot,
                           m_context.strings);
        m_sumner.load(device, world.items(), world.layout());
    }
    // What the tower opens to this party: its portals, their glows, its lifts and where it
    // stands coming back (fn_8005B5B8, SetPlayerStartPos).
    m_towerAccess = world.isTower() ? TowerAccess{party} : TowerAccess{};
    if (context.levels != nullptr) {
        m_portals.bind(device, world.layout(), world.items(), *context.levels, &world.collision(),
                       &world.realmItems(), world.isTower() ? &m_towerAccess : nullptr);
        for (const ExitPortals::ShutGate& gate : m_portals.shutGates()) {
            world.shutPortalGlow(gate.world, gate.gate);
        }
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
    // The window light is restored from the collection after the party has been loaded.
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == kTempleLight) {
            world.setObjectAlpha(i, 0.0f);
        }
    }
    spawnParty(party, options);
    PartyNames::show(m_players);
    // The bosses' keys show in the boxes as a level opens, but for a secret one (gamemain.c
    // 1990's music track 12).
    if (!world.ref().isSecret()) {
        m_hud.showRelics();
    }
    m_transporters.bind(device, world.layout(), world.items(), static_cast<s32>(m_players.size()),
                        &world.realmItems());
    for (const DroppedItem& item : options.items) {
        world.placeItem(device, item.name, item.position);
    }
    world.setPlayerCount(static_cast<s32>(m_players.size()));
    if (!world.isTower()) {
        const PlacedItems& items = world.placedItems();
        for (usize i = 0; i < items.size(); ++i) {
            const auto& item = items.item(i);
            if (item.subtype == ItemInfo::kRunestone && !item.taken &&
                item.shownTo(static_cast<s32>(m_players.size()))) {
                m_runeItem = i;
                m_runeMeter.begin(item.value, item.position,
                                  world.layout().maxBounds() - world.layout().minBounds(),
                                  world.ref().realmId, party);
            }
        }
        if (m_runeMeter.visible()) {
            if (const auto frame = m_staticTextures.find("THERMBASE")) {
                m_runeFrame = &m_staticTextures.texture(device, *frame);
            }
            if (const auto column = m_staticTextures.find("THERMCOL")) {
                m_runeColumn = &m_staticTextures.texture(device, *column);
            }
        }
    }
    beginChallenge();
    if (world.ref().isSecret() && m_challenge.state() == SecretChallenge::State::Inactive) {
        close();
        return false;
    }
    m_fixtures.setPlayerCount(static_cast<s32>(m_players.size()));
    m_opponents.open({device, world, m_weapons, m_effects, m_audio, context.unpackedRoot,
                      context.config != nullptr ? context.config->difficulty.gain() : 1.0f, true},
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
    world.startTriggers(visitors(), &m_towerAccess);
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
        m_bossCamera.reset(bossSubject(), subjects, *level->bossCamera, cameraView(),
                           world.cameraMarkers());
    }
    m_audio.startMusic(context.assets,
                       world.level() != nullptr ? world.level()->musicVolume : 1.0f);
    // The party materialises first; Sumner's welcome, when it is due, follows.
    m_welcome.open(world,
                   world.isTower() && options.welcome.value_or(TowerWelcome::freshParty(party)));
    // The start camera holds at the level's entrance and rides in to a party that stands
    // there; one back among a realm's portals (as when it has fallen, or come out of a level)
    // materialises with the follow camera already on it.
    const bool atEntrance =
        world.arrivalPoint(options.arrivalWorld, &m_towerAccess) == world.startPoint(0);
    beginSpawn(device, !options.position.has_value() && atEntrance);
    m_arsenal.bind({device, m_classes, m_weapons, world.collision(), m_effects, m_audio,
                    context.sounds, world.wallHitSound(), world.isTower(),
                    world.level() != nullptr && world.level()->bossType >= 0},
                   effectTextures);
    m_attacks.bind({device, m_classes, world, m_weapons, m_effects, m_audio, context.sounds,
                    m_arsenal, m_dimmer, &m_shake});
    m_bossSequence.bind(
        {device, world, m_weapons, m_staticTextures, m_effects, m_audio, context.levels});
    m_open = true;
    if (world.isTower()) {
        m_promotion.begin(m_players, m_hud.strings());
        m_promotion.bind(device, world.items(), world.layout(), m_players);
        beginTowerRelics();
    }
    log::info("Tower: {} in the party", m_players.size());
    log::info("Level {} ({}): {} exit portals", world.ref().name, world.ref().title,
              m_portals.size());
    return true;
}

void PlayScene::close() {
    m_runeMeter.clear();
    m_runeItem.reset();
    m_runeFrame = nullptr;
    m_runeColumn = nullptr;
    m_challenge.clear();
    m_challengeHud.clear();
    m_secretTravel = false;
    m_shake.clear();
    m_audio.stopCues();
    m_sumnerVisit.clear();
    m_promotion.clear();
    m_towerRelics.clear();
    m_relicVoice = kNoSound;
    m_promotionVoice = kNoSound;
    m_messages.clear();
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_portals.clear();
    m_transporters.clear();
    m_switchCutscene.clear();
    m_fixtures.clear();
    m_transition.release();
    m_departure.clear();
    m_figures.clear(); // before the archives whose skins it borrows
    m_leaving = false;
    m_attacks.clear();
    m_arsenal.clear(); // before the figures whose models they fly
    m_bossSequence.clear();
    m_effects.clear(); // and before the archive whose trees they play
    m_opponents.close();
    m_playSeconds = 0.0f;
    m_hud.clear();   // before the static texture borrowed for selector glow
    m_names.clear(); // before the static sheet its font borrows
    m_staticTextures.releaseTextures();
    m_welcome.clear();
    m_dimmer.reset();
    if (m_world != nullptr) {
        m_world->setAmbientOffset(0.0f);
    }
    m_gameOver.clear();
    m_audio.close(); // before the figures whose class voices it can play
    m_promotionFigures.clear();
    m_players.clear();
    m_arrival.clear();
    m_weapons.release(); // its textures must go before the device does
    m_world = nullptr;
    m_device = nullptr;
    m_open = false;
}

/** Stands the party side by side at the entrance, facing into the tower: the start marker's
 * heading points back out of the door. */
void PlayScene::spawnParty(std::span<const PartyMember> party, const PlayOptions& options) {
    // Arriving from a realm the party stands where the level marks that realm's way in.
    const WorldLocator* start = m_world->arrivalPoint(options.arrivalWorld, &m_towerAccess);
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
        auto& progress = runtime.actor.save().progress();
        progress.promotedLevel = progress.appearanceLevel();
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

/** A run of close blows is praised once it pauses (player.c 2553), unless the boss's wizard
 * has begun his visit (good_wiz_state past 2). */
void PlayScene::praiseStreaks(f32 seconds) {
    const bool wizard = m_bossSequence.victory().state().stage() >= BossVictory::Stage::Appearing;
    for (PlayerRuntime& runtime : m_players) {
        const std::string_view praise = runtime.streak.step(seconds);
        if (!praise.empty() && !wizard && m_audio.narrationRoom(MeleeStreak::kWait)) {
            m_audio.queueNarration(praise, LevelSoundscape::Narrator::Primary);
        }
    }
}

PartyPickups::Services PlayScene::pickupServices() {
    return {.world = *m_world,
            .fixtures = m_fixtures,
            .hud = m_hud,
            .audio = m_audio,
            .classes = m_classes,
            .sounds = m_context.sounds,
            .help = [this](s32 id, usize index) { return postHelp(id, index); },
            .openMessage = [this](std::string_view name,
                                  usize page) { return openMessage(name, page); },
            .challengeCoin = [this](usize item) { collectChallengeCoin(item); }};
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

const TurboMeter* PlayScene::turboMeter(s32 player) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            return &runtime.turbo;
        }
    }
    return nullptr;
}

void PlayScene::awardExperience(s32 player, s32 amount, bool kill) {
    PartyRecords::award(m_players, player, amount, kill, m_world->level());
}

/** Puts a help message up over a character, the narrator saying it, unless the party has
 * seen it. */
bool PlayScene::postHelp(s32 id, usize index, s32 number) {
    if (m_world != nullptr && m_world->isTower() && HelpMessages::gameplayTip(id)) {
        return false;
    }
    return m_hud.postHelp(id, index, m_players, m_audio, number);
}

PlayerAttacks::Targets PlayScene::attackTargets() {
    return {m_opponents, m_fixtures, fixtureEvents()};
}

LevelFixtures::Events PlayScene::fixtureEvents() {
    return {
        .hurt = [this](usize i, f32 damage, HurtKind kind,
                       bool directed) { hurt(i, damage, kind, directed); },
        .help = [this](s32 id, usize i) { return postHelp(id, i); },
        .card = [this](s32 player,
                       std::string_view name) { m_hud.pickups().addCard(player, name); },
        .opponents =
            [this](const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                   u32 flags) {
                m_opponents.blast(position, radius, damage, reached, m_players, flags);
            },
        .releaseEnemy =
            [this](s32 record, const Vec3& position, s32 count) {
                return m_opponents.releaseDeath(record, position, count);
            },
        .shatterPotion = [this](s32 kind,
                                const Vec3& position) { m_attacks.shatterPotion(kind, position); }};
}
void PlayScene::updateFixtures(s32 ticks, f32 seconds) {
    m_fixtures.update(ticks, seconds, m_players, fixtureEvents());
    const CameraFrame ear = CameraFrame::of(viewCamera());
    const Vec3 attention = bossCameraOn() ? m_bossCamera.attention() : m_camera.attention();
    for (const usize index : m_fixtures.traps().wakes()) {
        const auto& trap = m_fixtures.traps().trap(index);
        if (trap.subtype == Traps::kBlades) {
            f32 nearest = LevelSoundscape::kAttenuationFar;
            for (const auto& player : m_players) {
                if (player.life == PlayerLife::Standing) {
                    nearest = std::min(
                        nearest, glm::distance(trap.figure.position(), player.actor.position()));
                }
            }
            m_audio.playSerpent(trap.figure.position(), attention, nearest,
                                {ear.position, ear.right});
        }
    }
}
void PlayScene::blast(const Vec3& position, f32 radius, f32 damage) {
    m_fixtures.blast(position, radius, damage, m_players, fixtureEvents());
}
void PlayScene::settleBlasts() {
    m_fixtures.settleBlasts(m_players, fixtureEvents());
}
/** A level gained heals and announces immediately; tower promotions award the new appearance. */
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
    watchOpponents();
    m_opponents.update(ticks, seconds, m_players, m_fixtures.obstacles(), opponentEvents(),
                       m_fixtures.missileStops(), m_fixtures.critterObstacles());
    for (const Vec3& position : m_world->takeWorldExplosions()) {
        m_fixtures.worldExplosion(position, m_players, fixtureEvents());
    }
    for (const RockHit& hit : m_opponents.takeRockHits()) {
        m_fixtures.strikeSafeRock(hit.rock, hit.damage);
    }
    for (const GasReach& gas : m_opponents.takeGasReaches()) {
        m_fixtures.spoilFood(gas.position, gas.radius, gas.damage, m_players, fixtureEvents());
    }
    // A golem's or gargoyle's blow on a barrel is no one's (CritterCollideItems).
    const std::vector<CombatantRam> rams = m_opponents.takeBarrelRams();
    for (const CombatantRam& ram : rams) {
        m_fixtures.strikeBarrel(static_cast<usize>(ram.id), ram.damage, -1, m_players,
                                fixtureEvents());
    }
    if (!rams.empty()) {
        settleBlasts();
    }
}

/** What the enemies count as on screen is what the view shows, and the placed ones it comes
 * to see stand. */
void PlayScene::watchOpponents() {
    const CameraView view = cameraView();
    m_opponents.watch(ViewVolume::of(viewCamera(), view.horizontalFov, view.aspect),
                      bossCameraOn() ? m_bossCamera.attention() : m_camera.attention());
}

LevelOpponents::Events PlayScene::opponentEvents() {
    return {
        .hurt = [this](usize i, f32 damage, HurtKind kind, bool directed,
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
        .destroyedGenerator =
            [this](s32 player) { PartyRecords::destroyedGenerator(m_players, player); },
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
        .shake = [this] { m_shake.start(); },
        .help = [this](s32 id, usize i) { return postHelp(id, i); }};
}
void PlayScene::strikeEnemy(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer) {
    m_opponents.strikeEnemy(id, power, flags, direction, byPlayer, m_players);
}
void PlayScene::strikeCritter(s32 id, f32 power, u32 flags, const Vec3& direction, s32 byPlayer,
                              std::optional<Vec3> where, bool close) {
    m_opponents.strikeCritter(id, power, flags, direction, byPlayer, where, close, m_players);
}
void PlayScene::strikeGenerator(s32 id, f32 power, s32 byPlayer) {
    m_opponents.strikeGenerator(id, power, byPlayer, m_players);
}

void PlayScene::startGameOver() {
    std::string_view caption;
    if (const auto message = m_hud.strings().find(GameOver::kMessage)) {
        const auto& pages = m_hud.strings().message(*message).pages;
        if (!pages.empty()) {
            caption = pages.front();
        }
    }
    if (m_context.strings != nullptr && m_context.strings->has(GameOver::kTextId)) {
        caption = m_context.strings->get(GameOver::kTextId);
    }
    m_gameOver.begin(caption);
    m_audio.stopCues();
    log::info("The game is being quit; game over");
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
    if (!m_world->isTower() && !exit.secret) {
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

std::vector<LevelResults> PlayScene::levelResults() const {
    return PartyRecords::results(m_players);
}

std::vector<PartyMember> PlayScene::party() const {
    return PartyRecords::members(m_players);
}

std::vector<PartyMember> PlayScene::abandonedParty(std::span<const PartyMember> party) const {
    return PartyRecords::abandoned(m_players, party);
}

void PlayScene::updateSumnerVisit(f32 seconds) {
    const std::optional<s32> player = TowerWelcome::visitorOf(m_world->triggers(), m_players);
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
    m_audio.updateNarration(seconds);
    // The music's areas: the boss waking asks for the second, the zones for theirs.
    m_audio.bossAwake(m_opponents.bosses().view().awake);
    m_audio.updateMusic(seconds);
    // Once the good wizard appears the announcer keeps quiet (good_wiz_state past two).
    const BossVictory::Stage victory = m_bossSequence.victory().state().stage();
    m_audio.holdNarration(victory != BossVictory::Stage::None &&
                          victory != BossVictory::Stage::Waiting);
    m_hud.stepRelics(ticks);
    // The names over the heads run down unless a message, a cut or Sumner holds play.
    m_names.step(m_players, ticks,
                 m_gameOver.active() || m_switchCutscene.active() || m_messages.active() ||
                     m_welcome.cutting() || m_sumnerVisit.active() || m_promotion.active() ||
                     relicCeremonyOn());
    if (m_gameOver.active()) {
        // The world remains behind the caption, but no player input, portal,
        // reward or victory ceremony can restart the finished session.
        m_world->update(seconds);
        m_portals.animate(seconds);
        m_effects.update(seconds);
        updateAmbience();
        if (m_gameOver.step(ticks)) {
            m_audio.narrate(GameOver::kVoice, LevelSoundscape::Narrator::Primary);
        }
        return m_gameOver.finished() ? PlayOutcome::GameOver : PlayOutcome::Running;
    }
    if (m_switchCutscene.active()) {
        updateSwitchCutscene(ticks, seconds);
        return PlayOutcome::Running;
    }
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
        if (!m_messages.active()) {
            m_welcome.scrollClosed(m_world->layout(), m_sumner);
        }
        return PlayOutcome::Running;
    }
    // Gone through a portal, the party is out of play while the transition picture comes
    // up over the level, which goes on around it; once it covers the view they travel.
    m_transition.update(seconds);
    m_towerRelics.animate(seconds);
    if (m_leaving) {
        m_departure.update(ticks);
        m_portals.animate(seconds);
        if (m_departure.started()) {
            std::vector<CameraSubject> subjects;
            for (usize i = 0; i < m_players.size(); ++i) {
                if (!isDown(i)) {
                    const auto& actor = m_players[i].actor;
                    subjects.push_back({actor.position() + m_departure.displacement(),
                                        actor.followPoint() + m_departure.displacement()});
                }
            }
            m_camera.update(subjects, m_world->cameraMarkers(), m_world->cameraRange(),
                            cameraView(), seconds);
        }
        if (m_departure.finished()) {
            m_transition.comeUp();
        }
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
        watchOpponents();
        m_arrival.animate(seconds);
        for (const PlayerRuntime& runtime : m_players) {
            const std::unique_ptr<PlayerFigure>& figure = runtime.figure;
            if (figure != nullptr) {
                figure->animate(0.0f, ticks, seconds);
            }
        }
        m_world->update(seconds);
        updateAmbience();
        const bool cameraHandoff = m_arrival.camera().active();
        // Ride to the camera that will actually take over. A boss entrance must not
        // approach the normal level camera and then cut to a different fight view.
        const bool bossView = bossCameraOn();
        m_arrival.advance(ticks, anyButton(inputs),
                          bossView ? m_bossCamera.camera().position : m_camera.camera().position,
                          bossView ? m_bossCamera.attention() : m_camera.attention());
        if (m_arrival.takeTitleLanded()) {
            if (const LevelInfo* level = m_world->level();
                level != nullptr && !m_world->isTower()) {
                m_audio.announceTitle(level->flags);
            }
        }
        if (!spawning()) {
            if (cameraHandoff && m_arrival.camera().mode() == StartCamera::Mode::Standard) {
                std::vector<CameraSubject> subjects;
                for (const auto& player : m_players) {
                    if (player.life == PlayerLife::Standing) {
                        subjects.push_back({player.actor.position(), player.actor.followPoint()});
                    }
                }
                for (s32 i = 0; i < StartCamera::kWarmSteps; ++i) {
                    m_camera.update(subjects, m_world->cameraMarkers(), m_world->cameraRange(),
                                    cameraView(), 1.0f / TowerCamera::kStepRate);
                }
            }
            m_welcome.arrived(*m_device, m_messages, m_context.strings, m_world->layout(),
                              m_sumner);
        }
        return PlayOutcome::Running;
    }
    if (m_promotion.active() && !m_welcome.cutting()) {
        updatePromotion(ticks, seconds);
        return PlayOutcome::Running;
    }
    if (relicCeremonyOn()) {
        updateTowerRelics(ticks, seconds);
        return PlayOutcome::Running;
    }
    const bool held = m_welcome.hold(ticks);
    if (!held && updateChallenge(seconds)) {
        m_destination = LevelRef::tower(); // the application restores the parent when present
        m_secretTravel = true;
        return PlayOutcome::Travel;
    }
    auto powerupClock = PlayerPowerups::Clock::Paused;
    if (!held && !m_world->isTower()) {
        const bool bossLevel = m_world->level() != nullptr && m_world->level()->bossType >= 0;
        const BossView boss = m_opponents.bosses().view();
        if (!bossLevel) {
            powerupClock = PlayerPowerups::Clock::Level;
        } else if (boss.awake && boss.alive) {
            powerupClock = PlayerPowerups::Clock::BossFight;
        }
    }
    for (const PowerupEnding& ending : PlayerPowerups::update(m_players, seconds, powerupClock)) {
        m_audio.playNamed(ending.sound);
    }
    praiseStreaks(seconds);
    if (m_device != nullptr) {
        PartyFigures::greetGems(*m_device, m_players, m_world->powerups(), m_effects);
    }
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_world->revealCrystals(seconds);
    m_hud.pickups().step(ticks, seconds);
    m_sumner.update(seconds);
    for (auto& player : m_players) {
        if (player.figure != nullptr) {
            player.figure->setCompanionPowerups(*m_device, m_world->powerups(),
                                                player.actor.save().progress().inventory,
                                                &m_weapons);
        }
    }
    updateTransporters(ticks, seconds, held);
    const std::vector<CameraSubject> subjects = PartyMotion::step(
        m_players, inputs, held, bossCameraOn() ? m_bossCamera.yaw() : m_camera.yaw(), ticks,
        seconds, m_world->collision(), motionEvents());
    m_playSeconds += seconds;
    if (!held) {
        PartyRecords::advanceTime(m_players, seconds);
    }
    m_shake.update(ticks);
    m_hud.help().update(ticks);
    updateFixtures(ticks, seconds);
    if (!held) {
        updateHazardSurfaces(seconds);
        if (const auto beat = PlayerHealth::heartbeat(m_players, ticks, m_world->isTower())) {
            m_audio.playNamed(PlayerHealth::kHeartbeatSound, beat->volume);
        }
    }
    updateEnemies(ticks, seconds);
    m_attacks.updateProjectiles(seconds, m_players, attackTargets());
    m_attacks.updateStrikes(seconds, m_players, attackTargets());
    m_attacks.updateShields(seconds, m_players, attackTargets());
    m_attacks.updateArmour(seconds, m_players, attackTargets());
    // The level goes dark for the legend item's rite, as for a great move.
    if (m_opponents.bosses().legend().darkens()) {
        m_dimmer.ask(LegendRite::kDarkening);
        // The bearer shines through the rite, the rest of the party less (player.c 2508).
        for (PlayerRuntime& runtime : m_players) {
            runtime.glow.raise(runtime.actor.player() == m_opponents.bosses().legend().player()
                                   ? BodyGlow::kBearer
                                   : BodyGlow::kOthers);
        }
    }
    m_dimmer.update(seconds);
    m_world->setAmbientOffset(m_dimmer.offset());
    m_effects.update(seconds);
    if (m_device != nullptr) {
        m_pickups.collect(*m_device, m_players, pickupServices());
    }
    m_world->updateTriggers(seconds, visitors());
    handleTriggerEvents();
    if (m_switchCutscene.active()) {
        updateAmbience();
        return PlayOutcome::Running;
    }
    if (!held && !m_messages.active() && m_world->isTower()) {
        updateSumnerVisit(seconds);
    }
    answerTowerPrompts(inputs);
    if (!m_players.empty() && std::ranges::all_of(m_players, [](const PlayerRuntime& runtime) {
            return runtime.departed;
        })) {
        startGameOver(); // nobody is left in the game
        return PlayOutcome::Running;
    }
    if (!held) {
        // A portal waits for everyone still on their feet.
        std::vector<PortalVisitor> standing;
        standing.reserve(m_players.size());
        for (usize i = 0; i < m_players.size(); ++i) {
            if (!isDown(i)) {
                standing.push_back(PortalVisitor{m_players[i].actor.position(),
                                                 m_players[i].actor.radius(), static_cast<s32>(i),
                                                 !m_players[i].actor.moving()});
            }
        }
        // With everyone fallen, the last death played out and the announcer done, the party
        // goes back to the tower as it came into the level (game_main sends a party of the
        // dead or waiting to the tower, where PlayerRestoreState stands them again). The
        // defeat caption is for a game that is being quit, not for a wiped party.
        if (GameOver::ready(m_players) && m_audio.narrationBacklog() <= 0.0) {
            m_destination = LevelRef::tower();
            m_audio.stopCues();
            log::info("All players have fallen; back to the tower");
            return PlayOutcome::Travel;
        }
        const std::optional<usize> reached = m_portals.update(ticks, seconds, standing);
        const CameraFrame ear = CameraFrame::of(viewCamera());
        m_audio.updateExitFlame(m_portals.flamePosition(standing), {ear.position, ear.right});
        const LevelInfo* level = m_world->level();
        const std::vector<s32> waiting = m_portals.takeWaiting();
        for (const s32 index : waiting) {
            if (level == nullptr || level->bossType < 0) {
                postHelp(HelpMessages::kEveryoneToExit, static_cast<usize>(index));
            }
        }
        // Kept waiting, one is named and heard to wait (AudioPlayerBreath, player.c 2487).
        for (usize i = 0; i < m_players.size(); ++i) {
            const bool still = std::ranges::find(waiting, static_cast<s32>(i)) != waiting.end();
            if (m_players[i].exitWait.step(still, ticks)) {
                sayWithName(i, m_world->isTower() ? ExitWait::kTowerVoice : ExitWait::kVoice,
                            ExitWait::kWait);
            }
        }
        if (const auto portal = reached; portal.has_value() && leaveBy(*portal)) {
            m_opponents.settleRewards(m_players, opponentEvents());
            if (m_portals.portal(*portal).secret) {
                m_secretTravel = true;
                m_secretReturnPosition = m_portals.portal(*portal).departurePosition.value_or(
                    m_portals.portal(*portal).position);
                m_portals.consume(*portal);
                return PlayOutcome::Travel;
            }
            m_leaving = true;
            m_departure.begin(*m_device, m_weapons.textures);
            m_audio.playNamed(PortalDeparture::kSound);
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
                            *level->bossCamera, cameraView(), seconds, m_world->cameraMarkers());
    } else {
        m_camera.update(followed.empty() ? subjects : followed, m_world->cameraMarkers(),
                        m_world->cameraRange(), cameraView(), seconds);
    }
    if (m_runeItem) {
        const auto& items = m_world->placedItems();
        const bool available = *m_runeItem < items.size() && !items.item(*m_runeItem).taken;
        const auto cue = m_runeMeter.update(m_camera.attention(), available);
        if (cue != RuneMeter::Cue::None) {
            const auto frame = CameraFrame::of(viewCamera());
            m_audio.announceRune(cue == RuneMeter::Cue::Nearby, m_camera.attention(),
                                 {frame.position, frame.right});
        }
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
                           level != nullptr ? level->soundVolume : 1.0f,
                           m_switchCutscene.active() || m_promotion.active() || relicCeremonyOn());
    std::optional<Vec3> hourglass;
    for (const auto& runtime : m_players) {
        if (runtime.life == PlayerLife::Standing &&
            (PowerupEffects::of(runtime.actor.save().progress().inventory).special &
             powerup::kStopTime) != 0) {
            hourglass = runtime.actor.position();
            break;
        }
    }
    m_audio.updateHourglass(hourglass, {frame.position, frame.right});
    m_audio.updateMusicAreas(listeners);
}

/** The party as the level's triggers see it. */
std::vector<TriggerVisitor> PlayScene::visitors() const {
    std::vector<TriggerVisitor> out;
    for (usize i = 0; i < m_players.size(); ++i) {
        if (isDown(i)) {
            continue;
        }
        const PlayerActor& actor = m_players[i].actor;
        TriggerVisitor visitor;
        visitor.position = presenceOf(i);
        visitor.radius = actor.radius();
        visitor.crystals = actor.save().progress().crystals;
        visitor.gargoylePieces = actor.save().progress().relics.gargoylePieces;
        visitor.sumner = actor.save().character == kSumnerClass;
        visitor.party = static_cast<s32>(i);
        if (const auto floor = m_world->collision().floorAt(visitor.position, 0.5f, 1.0f)) {
            visitor.floorObject = floor->object;
        }
        out.push_back(visitor);
    }
    return out;
}

/** This frame's point lights (DoLighting clears them; ProcessEffects and do_players add
 * them again): the effects' own and, in a dark level, a lantern over each standing player; the
 * newest first, at most twelve. */
void PlayScene::gatherLights() {
    m_lights.clear();
    m_effects.lights(m_lights);
    m_opponents.lights(m_lights);
    PartyFigures::addLanterns(m_lights, m_players, m_world->level());
    if (m_lights.size() > WorldLighting::kMostPoints) {
        m_lights.resize(WorldLighting::kMostPoints);
    }
    m_world->setPointLights(m_lights);
}

PartyFigures::Scene PlayScene::figureScene() {
    return {.world = *m_world, .weapons = m_weapons, .departure = m_departure};
}

void PlayScene::drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye) {
    PartyFigures::drawShadows(device, m_players, figureScene(), clip, eye);
    m_opponents.enemies().drawShadows(device, clip, eye, m_world->lighting());
    m_opponents.critters().drawShadows(device, clip, eye, m_world->lighting());
    m_opponents.bosses().drawShadow(device, clip, eye, m_world->lighting());
}

void PlayScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                       f32 frameHeight) {
    if (!m_open || m_world == nullptr || m_context.config == nullptr) {
        return;
    }
    const GameConfig& config = *m_context.config;
    m_messages.prepare(device);
    m_sumnerVisit.prepare(device);
    gatherLights();
    const WorldCamera camera = viewCamera();
    const Mat4 clip = camera.clipTransform(config.horizontalFovRadians(), frameWidth, frameHeight,
                                           frameProjection);
    const CameraFrame companionCamera = CameraFrame::of(camera);
    m_world->drawOpaque(device, clip, camera);
    m_towerRelics.draw(device, clip, m_world->lighting(), camera);
    m_sumner.draw(device, clip, m_world->lighting());
    m_figures.draw(device, m_players, figureScene(), clip, companionCamera);
    m_portals.draw(device, clip, m_world->lighting());
    m_transporters.draw(device, clip, m_world->lighting());
    const CameraFrame effectCamera = companionCamera;
    m_fixtures.draw(device, clip, m_world->lighting(), &effectCamera);
    m_opponents.statues().draw(device, clip, m_world->lighting(), &effectCamera);
    m_opponents.generators().draw(device, clip, m_world->lighting());
    m_opponents.enemies().draw(device, clip, m_world->lighting(), m_figures.hitFlash(), &m_weapons);
    m_opponents.critters().draw(device, clip, m_world->lighting(), nullptr, &effectCamera);
    // The boss stands out in the level's own light while the rite darkens the rest.
    m_opponents.bosses().draw(device, clip,
                              m_opponents.bosses().legend().darkens() ? m_world->fullLighting()
                                                                      : m_world->lighting(),
                              m_bossSequence.frozenTexture());
    m_world->drawDeferred(device, clip, camera);
    m_fixtures.drawEffects(device, clip, m_world->lighting(), &effectCamera);
    drawShadows(device, clip, camera.position);
    // The wizards add onto the frame without writing depth, so the translucent scenery behind
    // them (the portals' horizon sheets) must be down first or it paints over them.
    if (!spawning()) {
        m_promotion.draw(device, clip, m_world->lighting());
    }
    if (relicCeremonyOn()) {
        m_towerRelics.drawWizard(device, clip, m_world->lighting(), camera);
    }
    m_bossSequence.victory().drawWizard(device, clip, m_world->lighting(), &effectCamera);
    m_opponents.missiles().draw(device, clip, m_world->lighting());
    m_arsenal.missiles().draw(device, clip, m_world->lighting(), &effectCamera);
    m_effects.draw(device, clip, m_world->fullLighting(), &effectCamera);
    m_arrival.drawEffects(device, clip, m_world->lighting());
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    if (m_gameOver.active()) {
        m_gameOver.draw(m_canvas, m_messages.text(), width);
        m_canvas.end();
        return;
    }
    // The welcome's cut is letterboxed the way the original's trigger cameras are: black
    // bars top and bottom, the status boxes hidden beneath the lower one.
    const bool cut = m_welcome.cutting() || (m_promotion.active() && !spawning()) ||
                     relicCeremonyOn() || m_switchCutscene.showing();
    m_transition.draw(m_canvas, width); // over the view, under the boxes
    m_names.draw(m_canvas, m_players, clip, width, height);
    if (!cut) {
        m_hud.drawStatus(m_canvas, m_players);
        if (m_runeFrame != nullptr && m_runeColumn != nullptr) {
            m_runeMeter.draw(m_canvas, *m_runeFrame, *m_runeColumn);
        }
        if (m_challenge.state() != SecretChallenge::State::Inactive) {
            m_challengeHud.draw(m_canvas, m_challenge.remaining(), m_challenge.duration(),
                                !spawning() && !m_messages.active());
        }
        if (config.camera.compass) {
            CompassHud::draw(m_canvas, m_messages.text(), m_context.strings, width, camera.yaw);
        }
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
    if (!spawning()) {
        m_promotion.drawCaption(m_canvas, m_messages.text(), width, height);
    }
    if (relicCeremonyOn()) {
        m_towerRelics.drawCaption(m_canvas, m_messages.text(), width, height);
    }
    m_sumnerVisit.draw(m_canvas, m_messages.text());
    m_canvas.end();
}

void PlayScene::setSaveSlot(s32 player, std::optional<usize> slot) {
    for (auto& runtime : m_players) {
        if (runtime.actor.player() == player) {
            runtime.slot = slot;
            return;
        }
    }
}

bool PlayScene::canPause(s32 player) const {
    return m_open && !m_leaving && !m_gameOver.active() && !m_switchCutscene.active() &&
           !m_promotion.active() && !m_towerRelics.active() && actor(player) != nullptr &&
           !fallen(player);
}

bool PlayScene::canJoin(s32 player) const {
    return m_open && m_world != nullptr && m_world->isTower() && actor(player) == nullptr &&
           m_players.size() < static_cast<usize>(kPlayerCount) && !m_leaving &&
           !m_gameOver.active() && !m_switchCutscene.active() && !spawning() &&
           !m_messages.active() && !m_sumnerVisit.active() && m_welcome.intro() == Intro::None &&
           !m_promotion.active() && !relicCeremonyOn();
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
    std::optional<Vec3> low;
    Vec3 high{0};
    for (const PlayerRuntime& runtime : m_players) {
        positions.push_back(runtime.actor.position());
        const Vec3 follow = runtime.actor.followPoint();
        high = low ? glm::max(high, follow) : follow;
        low = low ? glm::min(*low, follow) : follow;
    }
    m_arrival.begin(device, m_weapons, positions, marker,
                    bossCameraOn() ? StartCamera::Mode::Legacy : StartCamera::Mode::Standard,
                    bossCameraOn() && low ? std::optional<Vec3>{(*low + high) * 0.5f}
                                          : std::nullopt);
    if (!positions.empty()) {
        m_audio.playEntrance();
    }
}

/** Opens one page of a scroll message over the tower: the party reads it and presses on. */
bool PlayScene::openMessage(std::string_view name, usize page) {
    return m_device != nullptr && m_messages.open(*m_device, name, m_context.strings, page);
}

/** Tells a refused party what a gate wants; a target opening before them (a gate's field, a
 * lift, a gate) sounds its slot's note until it is done, then the note of its end. */
/** A turntable grinds as it turns and thuds as it stops (fn_8009D7E4): stone in the castle,
 * metal in the mines, nothing elsewhere. */
void PlayScene::handleTriggerEvents() {
    m_triggerCues.handle(
        *m_world, m_audio, m_context.sounds, m_switchCutscene,
        {.help = [this](s32 id, usize index) { return postHelp(id, index); },
         .openMessage = [this](std::string_view name,
                               usize page) { return openMessage(name, page); },
         .shake = [this] { m_shake.start(CameraShake::Target::Attention, 0, 180, 0.1f, 100); },
         .helpAt =
             [this](s32 id, const Vec3& position) {
                 return m_hud.postHelp(id, 0, m_players, m_audio, -1, position);
             }});
}

} // namespace gdl::game
