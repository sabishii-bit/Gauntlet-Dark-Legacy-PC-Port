#include "game/menu/ControlSettings.h"

#include <algorithm>

#include "game/config/ControlProfiles.h"

namespace gdl::game {
namespace {
constexpr s32 kPageSize = 6;
constexpr s32 kDevice = 100;
constexpr s32 kNext = 101;
constexpr s32 kDefaults = 102;
constexpr s32 kBack = 103;
constexpr s32 kApply = 104;
constexpr s32 kFirstAction = 8;
constexpr s32 kActionCount = 3;
constexpr s32 kActionY = 268;
constexpr s32 kActionGap = 24;
constexpr s32 kCanvasWidth = 512;
constexpr s32 kCaptureTicks = 600;
s32 pageCount() {
    return (static_cast<s32>(controlActions().size()) + kPageSize - 1) / kPageSize;
}
} // namespace
void ControlSettings::begin(const GameConfig& config) {
    m_draft = config;
    m_navigation = config;
    m_player = -1;
    m_page = 0;
    m_capture = -1;
    m_failed = false;
}
std::vector<PlayerControlConfig> ControlSettings::choices(const GameConfig& config) const {
    std::vector<PlayerControlConfig> result(3);
    result[1].device = "keyboard";
    result[2].device = "none";
    // Player 1 always retains an assignment, including keyboard recovery in Automatic.
    if (m_player == 0) {
        result.pop_back();
    }
    for (usize player = 0; player < config.controls.size(); ++player) {
        if (static_cast<s32>(player) != m_player && config.controls[player].device == "keyboard") {
            result.erase(result.begin() + 1);
            break;
        }
    }
    for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
        if (const auto* device = m_devices.padDevice(pad)) {
            PlayerControlConfig choice;
            choice.device = device->guid;
            choice.name = device->name;
            choice.occurrence = controllerOccurrence(m_devices, pad);
            bool assigned = false;
            for (usize player = 0; player < config.controls.size(); ++player) {
                const auto& other = config.controls[player];
                assigned |= static_cast<s32>(player) != m_player && other.device == choice.device &&
                            other.occurrence == choice.occurrence;
            }
            if (!assigned) {
                result.push_back(std::move(choice));
            }
        }
    }
    return result;
}
void ControlSettings::define(MenuDefinition& definition, const TextPainter& painter,
                             const std::function<std::string(std::string_view)>& text) const {
    definition.title = text("menu.controls");
    definition.titleScale = 0.75f;
    definition.playerLabel.clear();
    definition.fades = false;
    definition.x = 78;
    definition.y = 94;
    definition.scale = 0.45f;
    definition.cursorScale = 0.6f;
    definition.valueX = 282;
    definition.valueWidth = 148;
    definition.items.clear();
    definition.body.clear();
    definition.itemPositions.clear();
    const auto& config = m_draft;
    if (m_player < 0) {
        for (s32 player = 0; player < 4; ++player) {
            definition.items.push_back(
                {text("controls.player") + " " + std::to_string(player + 1), player});
        }
        definition.items.push_back({text("settings.back"), kBack});
    } else {
        auto profile = config.controls[static_cast<usize>(m_player)];
        if (!profile.customized) {
            profile.play = config.play;
            profile.menu = config.menu;
        }
        definition.title += " - " + text("controls.player") + " " + std::to_string(m_player + 1);
        std::string device = profile.name;
        if (profile.device.empty()) {
            device = text("controls.auto");
        } else if (profile.device == "keyboard") {
            device = text("controls.keyboard");
        } else if (profile.device == "none") {
            device = text("controls.none");
        } else if (controlDevices(config, m_devices)[static_cast<usize>(m_player)].pad < 0) {
            device = text("controls.disconnected");
        } else {
            device += " " + std::to_string(profile.occurrence + 1);
        }
        // The complete name remains in the selected device's profile; keep the row on parchment.
        if (device.size() > 25) {
            device = device.substr(0, 22) + "...";
        }
        definition.items.push_back({text("controls.device"), kDevice, 0, true, {}, 0, device});
        const auto actions = controlActions();
        const auto assigned = controlDevices(config, m_devices)[static_cast<usize>(m_player)];
        const bool canBind = assigned.keyboard || m_devices.isPadConnected(assigned.pad);
        const auto buttonLabel = [&](PadButton button) {
            switch (button) {
            case PadButton::LeftStickUp: return text("controls.stickUp");
            case PadButton::LeftStickDown: return text("controls.stickDown");
            case PadButton::LeftStickLeft: return text("controls.stickLeft");
            case PadButton::LeftStickRight: return text("controls.stickRight");
            default: return std::string(padButtonName(button));
            }
        };
        for (s32 row = 0; row < kPageSize; ++row) {
            const s32 index = m_page * kPageSize + row;
            if (index >= static_cast<s32>(actions.size())) {
                break;
            }
            const auto& action = actions[static_cast<usize>(index)];
            const std::string_view actionId = action.id;
            std::string value;
            if (m_capture == index) {
                value = text("controls.press");
            } else if (assigned.keyboard) {
                for (const auto key : action.keyboard(profile)) {
                    if (!value.empty()) {
                        value += "/";
                    }
                    value += keyName(key);
                }
                if (profile.device.empty() && !action.controller(profile).empty()) {
                    value += " / ";
                    value += buttonLabel(action.controller(profile).front());
                }
            } else {
                for (const auto button : action.controller(profile)) {
                    if (!value.empty()) {
                        value += "/";
                    }
                    value += buttonLabel(button);
                }
                if (value.empty() && profile.play.padMagicGestures &&
                    !profile.play.padUsePotion.empty() &&
                    (actionId == "throwPotion" || actionId == "shieldPotion")) {
                    value =
                        buttonLabel(profile.play.padUsePotion.front()) + " (" +
                        text(actionId == "throwPotion" ? "controls.hold" : "controls.doubleTap") +
                        ")";
                }
            }
            if (value.empty()) {
                value = text("controls.unbound");
            }
            if (value.size() > 25) {
                value = value.substr(0, 22) + "...";
            }
            definition.items.push_back(
                {text("controls." + std::string(action.id)), index, 0, canBind, {}, 0, value});
        }
        definition.items.push_back(
            {text("controls.page"),
             kNext,
             0,
             true,
             {},
             0,
             std::to_string(m_page + 1) + "/" + std::to_string(pageCount())});
        definition.items.push_back({text("settings.apply"), kApply});
        definition.items.push_back({text("settings.defaults"), kDefaults});
        definition.items.push_back({text("settings.back"), kBack});
    }
    for (usize i = 0; i < definition.items.size(); ++i) {
        definition.itemPositions.emplace_back(78, 94 + static_cast<s32>(i) * 18);
        auto& value = definition.items[i].value;
        if (painter.measure(value, definition.scale) > definition.valueWidth) {
            while (!value.empty() &&
                   painter.measure(value + "...", definition.scale) > definition.valueWidth) {
                usize end = value.size() - 1;
                while (end > 0 && (static_cast<u8>(value[end]) & 0xC0U) == 0x80U) {
                    --end;
                }
                value.resize(end);
            }
            value += "...";
        }
    }
    if (m_player >= 0) {
        s32 width = kActionGap * (kActionCount - 1);
        for (usize i = kFirstAction; i < definition.items.size(); ++i) {
            width += painter.measure(definition.items[i].text, definition.scale);
        }
        s32 x = (kCanvasWidth - width) / 2;
        for (usize i = kFirstAction; i < definition.items.size(); ++i) {
            definition.itemPositions[i] = Vec2{static_cast<f32>(x), kActionY};
            x += painter.measure(definition.items[i].text, definition.scale) + kActionGap;
        }
    }
    if (m_failed || m_capture >= 0) {
        definition.body = {text(m_failed ? "settings.failed" : "controls.cancel")};
        definition.bodyY = 304;
        definition.bodyScale = 0.4f;
    }
}
bool ControlSettings::update(const MenuInput& input, s32 ticks, OptionMenu& menu,
                             GameConfig& config,
                             const std::function<bool(const GameConfig&)>& persist,
                             const std::function<void(s32)>& rebuild) {
    bool devicesChanged = false;
    if (input.devices != nullptr) {
        for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
            const auto* old = m_devices.padDevice(pad);
            const auto* next = input.devices->padDevice(pad);
            devicesChanged |= (old == nullptr) != (next == nullptr) ||
                              (old != nullptr && next != nullptr &&
                               (old->guid != next->guid || old->name != next->name));
        }
        m_devices = *input.devices;
    }
    if (m_capture >= 0) {
        m_timeout -= std::max(0, ticks);
        const auto device = controlDevices(m_draft, m_devices)[static_cast<usize>(m_player)];
        if (m_timeout <= 0 || m_devices.wasKeyPressed(Key::Escape)) {
            m_capture = -1;
            rebuild(menu.selection());
            return false;
        }
        bool held = false;
        std::optional<Key> key;
        std::optional<PadButton> button;
        for (s32 i = 1; i < static_cast<s32>(Key::Count); ++i) {
            const auto candidate = static_cast<Key>(i);
            held |= device.keyboard && m_devices.isKeyDown(candidate);
            if (device.keyboard && m_devices.wasKeyPressed(candidate)) {
                key = candidate;
            }
        }
        for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
            for (s32 i = 0; i < static_cast<s32>(PadButton::Count); ++i) {
                const auto candidate = static_cast<PadButton>(i);
                held |= (pad == device.pad || m_devices.wasPadButtonPressed(pad, candidate)) &&
                        m_devices.isPadButtonDown(pad, candidate);
                if (pad == device.pad && m_devices.wasPadButtonPressed(pad, candidate)) {
                    button = candidate;
                }
            }
        }
        held |= m_devices.pointer().down || m_devices.isKeyDown(Key::Enter);
        if (!m_released) {
            m_released = !held;
            return false;
        }
        if (key || button) {
            auto& profile = m_draft.controls[static_cast<usize>(m_player)];
            if (!profile.customized) {
                profile.play = config.play;
                profile.menu = config.menu;
            }
            profile.customized = true;
            const auto& action = controlActions()[static_cast<usize>(m_capture)];
            if (key) {
                action.keyboard(profile) = {*key};
            }
            if (button) {
                action.controller(profile) = {*button};
            }
            m_capture = -1;
            rebuild(menu.selection());
        }
        return false;
    }
    if (devicesChanged) {
        rebuild(menu.selection());
    }
    auto mapped = input;
    // Keep the session's original controls alive even after Apply changes its owner.
    if (input.devices != nullptr) {
        const auto recovery = readSharedMenuInput(m_devices, m_navigation);
        mapped.up |= recovery.up;
        mapped.down |= recovery.down;
        mapped.left |= recovery.left;
        mapped.right |= recovery.right;
        mapped.select |= recovery.select;
        mapped.back |= recovery.back;
    }
    // A fixed keyboard escape route survives changing the device/menu bindings in this editor.
    mapped.back |= m_devices.wasKeyPressed(Key::Escape);
    mapped.up |= m_devices.wasKeyPressed(Key::Up);
    mapped.down |= m_devices.wasKeyPressed(Key::Down);
    mapped.left |= m_devices.wasKeyPressed(Key::Left);
    mapped.right |= m_devices.wasKeyPressed(Key::Right);
    mapped.select |= m_devices.wasKeyPressed(Key::Enter);
    if (m_player >= 0 && menu.selection() >= kFirstAction && !mapped.back && !mapped.select) {
        if (mapped.left || mapped.right) {
            rebuild(kFirstAction +
                    (menu.selection() - kFirstAction + (mapped.left ? -1 : 1) + kActionCount) %
                        kActionCount);
            return false;
        }
        if (mapped.up || mapped.down) {
            rebuild(mapped.up ? kFirstAction - 1 : 0);
            return false;
        }
    }
    auto event = menu.update(mapped, ticks);
    if ((mapped.left || mapped.right) && !mapped.back && !mapped.select && m_player >= 0) {
        const auto code = menu.definition().items[static_cast<usize>(menu.selection())].code;
        if (code == kDevice || code == kNext) {
            event = {MenuAction::Choice, code, 0, mapped.left ? -1 : 1};
        }
    }
    if (event.action == MenuAction::Back ||
        (event.action == MenuAction::Choice && event.code == kBack)) {
        if (m_player < 0) {
            return true;
        }
        const s32 player = m_player;
        m_player = -1;
        m_draft = config;
        m_failed = false;
        rebuild(player);
        return false;
    }
    if (event.action != MenuAction::Choice) {
        return false;
    }
    if (m_player < 0) {
        m_player = event.code;
        m_page = 0;
        m_draft = config;
        m_failed = false;
        rebuild(0);
        return false;
    }
    if (event.code == kNext) {
        m_page = (m_page + (event.direction < 0 ? -1 : 1) + pageCount()) % pageCount();
        rebuild(kFirstAction - 1);
    } else if (event.code == kDevice) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        const auto devices = choices(m_draft);
        const auto found = std::ranges::find_if(devices, [&](const auto& choice) {
            return choice.device == profile.device && choice.occurrence == profile.occurrence;
        });
        const auto count = static_cast<s32>(devices.size());
        const auto index = found == devices.end() ? 0 : static_cast<s32>(found - devices.begin());
        const auto& chosen =
            devices[static_cast<usize>((index + (event.direction < 0 ? -1 : 1) + count) % count)];
        profile.device = chosen.device;
        profile.name = chosen.name;
        profile.occurrence = chosen.occurrence;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code == kApply) {
        auto next = config;
        next.controls[static_cast<usize>(m_player)] =
            m_draft.controls[static_cast<usize>(m_player)];
        const auto devices = controlDevices(next, m_devices);
        const bool usable = std::ranges::any_of(devices, [&](const auto& device) {
            return device.keyboard || m_devices.isPadConnected(device.pad);
        });
        m_failed = next.controls[0].device == "none" || !usable || !persist || !persist(next);
        if (!m_failed) {
            config = next;
            m_draft = next;
        }
        rebuild(menu.selection());
    } else if (event.code == kDefaults) {
        auto& profile = m_draft.controls[static_cast<usize>(m_player)];
        profile.play = PlayBindings{};
        profile.menu = MenuBindings{};
        profile.customized = true;
        m_failed = false;
        rebuild(menu.selection());
    } else if (event.code >= 0 && event.code < static_cast<s32>(controlActions().size())) {
        m_capture = event.code;
        m_failed = false;
        m_released = false;
        m_timeout = kCaptureTicks;
        rebuild(menu.selection());
    }
    return false;
}
} // namespace gdl::game
