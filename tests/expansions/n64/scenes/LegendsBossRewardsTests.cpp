#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path rootOrSkip() {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(root / "_program/Boss.bin")) {
        SKIP("Native N64 Legends boss assets are not installed");
    }
    return root;
}

formats::LegendsMap mapAt(const std::filesystem::path& directory) {
    const auto models = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                             readFile(directory / "ipr.rom"));
    const auto dynamicPath = directory / "texdyn.rom";
    const auto dynamic =
        std::filesystem::is_regular_file(dynamicPath) ? readFile(dynamicPath) : std::vector<u8>{};
    for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    return map;
}

std::vector<usize> rewards(const LegendsSceneAdapter& scene) {
    std::vector<usize> indices;
    for (usize i = 0; i < scene.pickups().size(); ++i) {
        if (scene.pickups()[i].nativeIndex >= 0x80000000U) {
            indices.push_back(i);
        }
    }
    return indices;
}

TEST_CASE("native boss rewards are prebound hidden and released once without GPU allocation",
          "[legends][bosses][boss-rewards][assets]") {
    const auto root = rootOrSkip();
    const auto program = readFile(root / "_program/game.bin");
    const auto overlay = readFile(root / "_program/Boss.bin");
    for (const char* const name : {"mount6", "ice5", "town5", "castle5", "boss2", "hell2"}) {
        CAPTURE(name);
        const auto encounter = legendsN64Boss(name, program, overlay);
        REQUIRE(encounter);
        const auto directory = root / "levels" / name;
        auto map = mapAt(directory);
        test::FakeRenderDevice device;
        auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
        for (const auto& warning : scene->warnings()) {
            UNSCOPED_INFO(warning);
        }
        const auto indices = rewards(*scene);
        REQUIRE(indices.size() == encounter->rewards.size());
        CHECK(scene->portals().empty());
        for (const auto index : indices) {
            CHECK_FALSE(scene->pickups()[index].active);
            CHECK_FALSE(scene->pickups()[index].collected);
        }
        REQUIRE_FALSE(map.starts.empty());
        const u32 textures = device.texturesCreated;
        scene->spawnBossRewards({0, std::numeric_limits<f32>::quiet_NaN(), 0},
                                map.starts[0].position);
        for (const auto index : indices) {
            CHECK_FALSE(scene->pickups()[index].active);
        }
        scene->spawnBossRewards(encounter->combat.position, map.starts[0].position);
        std::vector<Vec3> first;
        for (usize i = 0; i < indices.size(); ++i) {
            const auto motion = legendsBossRewardMotion(
                encounter->rewards[i], encounter->combat.position, map.starts[0].position);
            REQUIRE(motion);
            const auto& pickup = scene->pickups()[indices[i]];
            CHECK(pickup.active);
            CHECK(glm::length(pickup.position - motion->position) < 0.0001f);
            first.push_back(pickup.position);
        }
        scene->spawnBossRewards({0, 100, 0}, {100, 0, 100});
        for (usize i = 0; i < indices.size(); ++i) {
            CHECK(scene->pickups()[indices[i]].position == first[i]);
        }
        CHECK(device.texturesCreated == textures);
        LegendsTraversal traversal;
        const CharacterSave save;
        traversal.open(map, 0, save);
        for (s32 frame = 0; frame < 360; ++frame) {
            scene->updateBossRewards(1.0f / 60.0f, traversal.collision());
        }
        for (const auto index : indices) {
            const auto& pickup = scene->pickups()[index];
            CAPTURE(pickup.name, pickup.position.x, pickup.position.y, pickup.position.z);
            CHECK(std::isfinite(pickup.position.x));
            CHECK(std::isfinite(pickup.position.y));
            CHECK(std::isfinite(pickup.position.z));
            const Vec3 stopped = pickup.position;
            scene->updateBossRewards(1.0f / 60.0f, traversal.collision());
            CHECK(scene->pickups()[index].position == stopped);
        }
        CHECK(device.texturesCreated == textures);
    }
}

TEST_CASE("native boss drops use ordinary physical collection and share shards only",
          "[legends][bosses][boss-rewards][assets]") {
    const auto root = rootOrSkip();
    for (const char* const name : {"mount6", "boss2"}) {
        CAPTURE(name);
        auto map = mapAt(root / "levels" / name);
        test::FakeRenderDevice device;
        auto scene =
            LegendsSceneAdapter::load(device, LegendsPlatform::N64, root / "levels" / name, map, 2);
        const auto indices = rewards(*scene);
        REQUIRE_FALSE(indices.empty());
        PlayerActor first;
        PlayerActor second;
        const CharacterSave initial;
        first.spawn(0, initial, nullptr, {10000, 10000, 10000}, 0);
        second.spawn(1, initial, nullptr, {10000, 10000, 10000}, 0);
        const auto gdlBefore = first.save().progress().relics;
        std::array recipients{&first.save(), &second.save()};
        CHECK(scene->collect(first, 1, true, {}, recipients).empty());
        CHECK(first.save().progress().inventory.powerupCount() == 0);
        CHECK(first.save().progress().legendsProgress(LegendsPlatform::N64).shards == 0);
        scene->spawnBossRewards({0, 0, 0}, {0, 0, 30});
        usize granted = 0;
        for (const auto index : indices) {
            first.place(scene->pickups()[index].position);
            scene->collect(
                first, 1, true,
                [&](const LegendsPickup& pickup, const ItemTaking& taking) {
                    if (pickup.nativeIndex >= 0x80000000U && taking.took()) {
                        ++granted;
                    }
                },
                recipients);
        }
        CHECK(granted == indices.size());
        for (const auto index : indices) {
            CHECK(scene->pickups()[index].collected);
        }
        CHECK(first.save().progress().relics == gdlBefore);
        if (std::string_view{name} == "mount6") {
            CHECK(first.save().progress().legendsProgress(LegendsPlatform::N64).shards == 1);
            CHECK(second.save().progress().legendsProgress(LegendsPlatform::N64).shards == 1);
            CHECK(first.save().progress().legendsProgress(LegendsPlatform::Dreamcast).shards == 0);
        } else {
            CHECK(first.save().progress().inventory.powerupCount() == 4);
            CHECK(second.save().progress().inventory.powerupCount() == 0);
        }
        scene->spawnBossRewards({0, 0, 0}, {0, 0, 30});
        CHECK(scene->collect(first, 1, true, {}, recipients).empty());
    }
}

} // namespace
