#include "game/widgets/PointerTargets.h"

#include <optional>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl::game {

bool pointerMoved(const std::optional<Vec2>& last, Vec2 now, f32 threshold) {
    return !last.has_value() || glm::distance(*last, now) > threshold;
}

} // namespace gdl::game
