#include "game/config/ControlProfiles.h"

#include <array>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"

namespace gdl::game {
namespace {
constexpr std::array kActions{
    ControlAction{"up", &PlayBindings::up, &PlayBindings::padUp},
    ControlAction{"down", &PlayBindings::down, &PlayBindings::padDown},
    ControlAction{"left", &PlayBindings::left, &PlayBindings::padLeft},
    ControlAction{"right", &PlayBindings::right, &PlayBindings::padRight},
    ControlAction{"attack", &PlayBindings::attack, &PlayBindings::padAttack},
    ControlAction{"strongAttack", &PlayBindings::strongAttack, &PlayBindings::padStrongAttack},
    ControlAction{"turbo", &PlayBindings::turbo, &PlayBindings::padTurbo},
    ControlAction{"usePotion", &PlayBindings::usePotion, &PlayBindings::padUsePotion},
    ControlAction{"throwPotion", &PlayBindings::throwPotion, &PlayBindings::padThrowPotion},
    ControlAction{"shieldPotion", &PlayBindings::shieldPotion, &PlayBindings::padShieldPotion},
    ControlAction{"charge", &PlayBindings::charge, &PlayBindings::padCharge},
    ControlAction{"combo", &PlayBindings::combo, &PlayBindings::padCombo},
    ControlAction{"strafe", &PlayBindings::strafe, &PlayBindings::padStrafe},
    ControlAction{"selectorUp", &PlayBindings::selectorUp, &PlayBindings::padSelectorUp},
    ControlAction{"selectorDown", &PlayBindings::selectorDown, &PlayBindings::padSelectorDown},
    ControlAction{"selectorLeft", &PlayBindings::selectorLeft, &PlayBindings::padSelectorLeft},
    ControlAction{"selectorRight", &PlayBindings::selectorRight, &PlayBindings::padSelectorRight},
    ControlAction{"menuUp", nullptr, nullptr, &MenuBindings::up, &MenuBindings::padUp},
    ControlAction{"menuDown", nullptr, nullptr, &MenuBindings::down, &MenuBindings::padDown},
    ControlAction{"menuLeft", nullptr, nullptr, &MenuBindings::left, &MenuBindings::padLeft},
    ControlAction{"menuRight", nullptr, nullptr, &MenuBindings::right, &MenuBindings::padRight},
    ControlAction{"menuSelect", nullptr, nullptr, &MenuBindings::select, &MenuBindings::padSelect},
    ControlAction{"menuBack", nullptr, nullptr, &MenuBindings::back, &MenuBindings::padBack},
    ControlAction{"menuStart", nullptr, nullptr, &MenuBindings::start, &MenuBindings::padStart},
};
// Context-only bindings remain configurable without becoming generic menu actions.
constexpr std::array kShopActions{
    ControlAction{"shopSell", nullptr, nullptr, &MenuBindings::shopSell,
                  &MenuBindings::padShopSell},
    ControlAction{"shopExit", nullptr, nullptr, &MenuBindings::shopExit,
                  &MenuBindings::padShopExit},
};
constexpr std::array kPersistedActions{std::span<const ControlAction>{kActions},
                                       std::span<const ControlAction>{kShopActions}};
} // namespace
std::vector<Key>& ControlAction::keyboard(PlayerControlConfig& profile) const {
    return keys != nullptr ? profile.play.*keys : profile.menu.*menuKeys;
}
std::vector<PadButton>& ControlAction::controller(PlayerControlConfig& profile) const {
    return buttons != nullptr ? profile.play.*buttons : profile.menu.*menuButtons;
}
std::span<const ControlAction> controlActions() {
    return kActions;
}
void readControlProfiles(const nlohmann::json& json, GameConfig& config) {
    if (!json.is_array() || json.size() != config.controls.size()) {
        throw FormatError("controls.players must contain four profiles");
    }
    auto profiles = config.controls;
    for (usize i = 0; i < profiles.size(); ++i) {
        const auto& value = json.at(i);
        auto& profile = profiles[i];
        profile.device = value.value("device", std::string{});
        profile.name = value.value("name", std::string{});
        profile.occurrence = value.value("occurrence", 0);
        profile.rumble = value.value("rumble", true);
        if (profile.occurrence < 0 || profile.occurrence >= Input::kMaxPads) {
            throw FormatError("invalid controller occurrence");
        }
        profile.customized = value.contains("bindings");
        profile.play = config.play;
        profile.menu = config.menu;
        if (!profile.customized) {
            continue;
        }
        const auto& bindings = value.at("bindings");
        for (const auto actions : kPersistedActions) {
            for (const auto& action : actions) {
                if (!bindings.contains(action.id)) {
                    continue;
                }
                const auto& binding = bindings.at(action.id);
                if (binding.contains("keys")) {
                    auto& keys = action.keyboard(profile);
                    keys.clear();
                    for (const auto& name : binding.at("keys")) {
                        const auto key = keyFromName(name.get<std::string>());
                        if (!key) {
                            throw FormatError("unknown control key");
                        }
                        keys.push_back(*key);
                    }
                }
                if (binding.contains("buttons")) {
                    auto& buttons = action.controller(profile);
                    buttons.clear();
                    for (const auto& name : binding.at("buttons")) {
                        const auto button = padButtonFromName(name.get<std::string>());
                        if (!button) {
                            throw FormatError("unknown control button");
                        }
                        buttons.push_back(*button);
                    }
                }
            }
        }
    }
    config.controls = std::move(profiles);
}
nlohmann::json writeControlProfiles(const GameConfig& config) {
    auto result = nlohmann::json::array();
    for (auto profile : config.controls) {
        nlohmann::json value{{"device", profile.device},
                             {"name", profile.name},
                             {"occurrence", profile.occurrence},
                             {"rumble", profile.rumble}};
        if (profile.customized) {
            for (const auto actions : kPersistedActions) {
                for (const auto& action : actions) {
                    auto& binding = value["bindings"][action.id];
                    binding["keys"] = nlohmann::json::array();
                    binding["buttons"] = nlohmann::json::array();
                    for (const auto key : action.keyboard(profile)) {
                        binding["keys"].push_back(keyName(key));
                    }
                    for (const auto button : action.controller(profile)) {
                        binding["buttons"].push_back(padButtonName(button));
                    }
                }
            }
        }
        result.push_back(std::move(value));
    }
    return result;
}
s32 controllerOccurrence(const Input& input, s32 pad) {
    const auto* device = input.padDevice(pad);
    s32 occurrence = 0;
    for (s32 i = 0; device != nullptr && i < pad; ++i) {
        const auto* other = input.padSlot(i);
        if (other != nullptr && other->guid == device->guid) {
            ++occurrence;
        }
    }
    return occurrence;
}
std::array<ControlDevice, 4> controlDevices(const GameConfig& config, const Input& input) {
    std::array<ControlDevice, 4> result{};
    std::array<bool, Input::kMaxPads> used{};
    bool keyboardUsed = false;
    for (usize i = 0; i < result.size(); ++i) {
        const auto& profile = config.controls[i];
        if (profile.device == "keyboard" && !keyboardUsed) {
            result[i].keyboard = true;
            keyboardUsed = true;
        } else if (!profile.device.empty() && profile.device != "none") {
            for (s32 pad = 0; pad < Input::kMaxPads; ++pad) {
                const auto* device = input.padDevice(pad);
                if (!used[static_cast<usize>(pad)] && device != nullptr &&
                    device->guid == profile.device &&
                    controllerOccurrence(input, pad) == profile.occurrence) {
                    result[i].pad = pad;
                    used[static_cast<usize>(pad)] = true;
                    break;
                }
            }
        }
    }
    for (usize i = 0; i < result.size(); ++i) {
        if (config.controls[i].device.empty()) {
            result[i].keyboard = i == 0 && !keyboardUsed;
            if (!used[i]) {
                result[i].pad = static_cast<s32>(i);
            }
        }
    }
    return result;
}
const MenuBindings& menuBindings(const GameConfig& config, s32 player) {
    const auto& profile = config.controls.at(static_cast<usize>(player));
    return profile.customized ? profile.menu : config.menu;
}
std::optional<ControlVibration> controlVibration(const GameConfig& config, const Input& input,
                                                 s32 player, s32 frames, ControlFeedback feedback) {
    if (player < 0 || static_cast<usize>(player) >= config.controls.size() || frames < 0 ||
        frames > 30 || !config.controls[static_cast<usize>(player)].rumble) {
        return std::nullopt;
    }
    const s32 pad = controlDevices(config, input)[static_cast<usize>(player)].pad;
    if (!input.isPadConnected(pad)) {
        return std::nullopt;
    }
    if (feedback == ControlFeedback::MeleeHit) {
        // Port feedback: a short high-frequency tap, distinct from the damage motor.
        constexpr u32 kHitMilliseconds = 90;
        constexpr u16 kHitStrength = 0x8000;
        return ControlVibration{pad, kHitMilliseconds, 0, kHitStrength, 0};
    }
    if (feedback == ControlFeedback::GeneratorDestroyed) {
        // A heavier confirmation than a melee tap, still subordinate to taking damage.
        constexpr u32 kDestroyedMilliseconds = 160;
        constexpr u16 kDestroyedLow = 0x4000;
        constexpr u16 kDestroyedHigh = 0xA000;
        return ControlVibration{pad, kDestroyedMilliseconds, kDestroyedLow, kDestroyedHigh, 0};
    }
    // PlayerControls decrements once per native rendered frame (30 Hz), stopping below
    // zero, not at zero. Keep that inclusive final frame independent of presentation FPS.
    const auto milliseconds = static_cast<u32>(((frames + 1) * 1000 + 29) / 30);
    // GC do_vibe (80031938) starts its binary motor only from damage_player's four
    // calls (80078b30..80078b7c). Damage has priority over the added melee taps.
    return ControlVibration{pad, milliseconds, 0xFFFF, 0, 1};
}
const PlayBindings& playBindings(const GameConfig& config, s32 player) {
    const auto& profile = config.controls.at(static_cast<usize>(player));
    return profile.customized ? profile.play : config.play;
}
} // namespace gdl::game
