#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "expansions/n64/bosses/LegendsBossAssets.h"
#include "game/bosses/Bosses.h"
#include "game/critters/CombatantAssets.h"
#include "game/enemies/Enemies.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Chimera keeps its support during dormant and active entrances on every platform",
          "[dreamcast][dc-boss-ai][boss-platform][assets]") {
    const auto installed = test::dataDirectory().parent_path() / "assets";
    const auto common = test::assetOrSkip("CRITTER").parent_path();
    test::FakeRenderDevice device;
    for (const std::string platform : {"GameCube", "N64", "Dreamcast"}) {
        DYNAMIC_SECTION(platform) {
            std::vector<std::string> warnings;
            std::unique_ptr<CombatantAssets> stock;
            if (platform == "GameCube") {
                stock = std::make_unique<CombatantAssets>();
                REQUIRE(stock->load(device, common, bossDefinition("CHIMERA"), 'A'));
            } else if (platform == "N64") {
                const auto root = installed / "n64/extracted";
                if (!std::filesystem::is_regular_file(root / "_program/game.bin")) {
                    SKIP("N64 data not installed");
                }
                stock = loadLegendsBossAssets(device, root, common, 35,
                                              readFile(root / "_program/game.bin"), {}, warnings);
            } else {
                const auto root = installed / "dreamcast/extracted/native";
                if (!std::filesystem::is_regular_file(root / "critters/chimera.wad")) {
                    SKIP("Dreamcast data not installed");
                }
                stock = loadLegendsDreamcastBossAssets(device, root, common, 35, {}, warnings);
            }
            REQUIRE(stock);
            CollisionTriangle floor;
            floor.vertices = {Vec3{-100, 0, -100}, Vec3{100, 0, -100}, Vec3{0, 0, 100}};
            floor.object = 15;
            WorldCollision collision;
            collision.build({floor});
            collision.setMovingObjects(std::array<s32, 1>{15});
            collision.setObjectTransform(15, Mat4{1});
            Bosses bosses;
            bosses.open(device, common, &collision, {}, 'A');
            const auto* preloaded = bosses.preload(35, std::move(stock));
            REQUIRE(preloaded);
            REQUIRE(bosses.spawn(35, {0, 0, 0}, 0));
            const std::array players{EnemyView{.player = 0, .position = {0, 0, 500}}};
            for (const bool awake : {false, true}) {
                CAPTURE(awake);
                if (awake) {
                    bosses.wake();
                }
                for (s32 tick = 1; tick <= 60; ++tick) {
                    const f32 height = -(static_cast<f32>(tick) + (awake ? 60.0f : 0)) * .2f;
                    collision.setObjectTransform(15, glm::translate(Mat4{1}, Vec3{0, height, 0}));
                    bosses.update(1, 1.0f / 60, players);
                    CHECK(bosses.fighter().position().y == height);
                    for (s32 child = 1; child <= static_cast<s32>(bosses.fighter().childCount());
                         ++child) {
                        const auto* part = bosses.fighter().child(bosses.fighter().id() + child);
                        REQUIRE(part);
                        CHECK(part->position().y == height);
                    }
                    CHECK(bosses.view().awake == awake);
                }
            }
            const Vec3 before = bosses.fighter().position();
            collision.setSolid(15, false);
            collision.setObjectTransform(15, glm::translate(Mat4{1}, Vec3{0, -100, 0}));
            bosses.update(1, 1.0f / 60, players);
            CHECK(bosses.fighter().position().y == before.y);
        }
    }
}

} // namespace
