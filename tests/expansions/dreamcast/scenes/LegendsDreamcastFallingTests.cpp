#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast F1 reported rockfall actually moves and retires after native contact",
          "[legends][dreamcast-falling][dreamcast-f1][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelf1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto scene = formats::LegendsDreamcastScene::parse(bytes);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(adapter);
    constexpr s32 kObjectId = 16777228;
    const auto before = adapter->fixtureObject(kObjectId);
    REQUIRE(before);
    REQUIRE(before->name == "F1ROCKFALL#06");
    REQUIRE(before->visible);
    CHECK(before->position == scene.itemInstances.at(12).position);
    adapter->update(1);
    CHECK(adapter->fixtureObject(kObjectId)->position == before->position);
    adapter->takeSounds();
    const std::array visitor{LegendsPortalVisitor{.position = before->position}};
    adapter->touchSwitches(visitor);
    const auto sounds = adapter->takeSounds();
    CHECK(std::ranges::any_of(sounds, [](const auto& sound) { return sound.token == 0x2d001e; }));
    adapter->touchSwitches(visitor);
    CHECK(std::ranges::none_of(adapter->takeSounds(),
                               [](const auto& sound) { return sound.token == 0x2d001e; }));
    adapter->capturePresentation();
    adapter->update(1.f / 30);
    const auto falling = adapter->fixtureObject(kObjectId);
    REQUIRE(falling);
    CHECK(falling->visible);
    CHECK(std::abs(falling->position.y - (before->position.y - 2.f / 30)) < .0001f);
    CHECK(falling->position.x == before->position.x);
    CHECK(falling->position.z == before->position.z);
    adapter->update(10);
    const auto removed = adapter->fixtureObject(kObjectId);
    REQUIRE(removed);
    CHECK_FALSE(removed->visible);
}

TEST_CASE("Dreamcast F1ROCKSINK_NEW2 sinks on contact and disappears below the native sentinel",
          "[legends][dreamcast-falling][dreamcast-f1][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelf1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    const auto scene = formats::LegendsDreamcastScene::parse(bytes);
    const auto& item = scene.itemInstances.at(20);
    REQUIRE(item.name == "F1ROCKSINK_NEW2");
    const auto& info = scene.itemInfos.at(item.info);
    REQUIRE(info.subtype == 53);
    REQUIRE(item.triangleCount == 0);
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    constexpr s32 kId = 16777236;
    REQUIRE(adapter->fixtureObject(kId));
    REQUIRE(adapter->fixtureObject(kId)->name == item.name);
    CHECK(adapter->fixtureObject(kId)->position == item.position);
    adapter->update(1);
    CHECK(adapter->fixtureObject(kId)->position == item.position);
    adapter->takeSounds();
    adapter->touchSwitches(std::array{LegendsPortalVisitor{.position = item.position}});
    const auto sounds = adapter->takeSounds();
    CHECK(std::ranges::any_of(sounds, [](const auto& sound) { return sound.token == 0x2d001e; }));
    adapter->capturePresentation();
    adapter->update(1.f / 30);
    CHECK(adapter->fixtureObject(kId)->visible);
    CHECK(std::abs(adapter->fixtureObject(kId)->position.y - (item.position.y - 2.f / 30)) <
          .0001f);
    adapter->update(10);
    CHECK_FALSE(adapter->fixtureObject(kId)->visible);
}
} // namespace
