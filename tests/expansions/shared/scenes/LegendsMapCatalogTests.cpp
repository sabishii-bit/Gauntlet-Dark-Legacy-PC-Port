#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/shared/scenes/LegendsMapCatalog.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::filesystem::path mapAt(const std::filesystem::path& levels, std::string_view name,
                            LegendsPlatform platform = LegendsPlatform::N64) {
    const auto directory = levels / name;
    std::filesystem::create_directories(directory);
    writeTextFile(directory / (platform == LegendsPlatform::N64 ? "worlds.wad" : "worlds.rom"),
                  "native world fixture");
    writeTextFile(directory / (platform == LegendsPlatform::N64 ? "objects.wad" : "objects.rom"),
                  "native objects fixture");
    return directory;
}
} // namespace

TEST_CASE("Legends map discovery follows configured assets independently of launch directory",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-configured");
    const auto map = mapAt(root / "Game Files/assets/n64/extracted/levels", "mount1");
    const auto entries =
        legendsMaps(LegendsPlatform::N64, root / "Game Files/assets/GUNE5D/Gauntlet",
                    root / "Other App/bin", root / "Unrelated Working Directory");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "mount1");
    CHECK(std::filesystem::equivalent(entries[0].directory, map));
}

TEST_CASE("Legends map discovery supports direct installed roots and build directory ancestors",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-executable");
    std::filesystem::path map;
    SECTION("installed extraction beside the executable") {
        map = mapAt(root / "Install Folder/n64/extracted/levels", "mount1");
    }
    SECTION("development assets beside an ancestor of the executable") {
        map = mapAt(root / "Install Folder/assets/n64/extracted/levels", "mount1");
    }
    const auto entries = legendsMaps(LegendsPlatform::N64, {}, root / "Install Folder/build/bin",
                                     root / "unrelated");
    REQUIRE(entries.size() == 1);
    CHECK(std::filesystem::equivalent(entries[0].directory, map));
}

TEST_CASE("Legends map discovery uses only the first usable catalog by location priority",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-priority");
    const auto configured = root / "configured";
    const auto executable = root / "installed";
    const auto working = root / "working";
    mapAt(executable / "assets/n64/extracted/levels", "executable_map");
    mapAt(working / "assets/n64/extracted/levels", "working_map");
    std::string expected;
    SECTION("configured assets outrank executable and working directories") {
        mapAt(configured / "n64/extracted/levels", "configured_map");
        expected = "configured_map";
    }
    SECTION("unusable configured extraction falls back to executable") {
        std::filesystem::create_directories(configured / "n64/extracted/levels/empty");
        expected = "executable_map";
    }
    const auto entries = legendsMaps(LegendsPlatform::N64, configured / "GUNE5D/Gauntlet",
                                     executable / "bin", working);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == expected);
}

TEST_CASE("Legends map discovery resolves relative assets against the supplied working directory",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-relative");
    const auto map = mapAt(root / "assets/n64/extracted/levels", "mount1");
    const auto entries = legendsMaps(LegendsPlatform::N64, "assets/GUNE5D/Gauntlet", {}, root);
    REQUIRE(entries.size() == 1);
    CHECK(std::filesystem::equivalent(entries[0].directory, map));
    CHECK(entries[0].directory.is_absolute());
}

TEST_CASE("Legends map discovery falls back to the working directory without duplicate entries",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-working");
    mapAt(root / "assets/n64/extracted/levels", "mount1");
    const auto entries = legendsMaps(LegendsPlatform::N64, {}, {}, root);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "mount1");
    const auto shared = legendsMaps(LegendsPlatform::N64, root / "assets", root / "./bin", root);
    REQUIRE(shared.size() == 1);
    CHECK(shared[0].directory == entries[0].directory);
    CHECK(legendsMaps(LegendsPlatform::N64, {}, {}, {}).empty());
}

TEST_CASE("Legends map catalogs ignore incomplete files and do not scan nested map trees",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-incomplete");
    const auto levels = root / "assets/n64/extracted/levels";
    mapAt(levels, "valid");
    auto missing = levels / "missing";
    std::filesystem::create_directories(missing);
    writeTextFile(missing / "worlds.wad", "world only");
    auto empty = mapAt(levels, "empty");
    writeTextFile(empty / "objects.wad", "");
    auto emptyWorld = mapAt(levels, "empty_world");
    writeTextFile(emptyWorld / "worlds.wad", "");
    auto directoryFile = levels / "directory_instead_of_file";
    std::filesystem::create_directories(directoryFile / "worlds.wad");
    writeTextFile(directoryFile / "objects.wad", "objects");
    mapAt(levels / "nested", "not_a_direct_map");
    mapAt(root / "elsewhere/n64/extracted/levels", "not_searched");
    const auto entries = legendsMaps(LegendsPlatform::N64, root / "assets", {}, root);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "valid");
}

