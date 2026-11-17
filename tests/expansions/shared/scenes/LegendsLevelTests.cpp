#include <array>
#include <filesystem>
#include <memory>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/characters/LegendsDreamcastVitality.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/n64/formats/LegendsN64Portal.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/Vitality.h"
#include "game/config/GameConfig.h"
#include "game/level/ImportedLevel.h"
#include "game/players/PlayerActor.h"

namespace {

using namespace gdl;
using namespace gdl::game;

TEST_CASE("a Dreamcast level holds health to the Dreamcast game's own limit",
          "[legends][dreamcast][health-limit][assets]") {
    // Its status update, 8C0C16E0, stores its 9999.0 (8C0C186C) over any health above it.
    CHECK(kLegendsDreamcastHealthLimit == 9999);
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    test::FakeRenderDevice device;
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    const LegendsLevel level(device, map, std::move(adapter), directory, test::dataDirectory());
    for (const s32 characterLevel : {1, 10, 99}) {
        CAPTURE(characterLevel);
        CHECK(level.healthLimit(characterLevel) == kLegendsDreamcastHealthLimit);
    }
}

TEST_CASE("an N64 level holds health to each character's own most, as the N64 game does",
          "[legends][n64][health-limit][assets]") {
    // e0054fdc stores inventory entry 0xD, the character's own most, over more on every gain.
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const LegendsLevel level(device, map, std::move(adapter), directory, characters);
    CHECK(level.healthLimit(1) == 500);
    CHECK(level.healthLimit(10) == 1400);
    CHECK(level.healthLimit(10) == mostHealth(10));
    CHECK(level.healthLimit(99) == kHealthLimit);
}

TEST_CASE("a Dreamcast level names the Dreamcast HUD look as its own",
          "[legends][dreamcast][hud-style][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::is_regular_file(directory / "worlds.rom")) {
        SKIP("Dreamcast extraction is not installed");
    }
    test::FakeRenderDevice device;
    const auto objects = readFile(directory / "objects.rom");
    auto map = formats::LegendsMap::parseDreamcast(readFile(directory / "worlds.rom"), objects);
    map.textures =
        formats::decodeLegendsDreamcastTextures(objects, readFile(directory / "textures.rom"));
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    const LegendsLevel level(device, map, std::move(adapter), directory, test::dataDirectory());
    REQUIRE(level.platform() == LegendsPlatform::Dreamcast);
    // Asked as the scene asks any imported level.
    const ImportedLevel& imported = level;
    CHECK(imported.hudStyleId() == kLegendsDreamcastHudStyle);
    CHECK(imported.hudStyleId() == legendsHudStyleOf(level.platform()));
    CHECK(imported.hudStyleId() != kDarkLegacyHudStyle);
}

TEST_CASE("an N64 level names the N64 HUD look as its own", "[legends][n64][hud-style][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    const LegendsLevel level(device, map, std::move(adapter), directory, characters);
    REQUIRE(level.platform() == LegendsPlatform::N64);
    const ImportedLevel& imported = level;
    CHECK(imported.hudStyleId() == kLegendsN64HudStyle);
    CHECK(imported.hudStyleId() == legendsHudStyleOf(level.platform()));
    CHECK(imported.hudStyleId() != kDarkLegacyHudStyle);
}

TEST_CASE("an N64 level with no exit under its party names none to the HUD",
          "[legends][n64][hud-style]") {
    const auto missing = test::scratchDirectory("legends-level-no-exits");
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    formats::LegendsMapObject object;
    object.mesh.vertices = {{{0, 0, 0}}, {{1, 0, 0}}, {{0, 0, 1}}};
    object.mesh.parts.push_back({0, 0, {0, 1, 2}});
    map.objects.push_back(std::move(object));
    LegendsLevel level(device, map, std::make_unique<LegendsSceneAdapter>(),
                       missing / "levels/ice1", missing);
    REQUIRE(level.platform() == LegendsPlatform::N64);
    const ImportedLevel& imported = level;
    CHECK(imported.hudStyleId() == kLegendsN64HudStyle);
    // Neither before the party is first seen nor after: the level has no exit to stand on.
    for (s32 seat = 0; seat < 4; ++seat) {
        CAPTURE(seat);
        CHECK_FALSE(imported.exitUnder(seat));
    }
    const CharacterSave save;
    PlayerActor actor;
    actor.spawn(0, save, nullptr, Vec3{0}, 0);
    std::array<PlayerActor*, 1> party{&actor};
    level.updateParty(party, 1.0f / 60, false);
    for (s32 seat = 0; seat < 4; ++seat) {
        CAPTURE(seat);
        CHECK_FALSE(imported.exitUnder(seat));
    }
}

TEST_CASE("an N64 level answers its exits for the HUD", "[legends][n64][hud-style][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("Native N64 extraction is not installed");
    }
    const auto characters = test::assetOrSkip("PLAYERS").parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    REQUIRE(adapter->portals().size() == 24);
    // The tower's exits to the mountain's first two levels, which game.bin's codes name.
    const formats::LegendsN64Portal first = adapter->portals()[5];
    const formats::LegendsN64Portal second = adapter->portals()[6];
    REQUIRE(first.map == "mount1");
    REQUIRE(second.map == "mount2");
    CHECK(first.destination == 0x0201U);
    CHECK(second.destination == 0x0202U);
    LegendsLevel level(device, map, std::move(adapter), directory, characters);
    const ImportedLevel& imported = level;
    CHECK(imported.hudStyleId() == kLegendsN64HudStyle);
    for (s32 seat = 0; seat < 4; ++seat) {
        CAPTURE(seat);
        CHECK_FALSE(imported.exitUnder(seat));
    }

    // Seat 1 on the first exit, seat 0 a little way off: seat 1 waits there for seat 0.
    const CharacterSave save;
    PlayerActor beside;
    PlayerActor on;
    beside.spawn(0, save, nullptr, first.position + Vec3{12, 0, 0}, 0);
    on.spawn(1, save, nullptr, first.position, 0);
    std::array<PlayerActor*, 2> party{&beside, &on};
    level.updateParty(party, 1.0f / 60, false);
    REQUIRE(level.adapter().portalUnlocked(5));
    auto under = imported.exitUnder(1);
    REQUIRE(under);
    CHECK(under->code == first.destination);
    CHECK(under->waiting);
    for (const s32 seat : {0, 2, 3}) {
        CAPTURE(seat);
        CHECK_FALSE(imported.exitUnder(seat));
    }

    // Both on the second, which a new party cannot take yet: named all the same, as the N64
    // names a locked pad, and nobody waits.
    REQUIRE_FALSE(level.adapter().portalUnlocked(6));
    beside.place(second.position);
    on.place(second.position);
    level.updateParty(party, 1.0f / 60, false);
    for (const s32 seat : {0, 1}) {
        CAPTURE(seat);
        under = imported.exitUnder(seat);
        REQUIRE(under);
        CHECK(under->code == second.destination);
        CHECK_FALSE(under->waiting);
    }
}

} // namespace
