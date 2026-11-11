#include "game/hud/ControlPrompts.h"

#include <algorithm>
#include <cmath>

#include "game/config/ControlProfiles.h"

namespace gdl::game {
namespace {
std::string keyLabel(Key key) {
    switch (key) {
    case Key::MouseLeft: return "LMB";
    case Key::MouseRight: return "RMB";
    case Key::MouseMiddle: return "MMB";
    case Key::LeftShift: return "Shift";
    case Key::LeftControl: return "Ctrl";
    case Key::LeftAlt: return "Alt";
    case Key::Escape: return "Esc";
    default: return std::string(keyName(key));
    }
}
std::string buttonLabel(PadButton button) {
    switch (button) {
    case PadButton::LeftBumper: return "LB";
    case PadButton::RightBumper: return "RB";
    case PadButton::LeftTrigger: return "LT";
    case PadButton::RightTrigger: return "RT";
    case PadButton::LeftThumb: return "LS";
    case PadButton::RightThumb: return "RS";
    case PadButton::DpadUp: return "D-Up";
    case PadButton::DpadDown: return "D-Down";
    case PadButton::DpadLeft: return "D-Left";
    case PadButton::DpadRight: return "D-Right";
    case PadButton::LeftStickUp: return "LS-Up";
    case PadButton::LeftStickDown: return "LS-Down";
    case PadButton::LeftStickLeft: return "LS-Left";
    case PadButton::LeftStickRight: return "LS-Right";
    default: return std::string(padButtonName(button));
    }
}
/** `strings`' text for `id`; without a table, the id itself, as the table gives an unknown one. */
std::string wordsOf(const StringTable* strings, std::string_view id) {
    return std::string(strings != nullptr ? strings->get(id) : id);
}
/** A binding a potion gesture borrows with the gesture after it, as the Controls page shows it. */
std::string gestureLabel(const std::string& binding, const StringTable* strings,
                         std::string_view gesture) {
    return binding + " (" + wordsOf(strings, gesture) + ")";
}
} // namespace

std::string boundControlLabel(const GameConfig& config, const StringTable* strings, s32 player,
                              bool keyboard, std::string_view action) {
    if (player < 0 || player >= static_cast<s32>(config.controls.size()) ||
        config.controls[static_cast<usize>(player)].device == "none") {
        return wordsOf(strings, "controls.unbound");
    }
    const auto& play = playBindings(config, player);
    const auto& menu = menuBindings(config, player);
    for (const auto& binding : controlActions()) {
        if (action != binding.id) {
            continue;
        }
        if (keyboard) {
            const auto& keys =
                binding.keys != nullptr ? play.*binding.keys : menu.*binding.menuKeys;
            if (!keys.empty()) {
                return keyLabel(keys.front());
            }
        } else {
            const auto& buttons =
                binding.buttons != nullptr ? play.*binding.buttons : menu.*binding.menuButtons;
            if (!buttons.empty()) {
                return buttonLabel(buttons.front());
            }
            if (play.padMagicGestures && !play.padUsePotion.empty()) {
                if (action == "throwPotion") {
                    return gestureLabel(buttonLabel(play.padUsePotion.front()), strings,
                                        "controls.hold");
                }
                if (action == "shieldPotion") {
                    return gestureLabel(buttonLabel(play.padUsePotion.front()), strings,
                                        "controls.doubleTap");
                }
            }
        }
        break;
    }
    return wordsOf(strings, "controls.unbound");
}

std::string controlLabel(const ControlLabels* labels, s32 player, std::string_view action) {
    return labels != nullptr
               ? labels->label(player, action)
               : boundControlLabel(GameConfig{}, nullptr, std::max(0, player), player <= 0, action);
}

std::string controlText(std::string_view text, const ControlLabels* labels, s32 player) {
    constexpr std::string_view kPrefix = "{bind:";
    std::string result;
    while (!text.empty()) {
        const auto at = text.find(kPrefix);
        if (at == std::string_view::npos) {
            result += text;
            break;
        }
        result += text.substr(0, at);
        const auto end = text.find('}', at);
        if (end == std::string_view::npos) {
            result += text.substr(at);
            break;
        }
        result += controlLabel(labels, player,
                               text.substr(at + kPrefix.size(), end - at - kPrefix.size()));
        text.remove_prefix(end + 1);
    }
    return result;
}

void drawControlLabel(Canvas& canvas, const TextPainter& painter, const Rect& area,
                      std::string_view label, Color color) {
    if (!painter.ready() || area.width <= 0 || area.height <= 0) {
        return;
    }
    TextStyle style;
    style.color = color;
    style.scale = std::min(area.width / static_cast<f32>(std::max(1, painter.measure(label))),
                           area.height / static_cast<f32>(std::max(1, painter.lineHeight())));
    painter.draw(
        canvas, -static_cast<s32>(area.x + area.width / 2),
        static_cast<s32>(area.y +
                         (area.height - static_cast<f32>(painter.lineHeight(style.scale))) / 2),
        label, style);
}

void PromptDevices::update(const Input& input, const GameConfig& config) {
    const auto devices = controlDevices(config, input);
    const Vec2 pointer{input.pointer().x, input.pointer().y};
    bool keyboardActivity = input.pointer().inside && m_pointer && *m_pointer != pointer;
    for (s32 key = 1; key < static_cast<s32>(Key::Count); ++key) {
        keyboardActivity |= input.wasKeyPressed(static_cast<Key>(key));
    }
    m_pointer = pointer;
    bool keyboardAutomatic = true;
    bool anyPadActivity = false;
    for (usize player = 0; player < devices.size(); ++player) {
        const auto& source = devices[player];
        if (source.keyboard && !config.controls[player].device.empty()) {
            keyboardAutomatic = false; // the keyboard is someone's by choice: the pointer stays
        }
        bool padActivity = false;
        if (input.isPadConnected(source.pad)) {
            for (s32 button = 0; button < static_cast<s32>(PadButton::Count); ++button) {
                padActivity |=
                    input.wasPadButtonPressed(source.pad, static_cast<PadButton>(button));
            }
            const auto deadZone = playBindings(config, static_cast<s32>(player)).stickDeadZone;
            padActivity |= std::abs(input.padAxis(source.pad, PadAxis::LeftX)) > deadZone ||
                           std::abs(input.padAxis(source.pad, PadAxis::LeftY)) > deadZone;
        }
        if (padActivity) {
            m_keyboard[player] = false;
            m_lastPlayer = static_cast<s32>(player);
            anyPadActivity = true;
        }
        if (source.keyboard && keyboardActivity) {
            m_keyboard[player] = true;
            m_lastPlayer = static_cast<s32>(player);
        }
    }
    if (keyboardActivity || !keyboardAutomatic) {
        m_cursorHidden = false;
    } else if (anyPadActivity) {
        m_cursorHidden = true;
    }
}

std::string PromptDevices::label(const Input& input, const GameConfig& config,
                                 const StringTable* strings, s32 player,
                                 std::string_view action) const {
    player = std::clamp(player, 0, static_cast<s32>(config.controls.size()) - 1);
    const auto source = controlDevices(config, input)[static_cast<usize>(player)];
    const bool keyboard = source.keyboard && (m_keyboard[static_cast<usize>(player)] ||
                                              !input.isPadConnected(source.pad));
    return boundControlLabel(config, strings, player, keyboard, action);
}
} // namespace gdl::game
