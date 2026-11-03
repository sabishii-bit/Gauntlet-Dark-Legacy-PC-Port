#include "game/level/ImportedCover.h"

#include "engine/math/Math.h"

#include "game/level/ImportedLevel.h"

namespace gdl::game {

bool ImportedCover::blocksBreath(const Vec3& from, const Vec3& to) const {
    return m_level.arenaBlocksSegment(from, to, kBreathProbeRadius);
}

bool ImportedCover::blocksArea(const Vec3& from, const Vec3& to) const {
    return m_level.arenaBlocksSegment(from, to, kAreaProbeRadius);
}

} // namespace gdl::game
