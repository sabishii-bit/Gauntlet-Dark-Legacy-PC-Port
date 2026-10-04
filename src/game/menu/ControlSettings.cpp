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
constexpr s32 kCaptureTicks = 600;
} // namespace
void ControlSettings::begin() {
    m_player = -1;
    m_page = 0;
    m_capture = -1;
    m_failed = false;
}
std::vector<PlayerControlConfig> ControlSettings::choices(const GameConfig& config) const {
    std::vector<PlayerControlConfig> result(3);
    result[1].device = "keyboard";
    result[2].device = "none";
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
void ControlSettings::define(MenuDefinition& definition, const GameConfig& config,
                             const TextPainter& painter,
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
        for (s32 row = 0; row < kPageSize; ++row) {
            const s32 index = m_page * kPageSize + row;
            if (index >= static_cast<s32>(actions.size())) {
                break;
            }
            const auto& action = actions[static_cast<usize>(index)];
            std::string value;
            if (m_capture == index) {
                value = text("controls.press");
            } else if (profile.device == "keyboard" || (profile.device.empty() && m_player == 0)) {
                for (const auto key : action.keyboard(profile)) {
                    if (!value.empty()) {
                        value += "/";
                    }
                    value += keyName(key);
                }
                if (profile.device.empty() && !action.controller(profile).empty()) {
                    value += " / ";
                    value += padButtonName(action.controller(profile).front());
                }
            } else {
                for (const auto button : action.controller(profile)) {
                    if (!value.empty()) {
                        value += "/";
                    }
                    value += padButtonName(button);
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
            {text("controls.page") + " " + std::to_string(m_page + 1) + "/4", kNext});
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
    if (m_failed || m_capture >= 0) {
        definition.body = {text(m_failed ? "settings.failed" : "controls.cancel")};
        definition.bodyY = 286;
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
    const auto save = [&](const GameConfig& next) {
        m_failed = !persist || !persist(next);
        if (!m_failed) {
            config = next;
        }
        rebuild(menu.selection());
    };
    if (m_capture >= 0) {
        m_timeout -= std::max(0, ticks);
        const auto device = controlDevices(config, m_devices)[static_cast<usize>(m_player)];
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
            auto next = config;
            auto& profile = next.controls[static_cast<usize>(m_player)];
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
            save(next);
        }
        return false;
    }
    if (devicesChanged) {
        rebuild(menu.selection());
    }
    auto mapped = input;
    // A fixed keyboard escape route survives changing the device/menu bindings in this editor.
    mapped.back |= m_devices.wasKeyPressed(Key::Escape);
    mapped.up |= m_devices.wasKeyPressed(Key::Up);
    mapped.down |= m_devices.wasKeyPressed(Key::Down);
    mapped.select |= m_devices.wasKeyPressed(Key::Enter);
    auto event = menu.update(mapped, ticks);
    if ((input.left || input.right) && m_player >= 0) {
        const auto code = menu.definition().items[static_cast<usize>(menu.selection())].code;
        if (code == kDevice || code == kNext) {
            event = {MenuAction::Choice, code, 0, input.left ? -1 : 1};
        }
    }
    if (event.action == MenuAction::Back ||
        (event.action == MenuAction::Choice && event.code == kBack)) {
        if (m_player < 0) {
            return true;
        }
        const s32 player = m_player;
        m_player = -1;
        rebuild(player);
        return false;
    }
    if (event.action != MenuAction::Choice) {
        return false;
    }
    if (m_player < 0) {
        m_player = event.code;
        m_page = 0;
        rebuild(0);
        return false;
    }
    if (event.code == kNext) {
        m_page = (m_page + (event.direction < 0 ? -1 : 1) + 4) % 4;
        rebuild(7);
    } else if (event.code == kDevice) {
        auto next = config;
        auto& profile = next.controls[static_cast<usize>(m_player)];
        const auto devices = choices(config);
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
        save(next);
    } else if (event.code == kDefaults) {
        auto next = config;
        next.controls[static_cast<usize>(m_player)].customized = false;
        save(next);
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
