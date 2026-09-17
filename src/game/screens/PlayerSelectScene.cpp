#include "game/screens/PlayerSelectScene.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

#include "engine/core/Log.h"

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kSelectDirectory = "SELECT";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kTowerLevel = "LEVELS/LEVELL1"; ///< the hub the screen looks into
constexpr std::string_view kFont32File = "fonts/font32.json";
constexpr std::string_view kFont8File = "fonts/font8x8.json";
constexpr std::string_view kInitialsFile = "fonts/initials.json";
constexpr std::string_view kScoreFile = "fonts/score.json";
constexpr std::string_view kSmallCapsFile = "fonts/8hifonts.json";
constexpr std::string_view kCommonSounds = "audio/COMMON";
constexpr std::string_view kSelectSounds = "audio/SELECT";
constexpr std::string_view kSoundMusic = "S_SELECTMUS";
constexpr s32 kFont32SpaceWidth = 16;
constexpr s32 kFont8SpaceWidth = 8;
constexpr s32 kInitialsSpaceWidth = 12;
constexpr s32 kScoreSpaceWidth = 9;
constexpr s32 kSmallCapsSpaceWidth = 8;
constexpr s32 kBoxY = 320;
constexpr s32 kBoxHeight = 64;
constexpr s32 kBoxIconSize = 20;
constexpr s32 kBoxIconY = 357;
constexpr s32 kBoxCoinX = 6;
constexpr s32 kBoxHeartX = 61;
constexpr s32 kBoxGoldRight = 60;
constexpr s32 kBoxHealthRight = 116;
constexpr s32 kBoxValueY = 359;
constexpr s32 kBoxNameY = 339;
constexpr s32 kBoxLevelY = 326;
constexpr f32 kBoxNameScale = 0.667f;
constexpr s32 kMaxTicksPerFrame = 6;
constexpr s32 kPanelTopHeight = 256;
constexpr s32 kPanelBottomHeight = 64;
constexpr Color kBackdrop = Color::rgba(8, 6, 12);

std::string_view soundName(SelectSound sound) {
    switch (sound) {
    case SelectSound::Select: return "S_OPTMENUSEL";
    case SelectSound::ClassChange: return "S_OPTMENUCHAR";
    case SelectSound::CursorVertical: return "S_OPTMENUMOVVRT";
    case SelectSound::CursorHorizontal: return "S_OPTMENUMOVHRZ";
    case SelectSound::Buzzer: return "S_NO";
    case SelectSound::LetterAccept: return "S_CHOOSESELSFX";
    case SelectSound::Welcome: return "S_WELCOME";
    case SelectSound::WelcomeBack: return "S_WELCOMEBACK";
    }
    return {};
}

} // namespace

std::string_view PlayerSelectScene::text(std::string_view id) const {
    return m_context.strings != nullptr ? m_context.strings->get(id) : id;
}

bool PlayerSelectScene::open(RenderDevice& device, const GameContext& context, s32 startingPlayer) {
    close();
    m_context = context;
    m_screen = MenuScreen{};
    if (m_context.config != nullptr) {
        m_screen.width = static_cast<s32>(m_context.config->display.virtualWidth);
        m_screen.height = static_cast<s32>(m_context.config->display.virtualHeight);
        m_screen.horizontalFov = m_context.config->horizontalFovRadians();
        m_tickRate = static_cast<s32>(m_context.config->timing.tickRate);
    }
    try {
        if (!loadResources(device, m_context.unpackedRoot)) {
            return false;
        }
    } catch (const std::exception& e) {
        log::warn("Player select: {}", e.what());
        m_selectTextures.releaseTextures();
        m_staticTextures.releaseTextures();
        return false;
    }
    m_device = &device;
    m_open = true;
    m_tickRemainder = 0.0;
    m_time = 0;
    m_idleFrames = 0;
    loadSounds(m_context.unpackedRoot);
    loadTower(device, m_context.unpackedRoot);

    if (!m_classes.load(m_context.unpackedRoot / kClassDataDirectory)) {
        log::warn("Player select: class stats are unavailable (run gdlunpack)");
    }
    if (m_context.config != nullptr) {
        const usize slots = m_context.config->save.slots;
        if (!m_saves.open(m_context.config->saveDirectory(), slots)) {
            log::warn("Player select: saving is unavailable");
        }
    }

    m_services = LaneServices{};
    m_services.slots = m_saves.opened() ? &m_saves : nullptr;
    m_services.classes = &m_classes;
    m_services.strings = m_context.strings;
    m_services.menuPainter = &m_large;
    m_services.smallPainter = &m_small;
    m_services.initialsPainter = &m_initials;
    m_services.largePainter = &m_large;
    m_services.screen = m_screen;
    m_services.playSound = [this](SelectSound sound) { playSound(sound); };
    m_services.selectTexture = [this](std::string_view name) { return selectTexture(name); };
    m_services.staticTexture = [this](std::string_view name) { return staticTexture(name); };
    m_services.glowSheet = staticTexture("FONT32_GLOW");
    m_services.menuTextures.font = staticTexture("FONT32");
    m_services.menuTextures.glow = m_services.glowSheet;
    for (s32 i = 0; i < kLaneCount; ++i) {
        m_lanes[static_cast<usize>(i)].reset(i, &m_services);
    }
    if (startingPlayer >= 0 && startingPlayer < kLaneCount) {
        m_lanes[static_cast<usize>(startingPlayer)].activate();
    }
    startMusic();
    return true;
}

void PlayerSelectScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    for (s32 i = 0; i < kLaneCount; ++i) {
        m_lanes[static_cast<usize>(i)].reset(i, nullptr);
    }
    m_tower.clear();
    m_camera.reset();
    m_towerTextures.releaseTextures();
    m_selectTextures.releaseTextures();
    m_staticTextures.releaseTextures();
    m_large.setFont(nullptr, nullptr);
    m_small.setFont(nullptr, nullptr);
    m_initials.setFont(nullptr, nullptr);
    m_score.setFont(nullptr, nullptr);
    m_smallCaps.setFont(nullptr, nullptr);
    m_device = nullptr;
    m_open = false;
}

bool PlayerSelectScene::loadResources(RenderDevice& device,
                                      const std::filesystem::path& unpackedRoot) {
    if (!m_selectTextures.load(unpackedRoot / kSelectDirectory) ||
        !m_staticTextures.load(unpackedRoot / kStaticDirectory)) {
        log::warn("Player select: unpacked textures not found under {} (run gdlunpack)",
                  unpackedRoot.string());
        return false;
    }
    if (!m_font32.load(unpackedRoot / kFont32File, kFont32SpaceWidth) ||
        !m_font8.load(unpackedRoot / kFont8File, kFont8SpaceWidth) ||
        !m_fontInitials.load(unpackedRoot / kInitialsFile, kInitialsSpaceWidth)) {
        return false;
    }
    m_device = &device;
    const Texture* font32 = staticTexture("FONT32");
    const Texture* font8 = staticTexture("FONT8X8");
    const Texture* initials = staticTexture("INITIALS");
    if (font32 == nullptr || font8 == nullptr || initials == nullptr) {
        log::warn("Player select: font textures are missing");
        return false;
    }
    m_large.setFont(&m_font32, font32);
    m_small.setFont(&m_font8, font8);
    m_initials.setFont(&m_fontInitials, initials);
    // The status boxes' number and caption fonts are optional: the boxes draw without them.
    if (m_fontScore.load(unpackedRoot / kScoreFile, kScoreSpaceWidth)) {
        m_score.setFont(&m_fontScore, staticTexture("SCORE"));
    }
    if (m_fontSmallCaps.load(unpackedRoot / kSmallCapsFile, kSmallCapsSpaceWidth)) {
        m_smallCaps.setFont(&m_fontSmallCaps, staticTexture("8HIFONTS"));
    }
    for (s32 i = 0; i < kLaneCount; ++i) {
        if (selectTexture(std::format("S1_PLYR{}", i + 1)) == nullptr ||
            selectTexture(std::format("S2_PLYR{}", i + 1)) == nullptr) {
            log::warn("Player select: lane panels are missing");
            return false;
        }
    }
    return true;
}

void PlayerSelectScene::loadSounds(const std::filesystem::path& unpackedRoot) {
    if (m_context.sounds == nullptr) {
        return;
    }
    if (!m_commonSounds.load(unpackedRoot / kCommonSounds) ||
        !m_selectSounds.load(unpackedRoot / kSelectSounds)) {
        log::warn("Player select: unpacked sound banks not found under {}", unpackedRoot.string());
    }
}

/** The tower hub stands behind the lanes, seen from its entrance camera. */
void PlayerSelectScene::loadTower(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    m_tower.clear();
    m_camera.reset();
    const std::filesystem::path directory = unpackedRoot / kTowerLevel;
    if (!std::filesystem::exists(directory / "world.json")) {
        log::info("Player select: the tower level is not unpacked ({}); drawing without it",
                  directory.string());
        return;
    }
    if (!m_towerLayout.load(directory) || !m_towerModels.load(directory) ||
        !m_towerTextures.load(directory)) {
        return;
    }
    const WorldLocator* locator = m_towerLayout.findLocator(LocatorKind::CameraStart);
    if (locator == nullptr) {
        locator = m_towerLayout.findLocator(LocatorKind::CameraGame);
    }
    if (locator == nullptr) {
        log::warn("Player select: the tower level has no camera");
        return;
    }
    if (!m_tower.build(m_towerLayout, m_towerModels, m_towerTextures, device)) {
        return;
    }
    WorldCamera camera;
    camera.position = locator->position;
    camera.pitch = locator->rotation.x;
    camera.yaw = locator->rotation.y;
    camera.roll = locator->rotation.z;
    m_camera = camera;
    log::info("Player select: tower placed {} objects in {} batches ({} triangles)",
              m_tower.placedCount(), m_tower.batchCount(), m_tower.triangleCount());
}

