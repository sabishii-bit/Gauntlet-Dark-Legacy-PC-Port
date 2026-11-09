#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {

bool LegendsSceneAdapter::strikeScenery(s32 localId, f32 power, u32 flags) {
    return m_platform == LegendsPlatform::Dreamcast ? strikeDreamcastScenery(localId, power, flags)
                                                    : strikeN64Scenery(localId, power, flags);
}

bool LegendsSceneAdapter::strikeWorldObject(s32 object, f32 power, u32 flags) {
    if (object < 0) {
        return false;
    }
    return m_platform == LegendsPlatform::Dreamcast
               ? strikeDreamcastWorldObject(object, power, flags)
               : strikeN64WorldObject(object, power, flags);
}

} // namespace gdl::game
