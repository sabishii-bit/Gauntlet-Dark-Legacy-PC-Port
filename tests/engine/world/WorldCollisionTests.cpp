#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
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
    // A footprint touching the edge can approach it, never walk away from it.
    const auto edge = collision.floorAt({10.4f, 1, 0}, 2, 3, 0.75f);
    REQUIRE(edge);
    CHECK_FALSE(collision.floorAt({10.8f, 1, 0}, 2, 3, 0.75f));
    CHECK(collision.floorAhead({10.4f, 1, 0}, {-0.1f, 0, 0}, 2, 3, 0.75f));
    CHECK_FALSE(collision.floorAhead({10.4f, 1, 0}, {0.1f, 0, 0}, 2, 3, 0.75f));
    CHECK_FALSE(collision.floorAhead({10.4f, 1, 0}, {0, 0, 0}, 2, 3, 0.75f));

    REQUIRE_FALSE(collision.floorAt(Vec3{0.0f, 10.0f, 0.0f}, 2.0f, 3.0f).has_value());
    REQUIRE_FALSE(collision.floorAt(Vec3{30.0f, 1.0f, 0.0f}, 2.0f, 3.0f).has_value());
    // Walls are never floors, even straight above their edge.
    REQUIRE(collision.floorAt(Vec3{5.0f, 1.0f, 0.0f}, 2.0f, 3.0f)->y == Approx(0.0f));
}

TEST_CASE("hazard contact covers caps slopes and the full body height without blocking",
          "[world][collision][contact-only]") {
    WorldCollision collision;
    collision.build({triangle({-2, 0.1f, -2}, {2, 0.1f, -2}, {0, 0.1f, 2}, {0, 1, 0}, 0),
                     triangle({-2, 5.9f, -2}, {0, 5.9f, 2}, {2, 5.9f, -2}, {0, -1, 0}, 1)});
    collision.setContactOnly(0, true);
    collision.setContactOnly(1, true);
    const auto contacts = collision.surfaceContacts({0, 0, 0}, 0.5f, 0, 6);
    CHECK(contacts.size() == 2);
    CHECK(collision.surfaceContacts({0, 0, 0}, 0.5f, 0.2f, 5.8f).empty());
    CHECK(collision.surfaceContacts({4, 0, 0}, 0.5f, 0, 6).empty());
    // Vertical edges and degenerate projections must not become infinite planes.
    collision.build({triangle({0, 0, -1}, {0, 4, -1}, {0, 0, 1}, {1, 0, 0})});
    CHECK(collision.surfaceContacts({0.4f, 0, 0}, 0.5f, 0, 6).size() == 1);
    CHECK(collision.surfaceContacts({0, 0, 4}, 0.5f, 0, 6).empty());
    CHECK(collision.surfaceContacts({0.6f, 0, 0}, 0.5f, 0, 6).empty());
    collision.setSolid(0, false);
    CHECK(collision.surfaceContacts({0, 0, 0}, 0.5f, 0, 6).empty());
}

