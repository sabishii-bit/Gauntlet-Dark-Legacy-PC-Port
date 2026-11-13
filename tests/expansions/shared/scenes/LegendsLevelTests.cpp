#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/characters/LegendsDreamcastVitality.h"
#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/characters/Vitality.h"

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
    CHECK(level.healthLimit() == kLegendsDreamcastHealthLimit);
}

TEST_CASE("an N64 level, whose game holds no fixed limit, keeps the base game's",
          "[legends][n64][health-limit][assets]") {
    // e0054fdc stops health at each character's own most (entry 0xD), never at a fixed number.
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
    CHECK(level.healthLimit() == kHealthLimit);
}

} // namespace
