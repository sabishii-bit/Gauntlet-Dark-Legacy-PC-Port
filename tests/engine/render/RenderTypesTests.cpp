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
    STATIC_REQUIRE(kDesc.mipLevels == 1);
    STATIC_REQUIRE(!kDesc.generateMipmaps);
    CHECK_FALSE(DrawState{}.mipmaps); // UI and video keep their base-level sampling.
}

TEST_CASE("mip chains handle thin and non-power-of-two textures", "[render][types][mipmaps]") {
    STATIC_REQUIRE(textureMipCount(1, 1) == 1);
    STATIC_REQUIRE(textureMipCount(8, 1) == 4);
    STATIC_REQUIRE(textureMipCount(1, 8) == 4);
    STATIC_REQUIRE(textureMipCount(13, 5) == 4);
    STATIC_REQUIRE(textureMipExtent({13, 5}, 1) == Extent2D{6, 2});
    STATIC_REQUIRE(textureMipExtent({13, 5}, 2) == Extent2D{3, 1});
    STATIC_REQUIRE(textureMipExtent({13, 5}, 3) == Extent2D{1, 1});
    STATIC_REQUIRE(textureMipExtent({1, 8}, 3) == Extent2D{1, 1});
    STATIC_REQUIRE(textureMipBytes({8, 4}, 2) == (u64{32} + 8) * 4);
    STATIC_REQUIRE(textureMipBytes({13, 5}, 4) == (u64{65} + 12 + 3 + 1) * 4);
    for (const u32 value : {0U, 1U, 2U, 4U, 8U, 16U}) {
        CHECK(validTextureFiltering(value) == value);
    }
    CHECK(validTextureFiltering(3) == 1);
    CHECK(validTextureFiltering(32) == 1);
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

TEST_CASE("alpha-to-coverage is limited to opted-in multisampled solid cutouts",
          "[render][types][alpha-coverage]") {
    test::FakeTexture mask(1, 1);
    DrawState state;
    state.alphaTest = DrawState::kTranslucentAlphaTest;
    CHECK_FALSE(state.usesAlphaToCoverage(4)); // UI defaults retain ordinary alpha blending.
    state.alphaToCoverage = true;
    CHECK_FALSE(state.usesAlphaToCoverage(1));
    CHECK(state.usesAlphaToCoverage(2));
    CHECK(state.usesAlphaToCoverage(4));
    SECTION("additive effects") {
        state.blend = BlendMode::Additive;
    }
    SECTION("translucent effects and fading models") {
        state.depthWrite = false;
    }
    SECTION("depthless overlays") {
        state.depthTest = false;
    }
    SECTION("solid textures without an alpha test") {
        state.alphaTest = 0;
    }
    SECTION("interpolated flipbooks") {
        state.nextTexture = &mask;
        state.textureBlend = 0.5f;
    }
    SECTION("masked skins retain their original coverage") {
        state.alphaTest = 0;
        state.maskedTexture = &mask;
        state.blend = BlendMode::Opaque;
        CHECK(state.usesAlphaToCoverage(4));
        state.alphaToCoverage = false;
    }
    CHECK_FALSE(state.usesAlphaToCoverage(4));
}

} // namespace
