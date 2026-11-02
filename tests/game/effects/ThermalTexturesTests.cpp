#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/render/HeatDistortion.h"
#include "engine/world/ParticleField.h"
#include "engine/world/ParticleSystem.h"
#include "engine/world/WorldCamera.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/effects/ThermalTextures.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("the thermal textures name the levels' flames, in order",
          "[game][heat][thermal-textures]") {
    constexpr std::array<std::string_view, 16> kParticles{
        "P_TORCH",   "P_TORCH_P",   "NU_TORCH",         "EMBER_SPARK2", "POOLFIRE",     "POOLFIRE2",
        "POOLFIREC", "BLUEP_TORCH", "BLUEEMBER_SPARK2", "BLUEPOOLFIRE", "DRAGONBREATH", "FBALLX",
        "FBALLX2",   "AXEPARTFIRE", "ARCPARTFIRE",      "SWORDPARTFIRE"};
    constexpr std::array<std::string_view, 10> kSurfaces{
        "TORCHA", "TORCHB",  "TORCHC",    "NU_TORCH",   "FIRE",
        "FLAME",  "FLAME2_", "FIRE_BOWL", "H_FIREWOOD", "H_FIREBACK"};
    CHECK(std::ranges::equal(kThermalParticles, kParticles));
    CHECK(std::ranges::equal(kThermalSurfaces, kSurfaces));
    CHECK(kThermalTextures.particles.data() == kThermalParticles.data());
    CHECK(kThermalTextures.particles.size() == kThermalParticles.size());
    CHECK(kThermalTextures.surfaces.data() == kThermalSurfaces.data());
    CHECK(kThermalTextures.surfaces.size() == kThermalSurfaces.size());
    for (const std::string_view name : kParticles) {
        CAPTURE(name);
        CHECK(kThermalTextures.particle(name));
    }
    for (const std::string_view name : kSurfaces) {
        CAPTURE(name);
        CHECK(kThermalTextures.surface(name));
    }
}

TEST_CASE("the thermal textures leave luminous scenery and magic cold",
          "[game][heat][thermal-textures]") {
    for (const std::string_view name :
         {"WHITE_GLOW", "GREEN_SMOKE3", "FIREFLY", "LIGHTPARTFIRE", "FIRE_PORT_BKA", "FIRECLOUDS",
          "LAVA", "TORCH_STONE", "SGTRAIL", ""}) {
        CAPTURE(name);
        CHECK_FALSE(kThermalTextures.particle(name));
        CHECK_FALSE(kThermalTextures.surface(name));
    }
}

TEST_CASE("native world fire templates resolve to heat sources across the level corpus",
          "[game][heat][thermal-textures][assets]") {
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
                if (kThermalTextures.particle(descriptor.texture)) {
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
                if (kThermalTextures.particle(descriptor.texture)) {
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
          "[game][heat][thermal-textures][particles][assets]") {
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
        field.draw(device, clip, Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.5f, kThermalTextures);
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
    field.draw(device, clip, Vec3{1, 0, 0}, Vec3{0, 1, 0}, -1.0f, kThermalTextures);
    CHECK(device.heat.sources()[0].radius == Vec2{0});
}

} // namespace
