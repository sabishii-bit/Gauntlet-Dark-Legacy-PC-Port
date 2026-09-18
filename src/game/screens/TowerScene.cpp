#include "game/screens/TowerScene.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <numbers>

#include "engine/audio/AdsStream.h"
#include "engine/core/Log.h"
#include "engine/world/WorldCamera.h"

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr std::string_view kBeamObject = "L1XPLIGHTRAY01"; ///< the light on Sumner's lectern
constexpr std::string_view kWeaponsArchive = "WEAPONS";
constexpr std::string_view kSpawnEffect = "STARTFX"; ///< the tree the party materialises in
constexpr std::string_view kFieldSound = "S_WARN";   ///< the tower's note as a gate opens
constexpr std::string_view kNeedCrystals = "NEEDCRYSTALS";
constexpr std::string_view kNeedIcons = "NEEDGARGITEMS";
constexpr std::string_view kUnlockLevel = "UNLOCKLEVEL";
constexpr std::string_view kUnlockSection = "UNLOCKSECTION";
constexpr s32 kIconMessages = 100;      ///< trigger ids from here name gargoyle tiers
constexpr s32 kTitleY = 48;              ///< where the level's title starts, on the canvas
constexpr s32 kTitleLift = 16;           ///< how far up it slides, per unit of slide
constexpr f32 kTitleSlideStart = 0.025f; ///< the original's slide, growing by this a tick
constexpr f32 kTitleSlideRate = 0.025f;
constexpr f32 kTitleSlideEnd = 2.0f;     ///< where it stops, and sits once the camera rides
constexpr s32 kLevelsPerTier = 10;                          ///< costume tiers
constexpr s32 kWeaponTierTwoLevel = 10;                     ///< the weapon's tiers
constexpr s32 kWeaponTierThreeLevel = 50;
/** The costume objects the weapon hangs from, by class: the base classes, then the others. */
constexpr std::array<std::string_view, 3> kHandObjects{"R_WRIST", "RIGHTHAN", "RHEND"};
constexpr std::string_view kHeldWeapon = "WEAP_HOLD"; ///< a tiered costume's own weapon
constexpr std::string_view kClassAnimations = "ANIM"; ///< under the class folder
constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kStreamsDirectory = "STREAMS"; ///< the music, among the game's files
constexpr std::string_view kSoundDirectory = "audio";
constexpr std::string_view kAmbientBank = "TOWAMB"; ///< the tower's ambience, with its own bank
constexpr std::string_view kCommonBank = "COMMON";
constexpr std::array<std::string_view, 2> kStepSounds{"S_STEPROCK1", "S_STEPROCK2"};
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kFontFile = "fonts/font32.json";
constexpr s32 kFont32SpaceWidth = 16;
constexpr std::string_view kFontTexture = "FONT32";
constexpr std::string_view kGlowTexture = "FONT32_GLOW";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr std::string_view kButtonTexture = "BUTTON_TRI";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
constexpr std::string_view kScrollTextFile = "text/scroll_e.json";
constexpr std::string_view kWelcomeMessage = "WELCOMEMESSAGE";
constexpr std::string_view kPromptText = "scroll.pressButton";
constexpr u32 kEntranceWorld = 0;
constexpr f32 kPi = std::numbers::pi_v<f32>;
constexpr s32 kMinTicks = 1; ///< a frame advances the clock by at least one tick
constexpr s32 kMaxTicks = 4; ///< and, however late, by at most four

} // namespace

bool TowerScene::open(RenderDevice& device, const GameContext& context, TowerWorld& world,
                      std::span<const PartyMember> party, const TowerOptions& options) {
    close();
    m_context = context;
    m_device = &device;
    m_world = &world;
    if (!world.built() && !world.load(device, context.unpackedRoot)) {
        return false;
    }
    if (!m_boxes.load(device, context.unpackedRoot, context.strings)) {
        return false;
    }
    m_classes.load(context.unpackedRoot / kClassDataDirectory);
    loadSounds();
    loadIntroArt(device);
    m_sumner.load(device, world.items(), world.layout());
    // Sumner's beam waits unseen until the party comes to him.
    m_beam = -1;
    m_beamAlpha = 0.0f;
    for (usize i = 0; i < world.layout().objects().size(); ++i) {
        if (world.layout().objects()[i].name == kBeamObject) {
            m_beam = static_cast<s32>(i);
            world.setObjectAlpha(i, 0.0f);
        }
    }
    spawnParty(party, options);
    world.setPlayerCount(static_cast<s32>(m_actors.size()));
    world.startTriggers(visitors());
    const std::array<SoundSet*, 2> banks{&m_ambientBank, &m_levelBank};
    m_ambience.bind(world.layout(), banks);
    for (const PlayerActor& actor : m_actors) {
        m_figures.push_back(loadFigure(device, actor.save()));
        m_subjects.push_back(CameraSubject{actor.position(), actor.followPoint()});
    }
    m_camera.reset(m_subjects, world.cameraMarkers(), world.cameraRange(), cameraView());
    startMusic();
    m_intro = Intro::None;
    m_beamWelcomed = false;
    // The party materialises first; Sumner's welcome, when it is due, follows.
    m_welcomePending = options.welcome.value_or(freshParty(party));
    if (m_welcomePending) {
        m_world->hideCrystals(); // Sumner reveals them once the scroll has gone
    }
    beginSpawn(device, !options.position.has_value());
    m_open = true;
    log::info("Tower: {} in the party", m_actors.size());
    return true;
}

void TowerScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    m_scroll.close();
    if (m_world != nullptr) {
        m_world->setPlayerCount(0);
    }
    m_sumner.clear();
    m_text.setFont(nullptr, nullptr);
    m_staticTextures.releaseTextures();
    m_intro = Intro::None;
    m_actors.clear();
    m_figures.clear();
    m_subjects.clear();
    if (m_context.sounds != nullptr) {
        m_ambience.stop(*m_context.sounds);
    }
    m_ambience.clear();
    m_spawns.clear();
    m_spawnTexmods.clear();
    m_weapons.release(); // its textures must go before the device does
    m_spawnTicks = 0;
    m_startCamera.stop();
    m_titleSlide = 0.0f;
    m_welcomePending = false;
    m_pickups.clear();
    m_boxes.release();
    m_world = nullptr;
    m_device = nullptr;
    m_open = false;
}

/** Stands the party side by side at the entrance, facing into the tower: the start marker's
 * heading points back out of the door. */
void TowerScene::spawnParty(std::span<const PartyMember> party, const TowerOptions& options) {
    const WorldLocator* start = m_world->startPoint(kEntranceWorld);
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
    for (usize i = 0; i < party.size(); ++i) {
        const PartyMember& member = party[i];
        PlayerActor actor;
        const Vec3 position = origin + sideways * (first + static_cast<f32>(i) * kSpawnSpacing);
        actor.spawn(member.player, member.save, m_classes.stats(member.save.character), position,
                    yaw);
        actor.settle(m_world->collision());
        m_actors.push_back(std::move(actor));
    }
}

std::filesystem::path TowerScene::costumeDirectory(const std::filesystem::path& unpackedRoot,
                                                   const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path base =
        unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(costume);
    const s32 tier = experienceLevel(save.progress().experience) / kLevelsPerTier;
    const std::filesystem::path tiered =
        base.parent_path() / std::format("{}{}0", costume, tier);
    return std::filesystem::exists(tiered / "objects.json") ? tiered : base;
}

std::unique_ptr<TowerScene::Figure> TowerScene::loadFigure(RenderDevice& device,
                                                           const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory = costumeDirectory(m_context.unpackedRoot, save);
    auto figure = std::make_unique<Figure>();
    if (!figure->models.load(directory) || !figure->textures.load(directory) ||
        !figure->trees.load(directory)) {
        log::warn("Tower: no model for the {} {} under {}", costume, cls, directory.string());
        return nullptr;
    }
    const auto tree = figure->trees.find(std::format("{}_{}", cls, costume));
    if (!tree.has_value() ||
        !figure->model.bind(figure->trees.tree(*tree), figure->models, figure->textures, device)) {
        log::warn("Tower: the {} {} figure could not be built", costume, cls);
        return nullptr;
    }
    figure->costume = &figure->trees.tree(*tree);
    figure->directory = directory;
    loadWeapon(*figure, save, device);
    loadActions(*figure, save);
    return figure;
}

/** Hangs the weapon from the costume's hand: a tiered costume holds its own `WEAP_HOLD`,
 * an untiered one `WEAP_<colour>_HD<tier>` for the character's level, and the hand is the
 * object named after the class's wrist. */
