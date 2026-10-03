#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/players/PowerupEffects.h"
#include "game/world/MikeyFigure.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Mikey's deployed native tree draws at the drop and retires with its controller",
          "[mikey][assets]") {
    const auto path = test::assetOrSkip("POWERUPS/ANIM.PS2").parent_path();
    ItemArchive archive;
    REQUIRE(archive.load(path));
    REQUIRE(archive.trees.find("MIKEYPUP_ON"));
    test::FakeRenderDevice device;
    Inventory inventory;
    inventory.addPowerup(powerup::kSpecial, powerup::kMikey, 0, -1);
    inventory.powerups[0].on = true;
    MikeyDecoy decoy;
    MikeyFigure figure;
    EffectTrees effects;
    decoy.update(1.0f / 30, inventory, Vec3{2, 3, 4});
    figure.update(device, archive, decoy, 1.0f / 30, effects);
    REQUIRE(figure.shown());
    figure.draw(device, Mat4{1}, {}, nullptr);
    REQUIRE_FALSE(device.draws.empty());
    for (s32 frame = 0; frame < 60; ++frame) {
        decoy.update(1.0f / 30, inventory, Vec3{50});
        figure.update(device, archive, decoy, 1.0f / 30, effects);
    }
    CHECK(decoy.position() == Vec3{2, 3, 4});
    CHECK(effects.count() == 5);
    decoy.clear();
    figure.update(device, archive, decoy, 0, effects);
    CHECK_FALSE(figure.shown());
    device.draws.clear();
    figure.draw(device, Mat4{1}, {}, nullptr);
    CHECK(device.draws.empty());
    effects.clear();
}
} // namespace
