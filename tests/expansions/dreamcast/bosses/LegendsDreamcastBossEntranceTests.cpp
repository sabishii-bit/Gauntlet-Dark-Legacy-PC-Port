#include <cmath>
#include <filesystem>
#include <limits>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossEntrance.h"
#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"

namespace gdl::game {

TEST_CASE("Dreamcast entrance rocks wait for all three authored impulses",
          "[dreamcast][dc-boss-entrance]") {
    LegendsDreamcastBossEntrance rocks;
    rocks.clear(-200);
    rocks.add(21, {10, 0, 0}, {});
    rocks.update(10);
    REQUIRE(rocks.pieces().size() == 1);
    CHECK(rocks.pieces()[0].motion.position == Vec3{10, 0, 0});
    CHECK(rocks.phase() == 0);
    for (s32 phase = 1; phase <= 3; ++phase) {
        rocks.cue({0, 0, 0});
        const auto velocity = rocks.pieces()[0].motion.velocity;
        CHECK(rocks.phase() == phase);
        const f32 base = phase == 1 ? 20.0f : phase == 2 ? 30.0f : 50.0f;
        const f32 spread = phase == 1 ? 10.0f : phase == 2 ? 15.0f : 100.0f;
        CHECK(velocity.y >= base);
        CHECK(velocity.y < base + spread);
        CHECK(std::hypot(velocity.x, velocity.z) == Catch::Approx(phase == 3 ? 10 : 0));
        const Vec3 before = rocks.pieces()[0].motion.position;
        rocks.update(1.0f / 30);
        CHECK(rocks.pieces()[0].motion.position.y > before.y);
    }
    const auto done = rocks.pieces()[0].motion.velocity;
    rocks.cue({0, 0, 0});
    CHECK(rocks.phase() == 3);
    CHECK(rocks.pieces()[0].motion.velocity == done);
    for (s32 frame = 0; frame < 600; ++frame) {
        rocks.update(1.0f / 30);
    }
    CHECK_FALSE(rocks.pieces()[0].motion.visible);
}

TEST_CASE("Dreamcast entrance rock state is independent of render and update cadence",
          "[dreamcast][dc-boss-entrance]") {
    LegendsDreamcastBossEntrance whole;
    LegendsDreamcastBossEntrance halves;
    for (auto* rocks : {&whole, &halves}) {
        rocks->clear(-200);
        rocks->add(23, {12, 4, -3}, {.2f, .4f, .6f});
        rocks->cue({0, 0, 0});
    }
    for (s32 frame = 0; frame < 30; ++frame) {
        whole.update(1.0f / 30);
        halves.update(1.0f / 60);
        [[maybe_unused]] auto sample =
            halves.pieces()[0].motion.presentedPosition(halves.presentationAlpha(.5f));
        halves.update(1.0f / 60);
    }
    CHECK(whole.pieces()[0].motion == halves.pieces()[0].motion);
    const auto before = whole.pieces()[0].motion;
    whole.cue({std::numeric_limits<f32>::quiet_NaN(), 0, 0});
    whole.update(std::numeric_limits<f32>::infinity());
    CHECK(whole.pieces()[0].motion == before);
}

TEST_CASE("Dreamcast E2 binds its twenty-two authored entrance rocks and native cue records",
          "[dreamcast][dc-boss-entrance][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levele2/worlds.rom")) {
        SKIP("Dreamcast assets not installed");
    }
    const auto scene =
        formats::LegendsDreamcastScene::parse(readFile(root / "levels/levele2/worlds.rom"));
    LegendsDreamcastBossEntrance rocks;
    for (usize index = 0; index < scene.itemInstances.size(); ++index) {
        const auto& instance = scene.itemInstances[index];
        REQUIRE(instance.info >= 0);
        const auto& info = scene.itemInfos[static_cast<usize>(instance.info)];
        if (LegendsDreamcastBossEntrance::accepts(info, instance)) {
            rocks.add(static_cast<u32>(index), instance.position, instance.rotation);
        }
    }
    REQUIRE(rocks.pieces().size() == 22);
    CHECK(rocks.pieces().front().nativeIndex == 21);
    CHECK(rocks.pieces().back().nativeIndex == 44);
    const auto boss =
        formats::decodeLegendsDreamcastCritter(readFile(root / "critters/skorne1.wad"));
    usize impulses = 0;
    for (const auto& sound : boss.sounds) {
        if ((sound.flags & 0x20U) != 0) {
            ++impulses;
        }
    }
    CHECK(impulses > 0);
    rocks.update(5);
    for (const auto& piece : rocks.pieces()) {
        CHECK(piece.motion.position == scene.itemInstances[piece.nativeIndex].position);
    }
    rocks.cue({-.125f, -25.375f, -19.625f});
    rocks.update(1.0f / 30);
    for (const auto& piece : rocks.pieces()) {
        CHECK(piece.motion.position.y > scene.itemInstances[piece.nativeIndex].position.y);
    }
}

} // namespace gdl::game
