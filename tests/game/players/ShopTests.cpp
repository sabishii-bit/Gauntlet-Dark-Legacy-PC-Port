#include <array>
#include <limits>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/ShopCatalog.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/players/LevelResults.h"
#include "game/players/ShopPurchase.h"
#include "game/screens/ShopSession.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
ShopCatalog catalog() {
    return ShopCatalog::fromJson(R"({"items":[
        {"texture":"","description":"EXIT","scale":1,"type":0,"price":0,"amount":0},
        {"texture":"KEY","description":"Key","scale":1,"type":1,"price":100,"amount":1},
        {"texture":"POT","description":"Potion","scale":1,"type":3,"price":250,"amount":1}
    ]})");
}
ClassDataSet classes() {
    const auto directory = test::scratchDirectory("shop-classes");
    writeTextFile(
        directory / "WAR.json",
        R"({"fight":[100,999],"speed":[100,999],"armor":[100,999],"magic":[100,999],"powerupTime":1.25})");
    writeTextFile(directory / "WIZ.json", readTextFile(directory / "WAR.json"));
    ClassDataSet data;
    REQUIRE(data.load(directory));
    return data;
}
ShopItem item(s32 type, s32 price = 100, s32 amount = 1) {
    return {"", "", 1, type, price, amount};
}
CharacterSave shopper() {
    CharacterSave save;
    save.gold = 5000;
    save.progress().health = 100;
    return save;
}
TEST_CASE("shop catalog requires an exit and rejects malformed rows", "[shop][assets-model]") {
    REQUIRE(catalog().items().size() == 3);
    REQUIRE(catalog().items()[1].price == 100);
    for (
        const auto* bad :
        {"{}", "null", "{\"items\":[]}",
         R"({"items":[{"texture":"","description":"x","scale":1,"type":1,"price":0,"amount":1}]})",
         R"({"items":[{"texture":"","description":"x","scale":-1,"type":0,"price":0,"amount":0}]})",
         R"({"items":[{"texture":"","description":"x","scale":1,"type":0,"price":-1,"amount":0}]})"}) {
        REQUIRE_THROWS_AS(ShopCatalog::fromJson(bad), FormatError);
    }
    ShopCatalog loaded;
    const auto directory = test::scratchDirectory("shop-catalog");
    writeTextFile(
        directory / "catalog.json",
        R"({"items":[{"texture":"","description":"EXIT","scale":1,"type":0,"price":0,"amount":0}]})");
    REQUIRE(loaded.load(directory / "catalog.json"));
    REQUIRE_FALSE(loaded.load(directory / "missing.json"));
    REQUIRE(loaded.items().empty());
}
TEST_CASE("shop money and capacity checks do not partially mutate saves", "[shop]") {
    auto save = shopper();
    const ClassStats stats;
    save.gold = 99;
    auto before = save.toJson();
    REQUIRE(buyShopItem(save, stats, item(1), 1) == ShopResult::InsufficientGold);
    REQUIRE(save.toJson() == before);
    save.gold = 500;
    save.progress().inventory.keys = 9;
    before = save.toJson();
    REQUIRE(buyShopItem(save, stats, item(1), 1) == ShopResult::Full);
    REQUIRE(save.toJson() == before);
    REQUIRE(buyShopItem(save, stats, item(3), 0) == ShopResult::Invalid);
    REQUIRE(save.toJson() == before);
    REQUIRE(buyShopItem(save, stats, item(40), 1) == ShopResult::Invalid);
    REQUIRE(buyShopItem(save, stats, item(-1), 1) == ShopResult::Invalid);
    REQUIRE(buyShopItem(save, stats, item(0, 0), 1) == ShopResult::Exit);
    REQUIRE(save.toJson() == before);
}
TEST_CASE("shop consumables buy one at a time and sell for three quarters", "[shop]") {
    auto save = shopper();
    const ClassStats stats;
    REQUIRE(buyShopItem(save, stats, item(1), 1) == ShopResult::Bought);
    REQUIRE(save.gold == 4900);
    REQUIRE(save.progress().inventory.keys == 1);
    REQUIRE(sellShopItem(save, item(1)) == ShopResult::Sold);
    REQUIRE(save.gold == 4975);
    REQUIRE(save.progress().inventory.keys == 0);
    REQUIRE(sellShopItem(save, item(1)) == ShopResult::NotOwned);
    REQUIRE(save.gold == 4975);
    for (s32 i = 0; i < 9; ++i) {
        REQUIRE(buyShopItem(save, stats, item(3, 250), i % 4 + 1) == ShopResult::Bought);
    }
    REQUIRE(buyShopItem(save, stats, item(3, 250), 1) == ShopResult::Full);
    REQUIRE(save.progress().inventory.potions.size() == 9);
    const auto gold = save.gold;
    REQUIRE(sellShopItem(save, item(3, 250)) == ShopResult::Sold);
    REQUIRE(save.gold == gold + 187);
    REQUIRE(save.progress().inventory.potions.size() == 8);
}
TEST_CASE("shop food heals without exceeding the level health limit", "[shop]") {
    auto save = shopper();
    save.progress().health = 480;
    const ClassStats stats;
    REQUIRE(buyShopItem(save, stats, item(17, 250, 100), 1) == ShopResult::Bought);
    REQUIRE(save.health() == 500);
    REQUIRE(save.gold == 4750);
    REQUIRE(buyShopItem(save, stats, item(17, 250, 100), 1) == ShopResult::Full);
    REQUIRE(sellShopItem(save, item(17, 250, 100)) == ShopResult::NotOwned);
}

