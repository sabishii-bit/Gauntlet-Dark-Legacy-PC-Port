#include "game/screens/PlayScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/render/AmbientOcclusion.h"
#include "engine/render/DepthOfField.h"
#include "engine/world/WorldCamera.h"

#include "game/players/ClassData.h"
#include "game/players/NameCheats.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
#include "game/screens/PartyRecords.h"
#include "game/screens/PlayerPowerups.h"
#include "game/world/TargetAssist.h"

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

void PlayScene::pauseGameplaySounds() {
    m_audio.pauseAmbience();
    m_attacks.stopDeathSounds(m_players);
    m_opponents.stopDeathSound();
}

bool PlayScene::open(RenderDevice& device, const GameContext& context, LevelWorld& world,
                     std::span<const PartyMember> party, const PlayOptions& options) {
    close();
    m_context = context;
    m_device = &device;
    m_world = &world;
    // A character menu can replace the party without replacing the shared tower.
    // Rebuild its initial pickups and gates before applying the new party's progress.
    if ((!world.built() || world.isTower()) && !world.load(device, context.unpackedRoot)) {
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
    m_messages.setControlLabels(context.controlLabels);
    m_hud.setControlLabels(context.controlLabels);
    m_names.load(device, m_context.unpackedRoot, m_staticTextures);
    m_weapons.load(context.unpackedRoot / kWeaponsArchive);
    m_figures.loadSkins(device, world.powerups(), m_weapons);
    m_compass.bind(device, world.powerups());
    m_hud.bindHourglass(device, world.powerups(), context.stopTimeTotal);
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
    m_hud.stepHourglass(0, m_players);
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
    m_levels.clear();
    for (const PlayerRuntime& runtime : m_players) {
        const PlayerActor& actor = runtime.actor;
        m_levels.observe(actor.player(), experienceLevel(actor.save().experience()));
    }
    world.startTriggers(visitors(), &m_towerAccess);
    m_audio.bindAmbience(world.layout(), &world.scene());
    std::vector<CameraSubject> subjects;
    subjects.reserve(m_players.size());
    for (PlayerRuntime& runtime : m_players) {
        runtime.figure = PlayerFigure::load(device, m_context.unpackedRoot, runtime.actor.save());
        subjects.push_back(CameraSubject{runtime.actor.position(), runtime.actor.followPoint(),
                                         runtime.actor.height() * 0.5f});
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
    // Every tower spawn has its own entrance view, including returns to a realm's wing.
    const WorldLocator* arrival = world.arrivalPoint(options.arrivalWorld, &m_towerAccess);
    beginSpawn(device, options.position.has_value()
                           ? std::nullopt
                           : world.entranceCamera(arrival != nullptr ? arrival->next : 0));
    m_arsenal.bind({device, m_classes, m_weapons, world.collision(), m_effects, m_audio,
                    context.sounds, world.wallHitSound(), world.isTower(),
                    world.level() != nullptr && world.level()->bossType >= 0, &world.powerups(),
                    context.config != nullptr ? &context.config->multiplayer.mode : nullptr},
                   effectTextures);
    m_attacks.bind({device, m_classes, world, m_weapons, m_effects, m_audio, context.sounds,
                    m_arsenal, m_dimmer, &m_shake,
                    context.config != nullptr ? &context.config->multiplayer.mode : nullptr,
                    context.config != nullptr && context.config->difficulty.level == "easy"
                        ? TargetAssist::kEasyFacingDot
                        : TargetAssist::kFacingDot});
    m_bossSequence.bind(
        {device, world, m_weapons, m_staticTextures, m_effects, m_audio, context.levels});
    m_bossSequence.advanceLegend(0, m_opponents.bosses(), m_players);
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
    m_presentedClip.reset();
    m_previousCamera.reset();
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
    m_compass.clear();
    m_leaving = false;
    m_attacks.clear();
    m_arsenal.clear(); // before the figures whose models they fly
    m_bossSequence.clear();
    m_effects.clear(); // and before the archive whose trees they play
    m_opponents.close();
    m_opponentsAdvanced = false;
    m_projectilesAdvanced = false;
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
        m_arsenal.launchWeapon(
            m_players[index].actor, m_players[index].figure.get(), direction, scale, spreads,
            m_players[index].cursorAiming
                ? std::nullopt
                : m_attacks.aim(m_players[index].actor, direction, attackTargets()));
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
    return {m_opponents,
            m_fixtures,
            fixtureEvents(),
            m_context.config != nullptr ? m_context.config->multiplayer.mode
                                        : MultiplayerMode::Normal,
            m_players,
            [this](usize index, f32 damage, HurtKind kind, const PlayerImpact& impact) {
                hurt(index, damage, kind, true, impact);
            },
            m_bossSequence.occupiedHand()};
}

LevelFixtures::Events PlayScene::fixtureEvents() {
    return {
        .hurt = [this](usize i, f32 damage, HurtKind kind, bool directed,
                       const PlayerImpact& impact) { hurt(i, damage, kind, directed, impact); },
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
    m_opponentsAdvanced = seconds > 0;
    for (const Vec3& position : m_world->takeWorldExplosions()) {
        m_fixtures.worldExplosion(position, m_players, fixtureEvents());
    }
    for (const RockHit& hit : m_opponents.takeRockHits()) {
        m_fixtures.strikeSafeRock(hit.rock, hit.damage);
    }
    for (const PickupBlastReach& blast : m_opponents.takePickupBlasts()) {
        m_fixtures.blastPickups(blast.position, blast.radius, blast.damage, blast.flags, m_players,
                                fixtureEvents());
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
    const WorldCamera camera = viewCamera();
    Vec3 attention = bossCameraOn() ? m_bossCamera.attention() : m_camera.attention();
    if (m_switchCutscene.showing()) {
        // TriggerCamUpdate uses the shot's eye as gCameras[0].attn for placement range.
        attention = camera.position;
    }
    m_opponents.watch(ViewVolume::of(camera, view.horizontalFov, view.aspect), attention);
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
        .help = [this](s32 id, usize i) { return postHelp(id, i); },
        .blastScenery =
            [this](const PickupBlastReach& reach, std::vector<s32>& reached) {
                m_fixtures.blastScenery(reach.position, reach.radius, reach.damage, reach.flags,
                                        m_players, fixtureEvents(), reached);
            }};
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

/** Ordinary stages return to the tower, except the Temple/Underworld boss approaches.
 * Those, tower routes and secret challenges use their available authored destination. */
bool PlayScene::leaveBy(usize portal) {
    const ExitPortals::Portal& exit = m_portals.portal(portal);
    if (!m_world->isTower() && !exit.secret && !m_world->ref().continuesToBoss()) {
        m_destination = LevelRef::tower();
        log::info("Portal {}: back to the tower", exit.tag);
        return true;
    }
    const bool reachable = exit.destination.has_value() &&
                           LevelCatalog::unpacked(m_context.unpackedRoot, *exit.destination);
    if (reachable) {
        m_destination = *exit.destination;
        log::info("Portal {}: on to {} ({})", exit.tag, m_destination.name, m_destination.title);
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
    PartyFigures::snapshot(m_players);
    m_effects.capturePresentation();
    m_world->capturePresentation();
    m_fixtures.capturePresentation();
    m_towerRelics.capturePresentation();
    m_arrival.capturePresentation();
    m_transporters.capturePresentation();
    m_promotion.capturePresentation();
    m_bossSequence.capturePresentation();
    m_opponentsAdvanced = false;
    m_projectilesAdvanced = false;
    m_previousCamera = scriptedCamera() ? std::nullopt : std::optional{viewCamera()};
    m_previousBossCamera = bossCameraOn();
    // Application supplies fixed simulation ticks independently of rendered frames.
    const f32 tickRate =
        m_context.config != nullptr ? static_cast<f32>(m_context.config->timing.tickRate) : 60.0f;
    const auto ticks =
        std::clamp(static_cast<s32>(std::lround(deltaSeconds * tickRate)), kMinTicks, kMaxTicks);
    const f32 seconds = static_cast<f32>(ticks) / tickRate;
    // Item availability counts joined players, including those waiting in the tower;
    // whole-party switch contact separately counts only the standing visitors.
    const auto joinedPlayers = static_cast<s32>(std::ranges::count_if(
        m_players, [](const PlayerRuntime& player) { return !player.departed; }));
    m_world->setPlayerCount(joinedPlayers);
    m_audio.setPlayerCount(joinedPlayers);
    m_audio.updateNarration(seconds);
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
                 spawning() || m_gameOver.active() || m_switchCutscene.active() ||
                     m_messages.active() || m_welcome.cutting() || m_sumnerVisit.active() ||
                     m_promotion.active() || relicCeremonyOn());
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
        // DoExit uses state4, outside msgUpdate's active-reader states1/2/3/5.
        // Dismiss the last lesson instead of freezing its timer through departure.
        m_hud.help().clear();
        // Departing survivors are held, but another player's death and tower choice
        // still run until the party can leave together.
        bool dying = false;
        for (usize i = 0; i < m_players.size(); ++i) {
            PlayerRuntime& runtime = m_players[i];
            if (runtime.life != PlayerLife::Dying) {
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
        m_departure.update(ticks);
        m_portals.animate(seconds);
        if (m_departure.started()) {
            std::vector<CameraSubject> subjects;
            for (usize i = 0; i < m_players.size(); ++i) {
                if (!isDown(i)) {
                    const auto& actor = m_players[i].actor;
                    subjects.push_back({actor.position() + m_departure.displacement(),
                                        actor.followPoint() + m_departure.displacement(),
                                        actor.height() * 0.5f});
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
        return m_transition.covering() && !dying ? PlayOutcome::Travel : PlayOutcome::Running;
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
        m_bossSequence.advanceLegend(0, m_opponents.bosses(), m_players);
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
                        subjects.push_back({player.actor.position(), player.actor.followPoint(),
                                            player.actor.height() * 0.5f});
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
        PartyFigures::updateDecoys(*m_device, m_players, m_world->powerups(), seconds, m_effects);
    }
    m_world->update(seconds, PlayerPowerups::timeStopped(m_players));
    m_opponents.syncFloors();
    m_world->revealCrystals(seconds);
    m_hud.pickups().step(ticks, seconds);
    m_sumner.update(seconds);
    for (auto& player : m_players) {
        if (player.figure != nullptr) {
            const auto* stats = m_classes.stats(player.actor.save().character);
            player.figure->setGuardArmor(
                stats != nullptr ? armorDefense(*stats, player.actor.save().progress()) : 0.0f);
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
    m_projectilesAdvanced = seconds > 0;
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
    m_fixtures.syncFloors();
    m_opponents.syncFloors();
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
            // The leaving branch no longer updates waiting visitors. Release their
            // activation loop before S_TUNNEL takes over for the descent.
            m_audio.updateExitFlame(std::nullopt, {});
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
            m_audio.playNamed(PortalDeparture::kSound, PortalDeparture::kVolume);
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
        // DistanceToClosestPlayer hears only state 1. damage_player changes a
        // dying player to state 8 immediately; waiting/quit slots are not ears.
        if (!runtime.departed && runtime.life == PlayerLife::Standing) {
            listeners.push_back(runtime.actor.position());
        }
    }
    const CameraFrame frame = CameraFrame::of(viewCamera());
    const LevelInfo* level = m_world->level();
    m_audio.updateAmbience(listeners, AmbientEar{frame.position, frame.right},
                           level != nullptr ? level->soundVolume : 1.0f,
                           m_switchCutscene.active() || m_promotion.active() || relicCeremonyOn(),
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
        if (isDown(i)) {
            continue;
        }
        const PlayerActor& actor = m_players[i].actor;
        TriggerVisitor visitor;
        visitor.position = presenceOf(i);
        // Pressure pads share fn_8005F0F4's item-contact radius: PDAT.width,
        // not the smaller footprint used by the remake's movement collision.
        visitor.radius = actor.reach();
        visitor.height = actor.height();
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

void PlayScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                       f32 frameHeight, bool optionsOpen, f32 frameBlend) {
    if (!m_open || m_world == nullptr || m_context.config == nullptr) {
        return;
    }
    const GameConfig& config = *m_context.config;
    m_messages.prepare(device);
    m_sumnerVisit.prepare(device);
    gatherLights();
    const f32 effectBlend = optionsOpen ? -1.0f : frameBlend;
    const f32 opponentBlend = m_opponentsAdvanced ? effectBlend : -1.0f;
    const f32 projectileBlend = m_projectilesAdvanced ? effectBlend : -1.0f;
    frameBlend = optionsOpen || frameBlend < 0 ? 1.0f : std::clamp(frameBlend, 0.0f, 1.0f);
    const WorldCamera currentCamera = viewCamera();
    const WorldCamera camera =
        m_previousCamera && !scriptedCamera() && m_previousBossCamera == bossCameraOn()
            ? currentCamera.interpolate(*m_previousCamera, frameBlend)
            : currentCamera;
    const Mat4 clip = camera.clipTransform(config.horizontalFovRadians(), frameWidth, frameHeight,
                                           frameProjection);
    m_presentedClip = clip;
    const CameraFrame companionCamera = CameraFrame::of(camera);
    m_world->drawOpaque(device, clip, camera, effectBlend);
    m_towerRelics.draw(device, clip, m_world->lighting(), camera, effectBlend);
    m_sumner.draw(device, clip, m_world->lighting());
    m_figures.draw(device, m_players, figureScene(frameBlend), clip, companionCamera);
    m_portals.draw(device, clip, m_world->lighting(), &companionCamera,
                   TreeModel::Pass::DepthWriting);
    m_transporters.draw(device, clip, m_world->lighting(), effectBlend);
    const CameraFrame effectCamera = companionCamera;
    m_fixtures.draw(device, clip, m_world->lighting(), &effectCamera, effectBlend);
    m_opponents.statues().draw(device, clip, m_world->lighting(), &effectCamera);
    m_opponents.generators().draw(device, clip, m_world->lighting(), opponentBlend);
    m_opponents.enemies().draw(device, clip, m_world->lighting(), m_figures.hitFlash(), &m_weapons,
                               &effectCamera, opponentBlend, TreeModel::Pass::DepthWriting);
    m_opponents.critters().draw(device, clip, m_world->lighting(), nullptr, &effectCamera,
                                opponentBlend);
    // The boss stands out in the level's own light while the rite darkens the rest.
    m_opponents.bosses().draw(device, clip,
                              m_opponents.bosses().legend().darkens() ? m_world->fullLighting()
                                                                      : m_world->lighting(),
                              m_bossSequence.frozenTexture(), opponentBlend);
    if (config.display.ambientOcclusion) {
        AmbientOcclusion occlusion;
        occlusion.clipToView = camera.view() * glm::inverse(clip);
        device.applyAmbientOcclusion(occlusion);
    }
    m_world->drawDeferred(device, clip, camera, effectBlend);
    // GHO's native 0xC01880 sorts its additive, non-depth-writing body after scenery.
    // A fence blended over it instead makes even a nearer ghost appear behind the fence.
    m_opponents.enemies().draw(device, clip, m_world->lighting(), m_figures.hitFlash(), &m_weapons,
                               &effectCamera, opponentBlend, TreeModel::Pass::Effects);
    m_portals.draw(device, clip, m_world->lighting(), &effectCamera, TreeModel::Pass::Effects);
    m_fixtures.drawEffects(device, clip, m_world->lighting(), &effectCamera, effectBlend);
    drawShadows(device, clip, camera.position, frameBlend, opponentBlend);
    // The wizards add onto the frame without writing depth, so the translucent scenery behind
    // them (the portals' horizon sheets) must be down first or it paints over them.
    if (!spawning()) {
        m_promotion.draw(device, clip, m_world->lighting(), effectBlend);
    }
    if (relicCeremonyOn()) {
        m_towerRelics.drawWizard(device, clip, m_world->lighting(), camera, effectBlend);
    }
    m_bossSequence.victory().drawWizard(device, clip, m_world->lighting(), &effectCamera,
                                        effectBlend);
    m_opponents.missiles().draw(device, clip, m_world->lighting(), &effectCamera);
    m_arsenal.missiles().draw(device, clip, m_world->lighting(), &effectCamera, projectileBlend);
    m_effects.draw(device, clip, m_world->fullLighting(), &effectCamera, effectBlend);
    m_arrival.drawEffects(device, clip, m_world->lighting(), effectBlend);
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
    if (config.camera.compass && !optionsOpen && !m_gameOver.active()) {
        m_compass.draw(device, clip, camera, config.horizontalFovRadians(),
                       frameWidth / frameHeight, m_world->lighting());
    }
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    const Mat4 canvasProjection =
        makeVirtualScreenTransform(frameProjection, width, height, frameWidth, frameHeight);
    m_canvas.begin(device, canvasProjection);
    if (m_gameOver.active()) {
        m_gameOver.draw(m_canvas, m_messages.text(), width);
        m_canvas.end();
        return;
    }
    // The welcome's cut is letterboxed the way the original's trigger cameras are: black
    // bars top and bottom, the status boxes hidden beneath the lower one.
    const bool cut = m_welcome.cutting() || (m_promotion.active() && !spawning()) ||
                     relicCeremonyOn() || m_switchCutscene.showing();
    m_transition.draw(m_canvas, width, height); // over the view, under the boxes
    // Rendering can precede the first simulation tick, before PartyNames has observed the hold.
    if (!spawning()) {
        m_names.draw(m_canvas, m_players, clip, canvasProjection, frameBlend);
    }
    if (!cut) {
        m_hud.drawStatus(m_canvas, m_players);
        if (m_runeFrame != nullptr && m_runeColumn != nullptr) {
            m_runeMeter.draw(m_canvas, *m_runeFrame, *m_runeColumn);
        }
        if (!m_hud.drawHourglass(m_canvas, m_players) &&
            m_challenge.state() != SecretChallenge::State::Inactive) {
            m_challengeHud.draw(m_canvas, m_challenge.remaining(), m_challenge.duration(),
                                !spawning() && !m_messages.active());
        }
        m_opponents.meter().draw(m_canvas, device);
        m_bossSequence.victory().drawCaption(m_canvas, m_messages.text(), m_hud.strings(), width,
                                             height);
    }
    if (const LevelInfo* level = m_world->level(); level != nullptr) {
        m_arrival.drawTitle(m_canvas, m_messages.text(), level->title, width);
    }
    if (cut) {
        m_canvas.fillHorizontalBand(0.0f, height * kCutBarTop, Color::black());
        m_canvas.fillHorizontalBand(height * (1.0f - kCutBarBottom), height * kCutBarBottom,
                                    Color::black());
    }
    if (!cut) {
        m_hud.drawSelectors(m_canvas, m_messages.text(), m_context.strings, m_players);
        m_hud.drawHelp(m_canvas, device, m_staticTextures, m_players, clip, canvasProjection, width,
                       height);
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

/** Stands the materialising effect at every character's feet and the optional start
 * camera at its arrival marker to hold and ride in; the party holds still for it. (The
 * realm's entering sound belongs to the loading screen, not to this.) */
void PlayScene::beginSpawn(RenderDevice& device, const std::optional<WorldCamera>& marker) {
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
                    low ? std::optional<Vec3>{(*low + high) * 0.5f} : std::nullopt);
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
