#include "game/screens/PlayerSelectScene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <format>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"

#include "game/players/Progression.h"

namespace gdl::game {

namespace {

constexpr std::string_view kSelectDirectory = "SELECT";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kClassDataDirectory = "pdata";
constexpr std::string_view kFont32File = "fonts/font32.json";
constexpr std::string_view kFont8File = "fonts/font8x8.json";
constexpr std::string_view kInitialsFile = "fonts/initials.json";
constexpr std::string_view kCommonSounds = "audio/COMMON";
constexpr std::string_view kSelectSounds = "audio/SELECT";
constexpr std::string_view kSoundMusic = "S_SELECTMUS";
constexpr int kFont32SpaceWidth = 16;
constexpr int kFont8SpaceWidth = 8;
constexpr int kInitialsSpaceWidth = 12;
constexpr int kMaxTicksPerFrame = 6;
constexpr int kPanelTopHeight = 256;
constexpr int kPanelBottomHeight = 64;
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

bool PlayerSelectScene::open(RenderDevice& device, const GameContext& context, int startingPlayer) {
    close();
    m_context = context;
    m_screen = MenuScreen{};
    if (m_context.config != nullptr) {
        m_screen.width = static_cast<int>(m_context.config->display.virtualWidth);
        m_screen.height = static_cast<int>(m_context.config->display.virtualHeight);
        m_screen.horizontalFov = m_context.config->horizontalFovRadians();
        m_tickRate = static_cast<int>(m_context.config->timing.tickRate);
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
    if (!m_boxes.load(device, m_context.unpackedRoot, m_context.strings)) {
        log::warn("Player select: the status boxes are unavailable");
    }
    m_open = true;
    m_tickRemainder = 0.0;
    m_time = 0;
    m_idleFrames = 0;
    loadSounds(m_context.unpackedRoot);
    loadTower(device);

    if (!m_classes.load(m_context.unpackedRoot / kClassDataDirectory)) {
        log::warn("Player select: class stats are unavailable (run gdlunpack)");
    }
    if (m_context.config != nullptr) {
        const std::size_t slots = m_context.config->save.slots;
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
    m_services.playSound = [this](SelectSound sound, const SelectLane& lane) {
        playSound(sound, lane);
    };
    m_services.selectTexture = [this](std::string_view name) { return selectTexture(name); };
    m_services.staticTexture = [this](std::string_view name) { return staticTexture(name); };
    m_services.glowSheet = staticTexture("FONT32_GLOW");
    m_services.keyboardLane = MenuInputSource::kKeyboardPlayer;
    m_services.menuTextures.font = staticTexture("FONT32");
    m_services.menuTextures.glow = m_services.glowSheet;
    for (int i = 0; i < kLaneCount; ++i) {
        m_lanes[static_cast<std::size_t>(i)].reset(i, &m_services);
    }
    if (startingPlayer >= 0 && startingPlayer < kLaneCount) {
        m_lanes[static_cast<std::size_t>(startingPlayer)].activate();
    }
    startMusic();
    return true;
}

void PlayerSelectScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    for (int i = 0; i < kLaneCount; ++i) {
        m_lanes[static_cast<std::size_t>(i)].reset(i, nullptr);
    }
    m_camera.reset();
    m_tower = nullptr;
    m_boxes.release();
    m_selectTextures.releaseTextures();
    m_staticTextures.releaseTextures();
    m_large.setFont(nullptr, nullptr);
    m_small.setFont(nullptr, nullptr);
    m_initials.setFont(nullptr, nullptr);
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
    for (int i = 0; i < kLaneCount; ++i) {
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
/** Looks into the shared tower from its entrance camera, loading it the first time. */
void PlayerSelectScene::loadTower(RenderDevice& device) {
    m_camera.reset();
    m_tower = m_context.tower;
    if (m_tower == nullptr) {
        return;
    }
    // The select screen looks into the tower, whatever level was last played.
    if ((!m_tower->built() || !m_tower->isTower()) &&
        !m_tower->load(device, m_context.unpackedRoot)) {
        return;
    }
    m_camera = m_tower->entranceCamera();
    if (!m_camera.has_value()) {
        log::warn("Player select: the tower has no entrance camera");
    }
}

/** The player's status box under the lane, as the in-game bar shows it. */
void PlayerSelectScene::drawStatusBox(const SelectLane& lane) {
    StatusBoxView view;
    view.active = lane.active();
    switch (lane.boxMode()) {
    case SelectLane::BoxMode::Character: view.mode = StatusBoxView::Mode::Character; break;
    case SelectLane::BoxMode::Status: view.mode = StatusBoxView::Mode::Status; break;
    default: view.mode = StatusBoxView::Mode::Plain; break;
    }
    view.classIndex = lane.boxClass();
    view.color = lane.active() ? lane.boxColor() : lane.index();
    view.name = lane.save().name;
    view.level = experienceLevel(lane.save().experience());
    view.gold = lane.save().gold;
    view.health = lane.save().health();
    m_boxes.draw(m_canvas, lane.index(), view, false);
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

void PlayerSelectScene::playSound(SelectSound sound, const SelectLane& lane) {
    if (m_context.sounds == nullptr) {
        return;
    }
    const std::string_view name = soundName(sound);
    const bool greeting = sound == SelectSound::Welcome || sound == SelectSound::WelcomeBack;
    SoundSet* bank = greeting ? &m_selectSounds : &m_commonSounds;
    if (!bank->loaded()) {
        return;
    }
    const auto index = bank->find(name);
    if (!index.has_value()) {
        return;
    }
    try {
        const SoundHandle handle =
            m_context.sounds->play(bank->sequence(*index), 1.0f, SoundCategory::Effects);
        if (greeting) {
            greetCharacter(handle, lane.save());
        }
    } catch (const std::exception& e) {
        log::warn("Player select: cannot play {}: {}", name, e.what());
    }
}

/** After his welcome Sumner names the costume and class ("red warrior"); he has no such
 * line for himself. */
void PlayerSelectScene::greetCharacter(SoundHandle greeting, const CharacterSave& save) {
    m_greeting = greeting;
    const std::string line =
        std::format("S_{}{}1S", colorCode(save.color), classCode(save.character));
    const auto index = m_selectSounds.find(line);
    if (!index.has_value()) {
        return;
    }
    m_greeting = m_context.sounds->playAfter(greeting, m_selectSounds.sequence(*index), 1.0f,
                                             SoundCategory::Effects);
}

bool PlayerSelectScene::speaking() const {
    return m_context.sounds != nullptr && m_context.sounds->isPlaying(m_greeting);
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

MenuInputSource PlayerSelectScene::inputSource(int index) const {
    MenuInputSource source = MenuInputSource::forPlayer(index);
    source.text = lane(index).typing();
    return source;
}

SelectOutcome PlayerSelectScene::update(double deltaSeconds, const Inputs& inputs) {
    m_tickRemainder += deltaSeconds * m_tickRate;
    auto ticks = static_cast<int>(std::floor(m_tickRemainder));
    m_tickRemainder -= ticks;
    ticks = std::clamp(ticks, 0, kMaxTicksPerFrame);
    return step(ticks, inputs);
}

SelectOutcome PlayerSelectScene::step(int ticks, const Inputs& inputs) {
    if (!m_open) {
        return SelectOutcome::Running;
    }
    m_time += ticks;
    if (m_tower != nullptr && m_tower->built()) {
        m_tower->update(static_cast<float>(ticks) / static_cast<float>(m_tickRate));
    }

    for (int i = 0; i < kLaneCount; ++i) {
        SelectLane& lane = m_lanes[static_cast<std::size_t>(i)];
        if (!lane.active() && inputs[static_cast<std::size_t>(i)].start) {
            lane.activate();
        }
    }

    bool leave = false;
    for (int i = 0; i < kLaneCount; ++i) {
        SelectLane::Frame frame;
        for (int j = 0; j < kLaneCount; ++j) {
            if (j == i) {
                continue;
            }
            const SelectLane& other = m_lanes[static_cast<std::size_t>(j)];
            frame.othersActive = frame.othersActive || other.active();
            frame.othersSelecting = frame.othersSelecting || other.selecting();
            if (other.slotInUse().has_value()) {
                frame.slotsInUse |= 1U << *other.slotInUse();
            }
        }
        const SelectLane::Result result = m_lanes[static_cast<std::size_t>(i)].update(
            inputs[static_cast<std::size_t>(i)], ticks, frame);
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

void PlayerSelectScene::render(RenderDevice& device, const Mat4& frameProjection, float frameWidth,
                               float frameHeight) {
    if (!m_open) {
        return;
    }
    const auto width = static_cast<float>(m_screen.width);
    const auto height = static_cast<float>(m_screen.height);
    if (towerVisible()) {
        GDL_VERIFY(m_camera.has_value(), "A visible tower requires its camera");
        const WorldCamera& camera = *m_camera;
        m_tower->draw(
            device,
            camera.clipTransform(m_screen.horizontalFov, frameWidth, frameHeight, frameProjection),
            camera);
    }
    m_canvas.begin(device, makeVirtualScreenTransform(frameProjection, width, height, frameWidth,
                                                      frameHeight));
    if (!towerVisible()) {
        m_canvas.fill(Rect{0.0f, 0.0f, width, height}, kBackdrop);
    }

    const auto laneWidth = static_cast<float>(SelectLane::kWidth);
    for (const SelectLane& lane : m_lanes) {
        drawStatusBox(lane);
    }
    for (int i = 0; i < kLaneCount; ++i) {
        const SelectLane& lane = m_lanes[static_cast<std::size_t>(i)];
        const auto left = static_cast<float>(lane.x());
        if (const Texture* top = selectTexture(std::format("S1_PLYR{}", i + 1))) {
            m_canvas.draw(*top, Rect{left, 0.0f, laneWidth, static_cast<float>(kPanelTopHeight)});
        }
        if (const Texture* bottom = selectTexture(std::format("S2_PLYR{}", i + 1))) {
            m_canvas.draw(*bottom, Rect{left, static_cast<float>(kPanelTopHeight), laneWidth,
                                        static_cast<float>(kPanelBottomHeight)});
        }
        lane.drawImages(m_canvas);
        if (const Texture* border = selectTexture("S1_BORDER")) {
            m_canvas.draw(*border,
                          Rect{left, 0.0f, laneWidth, static_cast<float>(kPanelTopHeight)});
        }
        if (const Texture* border = selectTexture("S2_BORDER")) {
            m_canvas.draw(*border, Rect{left, static_cast<float>(kPanelTopHeight), laneWidth,
                                        static_cast<float>(kPanelBottomHeight)});
        }
    }
    for (const SelectLane& lane : m_lanes) {
        lane.drawText(m_canvas, m_time);
    }
    m_canvas.end();
}

} // namespace gdl::game
