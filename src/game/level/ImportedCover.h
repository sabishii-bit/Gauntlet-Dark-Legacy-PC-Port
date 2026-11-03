#pragma once
#include "engine/math/Math.h"

#include "game/level/CombatCover.h"

namespace gdl::game {
class ImportedLevel;

/** An imported level's cover: whatever its own arena says stands in the way. Made for each use
 * from the world's current imported level and not kept past it. */
class ImportedCover final : public CombatCover {
public:
    explicit ImportedCover(const ImportedLevel& level) : m_level(level) {}
    bool blocksBreath(const Vec3& from, const Vec3& to) const override;
    bool blocksArea(const Vec3& from, const Vec3& to) const override;

private:
    const ImportedLevel& m_level;
};
} // namespace gdl::game