/** The player's status box under the lane, as the in-game bar shows it. */
void PlayerSelectScene::drawStatusBox(const SelectLane& lane) {
    const auto left = static_cast<f32>(lane.x());
    const Rect box{left, static_cast<f32>(kBoxY), static_cast<f32>(SelectLane::kWidth),
                   static_cast<f32>(kBoxHeight)};
    const SelectLane::BoxMode mode = lane.boxMode();
    const s32 color = lane.active() ? lane.boxColor() : lane.index();
    const Texture* panel = nullptr;
    if (mode != SelectLane::BoxMode::Plain) {
        panel = selectTexture(std::format("S4_{}", classCode(lane.boxClass())));
    }
    if (panel != nullptr) {
        m_canvas.draw(*panel, box);
    } else if (const Texture* stone = staticTexture("S4")) {
        m_canvas.draw(*stone, box, boxTint(color, lane.active()));
    }
    if (const Texture* frame = staticTexture("S4_FRAME")) {
        m_canvas.draw(*frame, box);
    }
    if (mode == SelectLane::BoxMode::Plain) {
        return;
    }
    const Color tint = playerColor(color);
    const auto icon = [&](std::string_view name, s32 x) {
        if (const Texture* texture = staticTexture(name)) {
            m_canvas.draw(*texture,
                          Rect{static_cast<f32>(lane.x() + x), static_cast<f32>(kBoxIconY),
                               static_cast<f32>(kBoxIconSize), static_cast<f32>(kBoxIconSize)});
        }
    };
    icon("COIN", kBoxCoinX);
    icon("HEART", kBoxHeartX);
    if (m_score.ready()) {
        TextStyle style;
        style.color = tint;
        const std::string gold = std::format("{}", lane.save().gold);
        const std::string health = std::format("{}", lane.save().health());
        m_score.draw(m_canvas, lane.x() + kBoxGoldRight - m_score.measure(gold), kBoxValueY, gold,
                     style);
        m_score.draw(m_canvas, lane.x() + kBoxHealthRight - m_score.measure(health), kBoxValueY,
                     health, style);
    }
    const s32 centerX = -(lane.x() + SelectLane::kWidth / 2);
    if (mode == SelectLane::BoxMode::Status && m_smallCaps.ready()) {
        const s32 level = experienceLevel(lane.save().experience());
        m_smallCaps.draw(m_canvas, centerX, kBoxLevelY,
                         std::vformat(text("select.levelShort"), std::make_format_args(level)),
                         TextStyle{});
    }
    TextStyle nameStyle;
    nameStyle.scale = kBoxNameScale;
    nameStyle.color = tint;
    m_initials.draw(m_canvas, centerX, kBoxNameY, lane.save().name, nameStyle);
}

const Texture* PlayerSelectScene::selectTexture(std::string_view name) {
    const auto index = m_selectTextures.find(name);
    if (!index.has_value() || m_device == nullptr) {
        return nullptr;
    }
    try {
        return &m_selectTextures.texture(*m_device, *index);
    } catch (const std::exception& e) {
        log::warn("Player select: texture {}: {}", name, e.what());
        return nullptr;
    }
}

const Texture* PlayerSelectScene::staticTexture(std::string_view name) {
    const auto index = m_staticTextures.find(name);
    if (!index.has_value() || m_device == nullptr) {
        return nullptr;
    }
    try {
        return &m_staticTextures.texture(*m_device, *index);
    } catch (const std::exception& e) {
        log::warn("Player select: texture {}: {}", name, e.what());
        return nullptr;
    }
}

void PlayerSelectScene::playSound(SelectSound sound) {
    if (m_context.sounds == nullptr) {
        return;
    }
    const std::string_view name = soundName(sound);
    SoundSet* bank = &m_commonSounds;
    if (sound == SelectSound::Welcome || sound == SelectSound::WelcomeBack) {
        bank = &m_selectSounds;
    }
    if (!bank->loaded()) {
        return;
    }
    const auto index = bank->find(name);
    if (!index.has_value()) {
        return;
    }
    try {
        m_context.sounds->play(bank->sequence(*index), 1.0f, SoundCategory::Effects);
    } catch (const std::exception& e) {
        log::warn("Player select: cannot play {}: {}", name, e.what());
    }
}

