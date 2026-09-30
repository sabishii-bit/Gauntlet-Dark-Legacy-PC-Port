#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "game/world/WorldDestruction.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    WorldScene scene;
    WorldCollision collision;
    WorldAnimator animator;
    WorldDestruction destruction;

    explicit Fixture(bool parented = false) {
        const auto dir =
            test::sampleLevel(parented ? "world-explode-parent" : "world-explode-cycle");
        writeTextFile(dir / "world.json", parented ? R"({"objects":[
          {"name":"GROUP","position":[0,0,0],"child":1,"next":3},
          {"name":"WALL","position":[0,0,0],"flags":327680,"next":2},
          {"name":"WINDOW","position":[0,0,0]},{"name":"WALL","position":[0,0,0]}]})"
                                                   : R"({"objects":[
          {"name":"WALL","flags":327680,"position":[10,0,0],"next":1},
          {"name":"WINDOW","position":[0,0,0]}],"animations":[{"object":0,"frames":4,
          "track":{"flags":16,"frames":[0,3],"values":[0,6]}}]})");
        REQUIRE(layout.load(dir));
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        destruction.bind(layout);
        REQUIRE(
            scene.build(layout, models, textures, device, {}, {}, destruction.controlledObjects()));
        animator.bind(layout);
    }
    void step(f32 seconds, bool paused = false) {
        animator.step(seconds, scene, paused);
        destruction.update(animator.cycleEvents(), scene, collision);
    }
};

TEST_CASE("looping carts explode at their posed endpoint and return on the next pass",
          "[world-destruction][animation]") {
    Fixture f;
    f.step(1.0f / 30);
    f.step(1.0f / 30);
    CHECK(f.destruction.takeExplosions().empty());
    f.step(1.0f / 30);
    const auto bursts = f.destruction.takeExplosions();
    REQUIRE(bursts.size() == 1);
    CHECK(bursts[0].x == Catch::Approx(14)); // frame two was posed before frame three wrapped
    CHECK(f.destruction.destroyed(0));
    CHECK_FALSE(f.scene.objectVisible(0));
    CHECK_FALSE(f.collision.solid(0));
    CHECK_FALSE(f.destruction.explode(0, {}, f.scene, f.collision));
    f.step(0);
    CHECK(f.destruction.destroyed(0));
    f.step(1, true);
    CHECK(f.destruction.destroyed(0));
    CHECK(f.destruction.takeExplosions().empty());
    f.step(1.0f / 30);
    CHECK_FALSE(f.destruction.destroyed(0));
    CHECK(f.scene.objectVisible(0));
    CHECK(f.collision.solid(0));
    f.step(2.0f / 30);
    CHECK(f.destruction.takeExplosions().size() == 1);
}

TEST_CASE("world explosions follow parents, never the next root in the level list",
          "[world-destruction]") {
    Fixture f(true);
    REQUIRE(f.layout.objects()[1].parent == 0);
    CHECK_FALSE(f.destruction.explode(3, {}, f.scene, f.collision));
    REQUIRE(f.destruction.explode(1, {4, 5, 6}, f.scene, f.collision));
    CHECK(f.destruction.destroyed(0));
    CHECK(f.destruction.destroyed(1));
    CHECK_FALSE(f.destruction.destroyed(2)); // rendering is recursive; collision marks the chain
    CHECK_FALSE(f.scene.objectVisible(1));
    CHECK_FALSE(f.scene.objectVisible(2));
    CHECK(f.collision.solid(3));
    const auto bursts = f.destruction.takeExplosions();
    REQUIRE(bursts.size() == 1);
    CHECK(bursts[0] == Vec3{4, 5, 6});
    f.destruction.clear();
    CHECK(f.destruction.controlledObjects().empty());
    CHECK(f.destruction.takeExplosions().empty());
}

TEST_CASE("one-shot and paused animations never generate a cart endpoint explosion",
          "[world-destruction][animation]") {
    Fixture f;
    f.step(2, true);
    CHECK(f.destruction.takeExplosions().empty());
    f.animator.fire(0, true);
    f.step(2);
    CHECK(f.destruction.takeExplosions().empty());
    CHECK_FALSE(f.destruction.destroyed(0));
}

TEST_CASE("a low render rate cannot skip the cart restart visibility window",
          "[world-destruction][animation]") {
    Fixture f;
    f.step(3.0f / 30.0f);
    REQUIRE(f.destruction.destroyed(0));
    REQUIRE(f.destruction.takeExplosions().size() == 1);
    // Cross another whole short cycle in a single render frame. The first-frame
    // reveal must precede the new explosion, rather than leaving it spent forever.
    f.step(3.0f / 30.0f);
    CHECK(f.destruction.takeExplosions().size() == 1);
    f.step(2.0f / 30.0f);
    CHECK_FALSE(f.destruction.destroyed(0));
    CHECK(f.scene.objectVisible(0));
    CHECK(f.collision.solid(0));
}
} // namespace
