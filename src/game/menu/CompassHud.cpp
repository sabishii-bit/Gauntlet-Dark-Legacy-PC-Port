#include "game/menu/CompassHud.h"

#include <cmath>

#include "engine/core/Types.h"

namespace gdl::game {
std::array<Vec2, 4> CompassHud::directions(f32 cameraYaw) {
    const f32 sine = std::sin(cameraYaw);
    const f32 cosine = std::cos(cameraYaw);
    return {Vec2{-sine, -cosine}, Vec2{cosine, -sine}, Vec2{sine, cosine}, Vec2{-cosine, sine}};
}
void CompassHud::draw(Canvas& canvas, const TextPainter& text, const StringTable* strings,
                      f32 width, f32 cameraYaw) {
    if (strings == nullptr) {
        return;
    }
    constexpr std::array kLabels{"compass.north", "compass.east", "compass.south", "compass.west"};
    constexpr f32 kRadius = 16;
    const Vec2 center{width - 38, 64};
    const auto axes = directions(cameraYaw);
    TextStyle style;
    style.scale = 0.4f;
    for (usize i = 0; i < axes.size(); ++i) {
        const auto position = center + axes[i] * kRadius;
        text.draw(canvas, -static_cast<s32>(position.x), static_cast<s32>(position.y),
                  strings->get(kLabels[i]), style);
    }
}
} // namespace gdl::game
