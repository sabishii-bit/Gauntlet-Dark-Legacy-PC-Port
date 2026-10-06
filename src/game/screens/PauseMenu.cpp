#include "game/screens/PauseMenu.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
// The yes/no OPTMENU records: title scale 1.2, text x -256, SCROLL_A at (-1, 64) 320 by 220.
constexpr f32 kQuitTitleScale = 1.2f;
constexpr s32 kQuitTextX = -256;
constexpr s32 kQuitBackdropY = 64;
constexpr s32 kQuitBackdropWidth = 320;
constexpr s32 kQuitBackdropHeight = 220;
constexpr std::string_view kSealTexture = "LOGO_BURN1";
constexpr Rect kSealArea{290, 142, 224, 172};
} // namespace
PauseMenu::~PauseMenu() {
    stopSounds();
}

std::string PauseMenu::text(std::string_view id) const {
    return std::string(m_context.strings != nullptr ? m_context.strings->get(id) : id);
}
bool PauseMenu::open(RenderDevice& device, const GameContext& context,
                     std::span<const PartyMember> party, s32 player) {
    close();
    m_context = context;
    m_device = &device;
    m_party.assign(party.begin(), party.end());
    m_player = player;
    m_inTower = context.tower == nullptr || context.tower->isTower();
    m_inSecretWorld = context.tower != nullptr && context.tower->ref().isSecret();
    if (std::ranges::none_of(party,
                             [player](const auto& member) { return member.player == player; })) {
        return false;
    }
    const auto config = context.config != nullptr ? *context.config : GameConfig{};
    m_screen = {static_cast<s32>(config.display.virtualWidth),
                static_cast<s32>(config.display.virtualHeight), config.horizontalFovRadians()};
    try {
        if (!m_textures.load(context.unpackedRoot / "STATIC") ||
            !m_font.load(context.unpackedRoot / "fonts/font32.json", 16)) {
            return false;
        }
        const auto font = m_textures.find("FONT32");
        if (!font) {
            return false;
        }
        m_art.font = &m_textures.texture(device, *font);
        for (usize i = 0; i < AudioSlider::kTextures.size(); ++i) {
            if (const auto image = m_textures.find(AudioSlider::kTextures[i])) {
                m_art.audioSlider.textures[i] = &m_textures.texture(device, *image);
            }
        }
        m_text.setFont(&m_font, m_art.font);
        if (const auto scroll = m_textures.find("SCROLL_A")) {
            m_art.backdrop = &m_textures.texture(device, *scroll);
        }
        if (const auto glow = m_textures.find("FONT32_GLOW")) {
            m_art.glow = &m_textures.texture(device, *glow);
        }
        if (const auto parchment = m_textures.find("FONT32_PARCH")) {
            m_art.parchment = &m_textures.texture(device, *parchment);
        }
        loadDecorations(device);
        loadFireFrames(device);
        if (m_context.sounds != nullptr) {
            m_commonSounds.load(context.unpackedRoot / "audio/COMMON");
        }
        showMain();
        m_open = true;
        playSound("S_OPTMENUSEL");
        return true;
    } catch (const std::exception& e) {
        log::warn("Pause menu: {}", e.what());
        close();
        return false;
    }
}
void PauseMenu::close() {
    stopSounds();
    m_settings.close();
    m_open = false;
    m_tickRemainder = 0.0;
    m_pendingOutcome = PauseOutcome::Running;
    m_fire.reset();
    m_fireMasks.clear();
    m_fireRing.clear();
    m_scrollImage = nullptr;
    m_device = nullptr;
    m_menu = OptionMenu{};
    m_settings = SettingsMenu{};
    m_art = MenuTextures{};
    m_text.setFont(nullptr, nullptr);
    m_textures.releaseTextures();
    m_powerupTextures.releaseTextures();
    m_arrow = ModelSprite{};
    m_party.clear();
}
void PauseMenu::loadDecorations(RenderDevice& device) {
    if (const auto arrows = m_textures.find("ARROWS")) {
        m_art.arrows = &m_textures.texture(device, *arrows);
    }
    if (const auto seal = m_textures.find(kSealTexture);
        seal && m_textures.size() - *seal >= m_art.burn.size()) {
        for (usize frame = 0; frame < m_art.burn.size(); ++frame) {
            m_art.burn[frame] = &m_textures.texture(device, *seal + static_cast<u32>(frame));
        }
    }
    const auto directory = m_context.unpackedRoot / "POWERUPS";
    if (m_powerupTextures.load(directory) && m_powerupModels.load(directory) &&
        m_powerupTrees.load(directory)) {
        if (const auto tree = m_powerupTrees.find("ICON_ARROW");
            tree &&
            m_arrow.bind(m_powerupTrees.tree(*tree), m_powerupModels, m_powerupTextures, device)) {
            m_art.icon = &m_arrow;
        }
    }
}
void PauseMenu::playSound(std::string_view name) {
    if (m_context.sounds == nullptr || !m_commonSounds.loaded()) {
        return;
    }
    const auto index = m_commonSounds.find(name);
    if (!index) {
        return;
    }
    try {
        std::erase_if(m_soundHandles,
                      [&](SoundHandle handle) { return !m_context.sounds->isPlaying(handle); });
        const auto handle =
            m_context.sounds->play(m_commonSounds.sequence(*index), 1, SoundCategory::Effects);
        if (handle != kNoSound) {
            m_soundHandles.push_back(handle);
        }
    } catch (const std::exception& error) {
        log::warn("Pause menu sound {}: {}", name, error.what());
    }
}