TEST_CASE("Legends map discovery separates native platforms and handles disc casing",
          "[legends][catalog]") {
    const auto root = test::scratchDirectory("legends-catalog-case");
    const auto levels = root / "ASSETS/N64/EXTRACTED/LEVELS";
    mapAt(levels, "Zeta");
    const auto alpha = mapAt(levels, "alpha");
    std::filesystem::rename(alpha / "worlds.wad", alpha / "WORLDS.WAD");
    std::filesystem::rename(alpha / "objects.wad", alpha / "OBJECTS.WAD");
    mapAt(levels, "Beta");
    mapAt(levels, "wrong_platform", LegendsPlatform::Dreamcast);
    mapAt(root / "ASSETS/DREAMCAST/EXTRACTED/NATIVE/LEVELS", "levelA1", LegendsPlatform::Dreamcast);
    const auto entries =
        legendsMaps(LegendsPlatform::N64, root / "assets/GUNE5D/Gauntlet", {}, root);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].name == "alpha");
    CHECK(entries[1].name == "Beta");
    CHECK(entries[2].name == "Zeta");
    const auto dc = legendsMaps(LegendsPlatform::Dreamcast, root / "assets", {}, root);
    REQUIRE(dc.size() == 1);
    CHECK(dc[0].name == "levelA1");
}

TEST_CASE("Legends short names resolve case insensitively while explicit paths are preserved",
          "[legends][catalog]") {
    const std::vector<LegendsMapEntry> maps{{"mount1", "native maps/mount1"},
                                            {"levelA1", "disc maps/levelA1"}};
    CHECK(legendsMapPath(maps, "MoUnT1") == std::filesystem::path{"native maps/mount1"});
    CHECK(legendsMapPath(maps, "levela1") == std::filesystem::path{"disc maps/levelA1"});
    CHECK_FALSE(legendsMapPath(maps, "unknown"));
    CHECK_FALSE(legendsMapPath(maps, ""));
    for (const char* const value : {"./mount1", "../Other Maps/mount1", "C:\\Native Maps\\mount1",
                                    "/native/maps/mount1", "folder\\levelA1"}) {
        INFO(value);
        CHECK(legendsMapPath({}, value) == std::filesystem::path{value});
    }
    const std::vector<LegendsMapEntry> ambiguous{{"MAP", "upper"}, {"map", "lower"}};
    CHECK(legendsMapPath(ambiguous, "map") == std::filesystem::path{"lower"});
}

TEST_CASE("Quit destinations stay in the current platform's extraction and require native files",
          "[legends][catalog][native-progress-flow]") {
    const auto root = test::scratchDirectory("legends-quit-tower");
    const auto n64 = root / "n64/levels";
    const auto dc = root / "dreamcast/levels";
    const auto currentN64 = mapAt(n64, "mount1");
    const auto currentDc = mapAt(dc, "levela1", LegendsPlatform::Dreamcast);
    // A valid Tower in another extraction is never a fallback.
    const auto nativeTower = mapAt(n64, "tower2");
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::Dreamcast, currentDc));
    const auto discTower = mapAt(dc, "LEVELT1", LegendsPlatform::Dreamcast);
    REQUIRE(legendsTowerPath(LegendsPlatform::N64, currentN64));
    CHECK(std::filesystem::equivalent(*legendsTowerPath(LegendsPlatform::N64, currentN64),
                                      nativeTower));
    REQUIRE(legendsTowerPath(LegendsPlatform::Dreamcast, currentDc));
    CHECK(std::filesystem::equivalent(*legendsTowerPath(LegendsPlatform::Dreamcast, currentDc),
                                      discTower));
    CHECK(legendsTowerPath(LegendsPlatform::Dreamcast, currentDc / "") ==
          legendsTowerPath(LegendsPlatform::Dreamcast, currentDc));
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::N64, currentDc));
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::Dreamcast, currentN64));
    CHECK_FALSE(legendsTowerPath(static_cast<LegendsPlatform>(3), currentDc));
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::Dreamcast, {}));
    writeTextFile(discTower / "objects.rom", "");
    CHECK_FALSE(legendsTowerPath(LegendsPlatform::Dreamcast, currentDc));
}

TEST_CASE("Legends discovery catalogs the complete extracted native map corpus",
          "[legends][catalog][assets]") {
    const auto root = test::dataDirectory().parent_path();
    SECTION("N64") {
        if (!std::filesystem::is_directory(root / "assets/n64/extracted/levels")) {
            SKIP("N64 native extraction is not installed");
        }
        const auto entries =
            legendsMaps(LegendsPlatform::N64, root / "assets/GUNE5D/Gauntlet", {}, root / "build");
        CHECK(entries.size() == 34);
        CHECK(legendsMapPath(entries, "MOUNT1").has_value());
    }
    SECTION("Dreamcast") {
        if (!std::filesystem::is_directory(root / "assets/dreamcast/extracted/native/levels")) {
            SKIP("Dreamcast native extraction is not installed");
        }
        const auto entries = legendsMaps(LegendsPlatform::Dreamcast,
                                         root / "assets/GUNE5D/Gauntlet", {}, root / "build");
        CHECK(entries.size() == 41);
        CHECK(legendsMapPath(entries, "LEVELA1").has_value());
        const auto current = legendsMapPath(entries, "levela1");
        REQUIRE(current);
        CHECK(legendsTowerPath(LegendsPlatform::Dreamcast, *current) ==
              legendsMapPath(entries, "levelt1"));
    }
}
