#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/assets/WorldData.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {
/** A successful swarm hit, captured before its actor can die or its slot be recycled. */
struct EnemyFeedback {
    s32 kind = 0;
    s32 tier = 1;
    s32 hitCount = 1;
    bool killed = false;
    bool close = false;
    u32 flags = 0;
    Vec3 position{0};
    f32 yaw = 0;
    f32 halfHeight = 0;

    std::string sound(std::span<const LevelEnemy> roster, s32 bossType = -1) const;
    /** A dedicated melee impact, or nullopt for the ordinary player pain path.
     * An empty name retains the dedicated path when its level sound is unavailable. */
    static std::optional<std::string>
    meleeSound(s32 kind, s32 tier, std::span<const LevelEnemy> roster, s32 bossType = -1);
    std::string_view effect() const;
    f32 effectScale() const;
    std::string_view deathSkin() const;
    s32 deathSkinFrames() const;
};
} // namespace gdl::game
