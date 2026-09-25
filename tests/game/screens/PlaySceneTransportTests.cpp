#include <array>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("the castle transporter moves a player without leaving the level or bouncing back",
          "[transporters][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELA1/world.json").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto ref = catalog.byName("A1");
    REQUIRE(ref);
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *ref));
    GameContext context;
    context.unpackedRoot = root;
    context.levels = &catalog;
    PlayOptions options;
    options.welcome = false;
    options.position = Vec3{-70.84375f, 0.325f, 94.78125f};
    options.yaw = 0.7f;
    const std::array party{PartyMember{0, CharacterSave{}}};
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, options));
    REQUIRE(scene.transporters().size() == 4);
    REQUIRE(scene.transporters().pad(0).destination == 1);
    const Vec3 destination = scene.transporters().pad(1).position;
    bool arrived = false;
    for (s32 frame = 0; frame < 300; ++frame) {
        REQUIRE(scene.update(1.0 / 30, {}) == PlayOutcome::Running);
        const auto& actor = *scene.actor(0);
        if (std::abs(actor.position().x - destination.x) < 0.01f) {
            arrived = true;
        }
        if (arrived) {
            CHECK(actor.position().x == Catch::Approx(destination.x));
            CHECK(actor.position().z == Catch::Approx(destination.z));
            CHECK(actor.yaw() == Catch::Approx(0.7f));
        }
    }
    CHECK(arrived);
    scene.close();
    CHECK(scene.transporters().size() == 0);
}
} // namespace
