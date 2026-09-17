#include "game/screens/TitleScene.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>
#include <utility>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

constexpr std::string_view kTitleDirectory = "TITLE";
constexpr std::string_view kStaticDirectory = "STATIC";
constexpr std::string_view kPowerupDirectory = "POWERUPS";
constexpr std::string_view kArrowTree = "ICON_ARROW";
constexpr std::string_view kFontFile = "fonts/font32.json";
constexpr std::string_view kCommonSounds = "audio/COMMON";
constexpr std::string_view kSelectSounds = "audio/SELECT";
constexpr std::string_view kSoundMove = "S_OPTMENUMOVVRT";
constexpr std::string_view kSoundSelect = "S_OPTMENUSEL";
constexpr std::string_view kSoundScroll = "S_OPTMENUSCROLL";
constexpr std::string_view kSoundMusic = "S_SELECTMUS";
constexpr std::string_view kFireRingTexture = "GREENCIRCTRANS";
constexpr std::string_view kFireMaskTexture = "GREENCIRCTRANSM";
constexpr std::string_view kScrollTexture = "SCROLL_A";
constexpr s32 kMaxTicksPerFrame = 6;
constexpr s32 kTextY = 320;
constexpr s32 kTextCenterX = -256;
constexpr s32 kGlowX = 192;
constexpr s32 kGlowSize = 128;
constexpr s32 kGlowTextRadius = 40;
constexpr s32 kGlowTextHold = 5;
constexpr s32 kFullAlpha = 255;
constexpr s32 kGlowHideThreshold = 8;
constexpr s32 kLoadingFadeSlope = 2;
constexpr s32 kMenuStart = 11;
constexpr s32 kMenuOptions = 12;
constexpr s32 kMenuAudio = 17;
constexpr s32 kMenuGameOptions = 16;
constexpr s32 kMenuCompass = 20;
constexpr s32 kMenuControls = 21;
constexpr s32 kTitleMenuY = 304;
constexpr s32 kOptionsMenuX = 128;
constexpr s32 kOptionsPromptY = 304;
constexpr s32 kOptionsBackdropY = 8;
constexpr s32 kOptionsBackdropWidth = 480;
constexpr s32 kOptionsBackdropHeight = 360;
constexpr Rect kOptionsBurnArea{290.0f, 142.0f, 224.0f, 172.0f};
constexpr Color kGlowColor = Color::rgba(130, 0, 234);
constexpr Color kOptionsOffColor = Color::rgba(92, 26, 3);

constexpr std::array<std::pair<s32, s32>, 4> kBackdropPositions{
    {{0, 0}, {256, 0}, {0, 256}, {256, 256}}};

/** Fills the first "{}" in a text-table entry, e.g. "Player {}" with the player number. */
std::string fillPlaceholder(std::string_view pattern, s32 value) {
    std::string out(pattern);
    const auto slot = out.find("{}");
    if (slot != std::string::npos) {
        out.replace(slot, 2, std::to_string(value));
    }
    return out;
}

} // namespace

std::string_view TitleScene::text(std::string_view id) const {
    return m_context.strings != nullptr ? m_context.strings->get(id) : id;
}

bool TitleScene::open(RenderDevice& device, const GameContext& context) {
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
        log::warn("Title screen: {}", e.what());
        m_titleTextures.releaseTextures();
        m_staticTextures.releaseTextures();
        return false;
    }
    m_device = &device;
    m_open = true;
    m_tickRemainder = 0.0;
    m_time = 0;
    m_idle = kIdleTicks;
    m_loadingTimer = 0;
    m_glowOpacity = 0;
    m_glowHidden = false;
    loadSounds(m_context.unpackedRoot);
    loadArrow(device, m_context.unpackedRoot);
    loadFireFrames(device);
    startMusic();
    return true;
}

void TitleScene::close() {
    if (m_context.sounds != nullptr && m_music != kNoSound) {
        m_context.sounds->stop(m_music);
    }
    m_music = kNoSound;
    m_titleMenu.close();
    m_optionsMenu.close();
    m_fire.reset();
    m_fireMasks.clear();
    m_fireRing.clear();
    m_scrollImage = nullptr;
    m_titleTextures.releaseTextures();
    m_staticTextures.releaseTextures();
    m_powerupTextures.releaseTextures();
    m_arrow = ModelSprite{};
    m_menuTextures = MenuTextures{};
    m_text.setFont(nullptr, nullptr);
    m_device = nullptr;
    m_open = false;
}

