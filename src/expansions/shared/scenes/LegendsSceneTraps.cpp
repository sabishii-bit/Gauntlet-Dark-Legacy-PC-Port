#include <cmath>
#include <utility>

#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::updateTraps(f32 seconds, bool timeStopped) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10) {
        return;
    }
    updateDreamcastTraps(seconds, timeStopped);
    updateN64Traps(seconds, timeStopped);
}

void LegendsSceneAdapter::touchTraps(std::span<const LegendsPortalVisitor> visitors) {
    touchDreamcastTraps(visitors);
    touchN64Traps(visitors);
}

std::vector<LegendsTrapHit> LegendsSceneAdapter::takeTrapHits() {
    return std::exchange(m_trapHits, {});
}

} // namespace gdl::game