TEST_CASE("contact-only walls retain contacts without blocking and leave other walls solid",
          "[world][collision][contact-only]") {
    auto triangles = room();
    for (auto surface : room()) {
        if (surface.object != 1) {
            continue;
        }
        surface.object = 2;
        for (Vec3& vertex : surface.vertices) {
            vertex.x += 3;
        }
        triangles.push_back(surface);
    }
    WorldCollision collision;
    collision.build(std::move(triangles));
    collision.setContactOnly(1, true);
    collision.setContactOnly(1, true);
    CHECK(collision.contactOnly(1));
    CHECK_FALSE(collision.contactOnly(2));
    std::vector<WallContact> contacts;
    const Vec3 position{4.8f, 0, 0};
    CHECK(collision.resolveWalls(position, 0.5f, 0.2f, 2.8f, &contacts) == position);
    REQUIRE(contacts.size() == 1);
    CHECK(contacts.front().object == 1);
    CHECK(collision.sweepWalls({4, 0, 0}, {6, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(6));
    CHECK(collision.sweepWalls({4, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(7.5f));
    collision.setSolid(1, false);
    contacts.clear();
    collision.resolveWalls(position, 0.5f, 0.2f, 2.8f, &contacts);
    CHECK(contacts.empty());
    collision.setSolid(1, true);
    collision.setContactOnly(1, false);
    CHECK(collision.resolveWalls(position, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    collision.setContactOnly(1, true);
    collision.clear();
    CHECK_FALSE(collision.contactOnly(1));
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

TEST_CASE("wall sweeps stop thin-wall crossings and preserve the starting side",
          "[world][collision][wall-sweep]") {
    WorldCollision collision;
    auto triangles = room();
    triangles.push_back(triangle({5.1f, 0, -10}, {5.1f, 3, 10}, {5.1f, 3, -10}, {1, 0, 0}, 2));
    triangles.push_back(triangle({5.1f, 0, -10}, {5.1f, 0, 10}, {5.1f, 3, 10}, {1, 0, 0}, 2));
    collision.build(triangles);
    const Vec3 stopped = collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f);
    CHECK(stopped.x == Approx(4.5f));
    const Vec3 back = collision.sweepWalls({9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f);
    CHECK(back.x == Approx(5.6f));
    const Vec3 slide = collision.sweepWalls({0, 0, 0}, {9, 0, 3}, 0.5f, 0.2f, 2.8f);
    CHECK(slide.x == Approx(4.5f));
    CHECK(slide.z == Approx(3));
    // Contact and initial overlap permit separation, not travel through the wall.
    CHECK(collision.sweepWalls({4.5f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == 0);
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.8f));
    CHECK(collision.sweepWalls({4.9999f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.9999f));
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == 0);
    CHECK(collision.sweepWalls({5, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == 5);
    CHECK(collision.sweepWalls({0, 0, 10.6f}, {9, 0, 10.6f}, 0.5f, 0.2f, 2.8f).x == 9);
    const Vec3 rounded = collision.sweepWalls({0, 0, 10.4f}, {9, 0, 10.4f}, 0.5f, 0.2f, 2.8f);
    CHECK(rounded.z > 10.4f);
    CHECK(collision.sweepWalls({0, 4, 0}, {9, 4, 0}, 0.5f, 4.2f, 6).x == 9);
    collision.setSolid(1, false);
    collision.setSolid(2, false);
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == 9);
    collision.setSolid(1, true);
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::translate(Mat4{1}, Vec3{-2, 0, 0}));
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(2.5f));
    auto corner = room();
    corner.push_back(triangle({-10, 0, 5}, {10, 3, 5}, {10, 0, 5}, {0, 0, -1}));
    corner.push_back(triangle({-10, 0, 5}, {-10, 3, 5}, {10, 3, 5}, {0, 0, -1}));
    collision.build(corner);
    const Vec3 stoppedCorner = collision.sweepWalls({0, 0, 0}, {9, 0, 9}, 0.5f, 0.2f, 2.8f);
    CHECK(stoppedCorner.x == Approx(4.5f));
    CHECK(stoppedCorner.z == Approx(4.5f));
}

TEST_CASE("wall sweeps respect the authored front face instead of sealing one-way entrances",
          "[world][collision][wall-sweep][wall-facing]") {
    WorldCollision collision;
    collision.build(room()); // the face at x=5 points toward -x
    CHECK(collision.sweepWalls({0, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    CHECK(collision.sweepWalls({9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
    // Starting within the radius on the back side must still permit entry.
    CHECK(collision.sweepWalls({5.2f, 0, 0}, {4, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(4));
    CHECK(collision.sweepWalls({5.2f, 0, 0}, {9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(9));
    CHECK(collision.sweepWalls({4.8f, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
    // Moving faces use their transformed normals, not the local face direction.
    collision.setMovingObjects(std::array<s32, 1>{1});
    collision.setObjectTransform(1, glm::rotate(Mat4{1}, kPi, Vec3{0, 1, 0}));
    CHECK(collision.sweepWalls({0, 0, 0}, {-9, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(-4.5f));
    CHECK(collision.sweepWalls({-9, 0, 0}, {0, 0, 0}, 0.5f, 0.2f, 2.8f).x == Approx(0));
}

TEST_CASE("floor edges retain tangential travel without bridging disconnected floors",
          "[world][collision][cliff]") {
    WorldCollision collision;
    const auto patch = [](f32 left, f32 right, f32 back, f32 front) {
        return std::vector<CollisionTriangle>{
            triangle({left, 0, back}, {right, 0, back}, {right, 0, front}, {0, 1, 0}),
            triangle({left, 0, back}, {right, 0, front}, {left, 0, front}, {0, 1, 0})};
    };
    auto floor = patch(-10, 0, -10, 10);
    collision.build(floor);
    const auto slide = collision.slideAlongFloor({-0.1f, 0, 0}, {0.2f, 0, 0.4f}, 1.5f, 3, 0.01f);
    REQUIRE(slide);
    CHECK(slide->x == Approx(0));
    CHECK(slide->z == Approx(0.4f));
    const auto stopped = collision.slideAlongFloor({-0.1f, 0, 0}, {0.2f, 0, 0}, 1.5f, 3, 0.01f);
    REQUIRE(stopped);
    CHECK(*stopped == Vec3{0, 0, 0});
    // The nearer destination floor is separated by a gap. Sliding must stay
    // on the starting floor instead of snapping across to the other island.
    auto island = patch(0.15f, 0.4f, 0.2f, 0.6f);
    floor.insert(floor.end(), island.begin(), island.end());
    collision.build(floor);
    const auto connected =
        collision.slideAlongFloor({-0.1f, 0, 0}, {0.45f, 0, 0.4f}, 1.5f, 3, 0.01f);
    REQUIRE(connected);
    CHECK(connected->x == Approx(0));
    CHECK(connected->z == Approx(0.4f));
    collision.clear();
    CHECK_FALSE(collision.slideAlongFloor({0, 0, 0}, {0.2f, 0, 0.4f}, 1.5f, 3));
}

TEST_CASE("world query flags distinguish walkable surfaces from wall-only geometry",
          "[world][collision]") {
    auto floor = triangle({-10, -5, -10}, {10, -5, -10}, {0, -5, 10}, {0, 1, 0});
    auto wall = triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, {-1, 0, 0});
    WorldCollision collision;
    // Temple's E1#32 points upward but has only flag 2. Normal direction alone must
    // not turn it into a floor beneath holes in the walkable mesh or beyond the stairs.
    for (const u32 flags : {0U, 2U, 0x800U}) {
        CAPTURE(flags);
        floor.objectFlags = flags;
        collision.build({floor});
        CHECK_FALSE(collision.floorAt({0, 0, 0}, 6, 6));
    }
    for (const u32 flags : {4U, 8U, 0x10U, 0x20U}) {
        CAPTURE(flags);
        floor.objectFlags = flags;
        collision.build({floor});
        REQUIRE(collision.floorAt({0, 0, 0}, 6, 6));
        CHECK(collision.floorAt({0, 0, 0}, 6, 6)->y == Approx(-5));
    }
    const Vec3 centre{4.8f, 0, 1};
    wall.objectFlags = 4; // a floor-only object is not a horizontal blocker
    collision.build({wall});
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f) == centre);
    wall.objectFlags = 2;
    collision.build({wall});
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f).x == Approx(4.5f));
    // Moving geometry keeps the same query policy after transformation.
    floor.objectFlags = 2;
    floor.object = 3;
    collision.build({floor});
    collision.setMovingObjects(std::array<s32, 1>{3});
    collision.setObjectTransform(3, glm::translate(Mat4{1}, Vec3{0, 5, 0}));
    CHECK_FALSE(collision.floorAt({0, 0, 0}, 6, 6));
}

TEST_CASE("water has its own surface without replacing the solid floor", "[world][collision]") {
    auto floor = triangle({-10, -2, -10}, {10, -2, -10}, {0, -2, 10}, {0, 1, 0}, 1);
    auto water = triangle({-10, 1, -10}, {10, 1, -10}, {0, 1, 10}, {0, 1, 0}, 2);
    // Mixed solid flags do not bypass the secondary-channel rule either.
    for (const auto flags : {0x200U, 0x206U}) {
        water.objectFlags = flags;
        WorldCollision collision;
        collision.build({floor, water});
        const auto solid = collision.floorAt(Vec3{0}, 5, 5);
        const auto surface = collision.liquidAt(Vec3{0}, 5, 5);
        REQUIRE(solid);
        REQUIRE(surface);
        CHECK(solid->object == 1);
        CHECK(solid->y == -2);
        CHECK(surface->object == 2);
        CHECK(surface->y == 1);
        CHECK_FALSE(collision.liquidAt(Vec3{0}, 0.5f, 0.5f));
        collision.setSolid(2, false);
        CHECK_FALSE(collision.liquidAt(Vec3{0}, 5, 5));
        collision.setSolid(2, true);
        collision.setMovingObjects(std::array<s32, 1>{2});
        collision.setObjectTransform(2, glm::translate(Mat4{1}, Vec3{0, 2, 0}));
        REQUIRE(collision.liquidAt(Vec3{0}, 5, 5));
        CHECK(collision.liquidAt(Vec3{0}, 5, 5)->y == 3);
        CHECK(collision.floorAt(Vec3{0}, 5, 5)->y == -2);
    }
    WorldCollision collision;
    collision.build({water});
    CHECK_FALSE(collision.floorAt(Vec3{0}, 5, 5));
    auto wall = triangle({5, 0, -10}, {5, 3, -10}, {5, 3, 10}, {-1, 0, 0}, 3);
    wall.objectFlags = 0x202;
    collision.build({wall});
    const Vec3 centre{4.8f, 0, 1};
    CHECK(collision.resolveWalls(centre, 0.5f, 0.2f, 2.8f) == centre);
}

TEST_CASE("a moving object's triangles stay in its own space and follow its transform",
          "[world][collision]") {
    WorldCollision collision;
    std::vector<CollisionTriangle> triangles = room();
    // A square platform half a unit up in object 3's space, and a pane along its front.
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 south{0.0f, 0.0f, 1.0f};
    triangles.push_back(triangle({-1, 0.5f, -1}, {1, 0.5f, -1}, {1, 0.5f, 1}, up, 3));
    triangles.back().objectFlags = WorldObject::kFloor;
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
    REQUIRE(hit->objectFlags == WorldObject::kFloor);
    // Placed, it is found there, and nowhere else.
    collision.setObjectTransform(3, glm::translate(Mat4{1.0f}, Vec3{-5.0f, 2.0f, -5.0f}));
    hit = collision.floorAt(Vec3{-5.0f, 3.0f, -5.0f}, 1.0f, 1.0f);
    REQUIRE(hit.has_value());
    REQUIRE(hit->object == 3);
    REQUIRE(hit->y == Approx(2.5f));
    REQUIRE(hit->objectFlags == WorldObject::kFloor);
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
    // Where a moving object stands can be asked; a still one has no placement.
    const auto placed = collision.objectTransform(3);
    REQUIRE(placed.has_value());
    REQUIRE(Vec3{(*placed)[3]} == Vec3{-5.0f, 2.0f, -5.0f});
    REQUIRE_FALSE(collision.objectTransform(1).has_value());
    REQUIRE_FALSE(collision.objectTransform(9).has_value());
    collision.clear();
    REQUIRE(collision.movingObjectCount() == 0);
}

TEST_CASE("collision files load their world-space triangles, skipping decoration",
          "[world][collision]") {
    const auto dir = test::scratchDirectory("world-collision");
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "ROOM", "position": [10, 0, 0], "next": -1, "child": 1},
    {"name": "FLOOR", "position": [0, 2, 0], "flags": 4, "next": 2, "child": -1},
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
    REQUIRE(hit->objectFlags == WorldObject::kFloor);

    writeTextFile(dir / "collision.json",
                  R"({"objects": [{"object": 7, "normals": [], "vertices": []}]})");
    REQUIRE_FALSE(collision.load(dir, layout));
    REQUIRE_FALSE(collision.loaded());
    REQUIRE_FALSE(collision.load(test::scratchDirectory("world-collision-none"), layout));
}

TEST_CASE("floor contact margins stay bounded and do not change projectile or liquid probes",
          "[world][collision][terrain-seams]") {
    WorldCollision collision;
    // Ends at a grid boundary; nearby contact must also search the adjacent cell.
    collision.build({triangle({0, 0, 0}, {8, 0, 0}, {8, 0, 8}, {0, 1, 0}),
                     triangle({0, 0, 0}, {8, 0, 8}, {0, 0, 8}, {0, 1, 0}),
                     triangle({16, 2, 0}, {20, 2, 0}, {20, 2, 8}, {0, 1, 0})});
    const Vec3 nearEdge{8.01f, 0, 4};
    REQUIRE_FALSE(collision.floorAt(nearEdge, 1, 1));
    REQUIRE(collision.floorAt(nearEdge, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt({8.04f, 0, 4}, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt({8.01f, 2, 4}, 1, 1, 0.03125f));
    REQUIRE_FALSE(collision.floorAt(nearEdge, 1, 1, -1));
    REQUIRE_FALSE(collision.projectileFloorAt(nearEdge, 1, 1));
    REQUIRE_FALSE(collision.liquidAt(nearEdge, 1, 1));
    // Corner distance is radial, not an expanded bounding rectangle.
    REQUIRE_FALSE(collision.floorAt({8.025f, 0, 8.025f}, 1, 1, 0.03125f));
    // A floor on a moving platform uses the same contact test after its placement.
    collision.setMovingObjects(std::array<s32, 1>{0});
    collision.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, 2, 0}));
    const auto moved = collision.floorAt({8.01f, 2, 4}, 0.5f, 0.5f, 0.03125f);
    REQUIRE(moved);
    REQUIRE(moved->y == Approx(2));
}

TEST_CASE("the native tower has floors under its start points", "[world][collision][assets]") {
    const std::filesystem::path dir = test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path();
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
