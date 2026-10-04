#pragma once
#include <span>

#include <nlohmann/json_fwd.hpp>

#include "game/config/GameConfig.h"

namespace gdl::game {
/** One action shared by persistence, capture and the settings UI. */
struct ControlAction {
    const char* id = "";
    std::vector<Key> PlayBindings::*keys = nullptr;
    std::vector<PadButton> PlayBindings::*buttons = nullptr;
    std::vector<Key> MenuBindings::*menuKeys = nullptr;
    std::vector<PadButton> MenuBindings::*menuButtons = nullptr;
    std::vector<Key>& keyboard(PlayerControlConfig& profile) const;
    std::vector<PadButton>& controller(PlayerControlConfig& profile) const;
};
std::span<const ControlAction> controlActions();
void readControlProfiles(const nlohmann::json& json, GameConfig& config);
nlohmann::json writeControlProfiles(const GameConfig& config);
struct ControlDevice {
    bool keyboard = false;
    s32 pad = -2;
};
std::array<ControlDevice, 4> controlDevices(const GameConfig& config, const Input& input);
s32 controllerOccurrence(const Input& input, s32 pad);
const MenuBindings& menuBindings(const GameConfig& config, s32 player);
const PlayBindings& playBindings(const GameConfig& config, s32 player);
} // namespace gdl::game
