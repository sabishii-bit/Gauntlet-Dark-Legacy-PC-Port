#pragma once

#include <array>
#include <optional>
#include <span>

#include "engine/core/Types.h"

#include "game/enemies/EnemyFeedback.h"

namespace gdl::game {
/** ROM-authored swarm hurt/death routing; neither text names nor GC bank aliases. */
class LegendsEnemySounds {
public:
    static LegendsEnemySounds decode(std::span<const u8> program, std::span<const u8> enemies);
    std::optional<u32> feedback(const EnemyFeedback& event, f32 health) const;
    std::optional<u32> melee(s32 kind, s32 tier) const;

private:
    std::array<s8, 34> m_families{};
    std::array<std::array<u16, 15>, 7> m_cues{};
};
} // namespace gdl::game
