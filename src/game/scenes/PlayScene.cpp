#include "game/scenes/PlayScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"
#include "engine/render/AmbientOcclusion.h"
#include "engine/render/DepthOfField.h"
#include "engine/world/WorldCamera.h"

#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/characters/ClassData.h"
#include "game/characters/NameCheats.h"
#include "game/characters/Progression.h"
#include "game/combat/PowerupFlags.h"
#include "game/hud/CinematicBars.h"
#include "game/hud/StatusBox.h"
#include "game/party/PartyLighting.h"
#include "game/party/PartyRecords.h"
#include "game/party/PlayerPowerups.h"
#include "game/party/TargetAssist.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

namespace {

constexpr std::string_view kWeaponsArchive = "WEAPONS";

/** The stained-glass light through the window over the door: the Desecrated Temple's, lit once
 * its shards are all found. */
constexpr std::string_view kTempleLight = "L1XPLIGHTRAY01";

constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kScrollBurnSound = "S_OPTMENUSCROLL"; ///< the options menu's, too
constexpr f32 kLevelUpEffectSeconds = 3.0f; ///< the fanfare's ring about the character

constexpr std::string_view kMenuMoveSound = "S_OPTMENUMOVVRT";
constexpr std::string_view kMenuSelectSound = "S_OPTMENUSEL";
constexpr std::string_view kMenuExitSound = "S_OPTMENUEXIT";
constexpr s32 kMinTicks = 1; ///< a frame advances the clock by at least one tick
constexpr s32 kMaxTicks = 4; ///< and, however late, by at most four

/** How fast the powerup clock runs (PlayerProcessPowerups 0x8007ce24-0x8007ce90): not while
 * held or in the tower; on a boss level three times as fast while its boss is engaged and its
 * death has not played (gBossActive, gBossDead), else not at all; elsewhere at its own rate, an
 * imported level, which has no level record, included. */
PlayerPowerups::Clock powerupClockOf(bool held, const LevelWorld& world, const BossView& boss) {
    if (held || world.isTower()) {
        return PlayerPowerups::Clock::Paused;
    }
    if (world.level() == nullptr || world.level()->bossType < 0) {
        return PlayerPowerups::Clock::Level;
    }
    return boss.engaged && !boss.deathPlayed ? PlayerPowerups::Clock::BossFight
                                             : PlayerPowerups::Clock::Paused;
}

} // namespace

void PlayScene::pauseGameplaySounds() {
    if (m_links.haptics != nullptr) {
        m_links.haptics->stopRumble();
    }
    m_audio.pauseAmbience();
    m_triggerCues.pause(m_audio);
    m_attacks.stopDeathSounds(m_players);
    m_opponents.stopDeathSound();
    if (m_world && m_world->imported()) {
        m_world->imported()->pauseSounds();
    }
}

bool PlayScene::open(RenderDevice& device, const GameServices& services, LevelWorld& world,
                     std::span<const PartyMember> party, const PlayOptions& options,
                     const Links& links) {
    close();
    m_services = &services;
    m_links = links;
    m_mechanics.open(services.mechanics, {.tower = world.isTower()});
    syncMechanics();
    m_sim.partyCountOverride = options.partyCount;
    m_device = &device;
    m_world = &world;
    // A character menu can replace the party without replacing the shared tower.
    // Rebuild its initial pickups and gates before applying the new party's progress.
    if ((!world.built() || world.isTower()) && !world.load(device, services.unpackedRoot)) {
        return false;
    }
    if (!m_hud.load(device, services.unpackedRoot, services.strings)) {
        return false;
    }
    m_classes.load(services.unpackedRoot / kClassDataDirectory);
    m_audio.open(services.unpackedRoot, services.sounds, world.audio(),
                 world.ref().name.empty() ? 'L' : world.ref().name.front(),
                 world.level() != nullptr && world.level()->bossType >= 0);
    m_messages.load(device, m_staticTextures, m_services->unpackedRoot, m_services->strings);
    if (auto* imported = world.imported()) {
        imported->bindStrings(services.strings);
        m_messages.setPresentation(imported->messagePresentation());
        m_audio.bindAnnouncements([imported](auto lines, auto subject) {
            return imported->gameplayAnnouncement(lines, subject);
        });
    }
    m_names.load(device, m_services->unpackedRoot, m_staticTextures);
    m_weapons.load(services.unpackedRoot / kWeaponsArchive);
    m_figures.loadSkins(device, world.powerups(), m_weapons);
    m_compass.bind(device, world.powerups());
    m_hud.bindHourglass(device, world.powerups(), links.stopTimeTotal);
    const std::array<TextureSet*, 5> effectTextures{&m_weapons.textures, &world.items().textures,
                                                    &world.realmItems().textures,
                                                    &world.powerups().textures, &m_staticTextures};
    m_effects.setTextureLenders(effectTextures);
    if (const auto glow = m_staticTextures.find("FONT32_GLOW")) {
        m_glowSheet = &m_staticTextures.texture(device, *glow);
        m_hud.setGlow(m_glowSheet);
    }
    // Imported residents reuse the menu artwork, not the GC tower figure or
    // its campaign advice. Bind it now, never during an interactive frame.
    if (world.isTower() || world.imported()) {
        m_sumnerVisit.load(device, m_staticTextures, m_world->powerups(), m_services->unpackedRoot,
                           m_services->strings);
    }
    if (world.isTower()) {
        m_sumner.load(device, world.items(), world.layout());
    }
    // What the tower opens to this party: its portals, their glows, its lifts and where it
    // stands coming back (fn_8005B5B8, SetPlayerStartPos).
    m_sim.towerAccess = world.isTower() ? TowerAccess{party} : TowerAccess{};
    if (services.levels != nullptr) {
        m_portals.bind(device, world.layout(), world.itemScope(), *services.levels,
                       &world.collision(), world.isTower() ? &m_sim.towerAccess : nullptr);
        for (const ExitPortals::ShutGate& gate : m_portals.shutGates()) {
            world.shutPortalGlow(gate.world, gate.gate);
        }
    }
    m_fixtures.bind({device, world, m_weapons, m_effects, m_audio,
                     services.config != nullptr ? services.config->difficulty.gain() : 1.0f});
    m_sim.refusedPortal = -1;
    m_sim.leaving = false;
    // The level cuts in: the original wipes the map, the movie's last frame or the transition
    // picture the moment the level has loaded (fn_8005351C, delete_map_blits), never fading
    // one out, so nothing is put up over an arriving party.
    m_transition.load(device, services.unpackedRoot);
    // The window light is restored from the collection after the party has been loaded.
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == kTempleLight) {
            world.setObjectAlpha(i, 0.0f);
        }
    }
    spawnParty(party, options);
    m_hud.stepHourglass(0, m_players);
    PartyNames::show(m_players);
    // The bosses' keys show in the boxes as a level opens, but for a secret one (gamemain.c
    // 1990's music track 12).
    if (!world.ref().isSecret()) {
        m_hud.showRelics();
    }
    const s32 partySize = partyCount(static_cast<s32>(m_players.size()));
    m_transporters.bind(device, world.layout(), world.itemScope(), partySize, &world.collision());
    for (const DroppedItem& item : options.items) {
        world.placeItem(device, item.name, item.position);
    }
    world.setPlayerCount(partySize);
    if (!world.isTower()) {
        const PlacedItems& items = world.placedItems();
        for (usize i = 0; i < items.size(); ++i) {
            const auto& item = items.item(i);
            if (item.subtype == ItemInfo::kRunestone && !item.taken && item.shownTo(partySize)) {
                m_sim.runeItem = i;
                m_sim.runeMeter.begin(item.value, item.position,
                                      world.layout().maxBounds() - world.layout().minBounds(),
                                      world.ref().realmId, party);
            }
        }
        if (m_sim.runeMeter.visible()) {
            if (const auto frame = m_staticTextures.find("THERMBASE")) {
                m_runeFrame = &m_staticTextures.texture(device, *frame);
            }
            if (const auto column = m_staticTextures.find("THERMCOL")) {
                m_runeColumn = &m_staticTextures.texture(device, *column);
            }
        }
    }
    beginChallenge();
    if (world.ref().isSecret() && m_sim.challenge.state() == SecretChallenge::State::Inactive) {
        close();
        return false;
    }
    m_fixtures.setPlayerCount(partySize);
    m_opponents.open({device, world, m_weapons, m_effects, m_audio, services.unpackedRoot,
                      services.config != nullptr ? services.config->difficulty.gain() : 1.0f, true},
                     m_players);
    m_opponents.setMechanics(&m_mechanics);
    m_opponents.setPartyCount(m_sim.partyCountOverride);
    if (auto* imported = world.imported()) {
        imported->updateGenerators(m_opponents.importedGeneratorStates());
    }
    // Native texture names may cross from arena items into the boss archive. Bind only
    // after that archive is loaded; fixtures are cleared before opponents on close.
    ItemArchive* bossArchive = m_opponents.bosses().archive();
    const std::array<TextureSet*, 3> arenaTextures{&world.textures(), &world.realmItems().textures,
                                                   bossArchive != nullptr ? &bossArchive->textures
                                                                          : nullptr};
    m_fixtures.safeRocks().bindAnimations(device, world.items(), arenaTextures);
    if (m_opponents.bosses().raisesArenaRocks()) {
        m_fixtures.safeRocks().hideForEruptions();
    }
    // The levels the party comes in at: what is gained from here is news.
    m_sim.levels.clear();
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        m_sim.levels.observe(actor.player(), experienceLevel(actor.save().experience()));
    }
    world.startTriggers(visitors(), &m_sim.towerAccess);
    m_audio.bindAmbience(world.layout(), &world.scene());
    std::vector<CameraSubject> subjects;
    subjects.reserve(m_players.size());
    for (PlayerRuntime& runtime : m_players) {
        const auto size = world.imported() && !world.imported()->playerLevelGrowth()
                              ? PlayerFigure::LevelSize::LevelOne
                              : PlayerFigure::LevelSize::Progression;
        runtime.figure = PlayerFigure::load(device, m_services->unpackedRoot, runtime.actor.save(),
                                            Entrance::Play, size);
        if (runtime.figure != nullptr) {
            runtime.figure->setCompanionPowerups(
                device, world.powerups(), runtime.actor.save().progress().inventory, &m_weapons);
        }
        subjects.push_back(CameraSubject{runtime.actor.position(), runtime.actor.followPoint(),
                                         runtime.actor.height() * 0.5f});
    }
    m_camera.reset(subjects, world.cameraMarkers(), world.cameraRange(), cameraView());
    resetSeatCameras(subjects, world.cameraMarkers(), world.cameraRange());
    if (auto* imported = world.imported()) {
        imported->resetCamera(subjects);
    }
    if (const LevelInfo* level = world.level();
        level != nullptr && level->bossCamera.has_value() && bossCameraOn()) {
        m_sim.bossCamera.reset(bossSubject(), subjects, *level->bossCamera, cameraView(),
                               world.cameraMarkers());
    }
    m_audio.startMusic(services.assets,
                       world.level() != nullptr ? world.level()->musicVolume : 1.0f);
    // The party materialises first; Sumner's welcome, or the scroll for the level it has just
    // beaten, follows when one is due.
    m_levelBeaten = world.isTower() ? options.beaten : std::nullopt;
    m_sim.welcome.open(world,
                       world.isTower() && options.welcome.value_or(TowerWelcome::freshParty(party)),
                       m_levelBeaten);
    // Every tower spawn has its own entrance view, including returns to a realm's wing.
    const WorldLocator* arrival = world.arrivalPoint(options.arrivalWorld, &m_sim.towerAccess);
    beginSpawn(device, options.position.has_value()
                           ? std::nullopt
                           : world.entranceCamera(arrival != nullptr ? arrival->next : 0));
    std::function<bool(s32, f32, u32, s32)> strikeMuzzleObject;
    if (auto* imported = world.imported()) {
        strikeMuzzleObject = [imported](s32 object, f32 power, u32 flags, s32 player) {
            return imported->strikeWorldObject(object, power, flags, player);
        };
    }
    m_arsenal.bind(
        {device, m_classes, m_weapons, world.collision(), m_effects, m_audio, services.sounds,
         world.wallHitSound(), world.isTower(),
         (world.level() != nullptr && world.level()->bossType >= 0) ||
             (world.imported() != nullptr && world.imported()->importedBoss() != nullptr),
         &world.powerups(),
         services.config != nullptr ? &services.config->multiplayer.mode : nullptr,
         std::move(strikeMuzzleObject)},
        effectTextures);
    m_attacks.bind({device, m_classes, world, m_weapons, m_effects, m_audio, services.sounds,
                    m_arsenal, m_sim.dimmer, &m_sim.shake,
                    services.config != nullptr ? &services.config->multiplayer.mode : nullptr,
                    services.config != nullptr && services.config->difficulty.level == "easy"
                        ? TargetAssist::kEasyFacingDot
                        : TargetAssist::kFacingDot,
                    [this](s32 player) {
                        emitOutput(RumbleOutput{player, 0, ControlFeedback::ThunderHammer});
                    }});
    m_bossSequence.bind(
        {device, world, m_weapons, m_staticTextures, m_effects, m_audio, services.levels});
    m_bossSequence.advanceLegend(0, m_opponents.bosses(), m_players);
    m_open = true;
    if (world.isTower()) {
        m_ceremonies.open(ceremonyScene(), m_hud.strings(), m_messages.strings(), m_portals);
    }
    log::info("Tower: {} in the party", m_players.size());
    log::info("Level {} ({}): {} exit portals", world.ref().name, world.ref().title,
              m_portals.size());
    gatherLights();
    // The carried cheats go on once every owner they reach is bound: the opponents' open
    // forgets who is unseen, and only a party standing can be made ghosts.
    for (s32 seat = 0; seat < kPlayerCount; ++seat) {
        setCheats(seat, options.consoleCheats[static_cast<usize>(seat)]);
    }
    return true;
}

