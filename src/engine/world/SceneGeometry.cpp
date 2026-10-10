#include "engine/world/SceneGeometry.h"

#include <cmath>

namespace gdl {
bool SceneGeometry::Object::valid() const {
    if (continuity == 0 || !std::isfinite(alpha) || alpha < 0 || alpha > 1) {
        return false;
    }
    for (s32 column = 0; column < 4; ++column) {
        const f32 limit = column == 3 ? 1'000'000.0f : 1024.0f;
        for (s32 row = 0; row < 4; ++row) {
            if (!std::isfinite(local[column][row]) || std::abs(local[column][row]) > limit) {
                return false;
            }
        }
    }
    return local[0].w == 0 && local[1].w == 0 && local[2].w == 0 && local[3].w == 1;
}
bool SceneGeometry::valid() const {
    if (layout == 0 || objectCount == 0 || objectCount > 65535 || objects.size() > kMaxStates ||
        !std::isfinite(darken) || darken < 0 || darken > 1) {
        return false;
    }
    for (usize i = 0; i < objects.size(); ++i) {
        if (!objects[i].valid() || objects[i].index >= objectCount ||
            (i > 0 && objects[i - 1].index >= objects[i].index)) {
            return false;
        }
    }
    return true;
}
} // namespace gdl