void TowerScene::loadWeapon(Figure& figure, const CharacterSave& save, RenderDevice& device) {
    const s32 level = experienceLevel(save.progress().experience);
    s32 tier = 1;
    if (level >= kWeaponTierThreeLevel) {
        tier = 3;
    } else if (level >= kWeaponTierTwoLevel) {
        tier = 2;
    }
    std::string weapon{kHeldWeapon};
    if (!figure.models.find(weapon).has_value()) {
        weapon = std::format("WEAP_{}_HD{}", colorCode(save.color), tier);
    }
    if (!figure.models.find(weapon).has_value()) {
        log::warn("Tower: no {} in {}", weapon, figure.directory.string());
        return;
    }
    for (usize n = 0; n < figure.costume->nodes.size() && figure.handNode < 0; ++n) {
        for (const std::string_view suffix : kHandObjects) {
            if (figure.costume->nodes[n].object.ends_with(suffix)) {
                figure.handNode = static_cast<s32>(n);
                break;
            }
        }
    }
    if (figure.handNode < 0) {
        log::warn("Tower: no hand to hold {} in {}", weapon, figure.directory.string());
        return;
    }
    figure.weaponTree.name = weapon;
    TreeNodeInfo held;
    held.name = weapon;
    held.object = weapon;
    figure.weaponTree.nodes.push_back(held);
    if (!figure.weapon.bind(figure.weaponTree, figure.models, figure.textures, device)) {
        figure.handNode = -1;
    }
}

std::optional<std::filesystem::path> TowerScene::figureDirectory(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player && m_figures[i] != nullptr) {
            return m_figures[i]->directory;
        }
    }
    return std::nullopt;
}

bool TowerScene::weaponHeld(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player && m_figures[i] != nullptr) {
            return m_figures[i]->handNode >= 0 && m_figures[i]->weapon.bound();
        }
    }
    return false;
}

/** Sumner's beam comes up over three seconds while the party is near him (or his welcome's
 * cut shows him) and goes again once they leave. */
void TowerScene::updateBeam(s32 ticks) {
    if (m_beam < 0) {
        return;
    }
    bool near = m_beamWelcomed || m_intro == Intro::Crystal;
    for (const PlayerActor& actor : m_actors) {
        near = near || glm::distance(actor.position(), m_sumner.position()) <= kBeamRadius;
    }
    const f32 step = static_cast<f32>(ticks) / static_cast<f32>(kBeamFadeTicks);
    const f32 alpha = std::clamp(m_beamAlpha + (near ? step : -step), 0.0f, 1.0f);
    if (alpha != m_beamAlpha) {
        m_beamAlpha = alpha;
        m_world->setObjectAlpha(static_cast<usize>(m_beam), alpha);
    }
}

/** Binds the class's sequences to the figure: the class tree carries the keys and its nodes
 * share their names with the costume's, so each costume node follows its namesake. */
void TowerScene::loadActions(Figure& figure, const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::filesystem::path directory =
        m_context.unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(kClassAnimations);
    const auto tree = figure.actions.load(directory) ? figure.actions.find(cls) : std::nullopt;
    if (!tree.has_value() || !figure.animator.bind(figure.actions.tree(*tree))) {
        log::warn("Tower: no sequences for the {} under {}; the figure stands still", cls,
                  directory.string());
        return;
    }
    const TreeInfo& actions = figure.actions.tree(*tree);
    figure.classNodeOfNode.clear();
    for (const TreeNodeInfo& node : figure.costume->nodes) {
        const auto match = actions.findNode(node.name);
        figure.classNodeOfNode.push_back(match.has_value() ? static_cast<s32>(*match) : -1);
    }
    figure.animate(0.0f, 0, 0.0f);
}

/** Finds the footstep sounds in the common bank. */
void TowerScene::loadSounds() {
    m_stepSounds.fill(std::nullopt);
    if (const LevelAudioInfo* audio = m_world->audio(); audio != nullptr) {
        m_levelBank.load(m_context.unpackedRoot / kSoundDirectory / audio->bank);
    }
    m_ambientBank.load(m_context.unpackedRoot / kSoundDirectory / kAmbientBank);
    if (!m_commonSounds.load(m_context.unpackedRoot / kSoundDirectory / kCommonBank)) {
        return;
    }
    for (usize foot = 0; foot < kStepSounds.size(); ++foot) {
        m_stepSounds[foot] = m_commonSounds.find(kStepSounds[foot]);
    }
    m_pickupSound = m_commonSounds.find(kPickupSound);
}

void TowerScene::playCommon(std::optional<u32> sound) {
    if (m_context.sounds == nullptr || !sound.has_value()) {
        return;
    }
    m_context.sounds->play(m_commonSounds.sequence(*sound), 1.0f, SoundCategory::Effects);
}