TEST_CASE("purchased inventory and stat bonuses survive a real save-slot round trip", "[shop]") {
    auto save = shopper();
    save.name = "SHOP";
    const ClassStats stats;
    REQUIRE(buyShopItem(save, stats, item(1), 1) == ShopResult::Bought);
    REQUIRE(buyShopItem(save, stats, item(5, 1000), 1) == ShopResult::Bought);
    REQUIRE(buyShopItem(save, stats, item(13, 500), 1) == ShopResult::Bought);
    const auto directory = test::scratchDirectory("shop-persistence");
    SaveSlots slots;
    REQUIRE(slots.open(directory, 4));
    const std::array<PartyMember, 1> party{{{3, save, 2}}};
    REQUIRE(saveParty(slots, party) == 1);
    CharacterSave loaded;
    REQUIRE(slots.load(2, loaded));
    REQUIRE(loaded.gold == 3400);
    REQUIRE(loaded.progress().inventory == save.progress().inventory);
    REQUIRE(loaded.progress().fightAdd == 10);
}
TEST_CASE("shop stat increases persist and stop at the stat cap", "[shop]") {
    const s32 type = GENERATE(5, 6, 7, 8);
    auto save = shopper();
    const ClassStats stats;
    REQUIRE(buyShopItem(save, stats, item(type, 1000), 1) == ShopResult::Bought);
    REQUIRE(displayStats(stats, 1, save.progress()).values[static_cast<usize>(type - 5)] == 10);
    save = CharacterSave::fromJson(save.toJson());
    REQUIRE(displayStats(stats, 1, save.progress()).values[static_cast<usize>(type - 5)] == 10);
    save.progress().fightAdd = save.progress().armorAdd = save.progress().magicAdd =
        save.progress().speedAdd = 999;
    REQUIRE(buyShopItem(save, stats, item(type), 1) == ShopResult::Full);
    REQUIRE(sellShopItem(save, item(type)) == ShopResult::NotOwned);
}
TEST_CASE("every shop powerup type creates a usable inventory slot and sells it", "[shop]") {
    for (s32 type = 1; type <= 39; ++type) {
        if (type == 1 || type == 3 || type == 17 || (type >= 5 && type <= 8)) {
            continue;
        }
        CAPTURE(type);
        auto save = shopper();
        ClassStats stats;
        stats.powerupTime = 1.5f;
        REQUIRE(buyShopItem(save, stats, item(type), 1) == ShopResult::Bought);
        REQUIRE(save.progress().inventory.powerupCount() == 1);
        REQUIRE(ownsShopItem(save, item(type)));
        save.progress().inventory.powerups[0].on = false;
        REQUIRE(sellShopItem(save, item(type)) == ShopResult::Sold);
        REQUIRE(save.progress().inventory.powerupCount() == 0);
        REQUIRE(save.gold == 4975);
    }
}
TEST_CASE("shop durations scale by class and charged items retain uses", "[shop]") {
    auto save = shopper();
    ClassStats stats;
    stats.powerupTime = 1.5f;
    REQUIRE(buyShopItem(save, stats, item(20), 1) == ShopResult::Bought);
    REQUIRE(save.progress().inventory.powerups[0].strength == 135);
    REQUIRE(buyShopItem(save, stats, item(20), 1) == ShopResult::Bought);
    REQUIRE(save.progress().inventory.powerups[0].strength == Approx(202.5f));
    REQUIRE(buyShopItem(save, stats, item(22), 1) == ShopResult::Bought);
    REQUIRE(sellShopItem(save, item(22)) == ShopResult::Sold);
    REQUIRE(ownsShopItem(save, item(20))); // light's enum 3 must not accidentally sell fire's 1
    REQUIRE(buyShopItem(save, stats, item(13), 1) == ShopResult::Bought);
    REQUIRE(save.progress().inventory.powerups[1].strength < 0);
    REQUIRE(save.progress().inventory.powerups[1].charge == 3);
}
TEST_CASE("level result tally is frame independent and never awards currency", "[shop][results]") {
    const auto entry = shopper();
    auto exit = entry;
    exit.gold += 200;
    exit.progress().experience = 900;
    const auto results = LevelResults::between(3, entry, exit, 47);
    REQUIRE(results.player == 3);
    REQUIRE(results.totals == std::array<s32, 3>{200, 47, 900});
    const s32 fps = GENERATE(30, 60, 120);
    LevelTally tally;
    tally.start(results, {1000, 100, 1000});
    for (s32 i = 0; i < fps * 8; ++i) {
        tally.update(1.0 / fps);
    }
    REQUIRE(tally.finished());
    for (usize i = 0; i < 3; ++i) {
        REQUIRE(tally.fraction(i) == 1);
    }
    tally.start(results, {0, 0, 0});
    tally.update(std::numeric_limits<f64>::quiet_NaN());
    tally.update(-1);
    REQUIRE(tally.fraction(0) == 0);
    tally.update(60);
    REQUIRE(tally.finished());
    REQUIRE(exit.gold == entry.gold + 200);
    auto losses = entry;
    losses.gold -= 5;
    REQUIRE(LevelResults::between(0, entry, losses, -1).totals == std::array<s32, 3>{0, 0, 0});
}
TEST_CASE("shop lanes independently tally buy sell and wait for sparse player IDs",
          "[shop][results]") {
    ShopSession session;
    const auto data = classes();
    const std::array<PartyMember, 2> party{{{3, shopper(), 2}, {1, shopper(), 5}}};
    const std::array<LevelResults, 2> results{{{1, {100, 9, 500}}, {3, {200, 20, 800}}}};
    session.start(party, results, {1000, 100, 1000}, data, catalog());
    REQUIRE(session.lanes()[0].tally.results().totals[0] == 200);
    ShopSession::Inputs input;
    input[3].select = true;
    session.update(60, input);
    REQUIRE(session.lanes()[0].phase == ShopPhase::Tally);
    session.update(0, input);
    REQUIRE(session.lanes()[0].phase == ShopPhase::BeforeStats);
    REQUIRE(session.lanes()[1].phase == ShopPhase::Tally);
    session.update(0, input);
    REQUIRE(session.lanes()[0].phase == ShopPhase::Shopping);
    REQUIRE(session.party()[0].save.gold == 5000); // entering never also buys
    input = {};
    input[3].down = true;
    session.update(0, input);
    REQUIRE(session.lanes()[0].cursor == 1);
    input = {};
    input[3].select = true;
    session.update(0, input);
    REQUIRE(session.party()[0].save.gold == 4900);
    REQUIRE(session.lanes()[0].transacted);
    session.update(0, {});
    REQUIRE_FALSE(session.lanes()[0].transacted);
    REQUIRE(session.party()[1].save.gold == 5000);
    input = {};
    input[3].back = true;
    session.update(0, input);
    REQUIRE(session.party()[0].save.gold == 4975);
    input = {};
    input[3].start = true;
    session.update(0, input);
    REQUIRE(session.lanes()[0].cursor == 0);
    input = {};
    input[3].select = true;
    session.update(0, input);
    REQUIRE(session.lanes()[0].phase == ShopPhase::AfterStats);
    session.update(0, input);
    REQUIRE(session.lanes()[0].phase == ShopPhase::Done);
    REQUIRE_FALSE(session.finished());
    input = {};
    input[1].select = true;
    for (s32 i = 0; i < 4; ++i) {
        session.update(0, input);
    }
    REQUIRE(session.finished());
    REQUIRE(session.party()[0].slot == 2);
    REQUIRE(session.party()[1].slot == 5);
}
TEST_CASE("fallen shop members cannot purchase and invalid parties are refused", "[shop]") {
    ShopSession session;
    const auto data = classes();
    const std::array<PartyMember, 1> party{{{2, shopper(), std::nullopt, true}}};
    session.start(party, {}, {}, data, catalog());
    REQUIRE(session.finished());
    ShopSession::Inputs input;
    input[2].select = input[2].down = true;
    session.update(1, input);
    REQUIRE(session.party()[0].save.gold == 5000);
    const std::array<PartyMember, 2> duplicate{{party[0], party[0]}};
    REQUIRE_THROWS_AS(session.start(duplicate, {}, {}, data, catalog()), FormatError);
}