void PlayerSelectScene::startMusic() {
    if (m_context.sounds == nullptr || !m_selectSounds.loaded()) {
        return;
    }
    const auto index = m_selectSounds.find(kSoundMusic);
    if (!index.has_value()) {
        return;
    }
    try {
        m_music =
            m_context.sounds->play(m_selectSounds.sequence(*index), 1.0f, SoundCategory::Music);
    } catch (const std::exception& e) {
        log::warn("Player select: cannot play {}: {}", kSoundMusic, e.what());
    }
}

bool PlayerSelectScene::musicPlaying() const {
    return m_context.sounds != nullptr && m_music != kNoSound &&
           m_context.sounds->isPlaying(m_music);
}

SelectOutcome PlayerSelectScene::update(f64 deltaSeconds, const Inputs& inputs) {
    m_tickRemainder += deltaSeconds * m_tickRate;
    auto ticks = static_cast<s32>(std::floor(m_tickRemainder));
    m_tickRemainder -= ticks;
    ticks = std::clamp(ticks, 0, kMaxTicksPerFrame);
    return step(ticks, inputs);
}

SelectOutcome PlayerSelectScene::step(s32 ticks, const Inputs& inputs) {
    if (!m_open) {
        return SelectOutcome::Running;
    }
    m_time += ticks;

    for (s32 i = 0; i < kLaneCount; ++i) {
        SelectLane& lane = m_lanes[static_cast<usize>(i)];
        if (!lane.active() && inputs[static_cast<usize>(i)].start) {
            lane.activate();
        }
    }

    bool leave = false;
    for (s32 i = 0; i < kLaneCount; ++i) {
        SelectLane::Frame frame;
        for (s32 j = 0; j < kLaneCount; ++j) {
            if (j == i) {
                continue;
            }
            const SelectLane& other = m_lanes[static_cast<usize>(j)];
            frame.othersActive = frame.othersActive || other.active();
            frame.othersSelecting = frame.othersSelecting || other.selecting();
            if (other.slotInUse().has_value()) {
                frame.slotsInUse |= 1U << *other.slotInUse();
            }
        }
        const SelectLane::Result result =
            m_lanes[static_cast<usize>(i)].update(inputs[static_cast<usize>(i)], ticks, frame);
        if (result == SelectLane::Result::Leave) {
            leave = true;
        }
    }

    bool anyActive = false;
    bool busy = false;
    for (const SelectLane& lane : m_lanes) {
        anyActive = anyActive || lane.active();
        busy = busy || lane.selecting() || lane.animating();
    }
    if (leave || !anyActive) {
        return SelectOutcome::Cancelled;
    }
    m_idleFrames = busy ? 0 : m_idleFrames + 1;
    return m_idleFrames > kIdleFrames ? SelectOutcome::Done : SelectOutcome::Running;
}

void PlayerSelectScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                               f32 frameHeight) {
    if (!m_open) {
        return;
    }
    const auto width = static_cast<f32>(m_screen.width);
    const auto height = static_cast<f32>(m_screen.height);
    if (m_tower.built() && m_camera.has_value()) {
        const WorldCamera& camera = *m_camera;
        m_tower.draw(device, camera.clipTransform(m_screen.horizontalFov, frameWidth, frameHeight,
                                                  frameProjection));
    }
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    if (!towerVisible()) {
        m_canvas.fill(Rect{0.0f, 0.0f, width, height}, kBackdrop);
    }

    const auto laneWidth = static_cast<f32>(SelectLane::kWidth);
    for (const SelectLane& lane : m_lanes) {
        drawStatusBox(lane);
    }
    for (s32 i = 0; i < kLaneCount; ++i) {
        const SelectLane& lane = m_lanes[static_cast<usize>(i)];
        const auto left = static_cast<f32>(lane.x());
        if (const Texture* top = selectTexture(std::format("S1_PLYR{}", i + 1))) {
            m_canvas.draw(*top, Rect{left, 0.0f, laneWidth, static_cast<f32>(kPanelTopHeight)});
        }
        if (const Texture* bottom = selectTexture(std::format("S2_PLYR{}", i + 1))) {
            m_canvas.draw(*bottom, Rect{left, static_cast<f32>(kPanelTopHeight), laneWidth,
                                        static_cast<f32>(kPanelBottomHeight)});
        }
        lane.drawImages(m_canvas);
        if (const Texture* border = selectTexture("S1_BORDER")) {
            m_canvas.draw(*border, Rect{left, 0.0f, laneWidth, static_cast<f32>(kPanelTopHeight)});
        }
        if (const Texture* border = selectTexture("S2_BORDER")) {
            m_canvas.draw(*border, Rect{left, static_cast<f32>(kPanelTopHeight), laneWidth,
                                        static_cast<f32>(kPanelBottomHeight)});
        }
    }
    for (const SelectLane& lane : m_lanes) {
        lane.drawText(m_canvas, m_time);
    }
    m_canvas.end();
}

} // namespace gdl::game
