#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/world/LevelCatalog.h"

namespace {

using namespace gdl;
using namespace gdl::game;

/** Two realms: a castle whose second level is its sixth by name, and a town. */
std::filesystem::path sampleRealms(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    std::filesystem::create_directories(dir / "wdata");
    writeTextFile(dir / "wdata/CASTLE.json", R"({"realm": 1, "prefix": "levelA", "levels": [
      {"name": "A1", "title": "Courtyard"}, {"name": "A6", "title": "Dungeon"},
      {"name": "A2", "title": "Barracks"}]})");
    writeTextFile(dir / "wdata/TOWN.json", R"({"realm": 7, "prefix": "levelG", "levels": [
      {"name": "G1", "title": "Fields"}]})");
    writeTextFile(dir / "wdata/BROKEN.json", "{nope");
    writeTextFile(dir / "wdata/EMPTY.json", R"({"realm": 3, "prefix": "levelC", "levels": []})");
    return dir;
}

TEST_CASE("an exit's tag names a realm by its letter and counts into the realm's own order",
          "[game][world][levels]") {
    LevelCatalog catalog;
    REQUIRE_FALSE(catalog.loaded());
    REQUIRE_FALSE(catalog.load(test::scratchDirectory("levels-none")));
    const auto root = sampleRealms("levels-sample");
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.realms().size() == 2);
    REQUIRE(catalog.realms()[0].file == "CASTLE"); // by realm id

    const auto fields = catalog.byTag("g1");
    REQUIRE(fields.has_value());
    REQUIRE(fields->realm == "TOWN");
    REQUIRE(fields->realmId == 7);
    REQUIRE(fields->name == "G1");
    REQUIRE(fields->title == "Fields");
    REQUIRE(fields->directory == "LEVELS/LEVELG1");
    REQUIRE(fields->items == "ITEMS/LEVELG");
    REQUIRE(fields->worldDataFile() == "wdata/TOWN.json");
    REQUIRE_FALSE(fields->isTower());
    // The castle's second is its dungeon, A6.
    REQUIRE(catalog.byTag("a2")->name == "A6");
    REQUIRE(catalog.byTag("A2")->directory == "LEVELS/LEVELA6");
    REQUIRE(catalog.byTag("a3")->name == "A2");
    REQUIRE_FALSE(catalog.byTag("a4").has_value()); // past the realm's levels
    REQUIRE_FALSE(catalog.byTag("z1").has_value());
    REQUIRE_FALSE(catalog.byTag("g").has_value());
    REQUIRE_FALSE(catalog.byTag("g0").has_value());
    REQUIRE(catalog.byName("a6")->title == "Dungeon");
    REQUIRE_FALSE(catalog.byName("Q9").has_value());

    REQUIRE_FALSE(LevelCatalog::unpacked(root, *fields));
    std::filesystem::create_directories(root / "LEVELS/LEVELG1");
    writeTextFile(root / "LEVELS/LEVELG1/world.json", "{}");
    REQUIRE(LevelCatalog::unpacked(root, *fields));

    const LevelRef tower = LevelRef::tower();
    REQUIRE(tower.isTower());
    REQUIRE(tower.directory == "LEVELS/LEVELL1");
    REQUIRE(tower == LevelRef::tower());
    REQUIRE_FALSE(tower == *fields);
}

TEST_CASE("the unpacked realm data finds the tower's first portals' levels",
          "[game][world][levels][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("wdata/TOWN.json").parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(catalog.realms().size() >= 13);
    REQUIRE(catalog.byTag("g1")->title == "Fields");
    REQUIRE(catalog.byTag("a2")->name == "A6");
    REQUIRE(catalog.byTag("l1")->isTower());
    REQUIRE(*catalog.byTag("l1") == LevelRef::tower());
}

} // namespace
