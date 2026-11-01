#include <array>
#include <optional>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Mesh.h"

#include "game/scenes/ScenePicking.h"

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

namespace {

ScenePicking::Ray forward(const Vec3& origin = Vec3{0.0f}) {
    return {origin, Vec3{0.0f, 0.0f, 1.0f}};
}

/** A clip looking down +Z from the origin, for the cursor ray. */
Mat4 lookDownZ() {
    const Mat4 view = glm::lookAt(Vec3{0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 1.0f, 0.0f});
    return glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f) * view;
}

} // namespace

TEST_CASE("the cursor ray leaves the eye through the view and refuses the outside",
          "[scenes][picking]") {
    const Mat4 clip = lookDownZ();
    const auto centre = ScenePicking::cursorRay(Vec2{0.5f, 0.5f}, clip, Vec3{0.0f});
    REQUIRE(centre.has_value());
    CHECK(glm::length(centre->direction) == Approx(1.0f));
    // The view's middle looks straight along the camera's way.
    CHECK(centre->direction.z == Approx(1.0f).margin(1e-4));
    CHECK(centre->origin == Vec3{0.0f});
    // A cursor off the middle leans its way: looking down +Z, the view's left
    // is the world's +X (the basis is right-handed). Outside the view, nothing.
    const auto left = ScenePicking::cursorRay(Vec2{0.0f, 0.5f}, clip, Vec3{0.0f});
    REQUIRE(left.has_value());
    CHECK(left->direction.x > 0.1f);
    CHECK_FALSE(ScenePicking::cursorRay(Vec2{-0.1f, 0.5f}, clip, Vec3{0.0f}).has_value());
    CHECK_FALSE(ScenePicking::cursorRay(Vec2{0.5f, 1.5f}, clip, Vec3{0.0f}).has_value());
}

TEST_CASE("a sphere is entered where the ray meets it and from inside at once",
          "[scenes][picking]") {
    const auto hit = ScenePicking::sphereHit(forward(), Vec3{0.0f, 0.0f, 10.0f}, 2.0f);
    REQUIRE(hit.has_value());
    CHECK(*hit == Approx(8.0f));
    // From inside, the pick is where the eye stands; behind the eye, nothing.
    const auto inside =
        ScenePicking::sphereHit(forward(Vec3{0.0f, 0.0f, 9.5f}), Vec3{0.0f, 0.0f, 10.0f}, 2.0f);
    REQUIRE(inside.has_value());
    CHECK(*inside == Approx(0.0f));
    CHECK_FALSE(ScenePicking::sphereHit(forward(), Vec3{0.0f, 0.0f, -10.0f}, 2.0f).has_value());
    CHECK_FALSE(ScenePicking::sphereHit(forward(), Vec3{5.0f, 0.0f, 10.0f}, 2.0f).has_value());
}

TEST_CASE("a triangle is met on either face and missed past its edges", "[scenes][picking]") {
    const Vec3 a{-1.0f, -1.0f, 5.0f};
    const Vec3 b{1.0f, -1.0f, 5.0f};
    const Vec3 c{0.0f, 1.0f, 5.0f};
    const auto front = ScenePicking::triangleHit(forward(), a, b, c);
    REQUIRE(front.has_value());
    CHECK(*front == Approx(5.0f));
    // The same face met from behind still counts: picking has no winding.
    const auto back = ScenePicking::triangleHit(forward(), a, c, b);
    REQUIRE(back.has_value());
    CHECK(*back == Approx(5.0f));
    const auto miss =
        ScenePicking::triangleHit({Vec3{2.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}}, a, b, c);
    CHECK_FALSE(miss.has_value());
    // A ray lying in the triangle's own plane is parallel: no hit.
    const auto parallel =
        ScenePicking::triangleHit({Vec3{-2.0f, 0.0f, 5.0f}, Vec3{1.0f, 0.0f, 0.0f}}, a, b, c);
    CHECK_FALSE(parallel.has_value());
}

TEST_CASE("the nearest of a mesh's triangles wins and its placement moves it",
          "[scenes][picking]") {
    Mesh mesh;
    mesh.vertices.resize(6);
    mesh.vertices[0].position = {-1.0f, -1.0f, 6.0f};
    mesh.vertices[1].position = {1.0f, -1.0f, 6.0f};
    mesh.vertices[2].position = {0.0f, 1.0f, 6.0f};
    mesh.vertices[3].position = {-1.0f, -1.0f, 3.0f};
    mesh.vertices[4].position = {1.0f, -1.0f, 3.0f};
    mesh.vertices[5].position = {0.0f, 1.0f, 3.0f};
    MeshPart part;
    part.indices = {0, 1, 2, 3, 4, 5};
    mesh.parts.push_back(part);
    const auto nearest = ScenePicking::meshHit(forward(), mesh, Mat4{1.0f});
    REQUIRE(nearest.has_value());
    CHECK(*nearest == Approx(3.0f));
    // Pushed five further out, the same mesh is met five further along.
    const Mat4 moved = glm::translate(Mat4{1.0f}, Vec3{0.0f, 0.0f, 5.0f});
    const auto pushed = ScenePicking::meshHit(forward(), mesh, moved);
    REQUIRE(pushed.has_value());
    CHECK(*pushed == Approx(8.0f));
    const auto missed =
        ScenePicking::meshHit({Vec3{10.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}}, mesh, Mat4{1.0f});
    CHECK_FALSE(missed.has_value());
}

TEST_CASE("the selection is held and handed back", "[scenes][picking]") {
    ScenePicking picking;
    CHECK_FALSE(picking.selection().has_value());
    picking.select(SceneSelection{SceneSelection::Kind::Enemy, 7, "GRUNT"});
    REQUIRE(picking.selection().has_value());
    CHECK(picking.selection()->token() == "enemy#7");
    picking.select(std::nullopt);
    CHECK_FALSE(picking.selection().has_value());
}
