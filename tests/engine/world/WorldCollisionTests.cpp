#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                           s32 object = 0) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = object;
    return out;
}

/** A 20x20 floor at y = 0 with a wall along x = 5 facing -x, three units tall. */
std::vector<CollisionTriangle> room() {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 west{-1.0f, 0.0f, 0.0f};
    return {
        triangle({-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, up),
        triangle({-10, 0, -10}, {10, 0, 10}, {-10, 0, 10}, up),
        triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, west, 1),
        triangle({5, 0, -10}, {5, 3, 10}, {5, 0, 10}, west, 1),
    };
}

TEST_CASE("the floor under a point is found within the probe range", "[world][collision]") {
    WorldCollision collision;
    REQUIRE_FALSE(collision.loaded());
    collision.build(room());
    REQUIRE(collision.triangleCount() == 4);

    const auto hit = collision.floorAt(Vec3{0.0f, 1.0f, 0.0f}, 2.0f, 3.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->y == Approx(0.0f));
    REQUIRE(hit->normal.y == 1.0f);
    REQUIRE(hit->object == 0);

    REQUIRE_FALSE(collision.floorAt(Vec3{0.0f, 10.0f, 0.0f}, 2.0f, 3.0f).has_value());
    REQUIRE_FALSE(collision.floorAt(Vec3{30.0f, 1.0f, 0.0f}, 2.0f, 3.0f).has_value());
    // Walls are never floors, even straight above their edge.
    REQUIRE(collision.floorAt(Vec3{5.0f, 1.0f, 0.0f}, 2.0f, 3.0f)->y == Approx(0.0f));
}

TEST_CASE("a cylinder is pushed out of walls but left alone elsewhere", "[world][collision]") {
    WorldCollision collision;
    collision.build(room());
    const Vec3 clear = collision.resolveWalls(Vec3{0.0f, 0.0f, 0.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(clear == Vec3{0.0f, 0.0f, 0.0f});

    const Vec3 pushed = collision.resolveWalls(Vec3{4.8f, 0.0f, 1.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(pushed.x == Approx(4.5f).margin(0.001f));
    REQUIRE(pushed.z == Approx(1.0f));

    // Behind the wall it comes back out to the front.
    const Vec3 behind = collision.resolveWalls(Vec3{5.2f, 0.0f, 0.0f}, 0.5f, 0.2f, 2.8f);
    REQUIRE(behind.x == Approx(4.5f).margin(0.001f));

    // Above the wall's top there is nothing to hit.
    const Vec3 over = collision.resolveWalls(Vec3{4.8f, 4.0f, 0.0f}, 0.5f, 4.2f, 6.0f);
    REQUIRE(over.x == Approx(4.8f));
}

TEST_CASE("a moving object's triangles stay in its own space and follow its transform",
          "[world][collision]") {
    WorldCollision collision;
    std::vector<CollisionTriangle> triangles = room();
    // A square platform half a unit up in object 3's space, and a pane along its front.
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 south{0.0f, 0.0f, 1.0f};
    triangles.push_back(triangle({-1, 0.5f, -1}, {1, 0.5f, -1}, {1, 0.5f, 1}, up, 3));
    triangles.push_back(triangle({-1, 0.5f, -1}, {1, 0.5f, 1}, {-1, 0.5f, 1}, up, 3));
    triangles.push_back(triangle({-1, 0.5f, 1}, {-1, 3, 1}, {1, 3, 1}, south, 3));
    triangles.push_back(triangle({-1, 0.5f, 1}, {1, 3, 1}, {1, 0.5f, 1}, south, 3));
    collision.build(triangles);
    const std::array<s32, 1> movers{3};
    collision.setMovingObjects(movers);
    REQUIRE(collision.moving(3));
    REQUIRE_FALSE(collision.moving(1));
    REQUIRE(collision.movingObjectCount() == 1);
    REQUIRE(collision.triangleCount() == 8);
    // Not yet placed, it sits where the file put it: over the room's floor at the origin.
    auto hit = collision.floorAt(Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 2.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 3);
    REQUIRE(hit->y == Approx(0.5f));
    // Placed, it is found there, and nowhere else.
    collision.setObjectTransform(3, glm::translate(Mat4{1.0f}, Vec3{-5.0f, 2.0f, -5.0f}));
    hit = collision.floorAt(Vec3{-5.0f, 3.0f, -5.0f}, 1.0f, 1.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 3);
    REQUIRE(hit->y == Approx(2.5f));
    hit = collision.floorAt(Vec3{0.0f, 0.5f, 0.0f}, 1.0f, 1.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 0); // the room's floor again
    // Its pane blocks until it is told not to.
    const Vec3 centre{-5.0f, 2.0f, -3.6f};
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) != centre);
    collision.setSolid(3, false);
    REQUIRE_FALSE(collision.solid(3));
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) == centre);
    collision.setSolid(3, true);
    REQUIRE(collision.solid(3));
    REQUIRE(collision.resolveWalls(centre, 0.5f, 2.2f, 3.0f) != centre);
    collision.setObjectTransform(9, Mat4{1.0f}); // not moving: ignored
    collision.clear();
    REQUIRE(collision.movingObjectCount() == 0);
}

TEST_CASE("collision files load their world-space triangles, skipping decoration",
          "[world][collision]") {
    const auto dir = test::scratchDirectory("world-collision");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "ROOM", "position": [10, 0, 0], "next": -1, "child": 1},
    {"name": "FLOOR", "position": [0, 2, 0], "next": 2, "child": -1},
    {"name": "DECOR", "position": [0, 0, 0], "next": -1, "child": -1, "noCollision": true}
  ],
  "locators": []
})");
    writeTextFile(dir / "collision.json", R"({
  "objects": [
    {"object": 1, "normals": [0, 1, 0], "vertices": [5, 2, -5, 15, 2, -5, 15, 2, 5]},
    {"object": 2, "normals": [0, 1, 0], "vertices": [-5, 0, -5, 5, 0, 5, -5, 0, 5]}
  ]
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    REQUIRE(layout.objects()[2].noCollision);
    WorldCollision collision;
    REQUIRE(collision.load(dir, layout));
    REQUIRE(collision.triangleCount() == 1); // the decoration's triangle is skipped
    const auto hit = collision.floorAt(Vec3{12.0f, 3.0f, -1.0f}, 2.0f, 3.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->y == Approx(2.0f)); // the file's coordinates, not offset by the object
    REQUIRE(hit->object == 1);

    writeTextFile(dir / "collision.json", R"({"objects": [{"object": 7, "normals": [], "vertices": []}]})");
    REQUIRE_FALSE(collision.load(dir, layout));
    REQUIRE_FALSE(collision.loaded());
    REQUIRE_FALSE(collision.load(test::scratchDirectory("world-collision-none"), layout));
}

TEST_CASE("the unpacked tower has floors under its start points", "[world][collision][unpacked]") {
    const std::filesystem::path dir = test::unpackedOrSkip("LEVELS/LEVELL1/collision.json").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    WorldCollision collision;
    REQUIRE(collision.load(dir, layout));
    REQUIRE(collision.triangleCount() > 5000);
    usize found = 0;
    for (const WorldLocator& locator : layout.locators()) {
        if (locator.kind != LocatorKind::Start) {
            continue;
        }
        const auto hit = collision.floorAt(locator.position, 3.0f, 3.0f);
        if (hit.has_value()) {
            ++found;
            REQUIRE(hit->y == Approx(locator.position.y).margin(1.5f));
        }
    }
    REQUIRE(found >= 10);
}

} // namespace
