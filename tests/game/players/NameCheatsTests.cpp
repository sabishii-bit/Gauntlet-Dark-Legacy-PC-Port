#include <array>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/players/CharacterSave.h"
#include "game/players/Inventory.h"
#include "game/players/NameCheats.h"
#include "game/players/PowerupEffects.h"
#include "game/screens/PlayerPowerups.h"
#include "game/screens/PlayerRuntime.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("name codes grant the retail permanent powerup rows", "[game][cheats][players]") {
    // set_hidden_player (8007B558), Cheats at 801209E4: eighteen rows,
    // fifteen powerups, two ALLFUL counts and one gold assignment.
    struct Expected {
        std::string_view name;
        s32 kind;
        f32 charge;
        u32 flags;
    };
    constexpr std::array<Expected, 15> kRows{{
        {"INVULN", 6, 0, 65536},
        {"SSHOTS", 5, -1, 1048576},
        {"EGG911", 9, 0, 1024},
        {"1ANGEL", 9, 0, 1},
        {"1ANGEL", 6, 0, 524288},
        {"DELTA1", 9, 0, 768},
        {"000000", 9, 0, 4},
        {"PEEKIN", 9, 0, 2},
        {"PURPLE", 9, 0, 524288},
        {"XSPEED", 9, 4, 65536},
        {"QCKSHT", 5, 0, 536870912},
        {"MENAGE", 5, 0, 524288},
        {"REFLEX", 5, 0, 2097152},
        {"NOVATO", 9, 0, 8},
        {"MEBERT", 9, 0, 128},
    }};
    for (const auto& row : kRows) {
        CAPTURE(row.name, row.kind, row.flags);
        CharacterSave save;
        save.name = row.name;
        save.character = 7;
        save.color = 2;
        REQUIRE(applyNameCheats(save));
        CHECK(save.character == 7);
        CHECK(save.color == 2);
        const auto* slot = save.progress().inventory.powerup(row.kind, row.flags);
        REQUIRE(slot);
        CHECK(slot->flags == row.flags);
        CHECK(slot->charge == row.charge);
        CHECK(slot->strength < 0);
        save.progress().inventory.advance(10000);
        CHECK(slot->working());
        const auto roundTrip = CharacterSave::fromJson(save.toJson());
        CHECK(roundTrip.progress().inventory == save.progress().inventory);
        CHECK(save.classes[0].inventory.powerupCount() == 0);
    }
}

TEST_CASE("paired codes grant every matching row without overwriting other inventory",
          "[game][cheats][players]") {
    CharacterSave save;
    save.name = "1ANGEL";
    save.progress().inventory.addPowerup(5, powerup::kThreeWayShot, 0, 60);
    REQUIRE(applyNameCheats(save));
    const auto effects = PowerupEffects::of(save.progress().inventory);
    CHECK(effects.levitating());
    CHECK((effects.armor & 0x80000) != 0);
    CHECK(effects.shots() == 3);
    CHECK(save.progress().inventory.powerupCount() == 3);
    REQUIRE(applyNameCheats(save));
    CHECK(save.progress().inventory.powerupCount() == 3);
}

TEST_CASE("gold and supply name codes restore fixed counts rather than adding rewards",
          "[game][cheats][players]") {
    CharacterSave save;
    save.name = "10000K";
    save.gold = 90000;
    REQUIRE(applyNameCheats(save));
    CHECK(save.gold == 10000);
    save.name = "ALLFUL";
    auto& inventory = save.progress().inventory;
    inventory.keys = 2;
    inventory.addPotions(4, 2);
    REQUIRE(applyNameCheats(save));
    CHECK(inventory.keys == 9);
    REQUIRE(inventory.potions.size() == 9);
    CHECK(inventory.potions[0] == 4);
    CHECK(inventory.potions[1] == 4);
    for (usize i = 2; i < inventory.potions.size(); ++i) {
        CHECK(inventory.potions[i] == static_cast<s32>(i % 4));
    }
    REQUIRE(applyNameCheats(save));
    CHECK(inventory.potions.size() == 9);
    CHECK(save.gold == 10000);
}

