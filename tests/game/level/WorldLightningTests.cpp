#include <algorithm>
#include <array>
#include <limits>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ModelSet.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/world/WorldScene.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/LevelCatalog.h"
#include "game/level/LevelWorld.h"
#include "game/level/WorldLightning.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

TEST_CASE("lightning binds only its native stage names and safely resets missing meshes",
          "[game][world][world-lightning]") {
    const auto root = test::scratchDirectory("world-lightning-selection");
    writeTextFile(root / "world.json", R"({"objects":[
      {"name":"F2XPNC_LNING3","position":[0,0,0],"next":1},
      {"name":"F2XPNC_LNING0","position":[0,0,0],"next":2},
      {"name":"F2XPNC_LNING1","position":[0,0,0],"next":3},
      {"name":"F2XPNC_LNING10","position":[0,0,0],"next":4},
      {"name":"F2XPNC_LNING4","position":[0,0,0],"next":5},
      {"name":"F2XPNC_LNING2","position":[0,0,0],"next":6},
      {"name":"F2XPNC_LNING5","position":[0,0,0],"next":7},
      {"name":"F2XPNC_LNING1COPY","position":[0,0,0],"next":8},
      {"name":"A5XPXLNC_LNING1","position":[0,0,0],"next":9},
      {"name":"F2XPNC_LNING1","position":[0,0,0],"next":-1}]})");
    WorldLayout layout;
    REQUIRE(layout.load(root));
    WorldLightning lightning;
    lightning.bind(layout, "F2");
    REQUIRE(lightning.flashes().size() == 4);
    constexpr std::array<usize, 4> kSelected{2, 5, 0, 4};
    for (usize i = 0; i < kSelected.size(); ++i) {
        CHECK(lightning.flashes()[i].object == kSelected[i]);
        CHECK(lightning.flashes()[i].remaining >= 5);
        CHECK(lightning.flashes()[i].remaining < 15);
        CHECK_FALSE(lightning.flashes()[i].visible);
    }
    const auto initial = lightning;
    // Layouts may name a sheet whose optional mesh was not available. Applying
    // and ticking those indices must not require a rendered unit to exist.
    WorldScene noMeshes;
    lightning.apply(noMeshes);
    for (const auto seconds :
         {0.f, -1.f, std::numeric_limits<f32>::infinity(), -std::numeric_limits<f32>::infinity(),
          std::numeric_limits<f32>::quiet_NaN()}) {
        lightning.update(seconds, noMeshes);
        CHECK(lightning == initial);
    }
    lightning.update(20, noMeshes);
    for (const auto& flash : lightning.flashes()) {
        CHECK(flash.visible);
        CHECK(flash.remaining == Approx(.2f));
    }
    lightning.update(.21f, noMeshes);
    for (const auto& flash : lightning.flashes()) {
        CHECK_FALSE(flash.visible);
    }
    CHECK_FALSE(lightning == initial);
    lightning.bind(layout, "F2");
    CHECK(lightning == initial);
    lightning.clear();
    CHECK(lightning.flashes().empty());
    CHECK(lightning == WorldLightning{});
    lightning.bind(layout, "F2");
    CHECK(lightning == initial);
    for (const auto* otherStage : {"F1", "A4", "f2", "n64:boss2", "dreamcast:levelf2"}) {
        lightning.bind(layout, otherStage);
        CHECK(lightning.flashes().empty());
    }
    lightning.bind(layout, "A5");
    REQUIRE(lightning.flashes().size() == 1);
    CHECK(lightning.flashes().front().object == 8);
}

TEST_CASE("retail A5 and F2 lightning flashes independently while Stop Time holds gameplay",
          "[game][world][world-lightning][assets]") {
    for (const auto* stage : {"A5", "F2"}) {
        DYNAMIC_SECTION(stage) {
            const auto file = std::string{"LEVELS/LEVEL"} + stage + "/WORLDS.PS2";
            const auto root = test::assetOrSkip(file).parent_path().parent_path().parent_path();
            test::FakeRenderDevice device;
            LevelCatalog catalog;
            REQUIRE(catalog.load(root));
            const auto level = catalog.byName(stage);
            REQUIRE(level);
            LevelWorld world;
            REQUIRE(world.load(device, root, *level));
            ModelSet models;
            REQUIRE(models.load(root / level->directory));
            const std::string prefix =
                std::string{stage} == "A5" ? "A5XPXLNC_LNING" : "F2XPNC_LNING";
            std::array<usize, 4> lightning{};
            for (usize i = 0; i < lightning.size(); ++i) {
                const auto& objects = world.layout().objects();
                const auto found =
                    std::ranges::find(objects, prefix + std::to_string(i + 1), &WorldObject::name);
                REQUIRE(found != objects.end());
                // The shipped root and its intermediate NODE have no model;
                // the actual two-triangle bolt is their grandchild. Observing
                // only the root would miss a frozen visible sheet entirely.
                CHECK_FALSE(models.find(found->name));
                REQUIRE(found->child >= 0);
                const auto& branch = objects.at(static_cast<usize>(found->child));
                CHECK_FALSE(models.find(branch.name));
                REQUIRE(branch.child >= 0);
                lightning[i] = static_cast<usize>(branch.child);
                const auto& leaf = objects.at(lightning[i]);
                REQUIRE(leaf.child == -1);
                const auto model = models.find(leaf.name);
                REQUIRE(model);
                REQUIRE(models.mesh(*model).triangleCount() == 2);
                CHECK_FALSE(world.scene().objectVisible(lightning[i]));
            }
            usize ordinary = 0;
            while (std::ranges::find(lightning, ordinary) != lightning.end()) {
                ++ordinary;
            }
            REQUIRE(ordinary < world.layout().objects().size());
            const bool ordinaryVisible = world.scene().objectVisible(ordinary);
            // Drawing repeatedly does not consume time or a new random wait.
            for (const f32 alpha : {0.f, .25f, .75f, 1.f}) {
                world.draw(device, Mat4{1}, {}, alpha);
                device.draws.clear();
                for (const auto object : lightning) {
                    CHECK_FALSE(world.scene().objectVisible(object));
                }
            }
            std::array<bool, 4> previous{};
            std::array<s32, 4> lastEdge{};
            std::array<s32, 4> flashes{};
            bool independent = false;
            constexpr f32 kStep = 1.f / 60;
            for (s32 tick = 1; tick <= 31 * 60; ++tick) {
                world.capturePresentation();
                world.update(kStep, true);
                s32 visibleCount = 0;
                for (usize i = 0; i < lightning.size(); ++i) {
                    const bool visible = world.scene().objectVisible(lightning[i]);
                    visibleCount += static_cast<s32>(visible);
                    if (visible != previous[i]) {
                        CAPTURE(stage, i, tick, lastEdge[i], visible);
                        const s32 elapsed = tick - lastEdge[i];
                        if (visible) {
                            // One simulation tick of boundary tolerance only.
                            CHECK(elapsed >= 5 * 60 - 1);
                            CHECK(elapsed <= 15 * 60 + 1);
                            ++flashes[i];
                        } else {
                            CHECK(elapsed >= 11);
                            CHECK(elapsed <= 13);
                        }
                        previous[i] = visible;
                        lastEdge[i] = tick;
                    }
                }
                independent |= visibleCount > 0 && visibleCount < 4;
                CHECK(world.scene().objectVisible(ordinary) == ordinaryVisible);
            }
            CHECK(independent);
            for (const auto count : flashes) {
                CHECK(count >= 2);
            }
        }
    }
}

} // namespace
