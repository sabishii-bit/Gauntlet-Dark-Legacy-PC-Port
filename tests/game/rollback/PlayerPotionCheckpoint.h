#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Types.h"

#include "game/characters/Inventory.h"

namespace gdl::game {
/** A private potion-stack observation and native emission tuning, never authority
 * to spend the saved inventory, apply healing or damage, or choose a shared colour. */
struct PlayerPotionState {
    static constexpr u32 kMaxTicks = 120;
    std::vector<s32> potions; ///< native last-in first-out order; unspecified kind zero is excluded
    std::array<f32, 5> powers{8, 8, 8, 8, 8};
    std::array<f32, 5> bonuses{1, 1, 1, 1, 1};
    u32 damageFlags = 0;
    u32 ticks = 0; ///< stable inventory prefix, also bounded by the enclosing movement contacts
    std::optional<f32> textureFrame; ///< native missile-pool WEAPONS bank; absent before first use
    bool valid() const;
    bool operator==(const PlayerPotionState&) const = default;
};

class PlayerPotionCheckpoint {
public:
    static constexpr usize kHeaderBytes = 60;
    static constexpr usize kMaxBytes = kHeaderBytes + usize{4} * Inventory::kMostPotions;
    static std::optional<std::vector<u8>> encode(const PlayerPotionState& state);
    static std::optional<PlayerPotionState> decode(std::span<const u8> bytes);
};
} // namespace gdl::game
