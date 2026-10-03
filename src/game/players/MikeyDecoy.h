#pragma once

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/players/Inventory.h"

namespace gdl::game {
/** A selector-activated, stationary distraction for the swarm, independent of the player body. */
class MikeyDecoy {
public:
    static constexpr s32 kFramesPerSecond = 30;
    static constexpr s32 kDespawnFrame = 300;
    static constexpr s32 kSparkleEnd = 60;
    static constexpr s32 kSparklePeriod = 10;

    /** Consume activation requests at the gameplay rate. Item time runs only while requested. */
    void update(f32 seconds, Inventory& inventory, const Vec3& position, f32 timerRate = 1);
    void clear();
    bool shown() const { return m_state >= 2; }
    std::optional<Vec3> target() const {
        return m_state > 2 ? std::optional<Vec3>{m_position} : std::nullopt;
    }
    const Vec3& position() const { return m_position; }
    u32 generation() const { return m_generation; }
    s32 state() const { return m_state; }
    s32 takeSparkles();

private:
    void step(Inventory& inventory, const Vec3& position, f32 timerRate);
    Vec3 m_position{0};
    f64 m_frames = 0;
    s32 m_state = 0;
    s32 m_sparkles = 0;
    u32 m_generation = 0;
};
} // namespace gdl::game
