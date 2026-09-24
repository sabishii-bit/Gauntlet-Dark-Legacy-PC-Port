#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

namespace gdl::game {
/** The five STATIC sprites used by the music and sound-effect volume controls. */
struct AudioSlider {
    static constexpr std::array<const char*, 5> kTextures{"MARKER_LEFT", "EMPTY_BAR", "PINK_BAR",
                                                          "SLIDER", "MARKER_RIGHT"};
    std::array<const Texture*, 5> textures{};

    static s32 value(f32 volume) {
        return std::isfinite(volume)
                   ? static_cast<s32>(std::lround(std::clamp(volume, 0.0f, 1.0f) * 255))
                   : 0;
    }
    void draw(Canvas& canvas, f32 x, f32 y, f32 volume, bool selected, u8 fade) const {
        // The original keeps a one-pixel fill at zero; the knob overlaps the fill's end.
        const f32 fill = static_cast<f32>(std::max(264 * value(volume) / 255, 1));
        const std::array<Vec2, 5> positions{Vec2{x - 52, y}, Vec2{x, y + 11}, Vec2{x, y + 15},
                                            Vec2{x + fill - 20, y + 2}, Vec2{x + 264 - 24, y}};
        constexpr std::array<u8, 5> kInactiveOpacity{155, 105, 95, 155, 155};
        for (usize i = 0; i < textures.size(); ++i) {
            if (const auto* texture = textures[i]) {
                auto width = static_cast<f32>(texture->width());
                if (i == 1) {
                    width = 264.0f;
                } else if (i == 2) {
                    width = fill;
                }
                const f32 height = i == 1 || i == 2 ? 32.0f : static_cast<f32>(texture->height());
                const auto opacity =
                    static_cast<u8>((selected ? 255 : kInactiveOpacity[i]) * fade / 255);
                canvas.draw(*texture, {positions[i].x, positions[i].y, width, height},
                            Color::white().withAlpha(opacity));
            }
        }
    }
};
} // namespace gdl::game
