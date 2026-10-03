#include <cstddef>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"

namespace {

using namespace gdl;

TEST_CASE("Extent2D reports zero when either side is zero", "[render][types]") {
    STATIC_REQUIRE(Extent2D{0, 10}.isZero());
    STATIC_REQUIRE(Extent2D{10, 0}.isZero());
    STATIC_REQUIRE(!Extent2D{1, 1}.isZero());
    REQUIRE(Extent2D{3, 4} == Extent2D{3, 4});
    REQUIRE(Extent2D{3, 4} != Extent2D{4, 3});
}

TEST_CASE("ImmediateVertex matches the GPU vertex layout", "[render][types]") {
    STATIC_REQUIRE(sizeof(ImmediateVertex) == 32);
    STATIC_REQUIRE(offsetof(ImmediateVertex, position) == 0);
    STATIC_REQUIRE(offsetof(ImmediateVertex, color) == 12);
    STATIC_REQUIRE(offsetof(ImmediateVertex, uv) == 16);
    STATIC_REQUIRE(offsetof(ImmediateVertex, uv2) == 24);
}

TEST_CASE("TextureDesc defaults to linear repeat sampling", "[render][types]") {
    constexpr TextureDesc kDesc{};
    STATIC_REQUIRE(kDesc.filter == TextureFilter::Linear);
    STATIC_REQUIRE(kDesc.wrap == TextureWrap::Repeat);
}

TEST_CASE("presentation samples fall back to the next supported count without rounding up",
          "[render][types][graphics-settings]") {
    STATIC_REQUIRE(presentationSamples(1, 7) == 1);
    STATIC_REQUIRE(presentationSamples(2, 7) == 2);
    STATIC_REQUIRE(presentationSamples(4, 7) == 4);
    // Some devices support 4x but not 2x. A 2x request must not select 4x.
    STATIC_REQUIRE(presentationSamples(2, 5) == 1);
    STATIC_REQUIRE(presentationSamples(4, 3) == 2);
    STATIC_REQUIRE(presentationSamples(4, 1) == 1);
    STATIC_REQUIRE(presentationSamples(4, 0) == 1);
    STATIC_REQUIRE(presentationSamples(0, 7) == 1);
    STATIC_REQUIRE(presentationSamples(3, 7) == 1);
    STATIC_REQUIRE(presentationSamples(8, 15) == 1);
}

TEST_CASE("flipbook blending preserves the second texture stage's existing owners",
          "[render][texture-blend]") {
    test::FakeTexture frame(1, 1);
    DrawState state;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.textureBlend = 0.5f;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.nextTexture = &frame;
    CHECK(state.effectiveTextureBlend() == 0.5f);
    state.maskedTexture = &frame;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.maskedTexture = nullptr;
    state.lightmap = &frame;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.lightmap = nullptr;
    CHECK(state.effectiveTextureBlend() == 0.5f);
    state.textureBlend = -1.0f;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.textureBlend = 0.0f;
    CHECK(state.effectiveTextureBlend() == 0.0f);
    state.textureBlend = 1.0f;
    CHECK(state.effectiveTextureBlend() == 1.0f);
    state.textureBlend = 2.0f;
    CHECK(state.effectiveTextureBlend() == 1.0f);
    state.textureBlend = std::numeric_limits<f32>::quiet_NaN();
    CHECK(state.effectiveTextureBlend() == 0.0f);
}

} // namespace