TEST_CASE("Sumner shops using Wizard data without a fictitious SUM class record", "[shop]") {
    ShopSession session;
    const auto data = classes();
    auto save = shopper();
    save.character = kSumnerClass;
    const std::array<PartyMember, 1> party{{{0, save}}};
    REQUIRE(data.stats(kSumnerClass) == nullptr);
    session.start(party, {}, {}, data, catalog());
    REQUIRE(session.lanes().size() == 1);
    REQUIRE(session.lanes()[0].stats.powerupTime == 1.25f);
    REQUIRE(buyShopItem(save, session.lanes()[0].stats, item(5), 1) == ShopResult::Full);
}
TEST_CASE("unpacked shop contains the retail catalog and every item can be bought",
          "[shop][unpacked]") {
    const auto file = test::unpackedOrSkip("shop/catalog.json");
    ShopCatalog data;
    REQUIRE(data.load(file));
    REQUIRE(data.items().size() == 34);
    REQUIRE(data.items()[1].type == 17);
    REQUIRE(data.items()[1].price == 50);
    REQUIRE(data.items()[1].amount == 20);
    for (const auto& row : data.items()) {
        auto save = shopper();
        const ClassStats stats;
        REQUIRE(buyShopItem(save, stats, row, 1) ==
                (row.type == 0 ? ShopResult::Exit : ShopResult::Bought));
    }
}
} // namespace
