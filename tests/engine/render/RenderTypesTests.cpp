#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "engine/render/RenderTypes.h"

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
    STATIC_REQUIRE(sizeof(ImmediateVertex) == 24);
    STATIC_REQUIRE(offsetof(ImmediateVertex, position) == 0);
    STATIC_REQUIRE(offsetof(ImmediateVertex, color) == 12);
    STATIC_REQUIRE(offsetof(ImmediateVertex, uv) == 16);
}

TEST_CASE("TextureDesc defaults to linear repeat sampling", "[render][types]") {
    constexpr TextureDesc kDesc{};
    STATIC_REQUIRE(kDesc.filter == TextureFilter::Linear);
    STATIC_REQUIRE(kDesc.wrap == TextureWrap::Repeat);
}

} // namespace
