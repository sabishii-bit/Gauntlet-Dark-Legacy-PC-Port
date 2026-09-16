#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/render/ImmediateBatch.h"

namespace {

using namespace gdl;

ImmediateVertex vertexAt(f32 x) {
    return ImmediateVertex{Vec3{x, 0.0f, 0.0f}, Color::white(), {}};
}

std::vector<f32> xs(const ImmediateBatch& batch) {
    std::vector<f32> result;
    for (const ImmediateVertex& v : batch.triangles()) {
        result.push_back(v.position.x);
    }
    return result;
}

void feed(ImmediateBatch& batch, PrimitiveTopology topology, int count) {
    batch.begin(topology);
    for (int i = 0; i < count; ++i) {
        batch.vertex(vertexAt(static_cast<f32>(i)));
    }
    batch.end();
}

TEST_CASE("a fresh batch is empty and clear resets it", "[render][batch]") {
    ImmediateBatch batch;
    REQUIRE(batch.empty());
    feed(batch, PrimitiveTopology::TriangleList, 3);
    REQUIRE_FALSE(batch.empty());
    batch.clear();
    REQUIRE(batch.empty());
    REQUIRE(batch.vertexCount() == 0);
}

TEST_CASE("triangle lists keep whole triangles only", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::TriangleList, 7);
    REQUIRE(xs(batch) == std::vector<f32>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("triangle strips alternate winding", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::TriangleStrip, 5);
    REQUIRE(xs(batch) == std::vector<f32>{0, 1, 2, 2, 1, 3, 2, 3, 4});
}

TEST_CASE("triangle fans pivot on the first vertex", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::TriangleFan, 5);
    REQUIRE(xs(batch) == std::vector<f32>{0, 1, 2, 0, 2, 3, 0, 3, 4});
}

TEST_CASE("quads become two triangles each", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::QuadList, 9);
    REQUIRE(xs(batch) == std::vector<f32>{0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7});
}

TEST_CASE("degenerate primitives produce nothing", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::TriangleList, 2);
    feed(batch, PrimitiveTopology::TriangleStrip, 2);
    feed(batch, PrimitiveTopology::TriangleFan, 1);
    feed(batch, PrimitiveTopology::QuadList, 3);
    REQUIRE(batch.empty());
}

TEST_CASE("primitives accumulate across begin/end pairs", "[render][batch]") {
    ImmediateBatch batch;
    feed(batch, PrimitiveTopology::TriangleList, 3);
    feed(batch, PrimitiveTopology::QuadList, 4);
    REQUIRE(batch.vertexCount() == 9);
}

TEST_CASE("rect emits a quad with matching uv corners", "[render][batch]") {
    ImmediateBatch batch;
    const Color tint = Color::rgba(10, 20, 30, 40);
    batch.rect(Rect{10.0f, 20.0f, 100.0f, 50.0f}, 0.75f, tint, Rect{0.25f, 0.5f, 0.25f, 0.25f});

    const auto triangles = batch.triangles();
    REQUIRE(triangles.size() == 6);
    REQUIRE(triangles[0] == ImmediateVertex{Vec3{10.0f, 20.0f, 0.75f}, tint, Vec2{0.25f, 0.5f}});
    REQUIRE(triangles[1] == ImmediateVertex{Vec3{110.0f, 20.0f, 0.75f}, tint, Vec2{0.5f, 0.5f}});
    REQUIRE(triangles[2] == ImmediateVertex{Vec3{110.0f, 70.0f, 0.75f}, tint, Vec2{0.5f, 0.75f}});
    REQUIRE(triangles[3] == triangles[0]);
    REQUIRE(triangles[4] == triangles[2]);
    REQUIRE(triangles[5] == ImmediateVertex{Vec3{10.0f, 70.0f, 0.75f}, tint, Vec2{0.25f, 0.75f}});
}

} // namespace
