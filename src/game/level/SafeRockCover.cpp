#include "game/level/SafeRockCover.h"

#include "engine/math/Math.h"

#include "game/level/SafeRocks.h"

namespace gdl::game {

bool SafeRockCover::blocksBreath(const Vec3& from, const Vec3& to) const {
    return m_rocks.blocksBreath(from, to);
}

bool SafeRockCover::blocksArea(const Vec3& from, const Vec3& to) const {
    return m_rocks.blocksSegment(from, to, kAreaProbeRadius);
}

} // namespace gdl::game
