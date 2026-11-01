#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace gdl::game {
bool LegendsSceneAdapter::strikeScenery(s32 localId, f32 power, u32 flags) {
    if (!m_scenery.strike(localId, power, flags)) {
        return false;
    }
    const auto slot = static_cast<usize>(localId - LegendsScenery::kTargetBase);
    const auto& definition = m_scenery.definitions()[slot];
    // Native e0077738: a surviving wall emits5f, final destruction emits75.
    m_soundEvents.push_back({m_scenery.health(slot) > 0 ? 0x5fU : 0x75U,
                             (definition.minimum + definition.maximum) * .5f});
    return true;
}

bool LegendsSceneAdapter::strikeWorldObject(s32 object, f32 power, u32 flags) {
    if (object < 0) {
        return false;
    }
    const auto definitions = m_scenery.definitions();
    for (usize i = 0; i < definitions.size(); ++i) {
        if (definitions[i].object == static_cast<u32>(object)) {
            return strikeScenery(LegendsScenery::kTargetBase + static_cast<s32>(i), power, flags);
        }
    }
    return false;
}
} // namespace gdl::game
