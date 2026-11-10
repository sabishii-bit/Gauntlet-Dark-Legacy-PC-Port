#include <array>
#include <bit>
#include <limits>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/level/LegendsDreamcastContainers.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using namespace gdl::formats;

ItemInfoRecord barrelDefinition() {
    ItemInfoRecord result;
    result.type = 2;
    result.subtype = 43;
    result.name = "BAROBJ";
    result.collisionType = 1;
    result.radius = 1;
    result.height = 2.5f;
    result.hitPoints = 5;
    result.armor = 1;
    result.activeType = 0x206;
    return result;
}

TEST_CASE("Dreamcast barrels use native bodies and stored health without changing N64 thresholds",
          "[legends][dreamcast-containers]") {
    ItemInstanceRecord instance;
    instance.position = {5, 2, 7};
    instance.rotation.y = .7f;
    const auto definition = legendsDreamcastContainer(barrelDefinition(), instance, 42);
    REQUIRE(definition);
    CHECK(definition->closedObstacle.centre == instance.position);
    CHECK(definition->closedObstacle.cylinderRadius == 1);
    CHECK(definition->closedObstacle.height == 2.5f);
    CHECK(definition->yaw == .7f);
    CHECK(definition->opening == "ACTIVE");
    CHECK(definition->broken == "DONE");
    instance.rotation.x = -8.88753e-8f;
    instance.rotation.z = -8.74107e-8f;
    REQUIRE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    instance.rotation.x = -.06654595f;
    instance.rotation.z = -.0761889f;
    const auto terrainAligned = legendsDreamcastContainer(barrelDefinition(), instance, 42);
    REQUIRE(terrainAligned);
    CHECK(terrainAligned->closedObstacle == definition->closedObstacle);
    instance.rotation.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    instance.rotation.x = std::numeric_limits<f32>::infinity();
    CHECK_FALSE(legendsDreamcastContainer(barrelDefinition(), instance, 42));
    LegendsContainer barrel{*definition};
    for (s32 hit = 0; hit < 2; ++hit) {
        REQUIRE(barrel.strike(3));
        CHECK(barrel.targetable());
        CHECK(barrel.obstacle().solid);
        CHECK(barrel.state() == LegendsContainer::State::Closed);
    }
    REQUIRE(barrel.strike(3));
    CHECK(barrel.state() == LegendsContainer::State::Opening);
    CHECK_FALSE(barrel.obstacle().solid);
    CHECK_FALSE(barrel.strike(3));
    REQUIRE(barrel.finishOpening());

    auto nativeN64 = *definition;
    nativeN64.damageMode = LegendsContainerDefinition::DamageMode::Threshold;
    LegendsContainer oldMode{nativeN64};
    for (s32 hit = 0; hit < 10; ++hit) {
        CHECK_FALSE(oldMode.strike(4));
    }
    REQUIRE(oldMode.strike(5));
    LegendsContainer minimumDamage{*definition};
    for (s32 hit = 0; hit < 5; ++hit) {
        REQUIRE(minimumDamage.strike(.5f));
    }
    CHECK(minimumDamage.state() == LegendsContainer::State::Opening);
}

