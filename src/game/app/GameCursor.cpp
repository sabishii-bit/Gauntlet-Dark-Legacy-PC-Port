#include "game/app/GameCursor.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

#include "engine/assets/TextureSet.h"
#include "engine/core/Log.h"

namespace gdl::game {

std::optional<GameCursor> GameCursor::load(const std::filesystem::path& assetRoot) {
    try {
        TextureSet textures;
        if (textures.load(assetRoot / "STATIC")) {
            if (const auto marker = textures.find("MARKER_LEFT")) {
                return fromMarker(textures.image(*marker));
            }
        }
    } catch (const std::exception& error) {
        log::warn("Mouse cursor asset unusable: {}", error.what());
    }
    return std::nullopt;
}

std::optional<GameCursor> GameCursor::fromMarker(const Image& marker) {
    if (marker.width == 0 || marker.height == 0 ||
        marker.width > static_cast<u32>(std::numeric_limits<s32>::max()) ||
        marker.height > static_cast<u32>(std::numeric_limits<s32>::max()) ||
        marker.pixels.size() != u64{marker.width} * marker.height * 4) {
        return std::nullopt;
    }
    u32 left = marker.width;
    u32 top = marker.height;
    u32 right = 0;
    u32 bottom = 0;
    for (u32 y = 0; y < marker.height; ++y) {
        for (u32 x = 0; x < marker.width; ++x) {
            if (marker.pixel(x, y).a != 0) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            }
        }
    }
    if (left == marker.width) {
        return std::nullopt;
    }
    const f32 width = static_cast<f32>(right - left + 1);
    const f32 height = static_cast<f32>(bottom - top + 1);
    const Vec2 center{static_cast<f32>(left) + width * 0.5f, static_cast<f32>(top) + height * 0.5f};
    // In top-down image coordinates, -135 degrees turns the marker's pointed
    // right end up-left, leaving its wider ornament behind the click hotspot.
    constexpr f32 kDiagonal = 0.70710678118f;
    constexpr f32 kMargin = 1.0f;
    const f32 scale = (static_cast<f32>(kSize) - 2 * kMargin) / ((width + height) * kDiagonal);
    constexpr f32 kCenter = static_cast<f32>(kSize) * 0.5f;
    const auto forward = [&](const Vec2& point) {
        const Vec2 delta = (point - center) * (scale * kDiagonal);
        return Vec2{kCenter - delta.x + delta.y, kCenter - delta.x - delta.y};
    };
    // Use the visible right tip, not the transparent corner of the rotated bitmap.
    u32 tipY = top;
    f32 nearest = std::numeric_limits<f32>::max();
    for (u32 y = top; y <= bottom; ++y) {
        const f32 distance = std::abs(static_cast<f32>(y) + 0.5f - center.y);
        if (marker.pixel(right, y).a != 0 && distance < nearest) {
            nearest = distance;
            tipY = y;
        }
    }
    const Vec2 tip = forward({static_cast<f32>(right) + 0.5f, static_cast<f32>(tipY) + 0.5f});
    GameCursor cursor;
    cursor.image = Image::filled(kSize, kSize, Color::rgba(0, 0, 0, 0));
    cursor.hotX =
        static_cast<u32>(std::clamp(std::floor(tip.x), 0.0f, static_cast<f32>(kSize - 1)));
    cursor.hotY =
        static_cast<u32>(std::clamp(std::floor(tip.y), 0.0f, static_cast<f32>(kSize - 1)));
    // Area samples keep the fine outline readable at pointer size. Accumulate
    // premultiplied colour so hidden RGB in transparent texels cannot make a halo;
    // the OS receives straight RGBA after filtering.
    constexpr s32 kSamples = 4;
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            Vec4 sum{0};
            for (s32 sy = 0; sy < kSamples; ++sy) {
                for (s32 sx = 0; sx < kSamples; ++sx) {
                    const Vec2 target{
                        static_cast<f32>(x) + (static_cast<f32>(sx) + 0.5f) / kSamples - kCenter,
                        static_cast<f32>(y) + (static_cast<f32>(sy) + 0.5f) / kSamples - kCenter};
                    const Vec2 source = center + Vec2{-target.x - target.y, target.x - target.y} *
                                                     (kDiagonal / scale);
                    const auto px = static_cast<s32>(std::floor(source.x));
                    const auto py = static_cast<s32>(std::floor(source.y));
                    if (px < 0 || py < 0 || px >= static_cast<s32>(marker.width) ||
                        py >= static_cast<s32>(marker.height)) {
                        continue;
                    }
                    const Color color = marker.pixel(static_cast<u32>(px), static_cast<u32>(py));
                    const f32 alpha = static_cast<f32>(color.a);
                    sum +=
                        Vec4{static_cast<f32>(color.r) * alpha, static_cast<f32>(color.g) * alpha,
                             static_cast<f32>(color.b) * alpha, alpha};
                }
            }
            if (sum.a > 0) {
                cursor.image.setPixel(
                    x, y,
                    Color::rgba(static_cast<u8>(std::lround(sum.r / sum.a)),
                                static_cast<u8>(std::lround(sum.g / sum.a)),
                                static_cast<u8>(std::lround(sum.b / sum.a)),
                                static_cast<u8>(std::lround(sum.a / (kSamples * kSamples)))));
            }
        }
    }
    return cursor;
}

} // namespace gdl::game
