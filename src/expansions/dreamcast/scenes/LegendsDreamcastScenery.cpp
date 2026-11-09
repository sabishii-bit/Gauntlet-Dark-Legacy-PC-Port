#include "expansions/dreamcast/level/LegendsDreamcastScenery.h"

#include <optional>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::strikeDreamcastScenery(s32 localId, f32 power, u32 flags) {
    if (!m_dreamcastScenery.strike(localId, power, flags)) {
        return false;
    }
    // AudioShootWall 8c0e1220: level-specific hit, global 27 on destruction.
    const auto token =
        m_dreamcastScenery.health(localId) > 0 ? m_dreamcastWallHitSound : std::optional<u32>{0x27};
    if (token) {
        m_soundEvents.push_back({*token, m_dreamcastScenery.position(localId)});
    }
    return true;
}

bool LegendsSceneAdapter::strikeDreamcastWorldObject(s32 object, f32 power, u32 flags) {
    const auto target = m_dreamcastScenery.targetForObject(object);
    return target && strikeDreamcastScenery(*target, power, flags);
}

} // namespace gdl::game
