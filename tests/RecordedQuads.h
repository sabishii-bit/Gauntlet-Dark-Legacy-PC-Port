#pragma once

#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"

namespace gdl::test {

/** One quad of the draws a FakeRenderDevice recorded: where it lands, in the units it was drawn
 * in; the part of its texture across it, which runs backwards where the picture is turned; its
 * texture and colour; and the transform it was drawn through. */
struct Quad {
    const Texture* texture = nullptr;
    Rect area{};
    Rect uv{};
    Color color{};
    Mat4 transform{1.0f};
};

/** Every quad `device` recorded, in the order drawn. A quad's first vertex is its top left and
 * its third its bottom right (ImmediateBatch::rect). */
inline std::vector<Quad> quadsOf(const FakeRenderDevice& device) {
    constexpr usize kQuadVertices = 6;
    std::vector<Quad> quads;
    for (const RecordedDraw& draw : device.draws) {
        for (usize at = 0; at + kQuadVertices <= draw.vertices.size(); at += kQuadVertices) {
            const ImmediateVertex& first = draw.vertices[at];
            const ImmediateVertex& last = draw.vertices[at + 2];
            quads.push_back(
                {.texture = draw.texture,
                 .area = {first.position.x, first.position.y, last.position.x - first.position.x,
                          last.position.y - first.position.y},
                 .uv = {first.uv.x, first.uv.y, last.uv.x - first.uv.x, last.uv.y - first.uv.y},
                 .color = first.color,
                 .transform = draw.transform});
        }
    }
    return quads;
}

/** Whether `quad` draws `picture`'s pixels, as they were uploaded to the fake device. */
inline bool showsPixels(const Quad& quad, const Image& picture) {
    const auto* made = dynamic_cast<const FakeTexture*>(quad.texture);
    return made != nullptr && made->width() == picture.width && made->height() == picture.height &&
           made->pixels == picture.pixels;
}

/** Checks `actual` lies on `expected`, but for float rounding within `margin`. */
inline void checkRect(const Rect& actual, const Rect& expected, f64 margin = 1e-4) {
    CHECK(actual.x == Catch::Approx(expected.x).margin(margin));
    CHECK(actual.y == Catch::Approx(expected.y).margin(margin));
    CHECK(actual.width == Catch::Approx(expected.width).margin(margin));
    CHECK(actual.height == Catch::Approx(expected.height).margin(margin));
}

} // namespace gdl::test
