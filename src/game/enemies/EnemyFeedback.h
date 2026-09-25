#pragma once

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
    std::string_view effect() const;
    f32 effectScale() const;
    std::string_view deathSkin() const;
    s32 deathSkinFrames() const;
};
} // namespace gdl::game