bool TitleScene::loadResources(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    if (!m_titleTextures.load(unpackedRoot / kTitleDirectory) ||
        !m_staticTextures.load(unpackedRoot / kStaticDirectory)) {
        log::warn("Title screen: unpacked textures not found under {} (run gdlunpack)",
                  unpackedRoot.string());
        return false;
    }
    if (!m_font32.load(unpackedRoot / kFontFile, kFont32SpaceWidth)) {
        return false;
    }
    for (usize i = 0; i < m_backdrops.size(); ++i) {
        const auto index = m_titleTextures.find(std::format("TITLE{:02}", i));
        if (!index.has_value()) {
            log::warn("Title screen: texture TITLE{:02} is missing", i);
            return false;
        }
        m_backdrops[i] = *index;
        m_titleTextures.texture(device, *index);
    }
    const auto glow = m_titleTextures.find("GLOWCROP_");
    if (!glow.has_value() || *glow + kGlowFrames > m_titleTextures.size()) {
        log::warn("Title screen: glow animation is missing");
        return false;
    }
    m_glowBase = *glow;
    for (s32 frame = 0; frame < kGlowFrames; ++frame) {
        m_titleTextures.texture(device, m_glowBase + static_cast<u32>(frame));
    }

    m_device = &device;
    m_menuTextures.font = staticTexture("FONT32");
    if (m_menuTextures.font == nullptr) {
        log::warn("Title screen: texture FONT32 is missing");
        return false;
    }
    m_menuTextures.glow = staticTexture("FONT32_GLOW");
    m_menuTextures.parchment = staticTexture("FONT32_PARCH");
    m_menuTextures.arrows = staticTexture("ARROWS");
    for (usize i = 0; i < m_menuTextures.garamond.size(); ++i) {
        m_menuTextures.garamond[i] = staticTexture(std::format("FONT32GAR{}", i));
    }
    m_menuTextures.backdrop = staticTexture(kScrollTexture);
    for (usize i = 0; i < m_menuTextures.burn.size(); ++i) {
        m_menuTextures.burn[i] = staticTexture("LOGO_BURN1", static_cast<u32>(i));
    }
    m_text.setFont(&m_font32, m_menuTextures.font);
    return true;
}

void TitleScene::loadSounds(const std::filesystem::path& unpackedRoot) {
    if (m_context.sounds == nullptr) {
        return;
    }
    if (!m_commonSounds.load(unpackedRoot / kCommonSounds) ||
        !m_selectSounds.load(unpackedRoot / kSelectSounds)) {
        log::warn("Title screen: unpacked sound banks not found under {}", unpackedRoot.string());
    }
}

void TitleScene::loadArrow(RenderDevice& device, const std::filesystem::path& unpackedRoot) {
    m_arrow = ModelSprite{};
    const std::filesystem::path directory = unpackedRoot / kPowerupDirectory;
    if (!m_powerupTextures.load(directory) || !m_powerupModels.load(directory) ||
        !m_powerupTrees.load(directory)) {
        log::warn("Title screen: unpacked POWERUPS archive not found; using the flat arrow");
        return;
    }
    const auto tree = m_powerupTrees.find(kArrowTree);
    if (!tree.has_value() ||
        !m_arrow.bind(m_powerupTrees.tree(*tree), m_powerupModels, m_powerupTextures, device)) {
        return;
    }
    m_menuTextures.icon = &m_arrow;
}

/** The burn effect's frames: the flame ring and the cut-out mask follow their base entries. */
void TitleScene::loadFireFrames(RenderDevice& device) {
    m_fireMasks.clear();
    m_fireRing.clear();
    m_scrollImage = nullptr;
    const auto ring = m_staticTextures.find(kFireRingTexture);
    const auto mask = m_staticTextures.find(kFireMaskTexture);
    const auto scroll = m_staticTextures.find(kScrollTexture);
    if (!ring.has_value() || !mask.has_value() || !scroll.has_value()) {
        log::warn("Title screen: burn effect textures are missing");
        return;
    }
    const auto frames = static_cast<u32>(FireScroll::kFrameCount);
    try {
        for (u32 i = 1; i <= frames && *ring + i < m_staticTextures.size(); ++i) {
            m_fireRing.push_back(&m_staticTextures.texture(device, *ring + i));
        }
        for (u32 i = 1; i <= frames && *mask + i < m_staticTextures.size(); ++i) {
            m_fireMasks.push_back(&m_staticTextures.image(*mask + i));
        }
        m_scrollImage = &m_staticTextures.image(*scroll);
    } catch (const std::exception& e) {
        log::warn("Title screen: burn effect frames: {}", e.what());
        m_fireMasks.clear();
        m_fireRing.clear();
        m_scrollImage = nullptr;
    }
}

