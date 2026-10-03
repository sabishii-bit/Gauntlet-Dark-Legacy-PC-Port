#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/render/HeatDistortion.h"
#include "engine/world/ParticleField.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"

using namespace gdl;

TEST_CASE("heat source projection respects widescreen, letterboxing and the near plane", "[heat]") {
    const WorldCamera camera;
    for (const f32 width : {640.0f, 960.0f}) {
        const auto clip = camera.clipTransform(degreesToRadians(60), width, 448,
                                               makeScreenProjection(width, 448));
        const auto source =
            HeatSource::project(clip, Vec3{0, 0, 20}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 2, 1.25f);
        CHECK(source.center.x == Catch::Approx(0.5));
        CHECK(source.radius.x > 0);
        CHECK(source.radius.y > 0);
        CHECK(source.radius.x * width == Catch::Approx(source.radius.y * 448));
        CHECK(source.depth > 0);
        CHECK(source.depth < 1);
        CHECK(source.seconds == 1.25f);
        CHECK(
            HeatSource::project(clip, Vec3{0, 0, -2}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 2, 0).radius ==
            Vec2{0});
    }
}

TEST_CASE("heat selection rejects invalid sources and bounds its screen-space budget", "[heat]") {
    HeatDistortion heat;
    heat.add({Vec2{0.5f}, Vec2{0.1f}, 0.5f, 1});
    heat.add({Vec2{10}, Vec2{0.1f}, 0.5f, 2}); // off screen
    heat.add({Vec2{0.5f}, Vec2{0.1f}, -1, 3}); // behind the camera
    heat.add({Vec2{0.5f}, Vec2{0}, 0.5f, 4});
    heat.add({Vec2{std::numeric_limits<f32>::quiet_NaN()}, Vec2{1}, 0.5f, 5});
    CHECK(heat.sources()[0].seconds == 1);
    CHECK(heat.sources()[1].radius == Vec2{0});
    for (s32 i = 2; i < 9; ++i) {
        heat.add({Vec2{0.5f}, Vec2{static_cast<f32>(i) / 50}, 0.5f, static_cast<f32>(i)});
    }
    CHECK(heat.sources()[0].seconds == 8);
    CHECK(heat.sources()[3].seconds == 5);
    heat.add({Vec2{0.5f}, Vec2{100}, 0.5f, 9});
    CHECK(heat.sources()[0].radius == Vec2{0.2f});
}

TEST_CASE("only live thermal particles submit heat, interpolated and held with their owner",
          "[heat][particles]") {
    test::FakeRenderDevice device;
    ParticleField field;
    ParticleDescriptor descriptor;
    descriptor.oneShot = true;
    descriptor.maxParticles = 1;
    descriptor.particleLife = 4;
    descriptor.width = {1, 1, 1, 1};
    descriptor.alpha = {255, 255, 255, 255};
    descriptor.texture = "P_TORCH";
    SECTION("torch") {}
    SECTION("lava embers") {
        descriptor.texture = "EMBER_SPARK2";
    }
    SECTION("dragon breath") {
        descriptor.texture = "DRAGONBREATH";
    }
    SECTION("magic glow is not heat") {
        descriptor.texture = "WHITE_GLOW";
    }
    const bool thermal = descriptor.texture != "WHITE_GLOW";
    field.start(descriptor, glm::translate(Mat4{1}, Vec3{0, 0, 20}), &device.whiteTexture());
    const WorldCamera camera;
    const auto clip =
        camera.clipTransform(degreesToRadians(60), 640, 448, makeScreenProjection(640, 448));
    const auto draw = [&](f32 alpha) {
        device.beginFrame();
        field.draw(device, clip, Vec3{1, 0, 0}, Vec3{0, 1, 0}, alpha);
        return device.heat.sources()[0];
    };
    CHECK(draw(-1).radius == Vec2{0});
    field.step(1.0f / 30);
    REQUIRE(field.particleCount() == 1);
    field.step(1.0f / 30);
    const auto age = field.emitter(0).particles()[0].age;
    const auto early = draw(0.25f);
    const auto late = draw(0.75f);
    if (thermal) {
        CHECK(late.radius.x > 0);
        CHECK(late.seconds > early.seconds);
        const auto paused = draw(-1);
        CHECK(draw(-1).seconds == paused.seconds);
    } else {
        CHECK(late.radius == Vec2{0});
    }
    CHECK(field.emitter(0).particles()[0].age == age); // presentation cannot advance simulation
    field.step(4.0f / 30);
    CHECK(field.particleCount() == 0);
    CHECK(draw(-1).radius == Vec2{0});
}
