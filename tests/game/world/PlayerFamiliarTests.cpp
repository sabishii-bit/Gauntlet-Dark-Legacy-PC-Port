#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/PlayerFamiliar.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("permanent familiar tiers are awarded at thirty and eighty", "[game][familiar]") {
    REQUIRE(PlayerFamiliar::tierFor(1) == 0);
    REQUIRE(PlayerFamiliar::tierFor(29) == 0);
    REQUIRE(PlayerFamiliar::tierFor(30) == 1);
    REQUIRE(PlayerFamiliar::tierFor(79) == 1);
    REQUIRE(PlayerFamiliar::tierFor(80) == 2);
    REQUIRE(PlayerFamiliar::tierFor(99) == 2);
    test::FakeRenderDevice device;
    PlayerFamiliar familiar;
    ItemArchive empty;
    REQUIRE_FALSE(familiar.bind(device, empty, 30, Vec3{0}));
    REQUIRE(familiar.tier() == 0);
    familiar.update(1, true);
    familiar.draw(device, Mat4{1}, Mat4{1}, WorldLighting{}, 1);
}

TEST_CASE("both permanent familiars bind the authored animated assets",
          "[game][familiar][unpacked]") {
    const auto path = test::unpackedOrSkip("PLAYERS/WAR/SFXYEL/animations.json").parent_path();
    test::FakeRenderDevice device;
    ItemArchive archive;
    REQUIRE(archive.load(path));
    PlayerFamiliar familiar;
    for (const s32 level : {30, 80}) {
        REQUIRE(familiar.bind(device, archive, level, Vec3{1, 5, -1}));
        REQUIRE(familiar.tier() == PlayerFamiliar::tierFor(level));
        for (s32 frame = 0; frame < 60; ++frame) {
            familiar.update(1.0f / 30.0f, frame == 20);
            familiar.draw(device, Mat4{1}, Mat4{1}, WorldLighting{}, 1);
        }
    }
    REQUIRE_FALSE(familiar.bind(device, archive, 29, Vec3{0}));
    REQUIRE(familiar.tier() == 0);
}
} // namespace
