#pragma once

#include <optional>

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "game/enemies/Enemies.h"
#include "game/level/ImportedTargets.h"

namespace gdl::game {

/** Native PPow Death is an inert statue until contact, not an inventory item.
 * A pending replacement remains visible until the common enemy pool accepts it. */
class LegendsDeath {
public:
    static constexpr s32 kTargetBase = 8192;
    static constexpr usize kMost = 4096;
    static std::optional<LegendsDeath> decode(const formats::LegendsN64Fixture& fixture,
                                              s32 players);
    bool touch(const Vec3& feet, f32 radius, f32 height);
    bool strike(f32 power);
    void placeAt(Vec3 position) { m_position = position; }
    bool pending() const { return m_pending && !m_released; }
    bool released() const { return m_released; }
    void release() { m_released = true; }
    EnemySpawn spawn() const;
    ImportedTarget target(s32 index) const;
    const Vec3& position() const { return m_position; }
    u32 nativeIndex() const { return m_nativeIndex; }

private:
    u32 m_nativeIndex = 0;
    Vec3 m_position{0};
    f32 m_radius = 51.0f / 32.0f;
    f32 m_height = 3;
    bool m_pending = false;
    bool m_released = false;
};

} // namespace gdl::game