/** Takes what the party stands on: a crystal counts for everyone, towards its realm's gate,
 * up to what the gate wants; the taker's box gets the card, every box the count. */
void TowerScene::collectItems() {
    if (m_device == nullptr) {
        return;
    }
    std::vector<Collector> collectors;
    collectors.reserve(m_actors.size());
    for (const PlayerActor& actor : m_actors) {
        collectors.push_back(Collector{actor.position(), actor.radius(), actor.height()});
    }
    for (const Pickup& pickup : m_world->collect(*m_device, collectors)) {
        if (pickup.realm > 0 && static_cast<usize>(pickup.realm) < kRealmCount) {
            const s32 wanted = LevelTriggers::crystalsNeeded(pickup.realm);
            bool enough = wanted > 0;
            for (PlayerActor& actor : m_actors) {
                s32& count = actor.save().progress().crystals[static_cast<usize>(pickup.realm)];
                if (wanted <= 0 || count < wanted) {
                    ++count;
                }
                enough = enough && count >= wanted;
                m_pickups.showCount(actor.player(), PickupHud::crystalIcon(pickup.realm), count,
                                    wanted);
            }
            if (pickup.collector < m_actors.size()) {
                m_pickups.addCard(m_actors[pickup.collector].player(), PickupHud::kCrystalCard);
            }
            if (enough) {
                announceUnlock(pickup.realm);
            }
        }
        playCommon(m_pickupSound);
    }
}

/** Loops the level's music stream from the game's files at the level's volume. */
void TowerScene::startMusic() {
    const LevelAudioInfo* audio = m_world->audio();
    if (m_context.sounds == nullptr || m_context.assets == nullptr || audio == nullptr ||
        audio->stream.empty()) {
        return;
    }
    const auto file =
        m_context.assets->find(std::format("{}/{}.ads", kStreamsDirectory, audio->stream));
    if (!file.has_value()) {
        log::warn("Tower: music stream {} is not among the game's files", audio->stream);
        return;
    }
    auto stream = std::make_shared<AdsStream>();
    if (!stream->open(*file)) {
        return;
    }
    const LevelInfo* level = m_world->level();
    m_music = m_context.sounds->playStream(std::move(stream), true,
                                          level != nullptr ? level->musicVolume : 1.0f,
                                          SoundCategory::Music);
}

void TowerScene::playStep(PlayerAnimator::Foot foot) {
    playCommon(m_stepSounds[foot == PlayerAnimator::Foot::Second ? 1 : 0]);
}

/** Gathers the scroll's art: the sheet, the prompt's font and glow, the button icon and the
 * burn frames, and the scroll texts. Missing pieces only lose the welcome's scroll. */
void TowerScene::loadIntroArt(RenderDevice& device) {
    ScrollBoxArt art;
    m_text.setFont(nullptr, nullptr);
    if (!m_staticTextures.load(m_context.unpackedRoot / kStaticDirectory) ||
        !m_font32.load(m_context.unpackedRoot / kFontFile, kFont32SpaceWidth) ||
        !m_scrollText.load(m_context.unpackedRoot / kScrollTextFile)) {
        log::warn("Tower: the scroll's art or texts are not unpacked; the welcome is skipped");
        m_scroll.setArt(art);
        return;
    }
    const auto texture = [&](std::string_view name, u32 frame = 0) -> const Texture* {
        const auto index = m_staticTextures.find(name);
        if (!index.has_value() || *index + frame >= m_staticTextures.size()) {
            return nullptr;
        }
        try {
            return &m_staticTextures.texture(device, *index + frame);
        } catch (const std::exception& e) {
            log::warn("Tower: texture {}: {}", name, e.what());
            return nullptr;
        }
    };
    const Texture* font = texture(kFontTexture);
    if (font != nullptr) {
        m_text.setFont(&m_font32, font);
    }
    art.backdrop = texture(kScrollTexture);
    art.glow = texture(kGlowTexture);
    art.button = texture(kButtonTexture);
    const auto scroll = m_staticTextures.find(kScrollTexture);
    const auto ring = m_staticTextures.find(kFireRingTexture);
    const auto mask = m_staticTextures.find(kFireMaskTexture);
    if (scroll.has_value() && ring.has_value() && mask.has_value()) {
        try {
            art.backdropImage = &m_staticTextures.image(*scroll);
            const auto frames = static_cast<u32>(FireScroll::kFrameCount);
            for (u32 i = 1; i <= frames && *ring + i < m_staticTextures.size(); ++i) {
                art.burnRing.push_back(&m_staticTextures.texture(device, *ring + i));
            }
            for (u32 i = 1; i <= frames && *mask + i < m_staticTextures.size(); ++i) {
                art.burnMasks.push_back(&m_staticTextures.image(*mask + i));
            }
        } catch (const std::exception& e) {
            log::warn("Tower: burn frames: {}", e.what());
            art.backdropImage = nullptr;
            art.burnRing.clear();
            art.burnMasks.clear();
        }
    }
    m_scroll.setText(&m_text);
    m_scroll.setArt(std::move(art));
}

