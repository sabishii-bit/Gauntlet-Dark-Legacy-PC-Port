#include <functional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/CanonicalDigestSink.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "DrawStreamDigest.h"
#include "FakeRenderDevice.h"

namespace {

using namespace gdl;

/** A texture with a size and no pixels a test can read. */
class SizedTexture final : public Texture {
public:
    SizedTexture(u32 width, u32 height) : m_width(width), m_height(height) {}
    u32 width() const override { return m_width; }
    u32 height() const override { return m_height; }

private:
    u32 m_width;
    u32 m_height;
};

/** A quad's two triangles over `texture`. */
test::RecordedDraw quad(const Texture& texture) {
    test::RecordedDraw draw;
    draw.texture = &texture;
    for (const Vec2 corner :
         {Vec2{0, 0}, Vec2{8, 0}, Vec2{8, 8}, Vec2{0, 0}, Vec2{8, 8}, Vec2{0, 8}}) {
        draw.vertices.push_back({Vec3{corner, 0.5f}, Color::white(), corner / 8.0f});
    }
    return draw;
}

CanonicalDigestSink::Digest digestOf(const std::vector<test::RecordedDraw>& draws) {
    return test::drawStreamDigest(draws);
}

TEST_CASE("a draw stream digest sees a texture by what it holds, not where it lives",
          "[draw-stream-digest]") {
    test::FakeTexture first(2, 1);
    first.pixels = {1, 2, 3, 4, 5, 6, 7, 8};
    test::FakeTexture copy(2, 1);
    copy.pixels = first.pixels;
    test::FakeTexture changed(2, 1);
    changed.pixels = {1, 2, 3, 4, 5, 6, 7, 9};
    test::FakeTexture turned(1, 2);
    turned.pixels = first.pixels;
    const auto reference = digestOf({quad(first)});
    CHECK(digestOf({quad(copy)}) == reference);
    CHECK(digestOf({quad(changed)}) != reference);
    CHECK(digestOf({quad(turned)}) != reference);
    // The state's own textures are read the same way, and none is told from one.
    auto lit = quad(first);
    lit.state.lightmap = &copy;
    auto litAlike = quad(first);
    litAlike.state.lightmap = &first;
    CHECK(digestOf({lit}) == digestOf({litAlike}));
    CHECK(digestOf({lit}) != reference);
    // One whose pixels cannot be read is known by its size alone.
    const SizedTexture sized(2, 1);
    const SizedTexture sizedAlike(2, 1);
    const SizedTexture sizedTurned(1, 2);
    const test::FakeTexture unread(2, 1);
    CHECK(digestOf({quad(sized)}) == digestOf({quad(sizedAlike)}));
    CHECK(digestOf({quad(sized)}) != digestOf({quad(sizedTurned)}));
    CHECK(digestOf({quad(sized)}) != digestOf({quad(unread)}));
}

TEST_CASE("a draw stream digest holds each draw's vertices, transform and state bit for bit, "
          "in order",
          "[draw-stream-digest]") {
    test::FakeTexture texture(1, 1);
    texture.pixels = {255, 255, 255, 255};
    test::FakeTexture other(1, 1);
    other.pixels = {0, 0, 0, 255};
    const auto reference = digestOf({quad(texture), quad(other)});
    CHECK(digestOf({quad(texture), quad(other)}) == reference);
    CHECK(digestOf({quad(other), quad(texture)}) != reference);
    CHECK(digestOf({quad(texture)}) != reference);
    const std::vector<std::function<void(test::RecordedDraw&)>> changes{
        [](test::RecordedDraw& draw) { draw.vertices[0].position.x = -0.0f; },
        [](test::RecordedDraw& draw) { draw.vertices[1].color.a = 254; },
        [](test::RecordedDraw& draw) { draw.vertices[2].uv.y = 0.5f; },
        [](test::RecordedDraw& draw) { draw.vertices[3].uv2.x = 0.25f; },
        [](test::RecordedDraw& draw) { draw.vertices.pop_back(); },
        [](test::RecordedDraw& draw) { draw.transform[3][0] = -0.0f; },
        [](test::RecordedDraw& draw) { draw.transform[1][1] = 2.0f; },
        [](test::RecordedDraw& draw) { draw.state.blend = BlendMode::Additive; },
        [&other](test::RecordedDraw& draw) { draw.state.maskedTexture = &other; },
        [](test::RecordedDraw& draw) { draw.state.uvOffset.y = 0.125f; },
        [](test::RecordedDraw& draw) { draw.state.alphaTest = DrawState::kTranslucentAlphaTest; },
        [](test::RecordedDraw& draw) { draw.state.depthWrite = false; },
        [](test::RecordedDraw& draw) { draw.state.smoothSprite = true; },
        [](test::RecordedDraw& draw) { draw.state.whiteBlend = 0.5f; },
        [](test::RecordedDraw& draw) { draw.state.fog.bias = 0.25f; },
    };
    for (usize index = 0; index < changes.size(); ++index) {
        CAPTURE(index);
        auto changed = quad(texture);
        changes[index](changed);
        CHECK(digestOf({changed, quad(other)}) != reference);
    }
}

} // namespace