TEST_CASE("reloading a name code preserves toggles and other classes' records",
          "[game][cheats][players]") {
    CharacterSave save;
    save.name = "INVULN";
    save.character = 4;
    save.classes[0].inventory.keys = 3;
    REQUIRE(applyNameCheats(save));
    save.progress().inventory.powerups[0].on = false;
    save = CharacterSave::fromJson(save.toJson());
    REQUIRE(applyNameCheats(save));
    CHECK(save.progress().inventory.powerupCount() == 1);
    CHECK_FALSE(save.progress().inventory.powerups[0].on);
    CHECK(save.progress().inventory.powerups[0].strength < 0);
    CHECK(PowerupEffects::of(save.progress().inventory).armor == 0);
    CHECK(save.classes[0].inventory.keys == 3);
    CHECK(save.classes[0].inventory.powerupCount() == 0);

    // Native PlayerAddPowerup accumulates a positive charge even on permanent
    // slots; XSPEED's special-kind charge is not a movement-speed stat bonus.
    save.name = "XSPEED";
    REQUIRE(applyNameCheats(save));
    REQUIRE(applyNameCheats(save));
    const auto* speed = save.progress().inventory.powerup(9, powerup::kSpeedBoost);
    REQUIRE(speed);
    CHECK(speed->charge == 8);
    CHECK(PowerupEffects::of(save.progress().inventory).paceAdd == 0);
}

TEST_CASE("ordinary names and developer-only codes do not silently enable cheats",
          "[game][cheats][players]") {
    for (const std::string_view name : {"TEST", "invuln", "INVUL", "INVULNX", "NAK069", "MNTHRX",
                                        "ARIENT", "AAAAAA", "ADMBLY", "NICO"}) {
        CAPTURE(name);
        CharacterSave save;
        save.name = name;
        const auto before = save.toJson();
        CHECK_FALSE(applyNameCheats(save));
        CHECK(save.toJson() == before);
        CHECK(hiddenCostume(name) == nullptr);
    }
}

TEST_CASE("hidden name codes choose their authored class colour and persistent appearance",
          "[game][cheats][players]") {
    REQUIRE(hiddenCostumes().size() == 26);
    for (const auto& costume : hiddenCostumes()) {
        CAPTURE(costume.name);
        CharacterSave save;
        save.name = costume.name;
        save.character = 15;
        save.color = 0;
        REQUIRE(applyNameCheats(save));
        CHECK(save.character == costume.character);
        CHECK(save.color == costume.color);
        CHECK(save.progress().inventory.powerupCount() == 0);
        const auto loaded = CharacterSave::fromJson(save.toJson());
        REQUIRE(hiddenCostume(loaded.name));
        CHECK(hiddenCostume(loaded.name)->directory == costume.directory);
    }
    REQUIRE(hiddenCostume("DBRNKR"));
    CHECK(hiddenCostume("DBRNKR") == hiddenCostume("AYA555"));
    CHECK(hiddenCostume("SUM224")->character == 2);
}

TEST_CASE("permanent turbo and super shot cheats use normal gameplay consumers",
          "[game][cheats][powerups]") {
    std::array<PlayerRuntime, 1> players;
    CharacterSave save;
    save.name = "PURPLE";
    REQUIRE(applyNameCheats(save));
    REQUIRE(save.progress().inventory.powerups[0].working());
    save.progress().inventory.powerups[0].on = false;
    REQUIRE(applyNameCheats(save)); // An explicit cheat grant enables its permanent slot again.
    REQUIRE(save.progress().inventory.powerups[0].working());
    players[0].actor.spawn(3, save, nullptr, {}, 0);
    players[0].life = PlayerLife::Standing;
    for (s32 i = 0; i < 3; ++i) {
        PlayerPowerups::update(players, 1, PlayerPowerups::Clock::Level);
        CHECK(players[0].turbo.held() == TurboMeter::kFull);
        players[0].turbo.spend(TurboMeter::kFull);
    }
    save.name = "SSHOTS";
    REQUIRE(applyNameCheats(save));
    for (s32 i = 0; i < 100; ++i) {
        CHECK(save.progress().inventory.spendPowerup(5, powerup::kSuperShot));
    }
    const auto* shot = save.progress().inventory.powerup(5, powerup::kSuperShot);
    REQUIRE(shot);
    CHECK(shot->charge == -1);
}

TEST_CASE("hidden costume class selection uses that class's wallet",
          "[game][cheats][class-wallet]") {
    CharacterSave save;
    save.name = "ICE600";
    save.gold = 350;
    save.classes[4].gold = 90;
    REQUIRE(applyNameCheats(save));
    CHECK(save.character == 4);
    CHECK(save.gold == 90);
    CHECK(save.classes[0].gold == 350);
    save.gold = 105;
    REQUIRE(applyNameCheats(save));
    CHECK(save.gold == 105);
    save.selectClass(0);
    CHECK(save.gold == 350);
}

} // namespace
