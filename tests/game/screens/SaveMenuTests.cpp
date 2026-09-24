#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/screens/SaveMenu.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Fixture {
    std::filesystem::path directory = test::scratchDirectory("save-menu");
    SaveSlots slots;
    SaveMenu menu;
    std::array<PartyMember, 2> party;
    Fixture() {
        REQUIRE(slots.open(directory, 8));
        party[0].player = 2;
        party[0].save.name = "HERO";
        party[0].save.gold = 300;
        party[1].player = 0;
        party[1].save.name = "OTHER";
        party[1].slot = 1;
        REQUIRE(slots.write(1, party[1].save));
    }
    void open(SaveMenu::Mode mode) { menu.open(slots, party, 2, mode, nullptr); }
};
TEST_CASE("manual saving associates a slot only after success and protects party slots",
          "[save-menu]") {
    Fixture f;
    f.open(SaveMenu::Mode::Save);
    REQUIRE_FALSE(f.menu.available(1));
    f.menu.choose(1);
    CHECK(f.menu.state() == SaveMenu::State::Slots);
    f.menu.choose(0);
    REQUIRE(f.menu.succeeded());
    REQUIRE(f.menu.state() == SaveMenu::State::Notice);
    CHECK(f.menu.party()[0].slot == 0);
    CHECK_FALSE(f.party[0].slot.has_value());
    CharacterSave loaded;
    REQUIRE(f.slots.load(0, loaded));
    CHECK(loaded.gold == 300);
    f.menu.back();
    CHECK(f.menu.state() == SaveMenu::State::Done);
}
TEST_CASE("overwriting a character requires confirmation including unreadable saves",
          "[save-menu]") {
    Fixture f;
    writeTextFile(f.slots.path(0), "broken file");
    f.open(SaveMenu::Mode::Save);
    REQUIRE(f.slots.slot(0).occupied);
    REQUIRE_FALSE(f.slots.slot(0).exists);
    f.menu.choose(0);
    REQUIRE(f.menu.state() == SaveMenu::State::Confirm);
    f.menu.choose(0);
    CHECK(readTextFile(f.slots.path(0)) == "broken file");
    f.menu.choose(0);
    f.menu.choose(1);
    REQUIRE(f.menu.succeeded());
    CHECK(f.slots.slot(0).exists);
}
TEST_CASE("loading replaces only the selected character after confirmation", "[save-menu]") {
    Fixture f;
    CharacterSave saved;
    saved.name = "LOADED";
    saved.gold = 99;
    REQUIRE(f.slots.write(0, saved));
    f.party[0].fallen = true;
    f.party[0].turbo = 10;
    f.party[0].helpHeard = {5};
    f.open(SaveMenu::Mode::Load);
    CHECK_FALSE(f.menu.available(1));
    CHECK_FALSE(f.menu.available(7));
    f.menu.choose(0);
    REQUIRE(f.menu.state() == SaveMenu::State::Confirm);
    CHECK_FALSE(f.menu.definition().body.empty());
    CHECK(f.menu.party()[0].save.name == "HERO");
    f.menu.choose(1);
    REQUIRE(f.menu.succeeded());
    CHECK(f.menu.party()[0].save.toJson() == saved.toJson());
    CHECK_FALSE(f.menu.party()[0].fallen);
    CHECK(f.menu.party()[0].turbo == 0);
    CHECK(f.menu.party()[0].helpHeard.empty());
    CHECK(f.menu.party()[1].save.toJson() == f.party[1].save.toJson());
    CHECK(readTextFile(f.slots.path(0)) == saved.toJson());
}
TEST_CASE("a save changing on disk or a failed write leaves the party untouched", "[save-menu]") {
    Fixture f;
    SECTION("load failure") {
        REQUIRE(f.slots.write(0, f.party[0].save));
        f.open(SaveMenu::Mode::Load);
        f.menu.choose(0);
        writeTextFile(f.slots.path(0), "corrupt");
        f.menu.choose(1);
    }
    SECTION("write failure") {
        std::filesystem::create_directory(f.slots.path(0));
        writeTextFile(f.slots.path(0) / "keep", "untouched");
        f.open(SaveMenu::Mode::Save);
        f.menu.choose(0);
        f.menu.choose(1);
        CHECK(readTextFile(f.slots.path(0) / "keep") == "untouched");
    }
    REQUIRE_FALSE(f.menu.succeeded());
    CHECK_FALSE(f.menu.party()[0].slot.has_value());
    CHECK(f.menu.party()[0].save.toJson() == f.party[0].save.toJson());
    f.menu.back();
    CHECK(f.menu.state() == SaveMenu::State::Slots);
}
TEST_CASE("save slots paginate and invalid owners cannot perform operations", "[save-menu]") {
    Fixture f;
    f.open(SaveMenu::Mode::Save);
    CHECK(f.menu.definition().items.front().code == 0);
    f.menu.choose(-3);
    CHECK(f.menu.definition().items.front().code == 6);
    f.menu.choose(7);
    REQUIRE(f.menu.succeeded());
    f.menu.open(f.slots, f.party, 3, SaveMenu::Mode::Save, nullptr);
    CHECK(f.menu.state() == SaveMenu::State::Done);
    f.menu.choose(0);
    CHECK_FALSE(f.menu.succeeded());
}
} // namespace