TEST_CASE("Dreamcast chest contact spends one key and holds collision until its animation finishes",
          "[legends][dreamcast-containers]") {
    auto info = barrelDefinition();
    info.subtype = 48;
    info.name = "CHESTS";
    info.activeType = 0x16;
    info.collisionType = 3;
    info.xSize = 1.4f;
    info.zSize = 1;
    const auto definition = legendsDreamcastContainer(info, {}, 7);
    REQUIRE(definition);
    CHECK(definition->closedObstacle.halfAcross == 1.4f);
    CHECK(definition->closedObstacle.halfAlong == 1);
    CHECK(definition->goldOpening == "CHESTSG");
    LegendsContainer chest{*definition};
    s32 keys = 2;
    REQUIRE(chest.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK_FALSE(chest.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK(chest.obstacle().solid);
    REQUIRE(chest.finishOpening());
    CHECK_FALSE(chest.obstacle().solid);
    auto deferred = *definition;
    deferred.interactive = false;
    LegendsContainer unavailable{deferred};
    CHECK_FALSE(unavailable.touch({}, .5f, 3, keys));
    CHECK(keys == 1);
    CHECK(unavailable.obstacle().solid);
    info.activeType = 0x42;
    std::string diagnostic;
    CHECK_FALSE(legendsDreamcastContainer(info, {}, 7, &diagnostic));
    CHECK_FALSE(diagnostic.empty());
}

TEST_CASE("Dreamcast contents resolve nested choices at opening using a shared native cursor",
          "[legends][dreamcast-containers]") {
    std::array<ItemInfoRecord, 5> rows;
    rows[0].type = rows[2].type = ItemInfoRecord::kChoiceList;
    rows[0].subtype = rows[2].subtype = 2;
    rows[0].choices = {1, 2};
    rows[2].choices = {3, 4};
    rows[1].type = rows[3].type = rows[4].type = 1;
    LegendsDreamcastLoot loot{rows, 255};
    CHECK(loot.outcomes(0) == std::vector<s16>{1, 3, 4});
    CHECK(loot.seed() == 255);
    CHECK(loot.select(0, 0) == 4); // 7%2 -> nested row; 21%2 -> final leaf.
    CHECK(loot.seed() == 255 + 439 * 2);
    const auto prior = loot.seed();
    CHECK(loot.select(1, 5) == 1);
    CHECK_FALSE(loot.select(-1, 5));
    CHECK(loot.seed() == prior);
    LegendsDreamcastLoot at30{rows, 255};
    LegendsDreamcastLoot at60{rows, 255};
    for (s32 tick = 0; tick < 90; ++tick) {
        at30.advance(1.0f / 30);
        at60.advance(1.0f / 60);
        at60.advance(1.0f / 60);
    }
    CHECK(at30.seed() == 345);
    CHECK(at60.seed() == at30.seed());
    CHECK(at30.select(0, 19) == at60.select(0, 19));
    rows[2].choices = {0, 4};
    LegendsDreamcastLoot cycle{rows, 255};
    CHECK_THROWS_AS(cycle.outcomes(0), FormatError);
    CHECK_THROWS_AS(cycle.select(0, 0), FormatError);
    CHECK(cycle.seed() == 255);
    rows[2].choices = {8, 4};
    CHECK_THROWS_AS(LegendsDreamcastLoot{rows}.outcomes(0), FormatError);
}

TEST_CASE("Every extracted Dreamcast ordinary container retains a native body and valid loot graph",
          "[legends][dreamcast-containers][assets]") {
    const auto root =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(root)) {
        SKIP("Dreamcast extraction is not installed");
    }
    usize barrels = 0;
    usize chests = 0;
    usize deathContents = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto path = entry.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto bytes = readFile(path);
        if (bytes.size() < 96) {
            continue;
        }
        const auto native = LegendsDreamcastScene::parse(bytes);
        const LegendsDreamcastLoot loot{native.itemInfos};
        for (usize index = 0; index < native.itemInstances.size(); ++index) {
            const auto& placement = native.itemInstances[index];
            const auto& info = native.itemInfos[static_cast<usize>(placement.info)];
            if (info.type != 2) {
                continue;
            }
            CAPTURE(path, index, info.subtype, info.name);
            std::string diagnostic;
            const auto definition =
                legendsDreamcastContainer(info, placement, static_cast<u32>(index), &diagnostic);
            CAPTURE(diagnostic);
            REQUIRE(definition);
            CHECK(definition->position == placement.position);
            if (definition->kind == LegendsContainerDefinition::Kind::Barrel) {
                ++barrels;
            } else {
                ++chests;
            }
            for (const auto outcome :
                 loot.outcomes(std::bit_cast<s16>(readU16LE(placement.params, 0)))) {
                deathContents += loot.definition(outcome).type == 4 ? 1 : 0;
            }
        }
    }
    CHECK(barrels == 610);
    CHECK(chests == 495);
    CHECK(deathContents >= 48); // Do not accidentally reinterpret Death as an empty pickup.
}
} // namespace