const Texture* TitleScene::staticTexture(std::string_view name, u32 frame) {
    const auto index = m_staticTextures.find(name);
    if (!index.has_value() || *index + frame >= m_staticTextures.size()) {
        return nullptr;
    }
    return &m_staticTextures.texture(*m_device, *index + frame);
}

void TitleScene::startMusic() {
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
        log::warn("Title screen: cannot play {}: {}", kSoundMusic, e.what());
    }
}

void TitleScene::playMenuSound(std::string_view name) {
    if (m_context.sounds == nullptr || !m_commonSounds.loaded()) {
        return;
    }
    const auto index = m_commonSounds.find(name);
    if (!index.has_value()) {
        return;
    }
    try {
        m_context.sounds->play(m_commonSounds.sequence(*index), 1.0f, SoundCategory::Effects);
    } catch (const std::exception& e) {
        log::warn("Title screen: cannot play {}: {}", name, e.what());
    }
}

bool TitleScene::musicPlaying() const {
    return m_context.sounds != nullptr && m_music != kNoSound &&
           m_context.sounds->isPlaying(m_music);
}

TitleOutcome TitleScene::update(f64 deltaSeconds, const MenuInput& input) {
    m_tickRemainder += deltaSeconds * m_tickRate;
    auto ticks = static_cast<s32>(std::floor(m_tickRemainder));
    m_tickRemainder -= ticks;
    ticks = std::clamp(ticks, 0, kMaxTicksPerFrame);
    return step(ticks, input);
}

TitleOutcome TitleScene::step(s32 ticks, const MenuInput& rawInput) {
    if (!m_open) {
        return TitleOutcome::Running;
    }
    m_time += ticks;
    m_glowOpacity = static_cast<u8>(std::min(kFullAlpha, m_time * kFullAlpha / kGlowFadeInTicks));
    m_glowHidden = false;

    // While the scroll burns the original blanks the controls, so nothing reacts to input.
    const bool burning = m_fire.active();
    m_fire.step(ticks);
    const MenuInput input = burning ? MenuInput{} : rawInput;

    if (m_loadingTimer > 0) {
        m_loadingTimer = std::max(0, m_loadingTimer - ticks);
        m_glowOpacity = static_cast<u8>(std::min(kFullAlpha, m_loadingTimer * kLoadingFadeSlope));
        m_glowHidden = m_glowOpacity < kGlowHideThreshold;
        return m_loadingTimer == 0 ? TitleOutcome::StartGame : TitleOutcome::Running;
    }

    if (m_optionsMenu.isOpen()) {
        const MenuEvent event = m_optionsMenu.update(input, ticks);
        if (event.action == MenuAction::Back) {
            closeOptionsMenu();
        } else if (event.action == MenuAction::Choice) {
            playMenuSound(kSoundSelect);
            log::info("Title screen: options item {} is not built yet", event.code);
        } else if (event.action == MenuAction::Moved) {
            playMenuSound(kSoundMove);
        }
        m_glowHidden = true;
        m_idle = kIdleTicks;
        return TitleOutcome::Running;
    }
    if (m_fire.active()) {
        m_glowHidden = true;
        m_idle = kIdleTicks;
        return TitleOutcome::Running;
    }

    if (m_titleMenu.isOpen()) {
        const MenuEvent event = m_titleMenu.update(input, ticks);
        if (event.action == MenuAction::Choice && event.code == kMenuStart) {
            m_titleMenu.close();
            m_loadingTimer = kLoadingTicks;
        } else if (event.action == MenuAction::Choice && event.code == kMenuOptions) {
            playMenuSound(kSoundSelect);
            openOptionsMenu();
        } else if (event.action == MenuAction::Back) {
            m_titleMenu.close();
        } else if (event.action == MenuAction::Moved) {
            playMenuSound(kSoundMove);
        }
        m_idle = kIdleTicks;
        return TitleOutcome::Running;
    }

    if (input.start || input.select) {
        playMenuSound(kSoundSelect);
        openTitleMenu();
        m_idle = kIdleTicks;
        return TitleOutcome::Running;
    }

    m_idle -= ticks;
    if (m_idle < kIdleFadeTicks) {
        m_glowOpacity = static_cast<u8>(std::max(0, m_idle) * kFullAlpha / kIdleFadeTicks);
    }
    return m_idle <= 0 ? TitleOutcome::TimedOut : TitleOutcome::Running;
}

void TitleScene::openTitleMenu() {
    MenuDefinition menu;
    menu.x = kTextCenterX;
    menu.y = kTitleMenuY;
    menu.items = {{std::string(text("menu.start")), kMenuStart},
                  {std::string(text("menu.options")), kMenuOptions}};
    menu.startSelects = true;
    m_titleMenu.open(menu, m_text, m_screen);
}

