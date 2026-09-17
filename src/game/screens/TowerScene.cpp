#include "game/screens/TowerScene.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <numbers>

#include "engine/audio/AdsStream.h"
#include "engine/core/Log.h"

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kPlayersDirectory = "PLAYERS";
constexpr std::string_view kClassAnimations = "ANIM"; ///< under the class folder
constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kStreamsDirectory = "STREAMS"; ///< the music, among the game's files
constexpr std::string_view kSoundDirectory = "audio";
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
    spawnParty(party, options);
    world.setPlayerCount(static_cast<s32>(m_actors.size()));
    world.startTriggers(visitors());
    for (const PlayerActor& actor : m_actors) {
        m_figures.push_back(loadFigure(device, actor.save()));
        m_subjects.push_back(CameraSubject{actor.position(), actor.followPoint()});
    }
    m_camera.reset(m_subjects, world.cameraMarkers(), world.cameraRange(), cameraView());
    startMusic();
    m_intro = Intro::None;
    if (options.welcome.value_or(freshParty(party))) {
        beginIntro(device);
        m_world->hideCrystals(); // Sumner reveals them once the scroll has gone
    }
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

std::unique_ptr<TowerScene::Figure> TowerScene::loadFigure(RenderDevice& device,
                                                           const CharacterSave& save) {
    const std::string_view cls = classCode(save.character);
    const std::string_view costume = colorCode(save.color);
    const std::filesystem::path directory =
        m_context.unpackedRoot / kPlayersDirectory / std::string(cls) / std::string(costume);
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
    loadActions(*figure, save);
    return figure;
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
            for (PlayerActor& actor : m_actors) {
                s32& count = actor.save().progress().crystals[static_cast<usize>(pickup.realm)];
                if (wanted <= 0 || count < wanted) {
                    ++count;
                }
                m_pickups.showCount(actor.player(), PickupHud::crystalIcon(pickup.realm), count,
                                    wanted);
            }
            if (pickup.collector < m_actors.size()) {
                m_pickups.addCard(m_actors[pickup.collector].player(), PickupHud::kCrystalCard);
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
    return m_intro == Intro::Crystal ? m_cutCamera : m_camera.camera();
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
    // The scroll holds everything else still, back included, until it has burnt away.
    if (m_intro == Intro::Scroll) {
        m_scroll.step(ticks, acceptedPlayers(inputs));
        if (!m_scroll.active()) {
            startCrystalCut();
        }
        return TowerOutcome::Running;
    }
    for (const PlayInput& input : inputs) {
        if (input.menu.back) {
            return TowerOutcome::Leave;
        }
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
    m_world->updateTriggers(seconds, visitors());
    m_camera.update(m_subjects, m_world->cameraMarkers(), m_world->cameraRange(), cameraView(),
                    seconds);
    return TowerOutcome::Running;
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
            m_figures[i]->model.draw(device, clip, m_actors[i].transform(), m_world->lighting(),
                                     m_figures[i]->transforms);
        }
    }
    const auto width = static_cast<f32>(config.display.virtualWidth);
    const auto height = static_cast<f32>(config.display.virtualHeight);
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    for (s32 player = 0; player < kPlayerCount; ++player) {
        m_boxes.draw(m_canvas, player, statusOf(player), true);
    }
    m_pickups.draw(m_canvas, m_boxes);
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

} // namespace gdl::game