/** A party is new to the tower while no class of any of its characters has experience. */
bool TowerScene::freshParty(std::span<const PartyMember> party) {
    return !party.empty() && std::ranges::all_of(party, [](const PartyMember& member) {
        return std::ranges::none_of(member.save.classes, [](const ClassProgress& progress) {
            return progress.experience > 0;
        });
    });
}

/** Opens Sumner's welcome scroll; without it the welcome goes straight to the crystals. */
void TowerScene::beginIntro(RenderDevice& device) {
    const auto message = m_scrollText.loaded() ? m_scrollText.find(kWelcomeMessage) : std::nullopt;
    if (message.has_value()) {
        const MessageInfo& welcome = m_scrollText.message(*message);
        const std::string prompt =
            m_context.strings != nullptr ? std::string(m_context.strings->get(kPromptText)) : "";
        if (m_scroll.open(device, welcome.pages, welcome.scale, prompt)) {
            m_intro = Intro::Scroll;
            m_beamWelcomed = true;
            return;
        }
    }
    log::warn("Tower: no welcome scroll to show; on to the crystals");
    startCrystalCut();
}

/** Sumner gestures at the crystals while the camera cuts to them from the level's marker. */
void TowerScene::startCrystalCut() {
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
u32 TowerScene::acceptedPlayers(const Inputs& inputs) const {
    u32 accepted = 0;
    for (const PlayerActor& actor : m_actors) {
        const auto player = static_cast<usize>(actor.player());
        if (player < inputs.size() && inputs[player].menu.select) {
            accepted |= 1U << player;
        }
    }
    return accepted;
}

const WorldCamera& TowerScene::viewCamera() const {
    if (m_startCamera.active()) {
        return m_startCamera.camera();
    }
    return m_intro == Intro::Crystal ? m_cutCamera : m_camera.camera();
}

/** Whether any party member's player pressed a button this frame. */
bool TowerScene::anyButton(const Inputs& inputs) const {
    return std::ranges::any_of(m_actors, [&inputs](const PlayerActor& actor) {
        const auto player = static_cast<usize>(actor.player());
        return player < inputs.size() && (inputs[player].menu.select ||
                                          inputs[player].menu.back || inputs[player].menu.start);
    });
}

void TowerScene::Figure::animate(f32 stickMagnitude, s32 ticks, f32 seconds) {
    if (!animator.bound()) {
        return;
    }
    animator.update(PlayerAnimator::motionFor(stickMagnitude), ticks, seconds);
    const std::span<const Mat4> matrices = animator.pose().matrices();
    transforms.resize(costume->nodes.size());
    for (usize n = 0; n < transforms.size(); ++n) {
        const s32 source = classNodeOfNode[n];
        transforms[n] = source >= 0 && static_cast<usize>(source) < matrices.size()
                            ? matrices[static_cast<usize>(source)]
                            : glm::translate(Mat4{1.0f}, costume->worldPosition(n));
    }
}

TowerOutcome TowerScene::update(f64 deltaSeconds, const Inputs& inputs) {
    if (!m_open) {
        return TowerOutcome::Running;
    }
    // The clock advances in whole ticks, two per frame at the 30 frames per second the game
    // runs at, so a late frame moves everything further rather than smoother.
    const f32 tickRate =
        m_context.config != nullptr ? static_cast<f32>(m_context.config->timing.tickRate) : 60.0f;
    const auto ticks = std::clamp(static_cast<s32>(std::lround(deltaSeconds * tickRate)),
                                  kMinTicks, kMaxTicks);
    const f32 seconds = static_cast<f32>(ticks) / tickRate;
    // A scroll holds everything else still until it has burnt away; the welcome's leads on
    // to the crystals.
    if (m_scroll.active()) {
        m_scroll.step(ticks, acceptedPlayers(inputs));
        if (!m_scroll.active() && m_intro == Intro::Scroll) {
            startCrystalCut();
        }
        return TowerOutcome::Running;
    }
    // Materialising, the party stands still while the level runs on around it and the start
    // camera holds, then rides in; the title slides up until the ride, when it sits.
    if (spawning()) {
        updateSpawn(ticks, seconds);
        m_world->update(seconds);
        updateAmbience();
        updateBeam(ticks);
        m_spawnTicks = std::max(m_spawnTicks - ticks, 0);
        m_startCamera.update(ticks, anyButton(inputs), m_camera.camera().position,
                             m_camera.attention());
        m_titleSlide += kTitleSlideRate * static_cast<f32>(ticks);
        if (m_startCamera.phase() != StartCamera::Phase::Hold || m_titleSlide > kTitleSlideEnd) {
            m_titleSlide = kTitleSlideEnd;
        }
        if (!spawning() && m_welcomePending) {
            m_welcomePending = false;
            beginIntro(*m_device);
        }
        return TowerOutcome::Running;
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
    m_pickups.step(ticks, seconds);
    m_sumner.update(seconds);
    const f32 cameraYaw = m_camera.yaw();
    for (usize i = 0; i < m_actors.size(); ++i) {
        PlayerActor& actor = m_actors[i];
        const auto player = static_cast<usize>(actor.player());
        const MoveInput& move =
            !held && player < inputs.size() ? inputs[player].move : MoveInput{};
        actor.update(move, cameraYaw, seconds, &m_world->collision());
        if (m_figures[i] != nullptr) {
            m_figures[i]->animate(move.magnitude, ticks, seconds);
            if (const PlayerAnimator::Foot foot = m_figures[i]->animator.footfall();
                foot != PlayerAnimator::Foot::None) {
                playStep(foot);
            }
        }
        m_subjects[i] = CameraSubject{actor.position(), actor.followPoint()};
    }
    collectItems();
    updateBeam(ticks);
    m_world->updateTriggers(seconds, visitors());
    handleTriggerEvents();
    m_camera.update(m_subjects, m_world->cameraMarkers(), m_world->cameraRange(), cameraView(),
                    seconds);
    updateAmbience();
    return TowerOutcome::Running;
}

/** Places the level's loops for the party, heard from the camera. */
void TowerScene::updateAmbience() {
    if (m_context.sounds == nullptr) {
        return;
    }
    std::vector<Vec3> listeners;
    listeners.reserve(m_actors.size());
    for (const PlayerActor& actor : m_actors) {
        listeners.push_back(actor.position());
    }
    const CameraFrame frame = CameraFrame::of(viewCamera());
    const LevelInfo* level = m_world->level();
    m_ambience.update(*m_context.sounds, listeners, AmbientEar{frame.position, frame.right},
                      level != nullptr ? level->soundVolume : 1.0f);
}

/** The party as the level's triggers see it. */
std::vector<TriggerVisitor> TowerScene::visitors() const {
    std::vector<TriggerVisitor> out;
    for (const PlayerActor& actor : m_actors) {
        TriggerVisitor visitor;
        visitor.position = actor.position();
        visitor.radius = actor.radius();
        visitor.crystals = actor.save().progress().crystals;
        out.push_back(visitor);
    }
    return out;
}

void TowerScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                        f32 frameHeight) {
    if (!m_open || m_context.config == nullptr) {
        return;
    }
    const GameConfig& config = *m_context.config;
    m_scroll.prepare(device);
    const Mat4 clip = viewCamera().clipTransform(config.horizontalFovRadians(), frameWidth,
                                                 frameHeight, frameProjection);
    m_world->draw(device, clip, viewCamera());
    m_sumner.draw(device, clip, m_world->lighting());
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_figures[i] != nullptr) {
            const Figure& figure = *m_figures[i];
            figure.model.draw(device, clip, m_actors[i].transform(), m_world->lighting(),
                              figure.transforms);
            if (figure.handNode >= 0 && figure.weapon.bound()) {
                const auto hand = static_cast<usize>(figure.handNode);
                const Mat4 wrist = hand < figure.transforms.size()
                                       ? figure.transforms[hand]
                                       : glm::translate(Mat4{1.0f}, figure.costume->worldPosition(hand));
                figure.weapon.draw(device, clip, m_actors[i].transform() * wrist,
                                   m_world->lighting());
            }
        }
    }
    drawSpawn(device, clip);
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    for (s32 player = 0; player < kPlayerCount; ++player) {
        m_boxes.draw(m_canvas, player, statusOf(player), true);
    }
    m_pickups.draw(m_canvas, m_boxes);
    if (spawning()) {
        drawLevelTitle(width);
    }
    m_scroll.draw(m_canvas);
    m_canvas.end();
}