void TitleScene::openOptionsMenu() {
    MenuDefinition menu;
    menu.title = std::string(text("menu.options"));
    menu.x = kOptionsMenuX;
    menu.y = -1;
    menu.items = {{std::string(text("menu.audio")), kMenuAudio},
                  {std::string(text("menu.gameOptions")), kMenuGameOptions},
                  {std::string(text("menu.compass")), kMenuCompass},
                  {std::string(text("menu.controls")), kMenuControls}};
    menu.colors.off = kOptionsOffColor;
    menu.prompts = true;
    menu.backLabel = std::string(text("menu.back"));
    menu.selectLabel = std::string(text("menu.select"));
    menu.promptY = kOptionsPromptY;
    menu.fades = true;
    menu.parchmentFont = true;
    menu.garamondIntro = true;
    menu.playerLabel = fillPlaceholder(text("menu.player"), 1);
    menu.backdrop = std::string(kScrollTexture);
    menu.backdropX = -1;
    menu.backdropY = kOptionsBackdropY;
    menu.backdropWidth = kOptionsBackdropWidth;
    menu.backdropHeight = kOptionsBackdropHeight;
    menu.burn = "LOGO_BURN1";
    menu.burnArea = kOptionsBurnArea;
    m_optionsMenu.open(menu, m_text, m_screen);
}

/** Backing out hands the scroll to the burn effect while the text fades out. */
void TitleScene::closeOptionsMenu() {
    const bool canBurn = m_device != nullptr && m_scrollImage != nullptr && !m_fireMasks.empty() &&
                         !m_fireRing.empty();
    if (canBurn && m_fire.start(*m_device, m_optionsMenu.backdropArea(), *m_scrollImage,
                                m_fireMasks, m_fireRing)) {
        m_optionsMenu.releaseBackdrop();
        playMenuSound(kSoundScroll);
    }
    m_optionsMenu.close();
}

void TitleScene::drawGlowText(s32 x, s32 y, std::string_view label) {
    TextStyle glow;
    glow.color = kGlowColor.withAlpha(pulseOpacity(m_time, kGlowTextRadius, kGlowTextHold));
    glow.texture = m_menuTextures.glow != nullptr ? m_menuTextures.glow : m_menuTextures.font;
    glow.expand = OptionMenu::kGlowExpand;
    m_text.draw(m_canvas, x, y, label, glow);
    m_text.draw(m_canvas, x, y, label, TextStyle{});
}

void TitleScene::render(RenderDevice& device, const Mat4& frameProjection, f32 frameWidth,
                        f32 frameHeight) {
    if (!m_open) {
        return;
    }
    m_fire.prepare(device);
    m_canvas.begin(device, makeVirtualScreenTransform(
                               frameProjection, static_cast<f32>(m_screen.width),
                               static_cast<f32>(m_screen.height), frameWidth, frameHeight));
    for (usize i = 0; i < m_backdrops.size(); ++i) {
        const TextureSetEntry& entry = m_titleTextures.entry(m_backdrops[i]);
        const Rect area{static_cast<f32>(kBackdropPositions[i].first),
                        static_cast<f32>(kBackdropPositions[i].second),
                        static_cast<f32>(entry.width), static_cast<f32>(entry.height)};
        m_canvas.draw(m_titleTextures.texture(device, m_backdrops[i]), area);
    }
    if (!m_glowHidden && m_glowOpacity > 0) {
        const auto frame = static_cast<u32>((m_time >> 2) % kGlowFrames);
        m_canvas.draw(m_titleTextures.texture(device, m_glowBase + frame),
                      Rect{static_cast<f32>(kGlowX), 0.0f, static_cast<f32>(kGlowSize),
                           static_cast<f32>(kGlowSize)},
                      Color::white().withAlpha(m_glowOpacity));
    }
    if (m_loadingTimer > 0) {
        drawGlowText(kTextCenterX, kTextY, text("title.loading"));
    } else if (!m_titleMenu.isOpen() && !m_optionsMenu.isOpen() && !m_fire.active()) {
        drawGlowText(kTextCenterX, kTextY, text("title.pressStart"));
    }
    m_fire.draw(m_canvas);
    if (m_optionsMenu.isOpen()) {
        m_optionsMenu.draw(m_canvas, m_text, m_menuTextures);
    } else if (m_titleMenu.isOpen()) {
        m_titleMenu.draw(m_canvas, m_text, m_menuTextures);
    }
    m_canvas.end();
}

} // namespace gdl::game
