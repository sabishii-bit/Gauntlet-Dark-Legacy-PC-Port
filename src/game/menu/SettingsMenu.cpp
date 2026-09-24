#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
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
constexpr s32 kBack = 99;
} // namespace

std::string SettingsMenu::text(std::string_view id) const {
    return std::string(m_strings != nullptr ? m_strings->get(id) : id);
}
void SettingsMenu::open(const GameConfig& config, const StringTable* strings, Persist persist,
                        const TextPainter& painter, const MenuScreen& screen,
                        MenuDefinition backdrop) {
    m_config = config;
    m_strings = strings;
    m_persist = std::move(persist);
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
    definition.title = text("menu.options");
    definition.scale = 0.667f;
    definition.fades = m_backdrop.fades && m_page == Page::Root;
    const auto add = [&](std::string label, s32 code) {
        definition.items.push_back({std::move(label), code});
    };
    switch (m_page) {
    case Page::Root:
        add(text("menu.audio"), 0);
        add(text("menu.gameOptions"), 1);
        add(text("menu.compass"), 2);
        add(text("menu.controls"), 3);
        break;
    case Page::Audio: {
        definition.title = text("menu.audio");
        const std::array volumes{m_config.audio.masterVolume, m_config.audio.musicVolume,
                                 m_config.audio.effectsVolume};
        constexpr std::array kLabels{"settings.master", "settings.music", "settings.effects"};
        for (usize i = 0; i < volumes.size(); ++i) {
            add(std::format("{}: {}%", text(kLabels[i]), std::lround(volumes[i] * 100)),
                static_cast<s32>(i));
        }
        break;
    }
    case Page::Game:
        definition.title = text("menu.gameOptions");
        add(text("settings.difficulty") + ": " + text("settings." + m_config.difficulty.level), 0);
        break;
    case Page::Compass:
        definition.title = text("menu.compass");
        add(text("settings.showCompass") + ": " +
                text(m_config.camera.compass ? "settings.on" : "settings.off"),
            0);
        break;
    case Page::Controls:
        definition.title = text("menu.controls");
        add(text("settings.device") + ": " +
                text(m_pad ? "settings.controller" : "settings.keyboard"),
            0);
        add(text("settings.action") + ": " + text(kBindings[m_action].label), 1);
        add(text("settings.binding") + ": " + bindingLabel(), 2);
        add(text("settings.clearBinding"), 3);
        add(text("settings.restoreControls"), 4);
        break;
    }
    add(text("menu.back"), kBack);
    m_menu.open(definition, *m_painter, m_screen, selection);
}
void SettingsMenu::commit(GameConfig next) {
    if (m_persist && m_persist(next)) {
        m_config = std::move(next);
        m_notice = text("settings.saved");
    } else {
        m_notice = text("settings.failed");
    }
    rebuild(m_menu.selection());
}
void SettingsMenu::change(s32 direction) {
    const s32 code = m_menu.definition().items[static_cast<usize>(m_menu.selection())].code;
    auto next = m_config;
    if (m_page == Page::Audio && code < 3) {
        const std::array volumes{&next.audio.masterVolume, &next.audio.musicVolume,
                                 &next.audio.effectsVolume};
        auto& volume = *volumes[static_cast<usize>(code)];
        volume = static_cast<f32>(
                     std::clamp(static_cast<s32>(std::lround(volume * 20)) + direction, 0, 20)) /
                 20;
    } else if (m_page == Page::Game && code == 0) {
        s32 at = 1;
        for (usize i = 0; i < DifficultyConfig::kNames.size(); ++i) {
            if (DifficultyConfig::kNames[i] == next.difficulty.level) {
                at = static_cast<s32>(i);
            }
        }
        next.difficulty.level =
            DifficultyConfig::kNames[static_cast<usize>((at + direction + 3) % 3)];
    } else if (m_page == Page::Compass && code == 0) {
        next.camera.compass = !next.camera.compass;
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
    if (!m_menu.closing() && (input.left || input.right) && m_page != Page::Root) {
        change(input.left ? -1 : 1);
        return {MenuAction::Moved, 0};
    }
    auto mapped = input;
    mapped.back |= input.escape;
    const auto event = m_menu.update(mapped, ticks);
    if (event.action == MenuAction::Back ||
        (event.action == MenuAction::Choice && event.code == kBack)) {
        if (m_page == Page::Root) {
            return {MenuAction::Back, 0};
        }
        const s32 previous = static_cast<s32>(m_page) - 1;
        m_page = Page::Root;
        m_notice.clear();
        rebuild(previous);
        return {};
    }
    if (event.action == MenuAction::Choice) {
        if (m_page == Page::Root) {
            m_page = static_cast<Page>(event.code + 1);
            m_notice.clear();
            rebuild();
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
    TextStyle style;
    style.scale = 0.5f;
    const auto notice = m_notice.empty()
                            ? text(m_page == Page::Game ? "settings.nextLevel" : "settings.adjust")
                            : m_notice;
    painter.draw(canvas, -m_screen.width / 2, 276, notice, style);
}
} // namespace gdl::game
