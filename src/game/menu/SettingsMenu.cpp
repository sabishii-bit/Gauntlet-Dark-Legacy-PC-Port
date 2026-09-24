#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
std::string SettingsMenu::text(std::string_view id) const {
    return std::string(m_strings != nullptr ? m_strings->get(id) : id);
}
void SettingsMenu::open(const GameConfig& config, const StringTable* strings, Persist persist,
                        const TextPainter& painter, const MenuScreen& screen,
                        MenuDefinition backdrop, Scope scope, PreviewAudio preview) {
    m_config = config;
    m_strings = strings;
    m_persist = std::move(persist);
    m_previewAudio = std::move(preview);
    m_audioDirty = false;
    m_scope = scope;
    m_painter = &painter;
    m_screen = screen;
    m_backdrop = std::move(backdrop);
    m_page = Page::Root;
    m_notice.clear();
    rebuild();
}
void SettingsMenu::rebuild(s32 selection) {
    auto definition = m_backdrop;
    definition.items.clear();
    definition.body.clear();
    definition.title = text(m_scope == Scope::Title ? "menu.options" : "pause.settings");
    definition.scale = 1.0f;
    definition.fades = m_backdrop.fades && m_page == Page::Root;
    const auto add = [&](std::string label, s32 code) {
        definition.items.push_back({std::move(label), code});
    };
    switch (m_page) {
    case Page::Root:
        add(text("menu.audio"), 0);
        if (m_scope == Scope::Title) {
            add(text("menu.gameOptions"), 1);
        }
        if (m_scope != Scope::Level) {
            add(text("menu.compass"), 2);
        }
        definition.items.push_back({text("menu.controls"), 3, 0, false});
        break;
    case Page::Audio: {
        definition.title = text("menu.audio");
        definition.x = 128;
        definition.y = 108;
        definition.scale = 0.8f;
        definition.selectLabel.clear();
        definition.items = {{text("settings.music"), 0, 52},
                            {text("settings.effects"), 1, 52},
                            {text("settings.mono"), 2, 0, true, text("settings.stereo"),
                             m_config.audio.stereo ? 2 : 1}};
        break;
    }
    case Page::Game:
        definition.title = text("menu.gameOptions");
        add(text("settings.difficulty"), 0);
        // Multiplayer rules need their own gameplay implementation; never present an
        // enabled preference which does not change the game.
        definition.items.push_back({text("settings.multiplayer"), 1, 0, false});
        break;
    case Page::Difficulty:
        definition.title = text("settings.difficulty");
        for (usize i = 0; i < DifficultyConfig::kNames.size(); ++i) {
            add(text("settings." + std::string(DifficultyConfig::kNames[i])), static_cast<s32>(i));
            definition.items.back().markedPart =
                m_config.difficulty.level == DifficultyConfig::kNames[i] ? 1 : 0;
        }
        break;
    case Page::Compass:
        definition.title = text("menu.compass");
        add(text("settings.hide"), 0);
        add(text("settings.show"), 1);
        definition.items[m_config.camera.compass ? 1 : 0].markedPart = 1;
        break;
    }
    m_menu.open(definition, *m_painter, m_screen, selection);
}
bool SettingsMenu::flushAudio() {
    if (!m_audioDirty) {
        return true;
    }
    if (!m_persist || !m_persist(m_config)) {
        m_notice = text("settings.failed");
        return false;
    }
    m_audioDirty = false;
    m_notice.clear();
    return true;
}
void SettingsMenu::commit(GameConfig next) {
    if (m_persist && m_persist(next)) {
        m_config = std::move(next);
        m_notice.clear();
    } else {
        m_notice = text("settings.failed");
    }
    rebuild(m_menu.selection());
}
void SettingsMenu::change(s32 direction) {
    const s32 code = m_menu.definition().items[static_cast<usize>(m_menu.selection())].code;
    auto next = m_config;
    if (m_page == Page::Audio && code < 3) {
        if (code == 2) {
            next.audio.stereo = !next.audio.stereo;
        } else {
            auto& volume = code == 0 ? next.audio.musicVolume : next.audio.effectsVolume;
            volume =
                static_cast<f32>(std::clamp(AudioSlider::value(volume) + direction, 0, 255)) / 255;
        }
        m_config = std::move(next);
        m_menu.markItem(2, m_config.audio.stereo ? 2 : 1);
        m_audioDirty = true;
        m_notice.clear();
        if (m_previewAudio) {
            m_previewAudio(m_config.audio);
        }
        return;
    }
    if (m_page == Page::Difficulty) {
        next.difficulty.level = DifficultyConfig::kNames[static_cast<usize>(code)];
    } else if (m_page == Page::Compass) {
        next.camera.compass = code == 1;
    } else {
        return;
    }
    commit(std::move(next));
}
MenuEvent SettingsMenu::update(const MenuInput& input, s32 ticks) {
    if (!m_menu.isOpen()) {
        return {};
    }
    const bool horizontal = input.left || input.right || input.leftHeld || input.rightHeld;
    if (m_page == Page::Audio && !m_menu.closing()) {
        const bool stereo = m_menu.selection() == 2;
        if ((!stereo && horizontal && ticks > 0) || (stereo && (input.left || input.right))) {
            change((input.left || input.leftHeld ? -1 : 1) * (stereo ? 1 : ticks));
            return {MenuAction::Moved, 0};
        }
        if (!flushAudio() && (input.back || input.escape)) {
            return {};
        }
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    if (event.action == MenuAction::Back) {
        if (m_page == Page::Root) {
            return {MenuAction::Back, 0};
        }
        if (m_page == Page::Difficulty) {
            m_page = Page::Game;
            rebuild();
            return {};
        }
        const s32 previousCode = static_cast<s32>(m_page) - 1;
        m_page = Page::Root;
        m_notice.clear();
        rebuild();
        const auto& items = m_menu.definition().items;
        const auto previous = std::ranges::find(items, previousCode, &MenuItem::code);
        rebuild(static_cast<s32>(previous - items.begin()));
        return {};
    }
    if (event.action == MenuAction::Choice) {
        if (m_page == Page::Root) {
            m_page = static_cast<Page>(event.code + 1);
            m_notice.clear();
            rebuild(m_page == Page::Compass && m_config.camera.compass ? 1 : 0);
        } else if (m_page == Page::Game && event.code == 0) {
            m_page = Page::Difficulty;
            // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
            // NOLINTNEXTLINE(readability-qualified-auto)
            const auto selected =
                std::ranges::find(DifficultyConfig::kNames, m_config.difficulty.level);
            rebuild(selected == DifficultyConfig::kNames.end()
                        ? 1
                        : static_cast<s32>(selected - DifficultyConfig::kNames.begin()));
        } else if (m_page == Page::Audio) {
            // Confirm selects the row but does not alter its setting.
        } else {
            change(1);
        }
    }
    return event;
}
void SettingsMenu::draw(Canvas& canvas, const TextPainter& painter,
                        const MenuTextures& textures) const {
    if (!m_menu.isOpen()) {
        return;
    }
    m_menu.draw(canvas, painter, textures);
    if (m_page == Page::Audio) {
        const std::array volumes{m_config.audio.musicVolume, m_config.audio.effectsVolume};
        for (usize i = 0; i < volumes.size(); ++i) {
            textures.audioSlider.draw(
                canvas, 128, static_cast<f32>(m_menu.itemY(i) + m_menu.lineHeight()), volumes[i],
                m_menu.selection() == static_cast<s32>(i), m_menu.fadeOpacity());
        }
    }
    TextStyle style;
    style.scale = 0.5f;
    style.color = m_menu.definition().colors.off;
    const auto notice =
        m_notice.empty() && m_page == Page::Difficulty ? text("settings.nextLevel") : m_notice;
    if (!notice.empty()) {
        painter.draw(canvas, -m_screen.width / 2, m_page == Page::Audio ? 348 : 276, notice, style);
    }
}
} // namespace gdl::game
