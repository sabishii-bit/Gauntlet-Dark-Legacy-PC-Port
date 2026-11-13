#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/app/LegendsGateways.h"
#include "expansions/shared/characters/LegendsProgress.h"

namespace {
using namespace gdl;
using namespace gdl::game;

constexpr u8 kN64 = static_cast<u8>(LegendsPlatform::N64);
constexpr u8 kDreamcast = static_cast<u8>(LegendsPlatform::Dreamcast);

/** A small native file at `path`, or an empty one, its folders made. */
void fixture(const std::filesystem::path& path, std::string_view bytes = "native fixture") {
    std::filesystem::create_directories(path.parent_path());
    writeTextFile(path, bytes);
}

/** What an N64 extraction holds of its tower beyond the tower's world and objects. */
struct N64Parts {
    bool program = true; ///< _program/game.bin, whose stage table names the tower
    bool tables = true;  ///< _program/Tower.bin, the tower's own code with Sumner's menu
};

void n64At(const std::filesystem::path& extracted, N64Parts parts = {}) {
    fixture(extracted / "levels/tower2/worlds.wad");
    fixture(extracted / "levels/tower2/objects.wad");
    if (parts.program) {
        fixture(extracted / "_program/game.bin");
    }
    if (parts.tables) {
        fixture(extracted / "_program/Tower.bin");
    }
}

void dreamcastAt(const std::filesystem::path& extracted, bool textures = true) {
    const auto tower = extracted / "native/levels/levelt1";
    fixture(tower / "worlds.rom");
    fixture(tower / "objects.rom");
    if (textures) {
        fixture(tower / "textures.rom");
    }
}

/** The towers offered to a game installed in `install`, its data in `install/Gauntlet`. */
std::vector<GatewayWorld> worldsOf(const std::filesystem::path& install) {
    return legendsGatewayWorlds(install / "Gauntlet", install / "bin", install / "working");
}

/** Whether the N64's tower, then the Dreamcast's, is usable. */
std::vector<bool> usable(const std::vector<GatewayWorld>& worlds) {
    std::vector<bool> result;
    result.reserve(worlds.size());
    for (const GatewayWorld& world : worlds) {
        result.push_back(world.usable);
    }
    return result;
}
} // namespace

TEST_CASE("a GameCube-only installation is offered both Legends towers, neither usable",
          "[legends][gateways]") {
    const auto install = test::scratchDirectory("legends-gateways-none");
    std::filesystem::create_directories(install / "Gauntlet");
    const auto worlds = worldsOf(install);
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0] == GatewayWorld{kN64, "legends.n64.gateway.label", false});
    CHECK(worlds[1] == GatewayWorld{kDreamcast, "legends.dreamcast.gateway.label", false});
    // Asked of roots that are not there at all, it still answers.
    CHECK(usable(legendsGatewayWorlds(install / "missing", {}, install / "missing too")) ==
          std::vector<bool>{false, false});
}

TEST_CASE("a Legends tower is usable once its campaign's extraction holds it whole",
          "[legends][gateways]") {
    const auto install = test::scratchDirectory("legends-gateways-installed");
    std::vector<bool> expected;
    SECTION("the N64 campaign alone, where the launcher installs it") {
        n64At(install / "n64/extracted");
        expected = {true, false};
    }
    SECTION("the Dreamcast campaign alone") {
        dreamcastAt(install / "dreamcast/extracted");
        expected = {false, true};
    }
    SECTION("both, under a development checkout's assets") {
        n64At(install / "assets/n64/extracted");
        dreamcastAt(install / "assets/dreamcast/extracted");
        const auto worlds = legendsGatewayWorlds(install / "assets/GUNE5D/Gauntlet",
                                                 install / "build/bin", install / "working");
        CHECK(usable(worlds) == std::vector<bool>{true, true});
        expected = {true, true};
    }
    SECTION("both, the towers' folders named in capitals") {
        fixture(install / "n64/extracted/LEVELS/TOWER2/WORLDS.WAD");
        fixture(install / "n64/extracted/LEVELS/TOWER2/OBJECTS.WAD");
        fixture(install / "n64/extracted/_PROGRAM/GAME.BIN");
        fixture(install / "n64/extracted/_PROGRAM/TOWER.BIN");
        fixture(install / "dreamcast/extracted/NATIVE/LEVELS/LEVELT1/WORLDS.ROM");
        fixture(install / "dreamcast/extracted/NATIVE/LEVELS/LEVELT1/OBJECTS.ROM");
        fixture(install / "dreamcast/extracted/NATIVE/LEVELS/LEVELT1/TEXTURES.ROM");
        expected = {true, true};
    }
    const auto worlds = worldsOf(install);
    CHECK(usable(worlds) == expected);
    // The worlds and their names stay the campaigns' own, whatever is installed.
    REQUIRE(worlds.size() == 2);
    CHECK(worlds[0].id == kN64);
    CHECK(worlds[0].label == "legends.n64.gateway.label");
    CHECK(worlds[1].id == kDreamcast);
    CHECK(worlds[1].label == "legends.dreamcast.gateway.label");
}

TEST_CASE("a Legends tower missing what opening it needs is not usable, and hides no other",
          "[legends][gateways]") {
    const auto install = test::scratchDirectory("legends-gateways-broken");
    std::vector<bool> expected;
    SECTION("an N64 extraction without its stage table") {
        n64At(install / "n64/extracted", {.program = false});
        dreamcastAt(install / "dreamcast/extracted");
        expected = {false, true};
    }
    SECTION("an N64 extraction without its tower's own code") {
        n64At(install / "n64/extracted", {.tables = false});
        dreamcastAt(install / "dreamcast/extracted");
        expected = {false, true};
    }
    SECTION("an N64 extraction whose tables are empty") {
        n64At(install / "n64/extracted", {.program = false, .tables = false});
        fixture(install / "n64/extracted/_program/game.bin", "");
        fixture(install / "n64/extracted/_program/Tower.bin", "");
        dreamcastAt(install / "dreamcast/extracted");
        expected = {false, true};
    }
    SECTION("an N64 extraction with maps but not its tower") {
        fixture(install / "n64/extracted/levels/mount1/worlds.wad");
        fixture(install / "n64/extracted/levels/mount1/objects.wad");
        fixture(install / "n64/extracted/_program/game.bin");
        fixture(install / "n64/extracted/_program/Tower.bin");
        dreamcastAt(install / "dreamcast/extracted");
        expected = {false, true};
    }
    SECTION("a Dreamcast tower without its textures") {
        n64At(install / "n64/extracted");
        dreamcastAt(install / "dreamcast/extracted", false);
        expected = {true, false};
    }
    SECTION("campaigns the launcher has switched off") {
        n64At(install / "n64/extracted.off");
        dreamcastAt(install / "dreamcast/extracted.off");
        expected = {false, false};
    }
    CHECK(usable(worldsOf(install)) == expected);
}

TEST_CASE("the Legends towers are named in the user's own words", "[legends][gateways]") {
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto worlds = worldsOf(test::scratchDirectory("legends-gateways-names"));
    REQUIRE(worlds.size() == 2);
    REQUIRE(strings.has(worlds[0].label));
    REQUIRE(strings.has(worlds[1].label));
    CHECK(strings.get(worlds[0].label) == "Legend of What Once Was");
    CHECK(strings.get(worlds[1].label) == "Legend of What Could Have Been");
}
