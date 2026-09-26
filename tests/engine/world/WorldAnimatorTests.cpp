#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "SampleLevel.h"
#include "TestSupport.h"

namespace {

using namespace gdl;
using Catch::Approx;

constexpr f32 kStep = 1.0f / 30.0f;

struct Fixture {
    test::FakeRenderDevice device;
    ModelSet models;
    TextureSet textures;
    WorldLayout layout;
    WorldScene scene;
    WorldAnimator animator;

    explicit Fixture(std::string_view name) {
        const auto dir = test::sampleLevel(name);
        REQUIRE(models.load(dir));
        REQUIRE(textures.load(dir));
        REQUIRE(layout.load(dir));
        REQUIRE(scene.build(layout, models, textures, device));
    }

    /** Where the blade's first vertex lands when the scene is drawn. */
    Vec3 bladeAt() {
        device.draws.clear();
        scene.draw(device, Mat4{1.0f}, Vec3{10.0f, 0.0f, 0.0f});
        return device.draws[2].vertices[0].position;
    }
};

/** A layout of one keyed object with the given level flags. */
WorldLayout layoutWithFlags(std::string_view name, u32 flags) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "SPIN", "position": [0, 0, 0], "flags": )" +
                                          std::to_string(flags) + R"(}],
  "animations": [{"object": 0, "frames": 4, "state": 257, "start": 0,
                  "track": {"flags": 2, "frames": [0, 3], "values": [0, 1]}}]
})");
    WorldLayout layout;
    REQUIRE(layout.load(dir));
    return layout;
}

TEST_CASE("world animations turn their objects thirty frames a second and loop",
          "[world][animation]") {
    Fixture f("world-animator");
    f.animator.bind(f.layout);
    REQUIRE(f.animator.size() == 1);
    REQUIRE(f.animator.object(0) == 8);
    REQUIRE(f.animator.frame(0) == 0.0f);

    f.animator.apply(f.scene);
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f});
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(1.0f));
    REQUIRE(f.bladeAt() == Vec3{11.0f, 0.0f, 50.0f}); // posed before moving on
    f.animator.step(kStep, f.scene);
    // A third of the way to one radian about y swings the blade off +x.
    const Vec3 blade = f.bladeAt();
    REQUIRE(blade.x == Approx(10.0f + std::cos(1.0f / 3.0f)).margin(1e-4f));
    REQUIRE(blade.z == Approx(50.0f - std::sin(1.0f / 3.0f)).margin(1e-4f));
    f.animator.step(kStep, f.scene);
    REQUIRE(f.animator.frame(0) == 0.0f); // the last frame wraps to the first
    REQUIRE_FALSE(f.animator.finished(0));
    f.animator.step(2.0f * kStep, f.scene);
    REQUIRE(f.animator.frame(0) == Approx(2.0f));

    f.animator.clear();
    REQUIRE(f.animator.size() == 0);
}

TEST_CASE("a held switch cycles its track and releases at the forward endpoint",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-cycle", 0));
    animator.hold(0);
    animator.cycle(0, true);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK_FALSE(animator.finished(0));
    animator.cycle(0, false);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == 3);
}

TEST_CASE("one-shot animations stop at their last frame, backwards ones at their first",
          "[world][animation]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-animator-once", WorldObject::kOnce));
    REQUIRE(animator.size() == 1);
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 3.0f);
    REQUIRE(animator.finished(0));

    animator.bind(layoutWithFlags("world-animator-reverse", WorldObject::kReverse));
    REQUIRE(animator.frame(0) == 3.0f);
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(2.0f));
    for (s32 i = 0; i < 10; ++i) {
        animator.step(kStep, scene);
    }
    REQUIRE(animator.frame(0) == 0.0f);
    REQUIRE(animator.finished(0));

    // Both flags together switch the animation off.
    animator.bind(
        layoutWithFlags("world-animator-off", WorldObject::kOnce | WorldObject::kReverse));
    REQUIRE(animator.size() == 0);
}

TEST_CASE("pausing looping world tracks does not hold one-shot trigger movement",
          "[world][animation][stop-time]") {
    WorldScene scene;
    WorldAnimator animator;
    animator.bind(layoutWithFlags("world-time-stop", 0));
    animator.step(kStep, scene);
    REQUIRE(animator.frame(0) == Approx(1));
    animator.step(5, scene, true);
    CHECK(animator.frame(0) == Approx(1));
    animator.step(kStep, scene);
    CHECK(animator.frame(0) == Approx(2));
    animator.fire(0, true);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(3));
    animator.fire(0, false);
    animator.step(1, scene, true);
    CHECK(animator.finished(0));
    CHECK(animator.frame(0) == Approx(0));
}
} // namespace