CameraView TowerScene::cameraView() const {
    CameraView view;
    if (m_context.config != nullptr) {
        view.horizontalFov = m_context.config->horizontalFovRadians();
        view.aspect = static_cast<f32>(m_context.config->display.frameWidth) /
                      static_cast<f32>(m_context.config->display.frameHeight);
    }
    return view;
}

const PlayerActor* TowerScene::actor(s32 player) const {
    for (const PlayerActor& actor : m_actors) {
        if (actor.player() == player) {
            return &actor;
        }
    }
    return nullptr;
}

const PlayerAnimator* TowerScene::animator(s32 player) const {
    for (usize i = 0; i < m_actors.size(); ++i) {
        if (m_actors[i].player() == player) {
            return m_figures[i] != nullptr && m_figures[i]->animator.bound()
                       ? &m_figures[i]->animator
                       : nullptr;
        }
    }
    return nullptr;
}

StatusBoxView TowerScene::statusOf(s32 player) const {
    StatusBoxView view;
    const PlayerActor* actor = this->actor(player);
    if (actor == nullptr) {
        return view;
    }
    const CharacterSave& save = actor->save();
    view.mode = StatusBoxView::Mode::Status;
    view.active = true;
    view.classIndex = save.character;
    view.color = save.color;
    view.name = save.name;
    view.level = experienceLevel(save.experience());
    view.gold = save.gold;
    view.health = save.health();
    return view;
}

