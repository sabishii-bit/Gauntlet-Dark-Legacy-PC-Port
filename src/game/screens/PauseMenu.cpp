#include "game/screens/PauseMenu.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
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
        m_slots.open(config.saveDirectory(), config.save.slots);
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
    MenuDefinition menu;
    menu.x = 128;
    menu.y = -1;
    menu.scale = 1.0f;
    menu.backdrop = "SCROLL_A";
    menu.backdropX = 16;
    menu.backdropY = 8;
    menu.backdropWidth = 480;
    menu.backdropHeight = 360;
    menu.colors.off = Color::rgba(92, 26, 3);
    menu.prompts = true;
    menu.backLabel = text("menu.back");
    menu.selectLabel = text("menu.select");
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
        // Keep the retail entry visible, but do not pretend an unrelated screen is
        // the character's inventory viewer. That viewer is not implemented yet.
        menu.items.push_back({text("pause.inventory"), 7, 0, false});
    }
    menu.items.push_back({text(m_inTower ? "pause.quit" : "pause.quitLevel"), 4});
    m_menu.open(menu, m_text, m_screen);
}
void PauseMenu::showManage() {
    m_page = Page::Manage;
    auto menu = backdrop();
    menu.title = text("pause.manage");
    // Character files are a PC implementation of management, not a mid-level save.
    menu.items = {{text("select.save"), 1}, {text("select.load"), 2}};
    m_menu.open(menu, m_text, m_screen);
}
void PauseMenu::showFiles() {
    const auto files = m_files.definition();
    auto menu = backdrop();
    menu.title = files.title;
    menu.titleScale = 0.667f;
    menu.items = files.items;
    menu.body = files.body;
    menu.bodyY = 215;
    menu.bodyScale = 0.4f;
    menu.scale = 0.5f;
    m_menu.open(menu, m_text, m_screen);
}
PauseOutcome PauseMenu::update(f64 seconds, const MenuInput& input, const Input* raw) {
    if (!m_open) {
        return PauseOutcome::Running;
    }
    const auto rate =
        m_context.config != nullptr ? m_context.config->timing.tickRate : TimingConfig{}.tickRate;
    const auto ticks = std::max(static_cast<s32>(std::lround(seconds * rate)), 0);
    if (m_page == Page::Options) {
        if (m_settings.update(input, ticks, raw, m_player).action == MenuAction::Back) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    if (m_page == Page::Files) {
        if (event.action == MenuAction::Back) {
            m_files.back();
        } else if (event.action == MenuAction::Choice) {
            m_files.choose(event.code);
        } else {
            return PauseOutcome::Running;
        }
        if (m_files.succeeded()) {
            m_party = m_files.party();
        }
        if (m_files.state() == SaveMenu::State::Done) {
            if (m_files.succeeded() && m_files.mode() == SaveMenu::Mode::Load) {
                return PauseOutcome::Reload;
            }
            showManage();
        } else {
            showFiles();
        }
        return PauseOutcome::Running;
    }
    if (m_page == Page::Quit) {
        if (event.action == MenuAction::Choice && event.code == 1) {
            return m_inTower ? PauseOutcome::Title : PauseOutcome::ReturnTower;
        }
        if (event.action == MenuAction::Back || event.action == MenuAction::Choice) {
            showMain();
        }
        return PauseOutcome::Running;
    }
    if (m_page == Page::Manage && event.action == MenuAction::Back) {
        showMain();
        return PauseOutcome::Running;
    }
    if (event.action == MenuAction::Back || (input.start && !input.select)) {
        return PauseOutcome::Resume;
    }
    if (event.action != MenuAction::Choice) {
        return PauseOutcome::Running;
    }
    if (event.code == 1 || event.code == 2) {
        m_files.open(m_slots, m_party, m_player,
                     event.code == 1 ? SaveMenu::Mode::Save : SaveMenu::Mode::Load,
                     m_context.strings);
        m_page = Page::Files;
        showFiles();
    } else if (event.code == 3) {
        m_page = Page::Options;
        m_settings.open(m_context.config != nullptr ? *m_context.config : GameConfig{},
                        m_context.strings, m_context.saveSettings, m_text, m_screen, backdrop(),
                        m_inTower ? SettingsMenu::Scope::Tower : SettingsMenu::Scope::Level,
                        m_context.previewAudio);
    } else if (event.code == 5) {
        showManage();
    } else if (event.code == 6) {
        return PauseOutcome::Shop;
    } else if (event.code == 4) {
        m_page = Page::Quit;
        auto menu = backdrop();
        menu.title = text(m_inTower ? "pause.quitConfirm" : "pause.abortConfirm");
        menu.titleScale = 0.667f;
        menu.body = {text("files.quitWarning")};
        menu.bodyY = 215;
        menu.bodyScale = 0.4f;
        menu.items = {{text("files.no"), 0}, {text("files.yes"), 1}};
        m_menu.open(menu, m_text, m_screen);
    }
    return PauseOutcome::Running;
}
void PauseMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    if (!m_open) {
        return;
    }
    const auto virtualWidth = static_cast<f32>(m_screen.width);
    const auto virtualHeight = static_cast<f32>(m_screen.height);
    m_canvas.begin(
        device, makeVirtualScreenTransform(projection, virtualWidth, virtualHeight, width, height));
    m_canvas.fill({0, 0, virtualWidth, virtualHeight}, Color::rgba(0, 0, 0, 150));
    if (m_page == Page::Options) {
        m_settings.draw(m_canvas, m_text, m_art);
    } else {
        m_menu.draw(m_canvas, m_text, m_art);
    }
    m_canvas.end();
}
} // namespace gdl::game
