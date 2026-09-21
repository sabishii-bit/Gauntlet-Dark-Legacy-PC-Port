#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/EffectTrees.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("an effect tree plays its sequence once where it was started, then goes",
          "[game][world][effects][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("WEAPONS/animations.json").parent_path().parent_path();
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    test::FakeRenderDevice device;
    EffectTrees effects;
    REQUIRE_FALSE(effects.start(device, weapons, "NO_SUCH_TREE", Vec3{0.0f}));
    REQUIRE(effects.start(device, weapons, "MP_FIRE", Vec3{5.0f, 1.0f, -3.0f}, 0.5f));
    REQUIRE(effects.count() == 1);
    REQUIRE(effects.effect(0).name == "MP_FIRE");
    REQUIRE(effects.effect(0).scale == 0.5f);
    // Thirty-seven frames of a fifteenth of a second: drawn meanwhile, gone in two and a half.
    bool drew = false;
    int steps = 0;
    while (effects.count() > 0 && steps < 240) {
        effects.update(1.0f / 60.0f);
        device.draws.clear();
        effects.draw(device, Mat4{1.0f}, WorldLighting{});
        drew = drew || !device.draws.empty();
        ++steps;
    }
    REQUIRE(drew);
    REQUIRE(effects.count() == 0);
    REQUIRE(steps > 120);
    REQUIRE(steps < 180);
    effects.start(device, weapons, "MP_ACID", Vec3{0.0f});
    effects.clear();
    REQUIRE(effects.count() == 0);
}

} // namespace
