#include <array>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/characters/AutoSave.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("autosave assigns free slots once and reuses each character's slot", "[save][autosave]") {
    SaveSlots slots;
    REQUIRE(slots.open(test::scratchDirectory("autosave-reuse"), 5));
    std::array<PartyMember, 2> party{};
    party[0].player = 3;
    party[0].save.name = "FIRST";
    party[0].save.gold = 750;
    party[1].player = 1;
    party[1].save.name = "SECOND";
    party[1].save.gold = 900;
    party[1].slot = 0; // Protect even a claimed slot whose file does not exist yet.
    auto result = autoSaveParty(slots, party);
    CHECK(result.written == 2);
    CHECK(result.failed == 0);
    CHECK(result.unavailable == 0);
    CHECK(party[0].slot == 1);
    CHECK(party[1].slot == 0);
    party[0].save.gold = 123;
    result = autoSaveParty(slots, party);
    CHECK(result.written == 2);
    CHECK(party[0].slot == 1);
    CHECK_FALSE(slots.slot(2).occupied);
    const auto loaded = slots.load(1);
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "FIRST");
    CHECK(loaded->gold == 123);
}

TEST_CASE("autosave leaves occupied and corrupt slots alone when the card is full",
          "[save][autosave]") {
    SaveSlots slots;
    REQUIRE(slots.open(test::scratchDirectory("autosave-full"), 2));
    CharacterSave other;
    other.name = "OTHER";
    REQUIRE(slots.write(0, other));
    writeTextFile(slots.path(1), "unreadable character data");
    std::array<PartyMember, 1> party{};
    const auto result = autoSaveParty(slots, party);
    CHECK(result.written == 0);
    CHECK(result.unavailable == 1);
    CHECK_FALSE(party[0].slot);
    CHECK(readTextFile(slots.path(0)) == other.toJson());
    CHECK(readTextFile(slots.path(1)) == "unreadable character data");
}

TEST_CASE("autosave refuses invalid or multiply owned slots without reallocating them",
          "[save][autosave]") {
    SaveSlots slots;
    REQUIRE(slots.open(test::scratchDirectory("autosave-conflict"), 3));
    std::array<PartyMember, 3> party{};
    party[0].slot = 1;
    party[1].slot = 1;
    party[2].slot = 10;
    const auto result = autoSaveParty(slots, party);
    CHECK(result.written == 0);
    CHECK(result.unavailable == 3);
    CHECK_FALSE(slots.anySaved());
    CHECK(party[0].slot == 1);
    CHECK(party[2].slot == 10);
}

TEST_CASE("failed autosaves do not associate a character with the failed slot",
          "[save][autosave]") {
    SaveSlots slots;
    const auto directory = test::scratchDirectory("autosave-failure");
    REQUIRE(slots.open(directory, 2));
    std::filesystem::remove(directory);
    writeTextFile(directory, "not a directory");
    std::array<PartyMember, 1> party{};
    const auto result = autoSaveParty(slots, party);
    CHECK(result.written == 0);
    CHECK(result.failed + result.unavailable == 1);
    CHECK_FALSE(party[0].slot);
    CHECK(readTextFile(directory) == "not a directory");
    party[0].slot = 0;
    const auto retry = autoSaveParty(slots, party);
    CHECK(retry.failed == 1);
    CHECK(retry.written == 0);
    CHECK(party[0].slot == 0);
}

TEST_CASE("autosave reports an unavailable save directory without mutating the party",
          "[save][autosave]") {
    SaveSlots slots;
    std::array<PartyMember, 1> party{};
    const auto result = autoSaveParty(slots, party);
    CHECK(result.unavailable == 1);
    CHECK(result.written == 0);
    CHECK_FALSE(party[0].slot);
}
} // namespace
