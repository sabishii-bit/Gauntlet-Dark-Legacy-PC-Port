#include <limits>
#include <set>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/ParticleField.h"
#include "engine/world/ThermalMaterial.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"

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
    SECTION("courtyard pool fire") {
        descriptor.texture = "POOLFIRE";
    }
    SECTION("overlay flames cannot distort the world") {
        descriptor.depthTest = false;
    }
    SECTION("magic glow is not heat") {
        descriptor.texture = "WHITE_GLOW";
    }
    const bool thermal = descriptor.texture != "WHITE_GLOW" && descriptor.depthTest;
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

TEST_CASE("heat materials distinguish flames from luminous scenery and magic", "[heat]") {
    for (const auto* name :
         {"P_TORCH", "P_TORCH_P", "NU_TORCH", "EMBER_SPARK2", "POOLFIRE", "POOLFIRE2", "POOLFIREC",
          "BLUEP_TORCH", "BLUEEMBER_SPARK2", "BLUEPOOLFIRE", "DRAGONBREATH", "FBALLX", "FBALLX2",
          "AXEPARTFIRE", "ARCPARTFIRE", "SWORDPARTFIRE"}) {
        CAPTURE(name);
        CHECK(ThermalMaterial::particle(name));
    }
    for (const auto* name : {"TORCHA", "TORCHB", "TORCHC", "NU_TORCH", "FIRE", "FLAME", "FLAME2_",
                             "FIRE_BOWL", "H_FIREWOOD", "H_FIREBACK"}) {
        CAPTURE(name);
        CHECK(ThermalMaterial::surface(name));
    }
    for (const auto* name : {"WHITE_GLOW", "GREEN_SMOKE3", "FIREFLY", "LIGHTPARTFIRE",
                             "FIRE_PORT_BKA", "FIRECLOUDS", "LAVA", "TORCH_STONE", "SGTRAIL"}) {
        CAPTURE(name);
        CHECK_FALSE(ThermalMaterial::particle(name));
        CHECK_FALSE(ThermalMaterial::surface(name));
    }
}

TEST_CASE("native world fire templates resolve to heat sources across the level corpus",
          "[heat][assets]") {
    const auto levels = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path();
    std::set<std::string> thermalNames;
    usize worlds = 0;
    for (const auto& entry : std::filesystem::directory_iterator(levels)) {
        if (!entry.is_directory() || !std::filesystem::exists(entry.path() / "WORLDS.PS2")) {
            continue;
        }
        CAPTURE(entry.path().string());
        WorldLayout layout;
        REQUIRE(layout.load(entry.path()));
        ++worlds;
        AnimationSet animations;
        REQUIRE(animations.load(entry.path()));
        for (const auto* templates :
             {&layout.particleTemplates(), &animations.particleTemplates()}) {
            for (const auto& source : *templates) {
                const auto descriptor = ParticleDescriptor::fromTemplate(source);
                if (ThermalMaterial::particle(descriptor.texture)) {
                    thermalNames.insert(descriptor.texture);
                }
            }
        }
    }
    // Fireballs and burning weapons live with their actors, not in WORLDS.PS2.
    usize actorArchives = 0;
    for (const auto* group : {"MONSTERS", "ITEMS", "WEAPONS"}) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(levels.parent_path() / group)) {
            if (entry.path().filename() != "ANIM.PS2") {
                continue;
            }
            CAPTURE(entry.path().string());
            AnimationSet animations;
            REQUIRE(animations.load(entry.path().parent_path()));
            ++actorArchives;
            for (const auto& source : animations.particleTemplates()) {
                const auto descriptor = ParticleDescriptor::fromTemplate(source);
                if (ThermalMaterial::particle(descriptor.texture)) {
                    thermalNames.insert(descriptor.texture);
                }
            }
        }
    }
    INFO("Native worlds checked: " << worlds << "; actor archives: " << actorArchives
                                   << "; thermal particle bindings: " << thermalNames.size());
    REQUIRE(worlds >= 50);
    REQUIRE(actorArchives > 0);
    for (const auto* name : {"P_TORCH", "EMBER_SPARK2", "POOLFIRE", "POOLFIRE2", "POOLFIREC"}) {
        CAPTURE(name);
        CHECK(thermalNames.contains(name));
    }
}

TEST_CASE("courtyard native pool fires submit live heat without a synthetic particle preset",
          "[heat][particles][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path();
    WorldLayout layout;
    REQUIRE(layout.load(directory));
    const auto source = std::ranges::find_if(
        layout.particleTemplates(), [](const auto& entry) { return entry.texture == "POOLFIRE"; });
    REQUIRE(source != layout.particleTemplates().end());
    const auto descriptor = ParticleDescriptor::fromTemplate(*source);
    REQUIRE(descriptor.depthTest);
    test::FakeRenderDevice device;
    ParticleField field;
    field.start(descriptor, glm::translate(Mat4{1}, Vec3{0, 0, 20}), &device.whiteTexture());
    const WorldCamera camera;
    const auto clip =
        camera.clipTransform(degreesToRadians(60), 640, 448, makeScreenProjection(640, 448));
    usize heatedFrames = 0;
    for (s32 frame = 0; frame < 120; ++frame) {
        field.step(1.0f / 30);
        device.beginFrame();
        field.draw(device, clip, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.5f);
        if (device.heat.sources()[0].radius.x > 0) {
            ++heatedFrames;
        }
    }
    CHECK(heatedFrames > 60);
    field.stop(0);
    for (s32 frame = 0; frame < 900; ++frame) {
        field.step(1.0f / 30);
    }
    REQUIRE(field.particleCount() == 0);
    device.beginFrame();
    field.draw(device, clip, Vec3{1, 0, 0}, Vec3{0, 1, 0});
    CHECK(device.heat.sources()[0].radius == Vec2{0});
}
