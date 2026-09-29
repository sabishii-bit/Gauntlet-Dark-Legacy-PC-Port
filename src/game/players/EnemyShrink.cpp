#include "game/players/EnemyShrink.h"

#include "engine/core/Types.h"

namespace gdl::game {

f32 EnemyShrink::scaleOf(s32 wearers, bool bossEncounter) {
    f32 scale = kWhole;
    if (bossEncounter) {
        return scale;
    }
    for (s32 i = 0; i < wearers; ++i) {
        scale *= kPerWearer;
    }
    return scale;
}

} // namespace gdl::game
