#pragma once
#include <array>
#include <optional>
#include <span>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/core/Types.h"

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
/** This machine's input slots: one per entry of the settings' controls. */
inline constexpr usize kInputSlots = 4;
static_assert(kInputSlots == std::tuple_size_v<decltype(GameConfig::controls)>);
/** The devices one input slot plays on: the keyboard or not, and one pad. */
struct ControlDevice {
    static constexpr s32 kAllPads = -1; ///< every pad, for a read that merges them
    static constexpr s32 kNoPad = -2;   ///< no pad
    bool keyboard = false;
    s32 pad = kNoPad;
};
std::array<ControlDevice, kInputSlots> controlDevices(const GameConfig& config, const Input& input);
enum class ControlFeedback : u8 { Damage, MeleeHit, GeneratorDestroyed, ThunderHammer };
struct ControlVibration {
    s32 pad = -1;
    u32 milliseconds = 0;
    u16 low = 0;
    u16 high = 0;
    u8 priority = 0;
};
/** Resolve feedback through the gameplay device assignment. Frames is the native damage
 * countdown; the port's attack pulses have their own fixed durations. */
std::optional<ControlVibration>
controlVibration(const GameConfig& config, const Input& input, s32 player, s32 frames,
                 ControlFeedback feedback = ControlFeedback::Damage);
s32 controllerOccurrence(const Input& input, s32 pad);
const MenuBindings& menuBindings(const GameConfig& config, s32 player);
const PlayBindings& playBindings(const GameConfig& config, s32 player);
} // namespace gdl::game