void PlayScene::close() {
    if (m_world && m_world->imported()) {
        m_world->imported()->stopSounds();
    }
    ++m_openGeneration;
    m_picking.select(std::nullopt);
    m_nativeDigestResources.clear();
    if (m_links.haptics != nullptr) {
        m_links.haptics->stopRumble();
    }
    m_links = {};
    m_presentedClip.reset();
    m_presentedCanvas.reset();
    m_previousCamera.reset();
    m_previousSeatView.reset();
    m_sim.runeMeter.clear();
    m_sim.runeItem.reset();
    m_runeFrame = nullptr;
    m_runeColumn = nullptr;
    m_sim.challenge.clear();
    m_challengeHud.clear();
    m_sim.secretTravel = false;
    m_sim.shake.clear();
    endRecordedAudio();
    m_audio.stopCues();
    m_sumnerVisit.clear();
    m_spawned.clear();
    m_sim.cheats = {};
    m_sim.consoleTravel.reset();
    m_sim.consoleSpawns = 0;
    m_borrowedItems.clear();
    m_ceremonies.clearCeremonies();
    m_messages.clear();
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_portals.clear();
    m_transporters.clear();
    m_sim.switchCutscene.clear();
    m_fixtures.clear();
    m_transition.release();
    m_departure.clear();
    m_figures.clear(); // before the archives whose skins it borrows
    m_compass.clear();
    m_sim.leaving = false;
    m_attacks.clear();
    m_arsenal.clear(); // before the figures whose models they fly
    m_bossSequence.clear();
    m_effects.clear();   // and before the archive whose trees they play
    m_mechanics.close(); // before the opponents they rule and the archives they draw from
    m_opponents.close();
    m_sim.opponentsAdvanced = false;
    m_sim.projectilesAdvanced = false;
    m_sim.playSeconds = 0.0f;
    m_sim.towerNoticeSeconds = 0.0f;
    m_hud.clear();   // before the static texture borrowed for selector glow
    m_names.clear(); // before the static sheet its font borrows
    m_glowSheet = nullptr;
    m_localSeats = 0xF;
    m_seats.setViewSeat(-1);
    m_previousSeatView.reset();
    m_sim.soloCameras = false;
    m_staticTextures.releaseTextures();
    m_sim.welcome.clear();
    m_levelBeaten.reset();
    m_sim.dimmer.reset();
    if (m_world != nullptr) {
        m_world->setAmbientOffset(0.0f);
    }
    m_sim.gameOver.clear();
    m_audio.close(); // before the figures whose class voices it can play
    m_ceremonies.releaseFigures();
    m_players.clear();
    m_arrival.clear();
    m_weapons.release(); // its textures must go before the device does
    m_world = nullptr;
    m_device = nullptr;
    m_open = false;
}

/** Stands the party side by side at the entrance, facing the way the start marker does: the
 * original takes the locator's yaw as stored (get_player_pos, gPlayerStartYaw), which points
 * into the level and, in the tower, in from the door. */
void PlayScene::spawnParty(std::span<const PartyMember> party, const PlayOptions& options) {
    // Arriving from a realm the party stands where the level marks that realm's way in.
    const WorldLocator* start = m_world->arrivalPoint(options.arrivalWorld, &m_sim.towerAccess);
    Vec3 origin{0.0f, 0.0f, 0.0f};
    f32 yaw = 0.0f;
    if (start != nullptr) {
        origin = start->position;
        yaw = start->rotation.y;
    } else if (!options.position.has_value()) {
        log::warn("Tower: no entrance start point; the party stands at the origin");
    }
    origin = options.position.value_or(origin);
    yaw = options.yaw.value_or(yaw);
    const Vec3 sideways{portable::cos(yaw), 0.0f, -portable::sin(yaw)};
    const f32 first = -0.5f * static_cast<f32>(party.size() - 1) * kSpawnSpacing;
    m_players.reserve(party.size());
    for (usize i = 0; i < party.size(); ++i) {
        const PartyMember& member = party[i];
        CharacterSave save = member.save;
        applyNameCheats(save);
        PlayerRuntime runtime;
        const Vec3 position = origin + sideways * (first + static_cast<f32>(i) * kSpawnSpacing);
        runtime.actor.spawn(member.player, save, m_classes.stats(save.character), position, yaw);
        auto& progress = runtime.actor.save().progress();
        progress.promotedLevel = progress.appearanceLevel();
        runtime.actor.settle(m_world->collision());
        runtime.slot = member.slot;
        // Someone who fell stands again in the tower; elsewhere they wait there still.
        runtime.life =
            member.fallen && !m_world->isTower() ? PlayerLife::InTower : PlayerLife::Standing;
        runtime.entrySave = save;
        // PlayerSaveState preserves save_backup for the two Skorne approaches.
        // Keep tally history separate from the per-stage death/abort checkpoint.
        if (!m_world->isTower()) {
            runtime.resultsCheckpoint = member.resultsCheckpoint;
        }
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
                               scale, spreads, assistedAim(index, direction), std::nullopt,
                               m_players[index].cursorAiming);
    }
}

std::optional<Vec3> PlayScene::assistedAim(usize index, const Vec3& direction) {
    // Auto Aim off (the original's per-pad control option): shots, the familiar's spit and
    // the standing attack keep the way the player faces; touch and melee reach are unaffected.
    if (m_services->config != nullptr && !m_services->config->combat.autoAim) {
        return std::nullopt;
    }
    // Retail PlayerGetTarget/ModifyPlayerDpos retain target-height correction
    // independently of facing adjustment. Cursor aiming only replaces the latter.
    return m_attacks.aim(m_players[index].actor, direction, attackTargets(),
                         m_players[index].cursorAiming);
}

std::optional<std::filesystem::path> PlayScene::seatFigureDirectory(s32 seat) const {
    for (const PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == seat && runtime.figure != nullptr) {
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

void PlayScene::setPartyCountOverride(std::optional<s32> count) {
    m_sim.partyCountOverride =
        count.has_value() ? std::optional{std::clamp(*count, 1, kPlayerCount)} : std::nullopt;
    m_opponents.setPartyCount(m_sim.partyCountOverride);
    if (m_open) {
        m_fixtures.setPlayerCount(partyCount(static_cast<s32>(m_players.size())));
    }
}

s32 PlayScene::partyCount(s32 present) const {
    return m_sim.partyCountOverride.has_value()
               ? std::clamp(*m_sim.partyCountOverride, 1, kPlayerCount)
               : present;
}

/** The mechanics follow the settings as they stand, so one switched in the menu answers from
 * the next tick; online every peer reads the same agreed rules. */
void PlayScene::syncMechanics() {
    m_mechanics.configure(m_services->config != nullptr ? m_services->config->game
                                                        : GameOptionsConfig{});
}

/** A run of close blows is praised once it pauses (player.c 2553), unless the boss's wizard
 * has begun his visit (good_wiz_state past 2). */
void PlayScene::praiseStreaks(f32 seconds) {
    const bool wizard = m_bossSequence.victory().state().stage() >= BossVictory::Stage::Appearing;
    for (PlayerRuntime& runtime : m_players) {
        // The record follows the run as it grows, so the meter can announce it live.
        s32& best = runtime.actor.save().progress().lifetime.bestStreak;
        best = std::max(best, runtime.streak.count());
        const f32 quiet =
            m_mechanics.quietWindow(runtime.streak.count()).value_or(MeleeStreak::kOriginalQuiet);
        const std::string_view praise = runtime.streak.step(seconds, quiet);
        if (!praise.empty() && !wizard && m_audio.narrationRoom(MeleeStreak::kWait)) {
            const auto audioScope =
                m_audio.soundScope({34, 0, 1, static_cast<u8>(runtime.actor.player())}, 2);
            m_audio.queueNarration(praise, LevelSoundscape::Narrator::Primary);
        }
    }
}

void PlayScene::drawMechanicsHud() {
    std::array<SeatRunView, kPlayerCount> runs{};
    usize watched = 0;
    for (const PlayerRuntime& runtime : m_players) {
        const s32 seat = runtime.actor.player();
        if (seat < 0 || seat >= kPlayerCount ||
            ((static_cast<u32>(m_localSeats) >> static_cast<u32>(seat)) & 1U) == 0 ||
            runtime.departed) {
            continue;
        }
        runs[watched++] = {.player = seat,
                           .count = runtime.streak.count(),
                           .quiet = runtime.streak.quiet(),
                           .running = runtime.streak.running(),
                           .record = runtime.actor.save().progress().lifetime.bestStreak};
    }
    m_mechanics.drawHud({.canvas = m_canvas,
                         .text = m_messages.text(),
                         .glow = m_glowSheet,
                         .strings = m_services->strings,
                         .runs = std::span(runs).first(watched)});
}

PartyPickups::Services PlayScene::pickupServices() {
    return {.world = *m_world,
            .fixtures = m_fixtures,
            .presentation = m_pickupRouting,
            .audio = m_audio,
            .classes = m_classes,
            .sounds = m_services->sounds,
            .help = [this](s32 id, usize index) { return postHelp(id, index); },
            .openMessage = [this](std::string_view name,
                                  usize page) { return openMessage(name, page); },
            .challengeCoin = [this](usize item) { collectChallengeCoin(item); },
            .autoActivateItems =
                m_services->config == nullptr || m_services->config->combat.autoActivateItems};
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

CharacterSave* PlayScene::liveSave(s32 player) {
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            return &runtime.actor.save();
        }
    }
    return nullptr;
}

void PlayScene::restat(s32 player) {
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            runtime.actor.restat(m_classes.stats(runtime.actor.save().character));
        }
    }
}