/** Stands the materialising effect at every character's feet and, with `ride`, the start
 * camera at the entrance marker to hold and ride in; the party holds still for it. (The
 * realm's entering sound belongs to the loading screen, not to this.) */
void TowerScene::beginSpawn(RenderDevice& device, bool ride) {
    m_spawns.clear();
    m_spawnTexmods.clear();
    m_spawnFrames = 0.0f;
    m_spawnTicks = kSpawnTicks;
    m_startCamera.stop();
    m_titleSlide = kTitleSlideStart;
    if (const auto marker = m_world->entranceCamera(); ride && marker.has_value()) {
        Vec3 centre{0.0f, 0.0f, 0.0f};
        for (const PlayerActor& actor : m_actors) {
            centre += actor.position();
        }
        if (!m_actors.empty()) {
            centre /= static_cast<f32>(m_actors.size());
        }
        m_startCamera.start(*marker, centre);
    } else if (ride) {
        log::warn("Tower: no start camera; the party appears under the follow camera");
    }
    if (!m_weapons.loaded()) {
        m_weapons.load(m_context.unpackedRoot / kWeaponsArchive);
    }
    const auto tree = m_weapons.loaded() ? m_weapons.trees.find(kSpawnEffect) : std::nullopt;
    if (!tree.has_value()) {
        log::warn("Tower: no {} in {}; the party appears without it", kSpawnEffect,
                  kWeaponsArchive);
    } else {
        const TreeInfo& effect = m_weapons.trees.tree(*tree);
        for (const PlayerActor& actor : m_actors) {
            Spawn spawn;
            spawn.position = actor.position();
            spawn.tree = &effect;
            if (!spawn.model.bind(effect, m_weapons.models, m_weapons.textures, device)) {
                continue;
            }
            if (!effect.sequences.empty()) {
                spawn.player.start(effect.sequences[0], 0);
                spawn.pose.evaluate(effect, 0, 0.0f);
                spawn.model.setFrame(0, 0);
            } else {
                spawn.pose.rest(effect);
            }
            m_spawns.push_back(std::move(spawn));
        }
        m_spawnTexmods.bind(m_weapons.trees.textureAnimations(), m_weapons.textures, device);
    }
}

void TowerScene::updateSpawn(s32 /*ticks*/, f32 seconds) {
    m_spawnFrames += seconds * AnimationPlayer::kDefaultRate;
    const f32 whole = std::floor(m_spawnFrames);
    m_spawnFrames -= whole;
    if (whole > 0.0f) {
        m_spawnTexmods.step(static_cast<u32>(whole));
    }
    for (Spawn& spawn : m_spawns) {
        if (spawn.player.playing() && !spawn.player.finished()) {
            spawn.player.advance(seconds, false);
            spawn.pose.evaluate(*spawn.tree, spawn.player.sequence(), spawn.player.frame());
            spawn.model.setFrame(spawn.player.sequence(),
                                 static_cast<s32>(spawn.player.frame()));
        }
        for (usize i = 0; i < m_spawnTexmods.size(); ++i) {
            const TextureMotion motion = m_spawnTexmods.motion(i);
            if (motion.frame != nullptr) {
                spawn.model.setTextureFrame(motion.slot, motion.frame);
            } else {
                spawn.model.setTextureOffset(motion.slot, motion.offset);
            }
        }
    }
}

