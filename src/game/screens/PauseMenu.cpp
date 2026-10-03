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
} // namespace
std::string PauseMenu::text(std::string_view id) const {
    return std::string(m_context.strings != nullptr ? m_context.strings->get(id) : id);
}
bool PauseMenu::open(RenderDevice& device, const GameContext& context,
                     std::span<const PartyMember> party, s32 player) {
    close();
    m_context = context;
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
        showMain();
        m_open = true;
        return true;
    } catch (const std::exception& e) {
        log::warn("Pause menu: {}", e.what());
        close();
        return false;
    }
}
void PauseMenu::close() {
    m_open = false;
    m_menu = OptionMenu{};
    m_settings = SettingsMenu{};
    m_art = MenuTextures{};
    m_text.setFont(nullptr, nullptr);
    m_textures.releaseTextures();
    m_party.clear();
}
MenuDefinition PauseMenu::backdrop() const {
    auto menu = MenuDefinition::parchment();
    menu.backdropX = 16;
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
    menu.items = {{text("pause.no"), 0}, {text("pause.yes"), 1}};
    m_menu.open(menu, m_text, m_screen);
}
PauseOutcome PauseMenu::update(f64 seconds, const MenuInput& input) {
    if (!m_open) {
        return PauseOutcome::Running;
    }
    const auto rate =
        m_context.config != nullptr ? m_context.config->timing.tickRate : TimingConfig{}.tickRate;
    const auto ticks = std::max(static_cast<s32>(std::lround(seconds * rate)), 0);
    if (m_page == Page::Options) {
        if (m_settings.update(input, ticks).action == MenuAction::Back) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    if (m_page == Page::Quit) {
        if (event.action == MenuAction::Choice && event.code == 1) {
            return m_inTower ? PauseOutcome::Title : PauseOutcome::ReturnTower;
        }
        if (event.action == MenuAction::Back || event.action == MenuAction::Choice) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    if (event.action == MenuAction::Back || (input.start && !input.select)) {
        return PauseOutcome::Resume;
    }
    if (event.action != MenuAction::Choice) {
        return PauseOutcome::Running;
    }
    if (event.code == 3) {
        m_page = Page::Options;
        m_settings.open(m_context.config != nullptr ? *m_context.config : GameConfig{},
                        m_context.strings, m_context.saveSettings, m_text, m_screen, backdrop(),
                        m_inTower ? SettingsMenu::Scope::Tower : SettingsMenu::Scope::Level,
                        m_context.previewAudio);
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
void PauseMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    const auto virtualWidth = static_cast<f32>(m_screen.width);
    const auto virtualHeight = static_cast<f32>(m_screen.height);
    m_canvas.begin(
        device, makeVirtualScreenTransform(projection, virtualWidth, virtualHeight, width, height));
    m_canvas.fillScreen(Color::rgba(0, 0, 0, 150));
    if (m_page == Page::Options) {
        m_settings.draw(m_canvas, m_text, m_art);
    } else {
        m_menu.draw(m_canvas, m_text, m_art);
    }
    m_canvas.end();
}
} // namespace gdl::game