void PauseMenu::stopSounds() {
    // Queued sequences borrow COMMON's samples. Stop only this menu's voices before reloading
    // or destroying its bank, and detach the output so a later close is harmless.
    if (m_context.sounds != nullptr) {
        for (const auto handle : m_soundHandles) {
            m_context.sounds->stop(handle);
        }
    }
    m_soundHandles.clear();
    m_context.sounds = nullptr;
}

void PauseMenu::loadFireFrames(RenderDevice& device) {
    const auto ring = m_textures.find("GREENCIRCTRANS");
    const auto mask = m_textures.find("GREENCIRCTRANSM");
    const auto scroll = m_textures.find("SCROLL_A");
    constexpr auto kFrames = static_cast<u32>(BurnDialogueScroll::kFrameCount);
    if (!ring || !mask || !scroll || m_textures.size() - *ring <= kFrames ||
        m_textures.size() - *mask <= kFrames) {
        log::warn("Pause menu: scroll dismissal textures are missing");
        return;
    }
    for (u32 frame = 1; frame <= kFrames; ++frame) {
        m_fireRing.push_back(&m_textures.texture(device, *ring + frame));
        m_fireMasks.push_back(&m_textures.image(*mask + frame));
    }
    m_scrollImage = &m_textures.image(*scroll);
}

PauseOutcome PauseMenu::dismiss(PauseOutcome outcome) {
    if (m_device == nullptr || m_scrollImage == nullptr ||
        !m_fire.start(*m_device, m_menu.backdropArea(), *m_scrollImage, m_fireMasks, m_fireRing)) {
        return outcome;
    }
    m_pendingOutcome = outcome;
    m_menu.releaseBackdrop();
    m_menu.closeWithFade();
    playSound("S_OPTMENUSCROLL");
    return PauseOutcome::Running;
}

void PauseMenu::playMenuSound(const MenuEvent& event, bool horizontal) {
    if (event.action == MenuAction::Moved) {
        playSound(horizontal ? "S_OPTMENUMOVHRZ" : "S_OPTMENUMOVVRT");
    } else if (event.action == MenuAction::Choice) {
        playSound("S_OPTMENUSEL");
    } else if (event.action == MenuAction::Back) {
        playSound("S_OPTMENUEXIT");
    }
}
MenuDefinition PauseMenu::backdrop() const {
    auto menu = MenuDefinition::parchment();
    menu.backdropX = 16;
    menu.burn = kSealTexture;
    menu.burnArea = kSealArea;
    menu.playerLabel = std::format("{}: {}", text("files.player"), m_player + 1);
    return menu;
}
void PauseMenu::showMain() {
    m_page = Page::Main;
    auto menu = backdrop();
    menu.title = text(m_inTower ? "pause.tower" : "pause.game");
    menu.items = {{text("pause.settings"), 3}};
    if (m_inTower) {
        menu.items.push_back({text("pause.manage"), 5});
        menu.items.push_back({text("pause.shop"), 6});
        menu.items.push_back({text("pause.inventory"), 7});
    }
    // The secret world cannot be quit (options.c 1462: OPT_QUITLEVEL greyed for world 12).
    menu.items.push_back(
        {text(m_inTower ? "pause.quit" : "pause.quitLevel"), 4, 0, m_inTower || !m_inSecretWorld});
    m_menu.open(menu, m_text, m_screen);
}
/** The retail yes/no dialogs (0x8011EB1C "Quit Game?", 0x8011E8E0 "Abort Level?"): a small
 * centred parchment fading in, with only the title and No then Yes, No first. */