bool PlayScene::relocate(const Vec3& position) {
    constexpr f32 kSpacing = 2.0f; ///< between those put down, so nobody stands in another
    constexpr f32 kLift = 1.0f;    ///< put down from just above, to settle onto the floor
    if (m_world == nullptr) {
        return false;
    }
    std::vector<CameraSubject> subjects;
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.life != PlayerLife::Standing) {
            continue;
        }
        const auto apart = static_cast<f32>(subjects.size()) * kSpacing;
        runtime.actor.place(position + Vec3{apart, kLift, 0.0f});
        runtime.actor.settle(m_world->collision());
        runtime.fixtureSpot.reset();
        subjects.push_back(CameraSubject{runtime.actor.position(), runtime.actor.followPoint(),
                                         runtime.actor.height() * 0.5f});
    }
    if (subjects.empty()) {
        return false;
    }
    m_camera.reset(subjects, m_world->cameraMarkers(), m_world->cameraRange(), cameraView());
    resetSeatCameras(subjects, m_world->cameraMarkers(), m_world->cameraRange());
    if (auto* imported = m_world->imported()) {
        imported->resetCamera(subjects);
    }
    return true;
}

void PlayScene::setCheats(s32 player, const ConsoleCheats& cheats) {
    if (player < 0 || player >= kPlayerCount) {
        return;
    }
    m_sim.cheats[static_cast<usize>(player)] = cheats;
    m_opponents.setUnseen(player, cheats.noTarget);
    for (PlayerRuntime& runtime : m_players) {
        if (runtime.actor.player() == player) {
            runtime.ghost = cheats.noclip;
        }
    }
}

const PlayScene::ConsoleCheats& PlayScene::cheats(s32 player) const {
    static constexpr ConsoleCheats kNone{};
    return player < 0 || player >= kPlayerCount ? kNone : m_sim.cheats[static_cast<usize>(player)];
}

void PlayScene::setViewOverride(const std::optional<WorldCamera>& camera) {
    m_viewOverride = camera;
}

bool PlayScene::spawnObject(const LevelRef& level, std::string_view name, const Vec3& position,
                            f32 yaw) {
    if (m_device == nullptr) {
        return false;
    }
    const Mat4 transform =
        portable::rotate(glm::translate(Mat4{1.0f}, position), yaw, Vec3{0.0f, 1.0f, 0.0f});
    return m_spawned.spawn(*m_device, m_services->unpackedRoot, level, name, transform);
}

ItemArchive* PlayScene::borrowItems(const LevelRef& level) {
    for (auto& [name, archives] : m_borrowedItems) {
        if (name == level.name) {
            return &archives->primary();
        }
    }
    auto archives = std::make_unique<LevelItemArchives>();
    if (!archives->load(m_services->unpackedRoot, level)) {
        return nullptr;
    }
    m_borrowedItems.emplace_back(level.name, std::move(archives));
    return &m_borrowedItems.back().second->primary();
}

bool PlayScene::borrowOpponentSounds(const LevelInfo& source, const LevelAudioInfo* audio) {
    if (m_world == nullptr) {
        return false;
    }
    m_opponents.borrowRoster(source.enemies);
    return audio != nullptr && !audio->bank.empty() &&
           m_audio.borrowBank(m_services->unpackedRoot / "audio" / audio->bank);
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
    return {m_opponents,
            m_fixtures,
            m_fixtureRouting,
            m_attackRouting,
            m_services->config != nullptr ? m_services->config->multiplayer.mode
                                          : MultiplayerMode::Normal,
            m_players,
            m_bossSequence.occupiedHand(),
            &m_mechanics};
}

