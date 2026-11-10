#include "expansions/dreamcast/level/LegendsDreamcastPlacement.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::bindDreamcastFalling(const formats::ItemInfoRecord& info,
                                               const formats::ItemInstanceRecord& instance,
                                               u32 nativeIndex, usize figure) {
    return figure < m_fixtures.size() &&
           m_dreamcastFalling.add(info, instance, nativeIndex, figure);
}

void LegendsSceneAdapter::touchDreamcastFalling(std::span<const LegendsPortalVisitor> visitors) {
    const auto newlyActive = m_dreamcastFalling.touch(visitors);
    if (m_dreamcastCollapseSound) {
        for (const Vec3 position : newlyActive) {
            m_soundEvents.push_back({*m_dreamcastCollapseSound, position});
        }
    }
}

void LegendsSceneAdapter::updateDreamcastFalling(f32 seconds) {
    m_dreamcastFalling.update(seconds);
    for (const auto& piece : m_dreamcastFalling.pieces()) {
        // Keep the existing hidden-state admission for optional/player-count
        // fixtures. Native removal is one-way until the level is reopened.
        if (!piece.body.visible) {
            m_hiddenFixtures[piece.figure] = true;
        }
        m_fixtures[piece.figure].placeAt(
            legendsDreamcastItemPlacement(piece.body.position, piece.body.rotation));
    }
}
} // namespace gdl::game