void TowerScene::drawSpawn(RenderDevice& device, const Mat4& clip) const {
    if (m_spawnTicks <= 0) {
        return;
    }
    for (const Spawn& spawn : m_spawns) {
        spawn.model.draw(device, clip, glm::translate(Mat4{1.0f}, spawn.position),
                         m_world->lighting(), spawn.pose.matrices());
    }
}

/** The level's title across the top of the screen while the party materialises, sliding up
 * as the start camera holds, the way the original places it. */
void TowerScene::drawLevelTitle(f32 width) {
    const LevelInfo* level = m_world->level();
    if (level == nullptr || level->title.empty() || !m_text.ready()) {
        return;
    }
    const TextStyle style;
    const s32 y = kTitleY - static_cast<s32>(static_cast<f32>(kTitleLift) * m_titleSlide);
    m_text.draw(m_canvas, -static_cast<s32>(width / 2.0f), y, level->title, style);
}

/** Opens one page of a scroll message over the tower: the party reads it and presses on. */
bool TowerScene::openMessage(std::string_view name, usize page) {
    if (m_device == nullptr || !m_scrollText.loaded()) {
        return false;
    }
    const auto found = m_scrollText.find(name);
    if (!found.has_value() || page >= m_scrollText.message(*found).pages.size()) {
        log::warn("Tower: no page {} of the message {}", page, name);
        return false;
    }
    const MessageInfo& message = m_scrollText.message(*found);
    const std::string prompt =
        m_context.strings != nullptr ? std::string(m_context.strings->get(kPromptText)) : "";
    return m_scroll.open(*m_device, {message.pages[page]}, message.scale, prompt);
}

/** Plays a sound by name from whichever loaded bank holds it; false when none does. */
bool TowerScene::playNamed(std::string_view name) {
    if (m_context.sounds == nullptr || name.empty()) {
        return false;
    }
    for (SoundSet* bank : {&m_levelBank, &m_commonSounds, &m_ambientBank}) {
        if (const auto found = bank->find(name); found.has_value()) {
            try {
                m_context.sounds->play(bank->sequence(*found), 1.0f, SoundCategory::Effects);
                return true;
            } catch (const std::exception& e) {
                log::warn("Tower: sound {}: {}", name, e.what());
                return false;
            }
        }
    }
    return false;
}

/** Congratulates the party once its crystals open a realm's gate: the scroll for the realm,
 * its announcing voice, and the save remembers so it is not said twice. */
void TowerScene::announceUnlock(s32 realm) {
    if (realm <= 0 || static_cast<usize>(realm) >= kRealmCount) {
        return;
    }
    const u32 bit = 1U << static_cast<u32>(realm);
    bool fresh = false;
    for (PlayerActor& actor : m_actors) {
        ClassProgress& progress = actor.save().progress();
        fresh = fresh || (progress.unlocked & bit) == 0;
        progress.unlocked |= bit;
    }
    if (!fresh) {
        return;
    }
    openMessage(kUnlockLevel, static_cast<usize>(realm));
    if (static_cast<usize>(realm) < kUnlockVoices.size()) {
        playNamed(kUnlockVoices[static_cast<usize>(realm)]);
    }
}

/** Tells a refused party what a gate wants, and sounds a gate that opens before them. */
void TowerScene::handleTriggerEvents() {
    // One scroll at a time: the frame's first refusal.
    if (const std::vector<TriggerRefusal> refusals = m_world->takeTriggerRefusals();
        !refusals.empty()) {
        const TriggerRefusal& refusal = refusals.front();
        if (refusal.crystals) {
            openMessage(kNeedCrystals, static_cast<usize>(refusal.id));
        } else if (refusal.id >= kIconMessages) {
            openMessage(kNeedIcons, static_cast<usize>(refusal.id - kIconMessages));
        }
    }
    for (const TriggerOpening& opening : m_world->takeTriggerOpenings()) {
        if (!opening.atOnce) {
            playNamed(kFieldSound);
        }
    }
}

} // namespace gdl::game
