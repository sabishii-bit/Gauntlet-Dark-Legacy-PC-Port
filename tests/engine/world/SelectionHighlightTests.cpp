#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Mesh.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/SelectionHighlight.h"

#include "FakeRenderDevice.h"

namespace {
using namespace gdl;
using Catch::Approx;

TEST_CASE("a selection highlight draws a part's rim pushed out and wound the other way, then "
          "its wash lifted toward the eye",
          "[world][selection][dev-console]") {
    // A triangle facing the eye, which stands ten units before it.
    Mesh mesh;
    mesh.vertices = {
        {.position = {0.0f, 0.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {0, 0}},
        {.position = {2.0f, 0.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {1, 0}},
        {.position = {0.0f, 2.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {0, 1}}};
    const MeshPart part{.texture = 0, .indices = {0, 1, 2}};
    test::FakeRenderDevice device;
    constexpr std::array<u8, 4> kTexel{255, 255, 255, 255};
    const auto skinTexture = device.createTexture(TextureDesc{1, 1, TextureFilter::Linear}, kTexel);
    const Texture& skin = *skinTexture;
    const SelectionHighlight highlight{Vec3{0.0f, 0.0f, 0.0f}};
    const Mat4 clip{1.0f}; // the identity: a depth of one, a scale of one
    CHECK(SelectionHighlight::rimReach(clip, Vec3{0.0f, 0.0f, 10.0f}) ==
          Approx(2.0f * SelectionHighlight::kRimWidth));
    highlight.draw(device, clip, mesh, part, Mat4{1.0f}, skin);
    REQUIRE(device.draws.size() == 2);
    const auto& rim = device.draws[0];
    const auto& wash = device.draws[1];
    // The rim is solid orange through the white mask, cut out by the part's own texture, each
    // corner pushed out along its normal, the second and third corners swapped so its front
    // is the part's back.
    CHECK(rim.texture == &skin);
    CHECK(rim.state.maskedTexture == &device.whiteTexture());
    REQUIRE(rim.vertices.size() == 3);
    const f32 reach = SelectionHighlight::rimReach(clip, Vec3{0.0f, 0.0f, 10.0f});
    CHECK(rim.vertices[0].position == Vec3{0.0f, 0.0f, 10.0f - reach});
    CHECK(rim.vertices[1].position == Vec3{0.0f, 2.0f, 10.0f - reach});
    CHECK(rim.vertices[2].position == Vec3{2.0f, 0.0f, 10.0f - reach});
    for (const auto& vertex : rim.vertices) {
        CHECK(vertex.color == SelectionHighlight::kRim);
    }
    // The wash is the part's own corners in order, a little nearer the eye, a fifth of the
    // orange through the mask, cut out the same way.
    CHECK(wash.texture == &skin);
    CHECK(wash.state.maskedTexture == &device.whiteTexture());
    REQUIRE(wash.vertices.size() == 3);
    CHECK(wash.vertices[1].position ==
          Vec3{2.0f, 0.0f, 10.0f} * (1.0f - SelectionHighlight::kLift));
    CHECK(wash.vertices[2].uv == Vec2{0.0f, 1.0f});
    for (const auto& vertex : wash.vertices) {
        CHECK(vertex.color == SelectionHighlight::kWash);
    }
    // Both lie over what is drawn without writing depth, tested against it, and cut out
    // where clear.
    for (const auto& draw : device.draws) {
        CHECK(draw.state.blend == BlendMode::Alpha);
        CHECK_FALSE(draw.state.depthWrite);
        CHECK(draw.state.depthTest);
        CHECK(draw.state.cullBack);
        CHECK_FALSE(draw.state.mipmaps);
        CHECK(draw.state.alphaTest == DrawState::kTranslucentAlphaTest);
    }
    // A part with no triangle draws nothing; a deeper point is pushed out further.
    device.draws.clear();
    highlight.draw(device, clip, mesh, MeshPart{}, Mat4{1.0f}, skin);
    CHECK(device.draws.empty());
    const Mat4 perspective = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    CHECK(SelectionHighlight::rimReach(perspective, Vec3{0.0f, 0.0f, -20.0f}) ==
          Approx(2.0f * SelectionHighlight::rimReach(perspective, Vec3{0.0f, 0.0f, -10.0f})));
}

TEST_CASE("a highlight can mark a part out in other colours and another rim width",
          "[world][selection]") {
    Mesh mesh;
    mesh.vertices = {
        {.position = {0.0f, 0.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {0, 0}},
        {.position = {2.0f, 0.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {1, 0}},
        {.position = {0.0f, 2.0f, 10.0f}, .normal = {0.0f, 0.0f, -1.0f}, .uv = {0, 1}}};
    const MeshPart part{.texture = 0, .indices = {0, 1, 2}};
    test::FakeRenderDevice device;
    constexpr std::array<u8, 4> kTexel{255, 255, 255, 255};
    const auto skinTexture = device.createTexture(TextureDesc{1, 1, TextureFilter::Linear}, kTexel);
    const Color rim = Color::rgba(235, 25, 25, 128);
    const Color wash = Color::rgba(170, 0, 0, 40);
    const f32 width = SelectionHighlight::kRimWidth * 2.0f;
    const SelectionHighlight aura{Vec3{0.0f, 0.0f, 0.0f}, rim, wash, width};
    CHECK(aura.rim() == rim);
    CHECK(aura.wash() == wash);
    CHECK(aura.rimWidth() == width);
    const Mat4 clip{1.0f};
    CHECK(SelectionHighlight::rimReach(clip, Vec3{0.0f, 0.0f, 10.0f}, width) ==
          Approx(2.0f * width));
    aura.draw(device, clip, mesh, part, Mat4{1.0f}, *skinTexture);
    REQUIRE(device.draws.size() == 2);
    const f32 reach = SelectionHighlight::rimReach(clip, Vec3{0.0f, 0.0f, 10.0f}, width);
    CHECK(device.draws[0].vertices[0].position == Vec3{0.0f, 0.0f, 10.0f - reach});
    for (const auto& vertex : device.draws[0].vertices) {
        CHECK(vertex.color == rim);
    }
    for (const auto& vertex : device.draws[1].vertices) {
        CHECK(vertex.color == wash);
    }
    // The plain one keeps the editor's orange and width.
    const SelectionHighlight plain{Vec3{0.0f, 0.0f, 0.0f}};
    CHECK(plain.rim() == SelectionHighlight::kRim);
    CHECK(plain.wash() == SelectionHighlight::kWash);
    CHECK(plain.rimWidth() == SelectionHighlight::kRimWidth);
}

} // namespace
