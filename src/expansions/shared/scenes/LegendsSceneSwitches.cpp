#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

void LegendsSceneAdapter::touchSwitches(std::span<const LegendsPortalVisitor> visitors) {
    if (m_dreamcastMovement) {
        touchDreamcastSwitches(visitors);
    } else {
        touchN64Switches(visitors);
    }
}

} // namespace gdl::game
