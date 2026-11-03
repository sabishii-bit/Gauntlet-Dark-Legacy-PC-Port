#pragma once
#include "engine/math/Math.h"

#include "game/level/CombatCover.h"

namespace gdl::game {
class SafeRocks;

/** A native level's cover: the safe rocks standing in its boss arena. Made for each use and
 * not kept past it. */
class SafeRockCover final : public CombatCover {
public:
    explicit SafeRockCover(const SafeRocks& rocks) : m_rocks(rocks) {}
    bool blocksBreath(const Vec3& from, const Vec3& to) const override;
    bool blocksArea(const Vec3& from, const Vec3& to) const override;

private:
    const SafeRocks& m_rocks;
};
} // namespace gdl::game
