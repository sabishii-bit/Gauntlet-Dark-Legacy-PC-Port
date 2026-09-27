#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "TestSupport.h"
#include "game/players/PlayerImpact.h"
#include "game/world/HazardSurfaces.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& normal,
                           s32 object) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = normal;
    out.object = object;
    return out;
}

TEST_CASE("a surface's flags name what it does to a body", "[game][world][hazards]") {
    CHECK_FALSE(HazardSurfaces::harmOf(0));
    CHECK_FALSE(HazardSurfaces::harmOf(0x1004));
    const auto burn = HazardSurfaces::harmOf(0x10000);
    REQUIRE(burn);
    CHECK(burn->damage == 5.0f);
    CHECK(burn->impact == 0);
    CHECK_FALSE(burn->jolts);
    const auto knock = HazardSurfaces::harmOf(0x20000);
    REQUIRE(knock);
    CHECK(knock->damage == 10.0f);
    CHECK(knock->impact == PlayerImpact::kKnockBack);
    for (const u32 kind : {0x30000U, 0x40000U, 0x50000U}) {
        const auto fell = HazardSurfaces::harmOf(kind);
        REQUIRE(fell);
        CHECK(fell->damage == 15.0f);
        CHECK(fell->impact == PlayerImpact::kKnockDown);
        CHECK(fell->jolts);
    }
    CHECK(HazardSurfaces::harmOf(0x60000)->damage == 5.0f);
    // Enemies take the knocking kind as a burn with a knock, and nothing from the sixth.
    CHECK(HazardSurfaces::enemyHarmOf(0x10000)->damage == 5.0f);
    CHECK(HazardSurfaces::enemyHarmOf(0x20000)->damage == 5.0f);
    CHECK(HazardSurfaces::enemyHarmOf(0x20000)->impact == PlayerImpact::kKnockBack);
    CHECK(HazardSurfaces::enemyHarmOf(0x40000)->damage == 15.0f);
    CHECK_FALSE(HazardSurfaces::enemyHarmOf(0x60000));
    // What a trigger drives hurts only when it is also marked to.
    CHECK_FALSE(HazardSurfaces::harmOf(0x30000 | HazardSurfaces::kTriggered));
    CHECK(HazardSurfaces::harmOf(0x30000 | HazardSurfaces::kTriggered |
                                 HazardSurfaces::kHarmsWhenTriggered));
}

TEST_CASE("a body is hurt by a harmful wall it is against or a harmful floor it stands on",
          "[game][world][hazards]") {
    const auto dir = test::scratchDirectory("hazard-surfaces");
    // A harmless floor (0), a group that knocks back (1) whose wall (2) inherits it, and a
    // burning floor (3).
    writeTextFile(dir / "world.json", R"({
  "objects": [
    {"name": "FLOOR", "position": [0, 0, 0], "flags": 4, "next": 1, "child": -1},
    {"name": "ROLLERS", "position": [0, 0, 0], "flags": 131072, "next": 3, "child": 2},
    {"name": "ROLLER", "position": [0, 0, 0], "flags": 0, "next": -1, "child": -1},
    {"name": "EMBERS", "position": [0, 0, 0], "flags": 65540, "next": -1, "child": -1}
  ],
  "animations": [], "particles": [], "locators": [], "itemInfos": [], "itemInstances": []
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    HazardSurfaces hazards;
    hazards.bind(layout);
    CHECK_FALSE(hazards.harmOfObject(0));
    REQUIRE(hazards.harmOfObject(2));
    CHECK(hazards.harmOfObject(2)->impact == PlayerImpact::kKnockBack);
    CHECK(hazards.harmful() == 3);

    const Vec3 up{0.0f, 1.0f, 0.0f};
    WorldCollision collision;
    collision.build({
        triangle({-20, 0, -20}, {0, 0, 20}, {0, 0, -20}, up, 0),
        triangle({-20, 0, -20}, {-20, 0, 20}, {0, 0, 20}, up, 0),
        triangle({0, 0, -20}, {0, 0, 20}, {20, 0, 20}, up, 3),
        triangle({0, 0, -20}, {20, 0, 20}, {20, 0, -20}, up, 3),
        // A wall along x at z 10, facing back towards -z.
        triangle({-20, 0, 10}, {-5, 8, 10}, {-5, 0, 10}, {0, 0, -1}, 2),
        triangle({-20, 0, 10}, {-20, 8, 10}, {-5, 8, 10}, {0, 0, -1}, 2),
    });
    // Out in the open on the harmless floor: nothing.
    CHECK_FALSE(hazards.touching(collision, Vec3{-10, 0, 0}, 0.75f, 5.0f));
    // Against the roller wall: knocked back, away from it.
    const auto wall = hazards.touching(collision, Vec3{-10, 0, 9.3f}, 0.75f, 5.0f);
    REQUIRE(wall);
    CHECK(wall->object == 2);
    CHECK(wall->harm.damage == 10.0f);
    CHECK(wall->away.z == Approx(-1.0f));
    // On the embers: burned, with nowhere in particular to be thrown.
    const auto floor = hazards.touching(collision, Vec3{10, 0, 0}, 0.75f, 5.0f);
    REQUIRE(floor);
    CHECK(floor->object == 3);
    CHECK(floor->harm.damage == 5.0f);
    CHECK(floor->away == Vec3{0.0f});
}

TEST_CASE("the fields keep their harmful floors and rollers", "[game][world][hazards][unpacked]") {
    const auto file = test::unpackedOrSkip("LEVELS/LEVELG1/world.json");
    WorldLayout layout;
    REQUIRE(layout.load(file.parent_path()));
    HazardSurfaces hazards;
    hazards.bind(layout);
    CHECK(hazards.harmful() == 16); // eight that burn, eight that fell
}

} // namespace