void PlayScene::updateFixtures(s32 ticks, f32 seconds) {
    m_fixtures.update(ticks, seconds, m_players, m_fixtureRouting);
    const CameraFrame ear = CameraFrame::of(presentedCamera());
    const Vec3 attention = bossCameraOn() ? m_sim.bossCamera.attention() : m_camera.attention();
    for (const usize index : m_fixtures.traps().wakes()) {
        const auto& trap = m_fixtures.traps().trap(index);
        if (trap.subtype == Traps::kBlades) {
            const auto audioScope = m_audio.soundScope({36, index, 1}, 2);
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
    m_fixtures.blast(position, radius, damage, m_players, m_fixtureRouting);
}
void PlayScene::settleBlasts() {
    m_fixtures.settleBlasts(m_players, m_fixtureRouting);
}
/** A level gained heals and announces immediately; tower promotions award the new appearance. */
void PlayScene::updateLevels() {
    for (usize i = 0; i < m_players.size(); ++i) {
        CharacterSave& save = m_players[i].actor.save();
        const auto change =
            m_sim.levels.observe(m_players[i].actor.player(), experienceLevel(save.experience()));
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
    if (m_bossSequence.advanceVictory(ticks, seconds, m_players, m_hud.strings()) &&
        !m_sim.leaving) {
        m_sim.destination = LevelRef::tower();
        m_sim.leaving = true;
        m_transition.comeUp();
    }
}

void PlayScene::updateEnemies(s32 ticks, f32 seconds) {
    watchOpponents();
    advanceOpponents(ticks, seconds);
    m_sim.opponentsAdvanced = seconds > 0;
    for (const Vec3& position : m_world->takeWorldExplosions()) {
        m_fixtures.worldExplosion(position, m_players, m_fixtureRouting);
    }
    for (const RockHit& hit : m_opponents.takeRockHits()) {
        if (auto* imported = m_world->imported(); imported && hit.rock >= kImportedArenaBase) {
            imported->strike(static_cast<s32>(hit.rock), hit.damage, 0, -1);
        } else {
            m_fixtures.strikeSafeRock(hit.rock, hit.damage);
        }
    }
    for (const PickupBlastReach& blast : m_opponents.takePickupBlasts()) {
        m_fixtures.blastPickups(blast.position, blast.radius, blast.damage, blast.flags, m_players,
                                m_fixtureRouting);
    }
    // A golem's or gargoyle's blow on a barrel is no one's (CritterCollideItems).
    const std::vector<CombatantRam> rams = m_opponents.takeBarrelRams();
    for (const CombatantRam& ram : rams) {
        m_fixtures.strikeBarrel(static_cast<usize>(ram.id), ram.damage, -1, m_players,
                                m_fixtureRouting);
    }
    if (!rams.empty()) {
        settleBlasts();
    }
}

/** What the enemies count as on screen is what the view shows, and the placed ones it comes
 * to see stand. */
void PlayScene::watchOpponents(bool standPlaced) {
    CameraView view = cameraView();
    const WorldCamera camera = simulationCamera();
    Vec3 attention = bossCameraOn() ? m_sim.bossCamera.attention() : m_camera.attention();
    if (const auto* imported = m_world->imported()) {
        attention = imported->cameraAttention().value_or(attention);
        view.horizontalFov = imported->horizontalFov(view.horizontalFov);
    }
    if (m_sim.switchCutscene.showing()) {
        // TriggerCamUpdate uses the shot's eye as gCameras[0].attn for placement range.
        attention = camera.position;
    }
    const auto volume = ViewVolume::of(camera, view.horizontalFov, view.aspect);
    if (auto* imported = m_world->imported()) {
        imported->watch(volume);
    }
    if (soloViewActive() && !scriptedCamera()) {
        // Under solo cameras what any standing seat's camera sees is on screen, a generator
        // judged against the camera looking its way.
        ViewSet views;
        for (usize i = 0; i < m_players.size(); ++i) {
            const s32 seat = m_players[i].actor.player();
            if (isDown(i) || seat < 0 || seat >= kPlayerCount) {
                continue;
            }
            views.add(ViewVolume::of(cameraOfSeat(seat), view.horizontalFov, view.aspect),
                      seatCamera(seat).attention());
        }
        if (views.count > 0) {
            m_opponents.watch(views, standPlaced);
            return;
        }
    }
    m_opponents.watch(volume, attention, standPlaced);
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
    m_sim.gameOver.begin(GameOver::captionFor(m_hud.strings(), m_services->strings));
    m_audio.stopCues();
    log::info("The game is being quit; game over");
}

/** Ordinary stages return to the tower, except the Temple/Underworld boss approaches.
 * Those, tower routes and secret challenges use their available authored destination. */
bool PlayScene::leaveBy(usize portal) {
    const ExitPortals::Portal& exit = m_portals.portal(portal);
    if (!m_world->isTower() && !exit.secret && !m_world->ref().continuesToBoss()) {
        m_sim.destination = LevelRef::tower();
        log::info("Portal {}: back to the tower", exit.tag);
        return true;
    }
    const bool reachable = exit.destination.has_value() &&
                           LevelCatalog::unpacked(m_services->unpackedRoot, *exit.destination);
    if (reachable) {
        m_sim.destination = *exit.destination;
        log::info("Portal {}: on to {} ({})", exit.tag, m_sim.destination.name,
                  m_sim.destination.title);
        return true;
    }
    if (m_sim.refusedPortal != static_cast<s32>(portal)) {
        m_sim.refusedPortal = static_cast<s32>(portal);
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
    auto party = PartyRecords::members(m_players);
    if (m_world != nullptr && m_world->isTower()) {
        for (auto& member : party) {
            member.resultsCheckpoint.reset();
        }
    }
    return party;
}

std::vector<PartyMember> PlayScene::abandonedParty(std::span<const PartyMember> party) const {
    return PartyRecords::abandoned(m_players, party);
}

void PlayScene::updateSumnerVisit(f32 seconds) {
    const std::optional<s32> player = TowerWelcome::visitorOf(m_world->triggers(), m_players);
    if (m_sumnerVisit.visit(seconds, player, m_sumner.loaded(), m_messages.text(),
                            m_services->config, m_services->strings)) {
        m_sumner.play(SumnerFigure::kWelcomeIndex);
    }
}

/** The scene routes the scroll owner's input and applies its sound/gesture cues. */
void PlayScene::updateHints(const Inputs& inputs, s32 ticks) {
    if (m_world->imported()) {
        updateImportedDialogue(inputs, ticks);
        return;
    }
    const auto player = static_cast<usize>(std::max(m_sumnerVisit.owner(), 0));
    const PlayInput input = player < inputs.size() ? inputs[player] : PlayInput{};
    const HintMenuEvent event =
        m_sumnerVisit.updateNative(*m_device, static_cast<s32>(player), input.menu, ticks,
                                   input.hintTopic, input.hintRevision);
    switch (event.kind) {
    case HintMenuEvent::Kind::Moved: m_audio.playNamed(kMenuMoveSound); break;
    case HintMenuEvent::Kind::Asked: {
        m_audio.playNamed(kMenuSelectSound);
        std::vector<ClassProgress> party;
        party.reserve(m_players.size());
        for (const PlayerRuntime& runtime : m_players) {
            party.push_back(runtime.actor.save().progress());
        }
        m_sumnerVisit.answer(event.topic, m_messages.text(), m_services->strings,
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

PlayInput PlayScene::captureNativeInput(s32 player, PlayInput input) const {
    input.hintTopic = 0;
    input.hintRevision = 0;
    if (!m_open || player < 0 || player != m_sumnerVisit.owner() || !m_sumnerVisit.active()) {
        return input;
    }
    MenuInput pointer = input.menu;
    if (pointer.pointerNormalized) {
        if (!m_presentedCanvas) {
            return input;
        }
        pointer = mapMenuPointer(std::move(pointer), *m_presentedCanvas);
    }
    input.menu.back |= pointer.pointerBack;
    input.hintTopic = m_sumnerVisit.captureTopic(player, pointer);
    if (input.hintTopic != 0) {
        input.hintRevision = m_sumnerVisit.menu().revision();
    }
    return input;
}

/** A bit per party member whose player pressed their button this frame. */
u32 PlayScene::acceptedPlayers(const Inputs& inputs) const {
    u32 accepted = 0;
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        const auto player = static_cast<usize>(actor.player());
        if (player < inputs.size() &&
            (inputs[player].menu.buttonPressed || inputs[player].menu.select ||
             inputs[player].menu.back || inputs[player].menu.start ||
             inputs[player].menu.pointerPressed || inputs[player].menu.pointerBack)) {
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
    return step(deltaSeconds, inputs, {}).outcome;
}

PlayScene::LocalStep PlayScene::step(f64 deltaSeconds, const Inputs& inputs,
                                     std::span<const ConsoleAction> actions) {
    if (m_audio.recordingSounds()) {
        throw std::logic_error("Recorded scene audio requires explicit tick and output ownership");
    }
    // Offline play owns no timeline/ledger. These labels are only meaningful to
    // explicit recordStep callers; every fresh offline journal dispatches once.
    auto recorded = advanceStep(1, 1, deltaSeconds, inputs, actions, ConsolePlay::Offline);
    if (m_world && m_services->sounds && m_world->imported()) {
        const Vec3 listener =
            m_players.empty() ? simulationCamera().position : m_players.front().actor.position();
        m_world->imported()->playSounds(*m_services->sounds, listener);
        if (const auto duck = m_world->imported()->ambientMusicDuck()) {
            m_audio.duckAmbientMusic(duck->seconds, duck->scale);
        }
    }
    for (const auto& event : recorded.journal.events) {
        const auto kind = event.policy == OutputLedger::Policy::Confirmed
                              ? OutputLedger::ActionKind::Commit
                              : OutputLedger::ActionKind::Present;
        dispatchOutput({kind, event});
    }
    return {recorded.outcome, std::move(recorded.consoleReplies)};
}

PlayScene::RecordedStep PlayScene::recordStep(u64 epoch, u64 tick, f64 deltaSeconds,
                                              const Inputs& inputs,
                                              std::span<const ConsoleAction> actions) {
    return advanceStep(epoch, tick, deltaSeconds, inputs, actions, ConsolePlay::Shared);
}

PlayScene::RecordedStep PlayScene::advanceStep(u64 epoch, u64 tick, f64 deltaSeconds,
                                               const Inputs& inputs,
                                               std::span<const ConsoleAction> actions,
                                               ConsolePlay play) {
    if (epoch == 0 || tick == 0) {
        throw std::invalid_argument("Recorded native output needs a nonzero epoch and tick");
    }
    if (m_outputFrame != nullptr) {
        throw std::logic_error("A native output step cannot be nested");
    }
    if (m_audio.recordingSounds() && m_recordedAudio.epoch() != epoch) {
        throw std::invalid_argument("Recorded native audio belongs to another loaded epoch");
    }
    RecordedStep result;
    OutputFrame frame{epoch, tick, &result.journal};
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): aggregate scope guard
    struct ResetCollector {
        OutputFrame*& collector;
        ~ResetCollector() { collector = nullptr; }
    } const reset{m_outputFrame};
    m_outputFrame = &frame;
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions): aggregate scope guard
    struct AbortSoundCapture {
        SceneSoundEvents& events;
        ~AbortSoundCapture() { events.abortTick(); }
    } const abortSounds{m_recordedAudio};
    if (m_audio.recordingSounds()) {
        m_recordedAudio.beginTick(tick);
        const auto camera = CameraFrame::of(presentedCamera());
        m_audio.prepareRecordedFrame({camera.position, camera.right});
    }
    {
        const auto sharedAudio = m_audio.soundScope({30, 0, 1, FrameEventEmitter::kNoSeat}, 1);
        for (const auto& action : actions) {
            result.consoleReplies.push_back({action.seat, applyConsoleAction(action, play)});
        }
        if (m_sim.consoleTravel) {
            result.outcome = PlayOutcome::Travel;
        } else {
            advanceConsoleFlight(deltaSeconds, inputs);
            Inputs gameplayInputs = inputs;
            for (s32 seat = 0; seat < kPlayerCount; ++seat) {
                if (cheats(seat).noclip) {
                    gameplayInputs[static_cast<usize>(seat)] = {};
                }
            }
            result.outcome = advanceNative(deltaSeconds, gameplayInputs);
        }
        if (m_open) {
            // Point lights are derived from native state. Refresh even when this peer
            // does not draw a frame; rendering must not decide what a tick observes.
            gatherLights();
        }
        if (m_audio.recordingSounds()) {
            gatherRecordedAudio(inputs, result.outcome);
        }
    }
    if (m_audio.recordingSounds()) {
        result.audio = m_recordedAudio.endFrame();
    }
    return result;
}

bool PlayScene::beginRecordedAudio(u64 epoch) {
    if (!m_open || epoch == 0 || m_outputFrame != nullptr || m_audio.recordingSounds() ||
        !m_audio.canAttachRecorder() || m_bossSequence.hasLegacyAudio() ||
        m_opponents.hasLegacyAudio() || m_triggerCues.turningSound() != kNoSound ||
        std::ranges::any_of(m_players, [](const PlayerRuntime& player) {
            return player.deathHeldCry != kNoSound || player.deathHeldSuck != kNoSound;
        })) {
        return false;
    }
    m_recordedAudio.open(epoch, m_services->sounds);
    m_audio.recordTo(&m_recordedAudio);
    return true;
}

void PlayScene::endRecordedAudio() {
    if (m_outputFrame != nullptr) {
        throw std::logic_error("Cannot detach scene audio during a native tick");
    }
    m_recordedAudio.close();
    m_audio.recordTo(nullptr);
}

bool PlayScene::restartRecordedAudio(u64 epoch) {
    endRecordedAudio();
    m_recordedAudio.discard();
    return beginRecordedAudio(epoch);
}

SceneSoundEvents::Result
PlayScene::reconcileRecordedAudio(u64 firstTick, std::span<const SceneSoundEvents::Frame> frames,
                                  std::optional<u64> newHead) {
    return m_recordedAudio.reconcileFrames(firstTick, frames, newHead);
}

SceneSoundEvents::Result PlayScene::confirmRecordedAudio(u64 allSeatsThrough) {
    return m_recordedAudio.confirm(allSeatsThrough);
}

void PlayScene::dispatchRecordedAudio(const SceneSoundEvents::Result& result) {
    m_recordedAudio.dispatch(result);
}

void PlayScene::serviceRecordedAudio(f32 seconds) {
    m_audio.servicePresentation(seconds);
}

void PlayScene::gatherRecordedAudio(const Inputs& inputs, PlayOutcome outcome) {
    if (!m_open || m_world == nullptr || outcome != PlayOutcome::Running) {
        return; // The complete empty set retracts loops when a terminal branch wins.
    }
    std::vector<Vec3> listeners;
    std::vector<PortalVisitor> visitors;
    std::optional<Vec3> hourglass;
    for (usize index = 0; index < m_players.size(); ++index) {
        const auto& runtime = m_players[index];
        if (runtime.departed || runtime.life != PlayerLife::Standing) {
            continue;
        }
        const auto& actor = runtime.actor;
        listeners.push_back(actor.position());
        const auto player = static_cast<usize>(actor.player());
        visitors.push_back({actor.position(), actor.radius(), static_cast<s32>(index),
                            player >= inputs.size() || !inputs[player].move.any()});
        if (!hourglass && ((PowerupEffects::of(actor.save().progress().inventory).special &
                            powerup::kStopTime) != 0u)) {
            hourglass = actor.position();
        }
    }
    const auto frame = CameraFrame::of(presentedCamera());
    const auto* level = m_world->level();
    m_audio.gatherLoops(
        listeners, {frame.position, frame.right}, level != nullptr ? level->soundVolume : 1.0f,
        m_sim.switchCutscene.active() || m_ceremonies.promotion().active() || relicCeremonyOn(),
        &m_world->scene(), m_sim.leaving ? std::nullopt : m_portals.flamePosition(visitors),
        hourglass);
    TriggerCues::gatherAudio(*m_world, m_audio);
    m_bossSequence.gatherAudio();
    m_attacks.gatherLoops();
    m_opponents.gatherLoops();
}

void PlayScene::emitOutput(OutputPayload payload) {
    if (m_outputFrame == nullptr) {
        // Debug harm/blasts may be invoked outside update. Preserve their direct
        // offline feedback without retaining any caller-owned output collector.
        if (const auto* rumble = std::get_if<RumbleOutput>(&payload)) {
            if (m_links.haptics != nullptr) {
                m_links.haptics->vibrate(rumble->player, rumble->frames, rumble->feedback);
            }
        } else if (const auto* unlock = std::get_if<UnlockClassesOutput>(&payload)) {
            if (m_links.unlocks != nullptr) {
                m_links.unlocks->unlockClasses(unlock->mask);
            }
        }
        return;
    }
    OutputLedger::Event event{{}, OutputLedger::Policy::OneShot, payload, {}};
    event.id.epoch = m_outputFrame->epoch;
    event.id.tick = m_outputFrame->tick;
    // The supplied epoch identifies this loaded, frozen party. These emitters
    // are seats/shared progress, not compacted enemy indexes or pointer values.
    event.id.emitter.generation = 1;
    if (const auto* rumble = std::get_if<RumbleOutput>(&event.payload)) {
        event.id.emitter.nameSpace = 1; // player feedback
        event.id.emitter.entity = static_cast<u64>(rumble->player);
        event.id.emitter.seat = static_cast<u8>(rumble->player);
        event.id.cue = static_cast<u32>(rumble->feedback) + 1;
        event.policy = OutputLedger::Policy::OneShot;
    } else {
        event.id.emitter.nameSpace = 2; // shared class availability
        event.id.cue = 1;
        event.policy = OutputLedger::Policy::Confirmed;
    }
    for (const auto& prior : m_outputFrame->journal->events) {
        if (prior.id.emitter == event.id.emitter && prior.id.cue == event.id.cue) {
            ++event.id.occurrence;
        }
    }
    m_outputFrame->journal->events.push_back(event);
}

bool PlayScene::dispatchOutput(const OutputLedger::Action& action) const {
    if (!m_open || !action.event.id.valid() || action.event.loop) {
        return false;
    }
    if (const auto* rumble = std::get_if<RumbleOutput>(&action.event.payload)) {
        if (action.kind != OutputLedger::ActionKind::Present ||
            action.event.policy != OutputLedger::Policy::OneShot || rumble->player < 0 ||
            rumble->player >= kPlayerCount) {
            return false;
        }
        if (m_links.haptics != nullptr) {
            m_links.haptics->vibrate(rumble->player, rumble->frames, rumble->feedback);
        }
    } else if (const auto* unlock = std::get_if<UnlockClassesOutput>(&action.event.payload)) {
        if (action.kind != OutputLedger::ActionKind::Commit ||
            action.event.policy != OutputLedger::Policy::Confirmed) {
            return false;
        }
        if (m_links.unlocks != nullptr) {
            m_links.unlocks->unlockClasses(unlock->mask);
        }
    }
    return true;
}

PlayOutcome PlayScene::advanceNative(f64 deltaSeconds, const Inputs& inputs) {
    m_sim.movementUpdated = false;
    if (!m_open) {
        return PlayOutcome::Running;
    }
    const auto [ticks, seconds] = beginTick(deltaSeconds);
    // Each hold below owns the whole tick while it lasts, in this order of precedence.
    if (m_sim.gameOver.active()) {
        return advanceGameOver(ticks, seconds);
    }
    if (m_sim.switchCutscene.active()) {
        updateSwitchCutscene(ticks, seconds);
        return PlayOutcome::Running;
    }
    // A scroll holds everything else still until it has burnt away; the welcome's leads on
    // to the crystals. Leaving one burns it to the options menu's note and cuts off whatever
    // Sumner was saying over it.
    if (m_messages.active()) {
        advanceScroll(ticks, inputs);
        return PlayOutcome::Running;
    }
    // Gone through a portal, the party is out of play while the transition picture comes
    // up over the level, which goes on around it; once it covers the view they travel.
    m_transition.update(seconds);
    m_ceremonies.relics().animate(seconds);
    if (m_sim.leaving) {
        return advanceDeparture(ticks, seconds, inputs);
    }
    // Sumner's scroll of hints holds play the same way, while he goes on moving behind it.
    if (m_sumnerVisit.active()) {
        advanceHintScroll(ticks, seconds, inputs);
        return PlayOutcome::Running;
    }
    // Materialising, the party stands still, playing its entrance, while the level runs on
    // around it and the start camera holds, then rides in; the title slides up until the
    // ride, when it sits.
    if (spawning()) {
        advanceEntrance(ticks, seconds, inputs);
        return PlayOutcome::Running;
    }
    if (m_ceremonies.promotion().active() && !m_sim.welcome.cutting()) {
        updatePromotion(ticks, seconds);
        return PlayOutcome::Running;
    }
    if (relicCeremonyOn()) {
        updateTowerRelics(ticks, seconds);
        return PlayOutcome::Running;
    }
    return advancePlay(ticks, seconds, inputs);
}

PlayScene::TickClock PlayScene::beginTick(f64 deltaSeconds) {
    syncMechanics();
    PartyFigures::snapshot(m_players);
    m_effects.capturePresentation();
    m_world->capturePresentation();
    m_fixtures.capturePresentation();
    m_ceremonies.relics().capturePresentation();
    m_arrival.capturePresentation();
    m_transporters.capturePresentation();
    m_sumner.capturePresentation();
    m_ceremonies.promotion().capturePresentation();
    m_bossSequence.capturePresentation();
    m_sim.opponentsAdvanced = false;
    m_sim.projectilesAdvanced = false;
    m_previousCamera = scriptedCamera() ? std::nullopt : std::optional{simulationCamera()};
    m_previousSeatView =
        soloViewActive() && !scriptedCamera() ? std::optional{presentedCamera()} : std::nullopt;
    m_previousBossCamera = bossCameraOn();
    // Application supplies fixed simulation ticks independently of rendered frames.
    const f32 tickRate = m_services->config != nullptr
                             ? static_cast<f32>(m_services->config->timing.tickRate)
                             : 60.0f;
    const auto ticks =
        std::clamp(static_cast<s32>(std::lround(deltaSeconds * tickRate)), kMinTicks, kMaxTicks);
    const f32 seconds = static_cast<f32>(ticks) / tickRate;
    // Item availability counts joined players, including those waiting in the tower (or
    // the console's party count); whole-party switch contact separately counts only the
    // standing visitors.
    const s32 joinedPlayers = partyCount(static_cast<s32>(std::ranges::count_if(
        m_players, [](const PlayerRuntime& player) { return !player.departed; })));
    m_world->setPlayerCount(joinedPlayers);
    m_audio.setPlayerCount(joinedPlayers);
    m_audio.updateNarration(seconds);
    m_ceremonies.advanceSpeech(seconds);
    m_bossSequence.advanceAudio(seconds);
    // The music's areas: the boss waking asks for the second, the zones for theirs.
    m_audio.bossAwake(m_opponents.bosses().view().awake);
    m_audio.updateMusic(seconds);
    // Once the good wizard appears the announcer keeps quiet (good_wiz_state past two).
    const BossVictory::Stage victory = m_bossSequence.victory().state().stage();
    m_audio.holdNarration(victory != BossVictory::Stage::None &&
                          victory != BossVictory::Stage::Waiting);
    m_hud.stepRelics(ticks);
    m_hud.stepHourglass(seconds, m_players);
    // Entry presentation also holds the names, preserving their full time after the camera ride.
    m_names.step(m_players, ticks,
                 spawning() || m_sim.gameOver.active() || m_sim.switchCutscene.active() ||
                     m_messages.active() || m_sim.welcome.cutting() || m_sumnerVisit.active() ||
                     m_ceremonies.promotion().active() || relicCeremonyOn());
    return {ticks, seconds};
}

PlayOutcome PlayScene::advanceGameOver(s32 ticks, f32 seconds) {
    // The world remains behind the caption, but no player input, portal,
    // reward or victory ceremony can restart the finished session.
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_portals.animate(seconds);
    m_effects.update(seconds);
    updateAmbience();
    if (m_sim.gameOver.step(ticks)) {
        m_audio.narrate(GameOver::kVoice, LevelSoundscape::Narrator::Primary);
    }
    return m_sim.gameOver.finished() ? PlayOutcome::GameOver : PlayOutcome::Running;
}

void PlayScene::advanceScroll(s32 ticks, const Inputs& inputs) {
    const LevelMessages::Cues cues = m_messages.step(ticks, acceptedPlayers(inputs));
    if (cues.stopVoice) {
        m_audio.stopVoice();
    }
    if (cues.burnSound) {
        m_audio.playNamed(kScrollBurnSound);
    }
    if (!m_messages.active()) {
        m_sim.welcome.scrollClosed(m_world->layout(), m_sumner);
    }
}

PlayOutcome PlayScene::advanceDeparture(s32 ticks, f32 seconds, const Inputs& inputs) {
    // DoExit uses state4, outside msgUpdate's active-reader states1/2/3/5.
    // Dismiss the last lesson instead of freezing its timer through departure.
    m_hud.help().clear();
    auto* imported = m_world->imported();
    const bool nativeDeparture = imported != nullptr && imported->departure().has_value();
    // Departing survivors cannot act, but native Legends continues their
    // current animation tree while its separate spin/shrink clock runs.
    // Another player's death and tower choice still run until all can leave.
    bool dying = false;
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerRuntime& runtime = m_players[i];
        if (runtime.life != PlayerLife::Dying) {
            if (nativeDeparture && runtime.life == PlayerLife::Standing && runtime.figure) {
                runtime.figure->animateDeparture(seconds);
            }
            continue;
        }
        if (runtime.figure != nullptr) {
            runtime.figure->animate(0.0f, ticks, seconds, PlayerDeed::Die);
        }
        if (runtime.figure == nullptr || runtime.figure->animator().dead()) {
            runtime.life = PlayerLife::InTower;
            perform(i, PartyMotion::Action::Fallen);
        } else {
            dying = true;
        }
    }
    answerTowerPrompts(inputs);
    if (!nativeDeparture) {
        m_departure.update(ticks);
    }
    m_portals.animate(seconds);
    if (!nativeDeparture && m_departure.started()) {
        std::vector<CameraSubject> subjects;
        for (usize i = 0; i < m_players.size(); ++i) {
            if (!isDown(i)) {
                const auto& actor = m_players[i].actor;
                subjects.push_back({actor.position() + m_departure.displacement(),
                                    actor.followPoint() + m_departure.displacement(),
                                    actor.height() * 0.5f});
            }
        }
        m_camera.update(subjects, m_world->cameraMarkers(), m_world->cameraRange(), cameraView(),
                        seconds);
        if (m_sim.soloCameras) {
            followSeatCameras(partySubjects(m_departure.displacement()), seconds);
        }
        if (imported) {
            imported->followCamera(subjects, seconds);
        }
    }
    if (!nativeDeparture && m_departure.finished()) {
        m_transition.comeUp();
    }
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    bool nativeCovered = false;
    if (nativeDeparture) {
        if (const auto native = imported->departure(); native && native->finished) {
            if (!native->whiteFade) {
                m_transition.comeUp();
            }
            nativeCovered = native->covered(m_transition.covering());
        }
    }
    m_effects.update(seconds);
    updateAmbience();
    return (nativeDeparture ? nativeCovered : m_transition.covering()) && !dying
               ? PlayOutcome::Travel
               : PlayOutcome::Running;
}

void PlayScene::advanceHintScroll(s32 ticks, f32 seconds, const Inputs& inputs) {
    updateHints(inputs, ticks);
    m_sumner.update(seconds);
    if (m_world->imported()) {
        m_world->update(seconds, false);
        updateAmbience();
    }
}

void PlayScene::advanceEntrance(s32 ticks, f32 seconds, const Inputs& inputs) {
    // The entrance holds the enemies still: nothing breeds and nothing fights until the
    // party is in play. world_update's earlyenemies (the Mountain's Peak) lifts only
    // fn_80060114's gate, so there the placed enemies the camera comes to see stand.
    const LevelInfo* level = m_world->level();
    watchOpponents(level != nullptr && level->earlyEnemies);
    m_arrival.animate(seconds);
    for (const PlayerRuntime& runtime : m_players) {
        const std::unique_ptr<PlayerFigure>& figure = runtime.figure;
        if (figure != nullptr) {
            figure->animate(0.0f, ticks, seconds);
        }
    }
    m_bossSequence.advanceLegend(0, m_opponents.bosses(), m_players);
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_opponents.armBoss(seconds);
    // An imported level's authored opening pan runs while the same party
    // materialises. It must not wait behind GDL's entrance presentation.
    if (auto* imported = m_world->imported()) {
        const auto subjects = partySubjects(Vec3{0});
        imported->followCamera(subjects, seconds);
    }
    holdImportedParty();
    updateAmbience();
    const bool cameraHandoff = m_arrival.camera().active();
    // Ride to the camera that will actually take over. A boss entrance must not
    // approach the normal level camera and then cut to a different fight view.
    const bool bossView = bossCameraOn();
    m_arrival.advance(ticks, anyButton(inputs),
                      bossView ? m_sim.bossCamera.camera().position : m_camera.camera().position,
                      bossView ? m_sim.bossCamera.attention() : m_camera.attention());
    if (m_arrival.takeTitleLanded()) {
        if (const LevelInfo* level = m_world->level(); level != nullptr && !m_world->isTower()) {
            m_audio.announceTitle(level->flags);
        }
    }
    if (!spawning()) {
        if (cameraHandoff && m_arrival.camera().mode() == StartCamera::Mode::Standard) {
            std::vector<CameraSubject> subjects;
            for (const auto& player : m_players) {
                if (player.life == PlayerLife::Standing) {
                    subjects.push_back({player.actor.position(), player.actor.followPoint(),
                                        player.actor.height() * 0.5f});
                }
            }
            const auto everyone = partySubjects(Vec3{0.0f});
            for (s32 i = 0; i < StartCamera::kWarmSteps; ++i) {
                m_camera.update(subjects, m_world->cameraMarkers(), m_world->cameraRange(),
                                cameraView(), 1.0f / TowerCamera::kStepRate);
                if (m_sim.soloCameras) {
                    followSeatCameras(everyone, 1.0f / TowerCamera::kStepRate);
                }
                // Native imported camera clocks already advance above;
                // GDL's follow-camera warm-up must not fast-forward them.
            }
        }
        m_sim.welcome.arrived(*m_device, m_messages, m_services->strings, m_world->layout(),
                              m_sumner);
    }
}

PlayOutcome PlayScene::advancePlay(s32 ticks, f32 seconds, const Inputs& inputs) {
    const bool welcomeHeld = m_sim.welcome.hold(ticks);
    const bool held = welcomeHeld || importedOpening();
    if (!held && m_world->isTower()) {
        m_sim.towerNoticeSeconds += seconds;
        if (m_sim.towerNoticeSeconds >= 3.0f &&
            PartyPickups::announceTowerUnlock(m_players, pickupServices())) {
            m_sim.towerNoticeSeconds = 0;
            return PlayOutcome::Running;
        }
    }
    if (!held && updateChallenge(seconds)) {
        m_sim.destination = LevelRef::tower(); // the application restores the parent when present
        m_sim.secretTravel = true;
        return PlayOutcome::Travel;
    }
    advancePowerupsAndWorld(ticks, seconds, held);
    std::vector<CameraSubject> subjects = advancePartyMotion(ticks, seconds, inputs, held);
    advanceCombat(ticks, seconds, held);
    if (!held) {
        advanceImportedLevel(seconds, subjects, inputs);
    } else {
        holdImportedParty();
    }
    advanceTriggers(seconds);
    if (m_sim.switchCutscene.active()) {
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
    if (!held && !m_sim.leaving) {
        if (const auto outcome = advancePortals(ticks, seconds, inputs)) {
            return *outcome;
        }
    }
    advanceCameras(seconds, subjects);
    advanceRuneMeter();
    updateAmbience();
    return PlayOutcome::Running;
}

void PlayScene::advancePowerupsAndWorld(s32 ticks, f32 seconds, bool held) {
    // The clock itself runs once the party and its opponents have had their turns
    // (advanceCombat). A decoy let loose now spends its slot by the boss as the last update left
    // it, the party's turn coming first here.
    const auto clock = powerupClockOf(held, *m_world, m_opponents.bosses().view());
    for (const PowerupEnding& ending : PlayerPowerups::wear(m_players, seconds, clock)) {
        // Separate semantic endings: removing Pojo's cue must not renumber the
        // same player's levitation cue, or another player's ending.
        const u32 cue = ending.sound == "S_UNPOJO"         ? 1
                        : ending.sound == "S_UNGROW"       ? 2
                        : ending.sound == "S_LEVITATEDOWN" ? 3
                                                           : 0;
        const auto audioScope = m_audio.soundScope(
            {45, 0, 1, static_cast<u8>(m_players[ending.partyIndex].actor.player())}, cue);
        m_audio.playNamed(ending.sound);
    }
    praiseStreaks(seconds);
    if (m_device != nullptr) {
        PartyFigures::greetGems(*m_device, m_players, m_world->powerups(), m_effects);
        PartyFigures::updateDecoys(*m_device, m_players, m_world->powerups(), seconds, m_effects);
    }
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_opponents.syncFloors();
    m_transporters.syncFloors(m_world->collision());
    m_world->revealCrystals(seconds);
    m_hud.pickups().step(ticks, seconds);
    m_sumner.update(seconds);
    for (auto& player : m_players) {
        if (player.figure != nullptr) {
            const auto* stats = m_classes.stats(player.actor.save().character);
            player.figure->setGuardArmor(
                stats != nullptr ? armorDefense(*stats, player.actor.save()) : 0.0f);
            player.figure->setCompanionPowerups(*m_device, m_world->powerups(),
                                                player.actor.save().progress().inventory,
                                                &m_weapons);
        }
    }
    updateTransporters(ticks, seconds, held);
}

std::vector<CameraSubject> PlayScene::advancePartyMotion(s32 ticks, f32 seconds,
                                                         const Inputs& inputs, bool held) {
    m_sim.movementUpdated = !held;
    std::vector<PlayerActor> importedBefore;
    std::vector<bool> importedCaptured;
    if (m_world->imported()) {
        importedBefore.reserve(m_players.size());
        importedCaptured.reserve(m_players.size());
        for (const auto& player : m_players) {
            importedBefore.push_back(player.actor);
            importedCaptured.push_back(player.capture.active());
        }
    }
    const f32 sharedYaw = m_world->imported() ? simulationCamera().yaw
                          : bossCameraOn()    ? m_sim.bossCamera.yaw()
                                              : m_camera.yaw();
    const std::vector<f32> yaws = movementYaws(sharedYaw);
    std::vector<CameraSubject> subjects = stepParty(inputs, held, yaws, ticks, seconds);
    if (auto* imported = m_world->imported()) {
        for (usize i = 0; i < m_players.size(); ++i) {
            auto& player = m_players[i];
            if (player.departed || player.life != PlayerLife::Standing || player.ghost ||
                player.combo.riding || importedCaptured[i] || player.capture.active() ||
                player.transport.active()) {
                // A release may land during this step. Capture owns its collision;
                // don't sweep from its former hand pose as ordinary locomotion.
                continue;
            }
            auto& actor = player.actor;
            if (!held) {
                imported->touchScenery(actor.player(), importedBefore[i].position(),
                                       actor.position(), actor.radius(), actor.height());
            }
            const Vec3 corrected = imported->resolveMovement(
                importedBefore[i].position(), actor.position(), actor.radius(), actor.height());
            if (glm::distance(corrected, actor.position()) > .00001f ||
                !std::isfinite(corrected.x) || !std::isfinite(corrected.y) ||
                !std::isfinite(corrected.z)) {
                correctImportedSceneryStep(actor, importedBefore[i], m_world->collision(),
                                           corrected, seconds);
            }
            subjects[i] = {actor.position(), actor.followPoint(), actor.height() * .5f};
        }
    }
    return subjects;
}

void PlayScene::advanceCombat(s32 ticks, f32 seconds, bool held) {
    m_sim.playSeconds += seconds;
    if (!held) {
        PartyRecords::advanceTime(m_players, seconds);
    }
    m_sim.shake.update(ticks);
    m_hud.help().update(ticks);
    updateFixtures(ticks, seconds);
    if (!held) {
        updateHazardSurfaces(seconds);
        if (const auto beat = PlayerHealth::heartbeat(m_players, ticks, m_world->isTower())) {
            m_audio.playNamed(PlayerHealth::kHeartbeatSound, beat->volume);
        }
    }
    if (m_world->imported() == nullptr || !held) {
        updateEnemies(ticks, seconds);
    }
    m_attacks.updateProjectiles(seconds, m_players, attackTargets());
    m_sim.projectilesAdvanced = seconds > 0;
    m_attacks.updateStrikes(seconds, m_players, attackTargets());
    m_attacks.updateShields(seconds, m_players, attackTargets());
    m_attacks.updateArmour(seconds, m_players, attackTargets());
    // PlayerProcessPowerups takes a slot's flags into its frame in the pass that runs its time
    // down, nought or not, and sets it aside only after (0x8007ce94-0x8007d070), so a powerup
    // works through the update its time runs out in: the clock runs once the party has acted
    // and its opponents have answered, before what it picks up. A boss level's reads the boss as
    // this update left it, retail's do_enemies coming before do_players (0x80054994, 0x8005499c):
    // its timers run three times as fast from the update the encounter begins in until the one
    // its death has played in.
    PlayerPowerups::runClock(m_players, seconds,
                             powerupClockOf(held, *m_world, m_opponents.bosses().view()));
    if (auto* imported = m_world->imported()) {
        imported->updateGenerators(m_opponents.importedGeneratorStates());
    }
    // The level goes dark for the legend item's rite, as for a great move.
    if (m_opponents.bosses().legend().darkens()) {
        m_sim.dimmer.ask(LegendRite::kDarkening);
        // The bearer shines through the rite, the rest of the party less (player.c 2508).
        for (PlayerRuntime& runtime : m_players) {
            runtime.glow.raise(runtime.actor.player() == m_opponents.bosses().legend().player()
                                   ? BodyGlow::kBearer
                                   : BodyGlow::kOthers);
        }
    }
    m_sim.dimmer.update(seconds);
    m_world->setAmbientOffset(m_sim.dimmer.offset());
    m_effects.update(seconds);
    if (m_device != nullptr) {
        m_sim.pickups.collect(*m_device, m_players, pickupServices());
    }
}

void PlayScene::advanceTriggers(f32 seconds) {
    m_world->updateTriggers(seconds, visitors(), false, occupiedFloors());
    m_fixtures.syncFloors();
    m_opponents.syncFloors();
    m_transporters.syncFloors(m_world->collision());
    handleTriggerEvents();
}

std::optional<PlayOutcome> PlayScene::advancePortals(s32 ticks, f32 seconds, const Inputs& inputs) {
    // A portal waits for everyone still on their feet.
    std::vector<PortalVisitor> standing;
    standing.reserve(m_players.size());
    for (usize i = 0; i < m_players.size(); ++i) {
        if (!isDown(i) && !m_players[i].ghost) {
            const auto player = static_cast<usize>(m_players[i].actor.player());
            standing.push_back(PortalVisitor{
                m_players[i].actor.position(), m_players[i].actor.radius(), static_cast<s32>(i),
                player >= inputs.size() || !inputs[player].move.any()});
        }
    }
    // With everyone fallen, the last death played out and the announcer done, the party
    // goes back to the tower as it came into the level (game_main sends a party of the
    // dead or waiting to the tower, where PlayerRestoreState stands them again). The
    // defeat caption is for a game that is being quit, not for a wiped party.
    if (GameOver::ready(m_players) && m_audio.narrationBacklog() <= 0.0) {
        m_sim.destination = LevelRef::tower();
        m_audio.stopCues();
        log::info("All players have fallen; back to the tower");
        return PlayOutcome::Travel;
    }
    const std::optional<usize> reached = m_portals.update(ticks, seconds, standing);
    const CameraFrame ear = CameraFrame::of(presentedCamera());
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
        // The leaving branch no longer updates waiting visitors. Release their
        // activation loop before S_TUNNEL takes over for the descent.
        m_audio.updateExitFlame(std::nullopt, {});
        settleOpponentRewards();
        if (m_portals.portal(*portal).secret) {
            m_sim.secretTravel = true;
            m_sim.secretReturnPosition = m_portals.portal(*portal).departurePosition.value_or(
                m_portals.portal(*portal).position);
            m_portals.consume(*portal);
            return PlayOutcome::Travel;
        }
        m_sim.leaving = true;
        m_departure.begin(*m_device, m_weapons.textures);
        m_audio.playNamed(PortalDeparture::kSound, PortalDeparture::kVolume);
    }
    return std::nullopt;
}

void PlayScene::advanceCameras(f32 seconds, const std::vector<CameraSubject>& subjects) {
    // The camera keeps to those still standing, while anyone is.
    std::vector<CameraSubject> followed;
    for (usize i = 0; i < subjects.size(); ++i) {
        if (!isDown(i)) {
            followed.push_back(subjects[i]);
        }
    }
    if (const LevelInfo* level = m_world->level();
        level != nullptr && level->bossCamera.has_value() && bossCameraOn()) {
        m_sim.bossCamera.update(bossSubject(), followed.empty() ? subjects : followed,
                                *level->bossCamera, cameraView(), seconds,
                                m_world->cameraMarkers());
    } else {
        m_camera.update(followed.empty() ? subjects : followed, m_world->cameraMarkers(),
                        m_world->cameraRange(), cameraView(), seconds);
    }
    if (m_sim.soloCameras) {
        followSeatCameras(subjects, seconds);
    }
    if (auto* imported = m_world->imported()) {
        const auto& boss = m_opponents.bosses();
        const auto view = boss.view();
        if (view.alive && boss.position()) {
            imported->trackBoss(bossSubject());
        } else if (view.awake) {
            imported->trackBoss(std::nullopt);
        }
        imported->followCamera(followed.empty() ? subjects : followed, seconds);
    }
}

void PlayScene::advanceRuneMeter() {
    if (m_sim.runeItem) {
        const auto& items = m_world->placedItems();
        const bool available = *m_sim.runeItem < items.size() && !items.item(*m_sim.runeItem).taken;
        const auto cue = m_sim.runeMeter.update(m_camera.attention(), available);
        if (cue != RuneMeter::Cue::None) {
            const auto frame = CameraFrame::of(presentedCamera());
            m_audio.announceRune(cue == RuneMeter::Cue::Nearby, m_camera.attention(),
                                 {frame.position, frame.right});
        }
    }
}

/** Places the level's loops for the party, heard from the camera. */
void PlayScene::updateAmbience() {
    std::vector<Vec3> listeners;
    listeners.reserve(m_players.size());
    for (const PlayerRuntime& runtime : m_players) {
        // DistanceToClosestPlayer hears only state 1. damage_player changes a
        // dying player to state 8 immediately; waiting/quit slots are not ears.
        if (!runtime.departed && runtime.life == PlayerLife::Standing) {
            listeners.push_back(runtime.actor.position());
        }
    }
    const CameraFrame frame = CameraFrame::of(presentedCamera());
    const LevelInfo* level = m_world->level();
    m_audio.updateAmbience(listeners, AmbientEar{frame.position, frame.right},
                           level != nullptr ? level->soundVolume : 1.0f,
                           m_sim.switchCutscene.active() || m_ceremonies.promotion().active() ||
                               relicCeremonyOn(),
                           &m_world->scene());
    std::optional<Vec3> hourglass;
    for (const auto& runtime : m_players) {
        if (!runtime.departed && runtime.life == PlayerLife::Standing &&
            (PowerupEffects::of(runtime.actor.save().progress().inventory).special &
             powerup::kStopTime) != 0) {
            hourglass = runtime.actor.position();
            break;
        }
    }
    m_audio.updateHourglass(hourglass, {frame.position, frame.right});
    m_audio.updateMusicAreas(listeners, &m_world->scene());
}

/** The party as the level's triggers see it. */
std::vector<TriggerVisitor> PlayScene::visitors() const {
    std::vector<TriggerVisitor> out;
    for (usize i = 0; i < m_players.size(); ++i) {
        if (isDown(i) || m_players[i].ghost) {
            continue;
        }
        const PlayerActor& actor = m_players[i].actor;
        TriggerVisitor visitor;
        // PlayerMotion sweeps items at the step's destination before the walls clip it, so
        // a lever whose handle stands in a wall (the Fissure's trap-wall lever is a hand's
        // breadth inside its rock line) is pulled from a width out.
        visitor.position = actor.stepAim();
        // Pressure pads share fn_8005F0F4's item-contact radius: PDAT.width,
        // not the smaller footprint used by the remake's movement collision.
        visitor.radius = actor.reach();
        visitor.height = actor.height();
        visitor.crystals = actor.save().progress().crystals;
        visitor.gargoylePieces = actor.save().progress().relics.gargoylePieces;
        visitor.sumner = actor.save().character == kSumnerClass;
        visitor.party = static_cast<s32>(i);
        // The original tests a pad's floor against the object the player rides
        // (floor_name2), which at a lift's deck edge is not always what a fresh probe
        // beneath the feet finds.
        if (m_players[i].floor.object >= 0) {
            visitor.floorObject = m_players[i].floor.object;
        } else if (const auto floor = m_world->collision().floorAt(
                       visitor.position, 0.5f, 1.0f, 0.0f, WorldCollision::kPlayerSide,
                       WorldCollision::Prober::Player)) {
            visitor.floorObject = floor->object;
        }
        out.push_back(visitor);
    }
    return out;
}

std::vector<s32> PlayScene::occupiedFloors() const {
    std::vector<s32> out;
    for (const auto& visitor : visitors()) {
        if (visitor.floorObject >= 0) {
            out.push_back(visitor.floorObject);
        }
    }
    const auto& enemies = m_opponents.enemies();
    for (s32 enemy = 0; enemy < enemies.most(); ++enemy) {
        if (enemies.alive(enemy)) {
            const auto state = enemies.observe(enemy);
            if (state && !state->asleep) {
                if (const s32 floor = enemies.floorOf(enemy); floor >= 0) {
                    out.push_back(floor);
                } else if (const auto contact = m_world->collision().floorAt(
                               enemies.positionOf(enemy), 0.1f, 0.1f)) {
                    // floorOf retains moving-floor attachment only. Static fade
                    // bridges still support a body (retail's floor_wobj); use
                    // rememberFloor's feet-contact range, never a floor below a jump.
                    out.push_back(contact->object);
                }
            }
        }
    }
    // The great ones stand on a bridge as the swarm does, and keep it up the same way.
    const auto& critters = m_opponents.critters();
    for (s32 critter = 0; static_cast<usize>(critter) < critters.count(); ++critter) {
        if (!critters.alive(critter)) {
            continue;
        }
        if (const auto contact =
                m_world->collision().floorAt(critters.positionOf(critter), 0.1f, 0.1f)) {
            out.push_back(contact->object);
        }
    }
    return out;
}

/** This frame's point lights (DoLighting clears them; ProcessEffects and do_players add
 * them again): the effects' own and, in a dark level, a lantern over each standing player; the
 * newest first, at most twelve. */
void PlayScene::gatherLights() {
    PartyLighting::gather(m_lights, m_effects, m_opponents, m_players, m_world->level());
    if (const auto* imported = m_world->imported()) {
        const auto native = imported->pointLights();
        m_lights.insert(m_lights.begin(), native.begin(), native.end());
        if (m_lights.size() > WorldLighting::kMostPoints) {
            m_lights.resize(WorldLighting::kMostPoints);
        }
    }
    m_world->setPointLights(m_lights);
}

PartyFigures::Scene PlayScene::figureScene(f32 frameBlend) {
    return {.world = *m_world,
            .weapons = m_weapons,
            .departure = m_departure,
            .frameBlend = frameBlend,
            .occupiedHand = m_bossSequence.occupiedHand()};
}

void PlayScene::drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye, f32 frameBlend,
                            f32 opponentBlend) {
    PartyFigures::drawShadows(device, m_players, figureScene(frameBlend), clip, eye);
    m_opponents.enemies().drawShadows(device, clip, eye, m_world->lighting(), opponentBlend);
    m_opponents.critters().drawShadows(device, clip, eye, m_world->lighting(), opponentBlend);
    m_opponents.bosses().drawShadow(device, clip, eye, m_world->lighting(), opponentBlend);
}

bool PlayScene::cinematicBars() const {
    return m_sim.welcome.cutting() || (m_ceremonies.promotion().active() && !ceremoniesWaiting()) ||
           relicCeremonyOn() || m_sim.switchCutscene.showing();
}

void PlayScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                       f32 frameHeight, bool optionsOpen, f32 frameBlend) {
    if (!m_open || m_world == nullptr || m_services->config == nullptr) {
        return;
    }
    const GameConfig& config = *m_services->config;
    m_messages.prepare(device);
    m_sumnerVisit.prepare(device);
    gatherLights();
    const f32 effectBlend = optionsOpen ? -1.0f : frameBlend;
    const f32 opponentBlend = m_sim.opponentsAdvanced ? effectBlend : -1.0f;
    const f32 projectileBlend = m_sim.projectilesAdvanced ? effectBlend : -1.0f;
    frameBlend = optionsOpen || frameBlend < 0 ? 1.0f : std::clamp(frameBlend, 0.0f, 1.0f);
    const WorldCamera currentCamera = viewCamera();
    const std::optional<WorldCamera>& previousView = previousViewCamera();
    WorldCamera camera = cameraContinuous() && previousView
                             ? currentCamera.interpolate(*previousView, frameBlend)
                             : currentCamera;
    if (!m_viewOverride && !scriptedCamera()) {
        if (const auto* imported = m_world->imported()) {
            if (const auto sampled = imported->camera(effectBlend)) {
                // The native camera owns its interpolation history. Sampling
                // it and interpolating m_previousCamera again adds one tick of lag.
                const f32 distance =
                    std::max(1.0f, glm::distance(sampled->position, m_camera.attention()));
                camera =
                    m_sim.shake.apply(*sampled, sampled->position + sampled->forward() * distance);
            }
        }
    }
    const f32 horizontalFov =
        m_world->imported() ? m_world->imported()->horizontalFov(config.horizontalFovRadians())
                            : config.horizontalFovRadians();
    const Mat4 clip = camera.clipTransform(horizontalFov, frameWidth, frameHeight, frameProjection);
    m_presentedClip = clip;
    m_presentedView = camera;
    m_presentedWorldAlpha = effectBlend;
    const CameraFrame companionCamera = CameraFrame::of(camera);
    const SelectionHighlight highlight{camera.position};
    const auto nativeDeparture =
        m_world->imported() ? m_world->imported()->departure(frameBlend) : std::nullopt;
    const SceneFog previousFog = device.sceneFog();
    device.setSceneFog(nativeDeparture && nativeDeparture->fog ? *nativeDeparture->fog
                                                               : SceneFog{});
    markSelection(&highlight);
    m_world->drawOpaque(device, clip, camera, effectBlend);
    m_spawned.draw(device, clip, m_world->lighting());
    m_ceremonies.relics().draw(device, clip, m_world->lighting(), camera, effectBlend);
    m_sumner.draw(device, clip, m_world->lighting(), effectBlend);
    m_figures.draw(device, m_players, figureScene(frameBlend), clip, companionCamera);
    m_portals.draw(device, clip, m_world->lighting(), &companionCamera,
                   TreeModel::Pass::DepthWriting);
    m_transporters.draw(device, clip, m_world->lighting(), effectBlend);
    const CameraFrame effectCamera = companionCamera;
    m_fixtures.draw(device, clip, m_world->lighting(), &effectCamera, effectBlend);
    m_opponents.statues().draw(device, clip, m_world->lighting(), &effectCamera);
    const bool importedOpponentsReady = !importedOpening();
    if (importedOpponentsReady) {
        m_opponents.importedActors().draw(device, clip, m_world->lighting(), &effectCamera,
                                          TreeModel::Pass::DepthWriting, opponentBlend);
    }
    m_opponents.generators().draw(device, clip, m_world->lighting(), opponentBlend,
                                  TreeModel::Pass::Opaque);
    m_opponents.enemies().draw(device, clip, m_world->lighting(), m_figures.hitFlash(), &m_weapons,
                               &effectCamera, opponentBlend, TreeModel::Pass::DepthWriting);
    m_opponents.critters().draw(device, clip, m_world->lighting(), nullptr, &effectCamera,
                                opponentBlend, m_services->config->combat.enemyHealthBars);
    // The boss stands out in the level's own light while the rite darkens the rest.
    m_opponents.bosses().draw(device, clip,
                              m_opponents.bosses().legend().darkens() ? m_world->fullLighting()
                                                                      : m_world->lighting(),
                              m_bossSequence.frozenTexture(), opponentBlend);
    // A generator is an item: its alpha-textured body (the vermin's skull pile, the war
    // dogs' tent) composites by its texture's alpha after the solids, as the sorted
    // pickups do, and before the veils that hang over it.
    m_opponents.generators().draw(device, clip, m_world->lighting(), opponentBlend,
                                  TreeModel::Pass::Blended);
    // An invisible or teleporting body writes no depth, so it goes down after the
    // generators and enemies it may stand before, blending over them.
    m_figures.draw(device, m_players, figureScene(frameBlend), clip, companionCamera,
                   PartyFigures::Bodies::Translucent);
    if (config.display.ambientOcclusion) {
        AmbientOcclusion occlusion;
        occlusion.clipToView = camera.view() * glm::inverse(clip);
        device.applyAmbientOcclusion(occlusion);
    }
    // Floor shadows belong below translucent pickups and scenery, even when those
    // authored materials write depth. Otherwise a nearer cloud rejects the shadow.
    drawShadows(device, clip, camera.position, frameBlend, opponentBlend);
    m_world->drawDeferred(device, clip, camera, effectBlend);
    if (importedOpponentsReady) {
        m_opponents.importedActors().draw(device, clip, m_world->lighting(), &effectCamera,
                                          TreeModel::Pass::Effects, opponentBlend);
    }
    // GHO's native 0xC01880 sorts its additive, non-depth-writing body after scenery.
    // A fence blended over it instead makes even a nearer ghost appear behind the fence.
    m_opponents.enemies().draw(device, clip, m_world->lighting(), m_figures.hitFlash(), &m_weapons,
                               &effectCamera, opponentBlend, TreeModel::Pass::Effects);
    if (!m_mechanics.empty()) {
        // Their shows run on frames, not ticks: presentation, never replayed.
        const std::vector<SwarmBodyView> swarm = m_opponents.swarmBodies();
        m_mechanics.drawWorld({.device = device,
                               .clip = clip,
                               .camera = effectCamera,
                               .paused = optionsOpen,
                               .bodies = swarm,
                               .weapons = &m_weapons});
    }
    m_portals.draw(device, clip, m_world->lighting(), &effectCamera, TreeModel::Pass::Effects);
    m_fixtures.drawEffects(device, clip, m_world->lighting(), &effectCamera, effectBlend);
    PartyFigures::drawEffects(device, m_players, figureScene(frameBlend), clip, effectCamera);
    // The wizards add onto the frame without writing depth, so the translucent scenery behind
    // them (the portals' horizon sheets) must be down first or it paints over them.
    if (!ceremoniesWaiting()) {
        m_ceremonies.promotion().draw(device, clip, m_world->lighting(), effectBlend);
    }
    if (relicCeremonyOn()) {
        m_ceremonies.relics().drawWizard(device, clip, m_world->lighting(), camera, effectBlend);
    }
    // Neither the wizard nor the boss's key effect writes depth; the original sorts both by
    // distance, so whichever stands nearer the camera is drawn over the other.
    const auto drawVictoryWizard = [&] {
        m_bossSequence.victory().drawWizard(device, clip, m_world->lighting(), &effectCamera,
                                            effectBlend);
    };
    const bool wizardNearer =
        m_bossSequence.victory().state().wizardShown() &&
        glm::distance(camera.position, m_bossSequence.victorySubject().position) <
            glm::distance(camera.position, m_bossSequence.shardPosition());
    if (!wizardNearer) {
        drawVictoryWizard();
    }
    m_opponents.missiles().draw(device, clip, m_world->lighting(), &effectCamera);
    m_arsenal.missiles().draw(device, clip, m_world->lighting(), &effectCamera, projectileBlend);
    const auto bossAttachments = m_opponents.presentedBossEffects(opponentBlend);
    m_effects.draw(device, clip, m_world->fullLighting(), &effectCamera, effectBlend,
                   bossAttachments);
    if (wizardNearer) {
        drawVictoryWizard();
    }
    m_arrival.drawEffects(device, clip, m_world->lighting(), effectBlend);
    if (const auto& selection = m_picking.selection();
        selection.has_value() && selection->kind == SceneSelection::Kind::Object) {
        if (const auto* imported = m_world->imported()) {
            imported->highlightObject(device, clip, camera, effectBlend, selection->id, highlight);
        } else {
            m_world->scene().drawHighlight(device, clip, static_cast<usize>(selection->id),
                                           highlight);
        }
    }
    markSelection(nullptr);
    device.setSceneFog(previousFog);
    if (config.display.bloom) {
        device.applyBloom();
    }
    if (config.display.depthOfField) {
        DepthOfField blur;
        const Mat4 view = camera.view();
        blur.clipToView = view * glm::inverse(clip);
        // This is an optional port effect, not retail camera behavior. Keep the entire
        // party (and nearby combat) sharp, not just player one's focal plane.
        for (const auto& player : m_players) {
            if (!player.departed && player.life == PlayerLife::Standing) {
                const Vec3 position =
                    player.previous.continuous
                        ? glm::mix(player.previous.position, player.actor.position(), frameBlend)
                        : player.actor.position();
                const f32 distance = (view * Vec4{position, 1.0f}).z;
                blur.focusEnd = std::max(blur.focusEnd, distance + player.actor.height() * 3.0f);
            }
        }
        blur.transition = std::max(20.0f, blur.focusEnd);
        device.applyDepthOfField(blur);
    }
    if (config.camera.compass && !optionsOpen && !m_sim.gameOver.active() && !m_hudHidden) {
        m_compass.draw(device, clip, camera, horizontalFov, frameWidth / frameHeight,
                       m_world->lighting());
    }
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    const Mat4 canvasProjection =
        makeVirtualScreenTransform(frameProjection, width, height, frameWidth, frameHeight);
    m_presentedCanvas = canvasProjection;
    m_canvas.begin(device, canvasProjection);
    if (m_sim.gameOver.active()) {
        m_sim.gameOver.draw(m_canvas, m_messages.text(), width);
        m_canvas.end();
        return;
    }
    // The welcome's cut is letterboxed the way the original's trigger cameras are: black
    // bars top and bottom, the status boxes hidden beneath the lower one.
    const bool cut = cinematicBars();
    const auto drawNativeCover = [&] {
        if (!nativeDeparture || !nativeDeparture->whiteFade) {
            return;
        }
        const f32 opacity = std::isfinite(*nativeDeparture->whiteFade)
                                ? std::clamp(*nativeDeparture->whiteFade, 0.0f, 1.0f)
                                : 0;
        const auto alpha = static_cast<u8>(opacity * 255);
        m_canvas.fillScreen(Color::white().withAlpha(alpha));
    };
    if (!nativeDeparture || !nativeDeparture->whiteFade) {
        m_transition.draw(m_canvas, width, height); // over the view, under the boxes
    }
    if (m_hudHidden) {
        drawNativeCover();
        m_canvas.end(); // the developer console asked for the view alone
        return;
    }
    // Rendering can precede the first simulation tick, before PartyNames has observed the hold.
    if (!spawning()) {
        m_names.draw(m_canvas, m_players, clip, canvasProjection, frameBlend);
    }
    if (!cut && !optionsOpen && !m_viewOverride) {
        // The way to each companion off this view, over the world and under the boxes.
        m_arrows.draw(m_canvas, m_players, arrowOwnSeats(), camera, clip, canvasProjection, width,
                      height, static_cast<f32>(StatusBoxPainter::kBarY), frameBlend);
    }
    if (!cut) {
        m_hud.drawStatus(m_canvas, m_players, !m_sim.fixedRoster);
        if (m_runeFrame != nullptr && m_runeColumn != nullptr) {
            m_sim.runeMeter.draw(m_canvas, *m_runeFrame, *m_runeColumn);
        }
        if (!m_hud.drawHourglass(m_canvas, m_players) &&
            m_sim.challenge.state() != SecretChallenge::State::Inactive) {
            m_challengeHud.draw(m_canvas, m_sim.challenge.remaining(), m_sim.challenge.duration(),
                                !spawning() && !m_messages.active());
        }
        m_opponents.meter().draw(m_canvas, device);
        m_bossSequence.victory().drawCaption(m_canvas, m_messages.text(), m_hud.strings(), width,
                                             height);
        drawMechanicsHud();
    }
    if (const auto* imported = m_world->imported()) {
        imported->drawEntranceTitle(m_canvas, width);
    } else if (const LevelInfo* level = m_world->level(); level != nullptr) {
        m_arrival.drawTitle(m_canvas, m_messages.text(), level->title, width);
    }
    if (cut) {
        drawCinematicBars(m_canvas, height);
    }
    if (!cut) {
        m_hud.drawSelectors(m_canvas, m_messages.text(), m_services->strings, m_players);
        m_hud.drawHelp(m_canvas, device, m_staticTextures, m_players, clip, canvasProjection, width,
                       height);
    }
    m_messages.draw(m_canvas);
    if (!ceremoniesWaiting()) {
        m_ceremonies.promotion().drawCaption(m_canvas, m_messages.text(), width, height);
    }
    if (relicCeremonyOn()) {
        m_ceremonies.relics().drawCaption(m_canvas, m_messages.text(), width, height);
    }
    m_sumnerVisit.draw(m_canvas, m_messages.text());
    drawNativeCover(); // Special departures cover the entire window, including the HUD.
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
    return m_open && !m_sim.leaving && !m_sim.gameOver.active() && !m_sim.switchCutscene.active() &&
           (m_world->imported() == nullptr || !m_sumnerVisit.active()) && !m_messages.active() &&
           !m_ceremonies.promotion().active() && !m_ceremonies.relics().active() &&
           actor(player) != nullptr && !fallen(player);
}

bool PlayScene::canJoin(s32 player) const {
    return m_open && m_world != nullptr && m_world->isTower() && actor(player) == nullptr &&
           m_players.size() < static_cast<usize>(kPlayerCount) && !m_sim.leaving &&
           !m_sim.gameOver.active() && !m_sim.switchCutscene.active() && !spawning() &&
           !m_messages.active() && !m_sumnerVisit.active() &&
           m_sim.welcome.intro() == Intro::None && !m_ceremonies.promotion().active() &&
           !relicCeremonyOn();
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

/** Stands the materialising effect at every character's feet and the optional start
 * camera at its arrival marker to hold and ride in; the party holds still for it. (The
 * realm's entering sound belongs to the loading screen, not to this.) A party an imported
 * level places itself keeps the hold without the effect or its sound. */
void PlayScene::beginSpawn(RenderDevice& device, const std::optional<WorldCamera>& marker) {
    if (!m_weapons.loaded()) {
        m_weapons.load(m_services->unpackedRoot / kWeaponsArchive);
    }
    std::vector<Vec3> positions;
    positions.reserve(m_players.size());
    std::vector<PlayerActor*> standing;
    std::optional<Vec3> low;
    Vec3 high{0};
    for (PlayerRuntime& runtime : m_players) {
        positions.push_back(runtime.actor.position());
        const Vec3 follow = runtime.actor.followPoint();
        high = low ? glm::max(high, follow) : follow;
        low = low ? glm::min(*low, follow) : follow;
        if (!runtime.departed && runtime.life == PlayerLife::Standing && !runtime.ghost) {
            standing.push_back(&runtime.actor);
        }
    }
    auto* imported = m_world->imported();
    const bool placed = imported != nullptr && imported->placeOpeningParty(standing);
    const std::span<const Vec3> materialising =
        placed ? std::span<const Vec3>{} : std::span<const Vec3>{positions};
    m_arrival.begin(device, m_weapons, materialising, marker,
                    bossCameraOn() ? StartCamera::Mode::Legacy : StartCamera::Mode::Standard,
                    low ? std::optional<Vec3>{(*low + high) * 0.5f} : std::nullopt);
    if (!positions.empty() && !placed) {
        m_audio.playEntrance();
    }
}

/** Opens one page of a scroll message over the tower: the party reads it and presses on. */
bool PlayScene::openMessage(std::string_view name, usize page) {
    return m_device != nullptr && m_messages.open(*m_device, name, m_services->strings, page);
}

/** Tells a refused party what a gate wants; a target opening before them (a gate's field, a
 * lift, a gate) sounds its slot's note until it is done, then the note of its end. */
/** A turntable grinds as it turns and thuds as it stops (fn_8009D7E4): stone in the castle,
 * metal in the mines, nothing elsewhere. */
void PlayScene::handleTriggerEvents() {
    m_triggerCues.handle(
        *m_world, m_audio, m_services->sounds, m_sim.switchCutscene,
        {.help = [this](s32 id, usize index) { return postHelp(id, index); },
         .openMessage = [this](std::string_view name,
                               usize page) { return openMessage(name, page); },
         .shake = [this] { m_sim.shake.start(CameraShake::Target::Attention, 0, 180, 0.1f, 100); },
         .helpAt =
             [this](s32 id, const Vec3& position) {
                 return m_hud.postHelp(id, 0, m_players, m_audio, -1, position);
             }});
}

} // namespace gdl::game