void PauseMenu::showQuit() {
    m_page = Page::Quit;
    auto menu = backdrop();
    menu.title = text(m_inTower ? "pause.quitConfirm" : "pause.abortConfirm");
    menu.titleScale = kQuitTitleScale;
    menu.x = kQuitTextX;
    menu.backdropX = -1;
    menu.backdropY = kQuitBackdropY;
    menu.backdropWidth = kQuitBackdropWidth;
    menu.backdropHeight = kQuitBackdropHeight;
    menu.parchmentFont = true;
    menu.fades = true;
    menu.prompts = false;
    menu.playerLabel.clear();
    menu.burn.clear();
    menu.burnArea = {};
    menu.items = {{text("pause.no"), 0}, {text("pause.yes"), 1}};
    m_menu.open(menu, m_text, m_screen);
}
PauseOutcome PauseMenu::update(f64 seconds, const MenuInput& rawInput) {
    if (!m_open) {
        return PauseOutcome::Running;
    }
    const auto input = mapMenuPointer(rawInput, m_pointerTransform);
    const auto rate =
        m_context.config != nullptr ? m_context.config->timing.tickRate : TimingConfig{}.tickRate;
    m_tickRemainder += std::max(seconds, 0.0) * rate;
    constexpr f64 kRoundingTolerance = 1.0e-9;
    const auto ticks = static_cast<s32>(std::floor(m_tickRemainder + kRoundingTolerance));
    m_tickRemainder = std::max(0.0, m_tickRemainder - ticks);
    if (m_pendingOutcome != PauseOutcome::Running) {
        m_menu.update({}, ticks);
        m_fire.step(ticks);
        return m_fire.active() ? PauseOutcome::Running : m_pendingOutcome;
    }
    if (m_page == Page::Options) {
        const auto previous = m_settings.page();
        const auto event = m_settings.update(input, ticks);
        playMenuSound(event, input.left || input.right);
        if (!m_settings.audioSample().empty()) {
            playSound(m_settings.audioSample());
        }
        if (event.action == MenuAction::None && previous != m_settings.page() &&
            (input.back || input.escape || input.pointerBack)) {
            playSound("S_OPTMENUEXIT");
        }
        if (event.action == MenuAction::Back) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    playMenuSound(event, false);
    if (m_page == Page::Quit) {
        if (event.action == MenuAction::Choice && event.code == 1) {
            return dismiss(m_inTower ? PauseOutcome::Title : PauseOutcome::ReturnTower);
        }
        if (event.action == MenuAction::Back || event.action == MenuAction::Choice) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    if (event.action == MenuAction::Back || (input.start && !input.select)) {
        return dismiss(PauseOutcome::Resume);
    }
    if (event.action != MenuAction::Choice) {
        return PauseOutcome::Running;
    }
    if (event.code == 3) {
        m_page = Page::Options;
        m_settings.open(m_context.config != nullptr ? *m_context.config : GameConfig{},
                        m_context.strings, m_context.saveSettings, m_text, m_screen, backdrop(),
                        m_inTower ? SettingsMenu::Scope::Tower : SettingsMenu::Scope::Level,
                        m_context.previewAudio, {}, m_context.displayOptions,
                        m_context.previewVideo);
    } else if (event.code == 5) {
        return PauseOutcome::Manage;
    } else if (event.code == 6) {
        return PauseOutcome::Shop;
    } else if (event.code == 7) {
        return PauseOutcome::Inventory;
    } else if (event.code == 4) {
        showQuit();
    }
    return PauseOutcome::Running;
}
bool PauseMenu::musicAudible() const {
    return m_open && m_page == Page::Options && m_settings.page() == SettingsMenu::Page::Audio;
}
void PauseMenu::prepare(RenderDevice& device) {
    if (m_open) {
        m_fire.prepare(device);
    }
}

void PauseMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    const auto virtualWidth = static_cast<f32>(m_screen.width);
    const auto virtualHeight = static_cast<f32>(m_screen.height);
    m_pointerTransform =
        makeVirtualScreenTransform(projection, virtualWidth, virtualHeight, width, height);
    m_canvas.begin(device, m_pointerTransform);
    m_canvas.fillScreen(Color::rgba(0, 0, 0, 150));
    m_fire.draw(m_canvas);
    auto art = m_art;
    if (m_page == Page::Quit ||
        (m_page == Page::Options && m_settings.page() != SettingsMenu::Page::Root &&
         m_settings.page() != SettingsMenu::Page::Compass)) {
        art.burn.fill(nullptr);
    }
    if (m_page == Page::Options) {
        m_settings.draw(m_canvas, m_text, art);
    } else {
        m_menu.draw(m_canvas, m_text, art);
    }
    m_canvas.end();
}
} // namespace gdl::game
