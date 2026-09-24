#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
struct Binding {
    std::string_view label;
    std::vector<Key> PlayBindings::*keys;
    std::vector<PadButton> PlayBindings::*buttons;
};
constexpr std::array kBindings{
    Binding{"controls.up", &PlayBindings::up, &PlayBindings::padUp},
    Binding{"controls.down", &PlayBindings::down, &PlayBindings::padDown},
    Binding{"controls.left", &PlayBindings::left, &PlayBindings::padLeft},
    Binding{"controls.right", &PlayBindings::right, &PlayBindings::padRight},
    Binding{"controls.attack", &PlayBindings::attack, &PlayBindings::padAttack},
    Binding{"controls.strongAttack", &PlayBindings::strongAttack, &PlayBindings::padStrongAttack},
    Binding{"controls.magic", &PlayBindings::usePotion, &PlayBindings::padUsePotion},
    Binding{"controls.throw", &PlayBindings::throwPotion, &PlayBindings::padThrowPotion},
    Binding{"controls.shield", &PlayBindings::shieldPotion, &PlayBindings::padShieldPotion},
    Binding{"controls.turbo", &PlayBindings::turbo, &PlayBindings::padTurbo},
    Binding{"controls.charge", &PlayBindings::charge, &PlayBindings::padCharge},
    Binding{"controls.strafe", &PlayBindings::strafe, &PlayBindings::padStrafe},
    Binding{"controls.selectorUp", &PlayBindings::selectorUp, &PlayBindings::padSelectorUp},
    Binding{"controls.selectorDown", &PlayBindings::selectorDown, &PlayBindings::padSelectorDown},
    Binding{"controls.selectorLeft", &PlayBindings::selectorLeft, &PlayBindings::padSelectorLeft},
    Binding{"controls.selectorRight", &PlayBindings::selectorRight,
            &PlayBindings::padSelectorRight}};
} // namespace

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
    m_capturing = false;
    m_pad = false;
    m_action = 0;
    m_notice.clear();
    rebuild();
}
std::string SettingsMenu::bindingLabel() const {
    std::string value;
    const auto append = [&](std::string_view name) {
        if (!value.empty()) {
            value += ", ";
        }
        value += name;
    };
    const auto& binding = kBindings[m_action];
    if (m_pad) {
        for (const auto button : m_config.play.*binding.buttons) {
            append(padButtonName(button));
        }
    } else {
        for (const auto key : m_config.play.*binding.keys) {
            append(keyName(key));
        }
    }
    return value.empty() ? text("settings.unbound") : value;
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
        add(text("menu.controls"), 3);
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
    case Page::Controls:
        definition.title = text("menu.controls");
        definition.scale = 0.667f;
        add(text("settings.device") + ": " +
                text(m_pad ? "settings.controller" : "settings.keyboard"),
            0);
        add(text("settings.action") + ": " + text(kBindings[m_action].label), 1);
        add(text("settings.binding") + ": " + bindingLabel(), 2);
        add(text("settings.clearBinding"), 3);
        add(text("settings.restoreControls"), 4);
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
    } else if (m_page == Page::Controls) {
        if (code == 0) {
            m_pad = !m_pad;
            rebuild(0);
            return;
        }
        if (code == 1) {
            m_action = static_cast<usize>(
                (static_cast<s32>(m_action) + direction + static_cast<s32>(kBindings.size())) %
                static_cast<s32>(kBindings.size()));
            rebuild(1);
            return;
        }
        if (code == 2) {
            m_capturing = true;
            m_notice = text("settings.capture");
            return;
        }
        if (code == 3) {
            if (m_pad) {
                (next.play.*kBindings[m_action].buttons).clear();
            } else {
                (next.play.*kBindings[m_action].keys).clear();
            }
        } else if (code == 4) {
            next.play = PlayBindings{};
        } else {
            return;
        }
    } else {
        return;
    }
    commit(std::move(next));
}
void SettingsMenu::capture(const Input& raw, s32 pad) {
    const s32 first = pad < 0 ? 0 : pad;
    const s32 last = pad < 0 ? Input::kMaxPads - 1 : pad;
    bool cancel = raw.wasKeyPressed(Key::Escape);
    for (s32 index = first; index <= last; ++index) {
        cancel |= raw.wasPadButtonPressed(index, PadButton::Back);
    }
    if (cancel) {
        m_capturing = false;
        m_notice.clear();
        return;
    }
    auto next = m_config;
    if (m_pad) {
        for (s32 index = first; index <= last; ++index) {
            for (usize i = 0; i < static_cast<usize>(PadButton::Count); ++i) {
                const auto button = static_cast<PadButton>(i);
                if (std::ranges::find(m_config.menu.padStart, button) !=
                    m_config.menu.padStart.end()) {
                    continue;
                }
                if (raw.wasPadButtonPressed(index, button)) {
                    next.play.*kBindings[m_action].buttons = {button};
                    m_capturing = false;
                    commit(std::move(next));
                    return;
                }
            }
        }
    } else {
        for (usize i = 1; i < static_cast<usize>(Key::Count); ++i) {
            const auto key = static_cast<Key>(i);
            if (std::ranges::find(m_config.menu.start, key) != m_config.menu.start.end()) {
                continue;
            }
            if (raw.wasKeyPressed(key)) {
                next.play.*kBindings[m_action].keys = {key};
                m_capturing = false;
                commit(std::move(next));
                return;
            }
        }
    }
}
MenuEvent SettingsMenu::update(const MenuInput& input, s32 ticks, const Input* raw, s32 pad) {
    if (!m_menu.isOpen()) {
        return {};
    }
    if (m_capturing) {
        if (raw != nullptr) {
            capture(*raw, pad);
        } else if (input.back || input.escape) {
            m_capturing = false;
            m_notice.clear();
        }
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
    if (!m_menu.closing() && (input.left || input.right) && m_page == Page::Controls) {
        change(input.left ? -1 : 1);
        return {MenuAction::Moved, 0};
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
    const auto notice =
        m_notice.empty() && m_page == Page::Difficulty ? text("settings.nextLevel") : m_notice;
    if (!notice.empty()) {
        painter.draw(canvas, -m_screen.width / 2, m_page == Page::Audio ? 348 : 276, notice, style);
    }
}
} // namespace gdl::game
